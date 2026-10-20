"""Tests for fwload.py — the reference loader (design/sgx545_fw_format.md §3-§6).

The capture tests are the format's completeness proof: with the addresses the harness
handed srvinit, the loader must reproduce what srvinit uploaded and passed to the kernel.
"""
import pathlib
import struct
import sys

import pytest

import facts
import fwcutter
import fwformat as F
import fwload

HERE = pathlib.Path(__file__).resolve().parent
HARNESS = HERE.parent.parent / "harness" / "ukernel-extract"
sys.path.insert(0, str(HARNESS))
import initinfo  # noqa: E402

# (template, runtime) words at the 9 ukernel patch sites, harness layout_base.
LIMM_PAIRS = [
    (0xFC237150000DBEEF, 0xFC20008000001000), (0xFC237150004DBEEF, 0xFC20314000400F00),
    (0xFC237150008DBEEF, 0xFC23C00000805000), (0xFC237150000DBEEF, 0xFC20004000000100),
    (0xFC237150004DBEEF, 0xFC20314000401000), (0xFC237150000DBEEF, 0xFC23A00000011000),
    (0xFC237150000DBEEF, 0xFC20000000000006), (0xFC237150028DBEEF, 0xFC2000C002800100),
    (0xFC237150024DBEEF, 0xFC20314002401200),
]


@pytest.mark.parametrize("template,runtime", LIMM_PAIRS)
def test_limm_patches_only_the_immediate(template, runtime):
    assert fwload.limm_get(template) == 0xDEADBEEF
    assert fwload.limm_set(template, fwload.limm_get(runtime)) == runtime


def test_limm_round_trips_every_bit():
    for bit in range(32):
        assert fwload.limm_get(fwload.limm_set(0xFC237150000DBEEF, 1 << bit)) == 1 << bit


# A synthetic firmware: one 16-byte "ukernel", one PDS program, one data buffer.
def tiny() -> F.Firmware:
    return F.Firmware(
        ddk=(1, 7, 862890), build_options=0x60218,
        images={0: struct.pack("<QQ", 0, 0xFC237150000DBEEF)},
        buffers=[F.Buffer(0, 2, 0, 16, 8), F.Buffer(1, 2, 3, 0x40, 16),
                 F.Buffer(9, 3, 0, 0x20, 32)],
        patches=[
            F.Expr(0, F.ENC_USSE_LIMM, F.K_ADDR_SHR4, 0, 8, 1, 0),
            F.Expr(1, F.ENC_WORD32, F.K_CODE_WORD, 0, 0, 0x1ac, 0x0800000F),
            F.Expr(1, F.ENC_WORD32, F.K_PDS_CTL, 0, 4, 1, 0x100),
            F.Expr(1, F.ENC_WORD32, F.K_PDS_ROWS, 0, 8, 1, 0),
            F.Expr(9, F.ENC_WORD32, F.K_PD_PHYS, 0, 0, 0, 0),
            F.Expr(9, F.ENC_WORD32, F.K_ADDR, 0, 4, 9, 0x10),
            F.Expr(9, F.ENC_WORD32, F.K_CONST, 0, 8, 0, 0x3C0003F),
        ],
        values={1: [F.Expr(0xFF, F.ENC_CODE_BASE, F.K_HEAP, 3, 0xA48, 2, 0),
                    F.Expr(0xFF, F.ENC_CODE_BASE, F.K_HEAP, 1, 0xA10, 4, 0x80000)],
                3: [F.Expr(0xFF, F.ENC_WORD32, F.K_HEAP, 0, 12, 6, 0)]},
        meta=list(range(F.META_WORDS)),
    )


TINY_LAYOUT = fwload.Layout(addr={0: 0xE8001000, 1: 0xE800E000, 9: 0xF0005000},
                            heap_base={2: 0xE8000000, 4: 0xF4000000, 6: 0xE4000000},
                            pd_phys=0x2345000)


def test_every_expression_kind():
    boot = fwload.load(tiny(), TINY_LAYOUT)
    assert struct.unpack("<QQ", boot.images[0])[1] == 0xFC20314000000E00   # E800E000 >> 4, r0
    assert struct.unpack("<3I", boot.images[1][:12]) == (
        0x08003ACF,                    # code word: (0x1ac + uk_off 0x200) << 4 | tag
        (3 << 23) | 0x100,             # emitpds control: 3 data rows
        3)
    assert len(boot.images[1]) == 0x40 and len(boot.images[9]) == 0x20
    assert struct.unpack("<3I", boot.images[9][:12]) == (0x2345000, 0xF0005010, 0x3C0003F)
    assert boot.scripts["part2"] == [(0xA48, 0x07D00000), (0xA10, 0x03E81000)]
    assert boot.dev_data[12] == 0xE4000000 and len(boot.dev_data) == 24


