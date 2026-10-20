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


@pytest.mark.parametrize("bad", [
    dict(addr={0: 0xE8001004, 1: 0xE800E000, 9: 0xF0005000}),     # ukernel not 8-aligned
    dict(addr={0: 0xE8001000, 1: 0xE800E008, 9: 0xF0005000}),     # PDS not 16-aligned
    dict(addr={0: 0xE8001000, 9: 0xF0005000}),                    # a buffer has no address
    dict(heap_base={2: 0xE9000000, 4: 0, 6: 0}),                  # ukernel below code base
])
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