BAD_LAYOUTS = [
    dict(addr={0: 0xE8001004, 1: 0xE800E000, 9: 0xF0005000}),     # ukernel not 8-aligned
    dict(addr={0: 0xE8001000, 1: 0xE800E008, 9: 0xF0005000}),     # PDS not 16-aligned
    dict(addr={0: 0xE8001000, 9: 0xF0005000}),                    # a buffer has no address
    dict(heap_base={2: 0xE9000000, 4: 0, 6: 0}),                  # ukernel below code base
]


@pytest.mark.parametrize("bad", BAD_LAYOUTS)
def test_layout_constraints(bad):
    fields = dict(addr=TINY_LAYOUT.addr, heap_base=TINY_LAYOUT.heap_base, pd_phys=0)
    with pytest.raises(fwload.LayoutError):
        fwload.load(tiny(), fwload.Layout(**{**fields, **bad}))


def test_meta_is_named():
    boot = fwload.load(tiny(), TINY_LAYOUT)
    assert boot.kick["TA"] == 0 and boot.kick["FLUSHPDCACHE"] == 10
    assert boot.struct_sizes["COMMAND"] == 23
    assert boot.edm_task_regs == (24, 25) and boot.cache_control == 30


# --- against the harness captures -------------------------------------------------------

EXTRACTED = HARNESS / "extracted"
# Harness fakedrm.c: srvinit's allocation order and a page-granular bump allocator.
ALLOC_ORDER = [6, 7, 8, 9, 10, 11, 12, 13, 14, 0, 1, 2, 3, 4, 5]
LAYOUTS = {
    "layout_base": dict(start=0x1000, shift=0, pd_phys=0x1000),
    "layout_moved": dict(start=0x5000, shift=0x100000, pd_phys=0x2345000),
}
SGXCONFIG_BASES = {2: 0xE8000000, 3: 0xF0000000, 4: 0xF4000000, 5: 0xFC000000, 6: 0xE4000000}


def harness_layout(start: int, shift: int, pd_phys: int) -> fwload.Layout:
    bases = {h: b + shift for h, b in SGXCONFIG_BASES.items()}
    nxt = {h: b + start for h, b in bases.items()}
    by_id = {b.id: b for b in facts.BUFFERS}
    addr = {}
    for i in ALLOC_ORDER:
        b = by_id[i]
        a = (nxt[b.heap] + max(b.align, 0x1000) - 1) & ~(max(b.align, 0x1000) - 1)
        addr[i] = a
        nxt[b.heap] = a + ((b.size + 0xFFF) & ~0xFFF)
    return fwload.Layout(addr=addr, heap_base=bases, pd_phys=pd_phys)


def capture_file(layout: str, buffer_id: int) -> bytes:
    n = ALLOC_ORDER.index(buffer_id)
    (path,) = (EXTRACTED / layout).glob(f"alloc{n:02d}_*.bin")
    return path.read_bytes()


BLOB = fwcutter.default_input()


@pytest.fixture(scope="module")
def fw():
    if BLOB is None:
        pytest.skip("libsrv_init.so not available")
    return F.unpack(F.pack(fwcutter.cut(BLOB.read_bytes())))


@pytest.mark.parametrize("layout", LAYOUTS)
def test_loader_reproduces_capture(fw, layout):
    if not (EXTRACTED / layout).is_dir():
        pytest.skip(f"harness capture {layout} not present")
    boot = fwload.load(fw, harness_layout(**LAYOUTS[layout]))

    for b in facts.BUFFERS:
        cap = capture_file(layout, b.id)
        assert boot.images[b.id] == cap[:b.size], f"buffer {b.id}"
        assert not any(cap[b.size:]), f"buffer {b.id} tail"

    info = initinfo.parse((EXTRACTED / layout / "devinitpart2_in.bin").read_bytes())
    assert boot.scripts == info.scripts
    assert boot.dev_data == info.dev_data
    assert boot.kick == info.kick
    assert boot.struct_sizes == info.struct_sizes
    assert boot.edm_task_regs == info.edm_task_regs
    assert boot.clkgate == info.clkgate
    assert boot.cache_control == info.cache_control
    assert fw.build_options == info.build_options


def test_layouts_differ_in_every_address_kind(fw):
    a, b = (fwload.load(fw, harness_layout(**LAYOUTS[k])) for k in LAYOUTS)
    for e in fw.patches + [e for v in fw.values.values() for e in v]:
        if e.kind not in (F.K_CONST, F.K_PDS_CTL, F.K_PDS_ROWS):
            ev = [fwload.evaluate(e, x.layout, fw) for x in (a, b)]
            assert ev[0] != ev[1], e


def test_cli_writes_images_and_scripts(fw, tmp_path):
    import json
    import subprocess
    lay = harness_layout(**LAYOUTS["layout_base"])
    doc = {"addr": {facts.BUFFER_NAMES[i]: hex(a) for i, a in lay.addr.items()},
           "heap_base": {str(h): hex(b) for h, b in lay.heap_base.items()},
           "pd_phys": hex(lay.pd_phys)}
    (tmp_path / "layout.json").write_text(json.dumps(doc))
    (tmp_path / "fw").write_bytes(F.pack(fw))
    out = subprocess.run([sys.executable, str(HERE / "fwload.py"), str(tmp_path / "fw"),
                          str(tmp_path / "layout.json"), "-o", str(tmp_path / "img")],
                         capture_output=True, text=True, check=True).stdout
    assert "0x0a68 = 0xe800e000" in out and "init script part2 (38 writes)" in out
    assert (tmp_path / "img" / "00_ukernel.bin").read_bytes() == fwload.load(fw, lay).images[0]


# --- format v2: transfer-queue templates (format §9) -------------------------------------

def test_emitpix_sb_scatter():
    assert fwload.emitpix_sb_set(0xFB24000000208180, 0x200) == 0xFB24004000208180
    for bit in range(14):
        insn = fwload.emitpix_sb_set(0xFB24000000208180, 1 << bit)
        assert fwload.emitpix_sb_get(insn) == 1 << bit
        assert insn & ~fwload.emitpix_sb_set(0, 0x3FFF) == 0xFB24000000208180


def test_fill_params():
    # transfer §4.2.5: 64 x 32 at A with a 256-byte stride
    assert fwload.fill_params(0xF0200000, 64, 32, 256, 0xFF336699) == [
        0xFF336699, 0x00000000, 0x001F003F, 0x001F8000, 0xF0200000, 0x00000200]
    assert fwload.fill_params(0, 8192, 8192, 0x8000, 0)[2:4] == [0x1FFF1FFF, 0x0FFF8000]


# (dest, width, height, stride, colour) that fill_params refuses.
BAD_FILLS = [
    (0xF0200000, 0, 32, 256, 0), (0xF0200000, 64, 0, 256, 0),
    (0xF0200000, 8193, 1, 0x8008, 0), (0xF0200000, 64, 8193, 256, 0),
    (0xF0200000, 64, 32, 252, 0),            # pixel stride odd
    (0xF0200000, 64, 32, 248, 0),            # stride below the width
    (0xF0200000, 64, 32, 0x8008, 0),         # LINESTRIDE overflows 12 bits
    (0xF0200002, 64, 32, 256, 0),            # address not a multiple of 4
    (0xFFFFF000, 64, 32, 256, 0),            # surface past 4 GiB
]


@pytest.mark.parametrize("bad", BAD_FILLS)
def test_fill_params_rejects(bad):
    with pytest.raises(fwload.ParamError):
        fwload.fill_params(*bad)


FILL = (0xF0200000, 64, 32, 256, 0xFF336699)
PARAMS = fwload.fill_params(*FILL)


def tiny_v2() -> F.Firmware:
    """tiny() plus a USE program, an EOT-like program and a PDS program."""
    fw = tiny()
    fw.meta[F.META_TRANSFERCMD_SIZE] = 0x4C8
    fw.tq_meta = [F.TQMeta(0, 4, 0, 0, 0x10, 8, 0, F.TQ_NO_STRADDLE),
                  F.TQMeta(2, 4, 4, 0, 0x38, 8, 0, F.TQ_NO_STRADDLE),
                  F.TQMeta(4, 6, 0, 0, 0x18, 16, 0x10, 0)]
    fw.tq_images = {0: struct.pack("<QQ", 0x1111, 0xFCA4000100000000),
                    2: bytes(48) + struct.pack("<Q", 0xFB24000000208180)}
    fw.tq_patches = [
        F.Expr(0, F.ENC_USSE_LIMM, F.K_PARAM, 0, 8, 0, 0),
        F.Expr(2, F.ENC_WORD32, F.K_CONST, 0, 16, 0, 1 << 21),
        F.Expr(2, F.ENC_USSE_LIMM, F.K_PARAM, 0, 16, 2, 0),
        F.Expr(2, F.ENC_EMITPIX_SB, F.K_PARAM, 0, 48, 5, 0),
        F.Expr(4, F.ENC_WORD32, F.K_DOUTU, 0, 0, 0, 0),
        F.Expr(4, F.ENC_WORD32, F.K_DOUTU, 0, 4, 2, 0),
        F.Expr(4, F.ENC_WORD32, F.K_HEAP, 0, 8, 6, 0),
        F.Expr(4, F.ENC_WORD32, F.K_ADDR, 0, 12, 2, 4),
        F.Expr(4, F.ENC_WORD32, F.K_PARAM, 0, 16, 4, 1),
    ]
    fw.values[F.V_TQ_CMD] = [
        F.Expr(0xFF, F.ENC_WORD32, F.K_CONST, 0, 0x4, 0, 0x200),
        F.Expr(0xFF, F.ENC_WORD32, F.K_ADDR, 0, 0x24, 4, 0),
        F.Expr(0xFF, F.ENC_WORD32, F.K_HEAP, 0, 0x3C, 6, 0),
        F.Expr(0xFF, F.ENC_WORD32, F.K_PARAM, 0, 0x40, 4, 0),
    ]
    return fw


TINY_V2_LAYOUT = fwload.Layout(addr=TINY_LAYOUT.addr, heap_base=TINY_LAYOUT.heap_base,
                               pd_phys=TINY_LAYOUT.pd_phys,
                               tq_addr={0: 0xF4000000, 2: 0xF4000040, 4: 0xE4000040})


def test_tq_every_kind():
    tq = fwload.build_tq(tiny_v2(), TINY_V2_LAYOUT, PARAMS)
    assert sorted(tq.images) == [0, 2, 4]
    assert [len(tq.images[i]) for i in (0, 2, 4)] == [0x10, 0x38, 0x18]
    fill = struct.unpack("<QQ", tq.images[0])
    assert fill == (0x1111, fwload.limm_set(0xFCA4000100000000, 0xFF336699))
    eot = tq.images[2]
    assert struct.unpack_from("<Q", eot, 16)[0] == fwload.limm_set(1 << 21, 0x001F003F)
    assert struct.unpack_from("<Q", eot, 48)[0] == 0xFB24004000208180
    assert struct.unpack("<6I", tq.images[4]) == (
        0x00000000,                    # DOUTU of template 0 at the heap base, 0 temps
        0x01000080,                    # template 2: 0x40 bytes in, 4 temps
        0xE4000000, 0xF4000044, 0xF0200001, 0)
    assert tq.cmd == [(0x4, 0x200), (0x24, 0xE4000040), (0x3C, 0xE4000000),
                      (0x40, 0xF0200000)]


def test_fill_is_fill_params_then_build():
    a = fwload.fill(tiny_v2(), TINY_V2_LAYOUT, *FILL)
    assert a == fwload.build_tq(tiny_v2(), TINY_V2_LAYOUT, PARAMS)
    assert a.params == PARAMS


def test_build_tq_needs_tq_content():
    with pytest.raises(F.FormatError):
        fwload.build_tq(tiny(), TINY_V2_LAYOUT, PARAMS)


BAD_TQ_LAYOUTS = {
    "template without address": dict(tq_addr={0: 0xF4000000, 2: 0xF4000040}),
    "misaligned": dict(tq_addr={0: 0xF4000004, 2: 0xF4000040, 4: 0xE4000040}),
    "below its heap": dict(tq_addr={0: 0xF4000000, 2: 0xF4000040, 4: 0xE3FFFFF0}),
    "straddles a page": dict(tq_addr={0: 0xF4000FF8, 2: 0xF4000040, 4: 0xE4000040}),
    "DOUTU target too far": dict(tq_addr={0: 0xF4080000, 2: 0xF4000040, 4: 0xE4000040}),
    "missing heap": dict(heap_base={2: 0xE8000000, 4: 0xF4000000}),
    "past 4 GiB": dict(tq_addr={0: 0xF4000000, 2: 0xF4000040, 4: 0xFFFFFFF0},
                       heap_base={2: 0xE8000000, 4: 0xF4000000, 6: 0xFFFFF000}),
}


@pytest.mark.parametrize("name", BAD_TQ_LAYOUTS)
def test_tq_layout_constraints(name):
    import dataclasses
    lay = dataclasses.replace(TINY_V2_LAYOUT, **BAD_TQ_LAYOUTS[name])
    with pytest.raises(fwload.LayoutError):
        fwload.build_tq(tiny_v2(), lay, PARAMS)


def test_no_straddle_is_per_template():
    import dataclasses
    # template 4 has no NO_STRADDLE flag: it may cross a page
    lay = dataclasses.replace(TINY_V2_LAYOUT, tq_addr={**TINY_V2_LAYOUT.tq_addr,
                                                       4: 0xE4000FF0})
    assert fwload.build_tq(tiny_v2(), lay, PARAMS).cmd[1] == (0x24, 0xE4000FF0)


# --- the real templates, against transfer §4.2.7 -----------------------------------------

UM = fwcutter.default_um_input()


@pytest.fixture(scope="module")
def fw2():
    if BLOB is None or UM is None:
        pytest.skip("libsrv_init.so / libsrv_um.so not available")
    return F.unpack(F.pack(fwcutter.cut(BLOB.read_bytes(), UM.read_bytes())))


# Transfer §4.2.7 step 1: where the recipe puts the seven programs.
RECIPE_LAYOUT = fwload.Layout(
    addr={}, heap_base={4: 0xF4000000, 6: 0xE4000000},
    tq_addr={facts.TQ_PDS_PRIM: 0xE4000000, facts.TQ_PDS_SEC: 0xE4000020,
             facts.TQ_PDS_PIXEVENT: 0xE4000040, facts.TQ_USE_FILL: 0xF4000000,
             facts.TQ_USE_SEC: 0xF4000010, facts.TQ_USE_EOT: 0xF4000020,
             facts.TQ_USE_EOR: 0xF4000060})


def words(b: bytes) -> list[int]:
    return list(struct.unpack(f"<{len(b) // 4}I", b))


def test_recipe_words(fw2):
    tq = fwload.fill(fw2, RECIPE_LAYOUT, *FILL)
    img = tq.images
    assert words(img[facts.TQ_USE_FILL]) == [0, 0xFA440700, 0x00136699, 0xFCA7F191]
    assert img[facts.TQ_USE_SEC] == fw2.tq_images[facts.TQ_USE_SEC]
    assert words(img[facts.TQ_USE_EOT]) == [
        0x00000000, 0xFA440700, 0x00000000, 0xFCA00000, 0x003F003F, 0xFCA00000,
        0x005F8000, 0xFCA00000, 0x00600000, 0xFCA3C010, 0x00200080, 0xFB200100,
        0x00208180, 0xFB240040]
    assert words(img[facts.TQ_USE_EOR]) == [0, 0xFA440700, 0, 0xFCA00000, 0x80200000,
                                            0xFB260040]
    assert words(img[facts.TQ_PDS_PRIM]) == [0, 4, 0, 0, 0x07000305, 0xAF000000]
    assert words(img[facts.TQ_PDS_SEC]) == [0x20, 4, 0, 0, 0x07000305, 0xAF000000]
    pix = words(img[facts.TQ_PDS_PIXEVENT])
    assert pix[:20] == [0x01000040, 0x00800000, 0, 0x01000040, 0x010000C0, 0x00C00000,
                        8, 2, 0x100, 0xFF8, 4, 0, 0x00800000, 0x003FF000, 0, 0,
                        0x00400000, 0, 0, 0]
    assert pix[20:] == words(fw2.tq_images[facts.TQ_PDS_PIXEVENT])[20:]
    assert dict(tq.cmd) == {
        0x004: 0x200, 0x008: 0x3F800000, 0x00C: 3, 0x014: 0x100, 0x01C: 0x80000000,
        0x020: 0x88, 0x024: 0xE4000040, 0x028: 5, 0x02C: 1, 0x038: 0x80,
        0x03C: 0xE4000000, 0x194: 0x07C00000}


def test_cmd_pixel_pds_words_follow_meta(fw2):
    meta = {m.id: m for m in fw2.tq_meta}[facts.TQ_PDS_PIXEVENT]
    cmd = dict(fwload.fill(fw2, RECIPE_LAYOUT, *FILL).cmd)
    assert cmd[0x28] == meta.pds_data >> 4
    assert cmd[0x2C] == (4 * meta.pds_attrs + 15) >> 4


USSE_DIS = HERE.parent / "usse-dis"
REFDISASM = USSE_DIS / "refdisasm" / "refdisasm545"


def disasm(tool: str, img: bytes, tmp_path, *args: str) -> list[str]:
    path = tmp_path / "prog.bin"
    path.write_bytes(img)
    import subprocess
    out = subprocess.run([tool, str(path), *args], capture_output=True, text=True,
                         check=True).stdout
    # "<index>: 0x<hex>\t<text>"; skip refdisasm's banner lines
    return [ln.split("\t", 1)[1].strip() for ln in out.splitlines() if "\t" in ln]


@pytest.fixture(scope="module")
def pds_dis():
    import subprocess
    subprocess.run(["make", "-s", "-C", str(USSE_DIS), "pds-dis"], check=True)
    return lambda img, ndata, tmp: disasm(str(USSE_DIS / "pds-dis"), img, tmp,
                                          str(ndata))[ndata:]


@pytest.fixture
def use_dis(tmp_path):
    if not REFDISASM.exists():
        pytest.skip("refdisasm545 not built (tools/usse-dis/refdisasm/build.sh)")
    return lambda img: disasm(str(REFDISASM), img, tmp_path)


PHAS = "phas #0, #0, pixel, end, parallel"


def test_patched_use_programs_disassemble(fw2, use_dis):
    img = fwload.fill(fw2, RECIPE_LAYOUT, *FILL).images
    assert use_dis(img[facts.TQ_USE_FILL]) == [PHAS, "mov.skipinv.end o0, #0xFF336699"]
    assert use_dis(img[facts.TQ_USE_SEC]) == [PHAS, "nop.end"]
    assert use_dis(img[facts.TQ_USE_EOT]) == [
        PHAS, "mov.skipinv r0, #0x00000000", "mov.skipinv r1, #0x001F003F",
        "mov.skipinv r2, #0x001F8000", "mov.skipinv r3, #0xF0200000",
        "emitpix1.freep #0 /* incp */, r0, r1",
        "emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000200"]
    assert use_dis(img[facts.TQ_USE_EOR]) == [
        PHAS, "mov.skipinv r0, #0x00000000",
        "emitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200"]


def test_patched_pds_programs_disassemble(fw2, pds_dis, tmp_path):
    img = fwload.fill(fw2, RECIPE_LAYOUT, *FILL).images
    kick = ["movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]", "halt"]
    assert pds_dis(img[facts.TQ_PDS_PRIM], 4, tmp_path) == kick
    assert pds_dis(img[facts.TQ_PDS_SEC], 4, tmp_path) == kick
    code = pds_dis(img[facts.TQ_PDS_PIXEVENT], 20, tmp_path)
    assert len(code) == 29
    assert code[3] == "movs doutu, ds0[0], ds0[1], ds0[0], ds0[0]"      # PTOFF task
    assert code[22] == "movs doutu, ds1[1], ds0[48], ds0[48], ds0[48]"  # EOT task
    assert code[27] == "movs doutu, ds0[2], ds0[3], ds0[2], ds0[2]"     # EOR task


def test_cli_fill(fw2, tmp_path):
    import json
    import subprocess
    doc = {"addr": {}, "heap_base": {str(h): hex(b) for h, b in
                                     RECIPE_LAYOUT.heap_base.items()},
           "tq_addr": {facts.TQ_NAMES[i]: hex(a) for i, a in RECIPE_LAYOUT.tq_addr.items()}}
    (tmp_path / "layout.json").write_text(json.dumps(doc))
    (tmp_path / "fw").write_bytes(F.pack(fw2))
    out = subprocess.run([sys.executable, str(HERE / "fwload.py"), str(tmp_path / "fw"),
                          str(tmp_path / "layout.json"), "--fill",
                          "0xf0200000,64,32,256,0xff336699", "-o", str(tmp_path / "img")],
                         capture_output=True, text=True, check=True).stdout
    assert "0x024 = 0xe4000040" in out
    got = (tmp_path / "img" / "tq0_USE_FILL.bin").read_bytes()
    assert got == fwload.fill(fw2, RECIPE_LAYOUT, *FILL).images[0]
