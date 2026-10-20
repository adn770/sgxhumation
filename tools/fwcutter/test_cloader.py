"""Tests for the C port of the loader (kernel/sgx_fw/, via the fwdump harness).

fwload.py is the reference: for every input, fwdump must print exactly what fwload.load()
yields and write the same buffer images, or reject the input where fwload rejects it.
With a fill or a copy, it must also build the same transfer-queue programs and command words
as fwload.fill() (format v2) or fwload.copy() (format v3). The real-blob cases cut the user's
libsrv_init.so and libsrv_um.so at runtime, as test_fwload.py does.
"""
from __future__ import annotations

import dataclasses
import pathlib
import random
import struct
import subprocess

import pytest

import fwcutter
import fwformat as F
import fwload
from test_fwformat import CORRUPTIONS, TQ_DAMAGED, TQ_V3_DAMAGED, unknown_section_type
from test_fwload import (BAD_COPIES, BAD_FILLS, BAD_LAYOUTS, BAD_TQ_LAYOUTS, COPY,
                         COPY_RECIPE_LAYOUT, FILL, LAYOUTS, RECIPE_LAYOUT, TINY_LAYOUT,
                         TINY_V2_LAYOUT, TINY_V3_LAYOUT, harness_layout, tiny, tiny_v2,
                         tiny_v3)

HERE = pathlib.Path(__file__).resolve().parent
KDIR = HERE.parent.parent / "kernel" / "sgx_fw"
# What fwload rejects with, besides its own errors: a missing key, a bad index, a store
# past the end of an image.
PY_ERRORS = (F.FormatError, fwload.LayoutError, fwload.ParamError, KeyError, IndexError,
             struct.error)

# stdout and the written images: "NN" for buffer NN, "tqNN" for template NN.
Result = tuple[str, dict[str, bytes]] | None
Fill = tuple[int, ...] | None


def layout_text(lay: fwload.Layout, fill: Fill = None, copy: Fill = None) -> str:
    lines = [f"addr {i} {a:#x}" for i, a in lay.addr.items()]
    lines += [f"heap {h} {b:#x}" for h, b in lay.heap_base.items()]
    lines += [f"tq {i} {a:#x}" for i, a in lay.tq_addr.items()]
    if fill is not None:
        lines.append("fill " + " ".join(f"{v:#x}" for v in fill))
    if copy is not None:
        lines.append("copy " + " ".join(f"{v:#x}" for v in copy))
    return "\n".join(lines + [f"pd_phys {lay.pd_phys:#x}", ""])


def tq_text(fw: F.Firmware, tq: fwload.TQBuild) -> list[str]:
    def hx(v: int) -> str:
        return f"0x{v:08x}"
    out = ["tq_params " + " ".join(map(hx, tq.params))]
    out += [f"tq {m.id} heap {m.heap} temps {m.temps} attrs {m.pds_attrs} size {hx(m.size)} "
            f"align {hx(m.align)} data {hx(m.pds_data)} flags {m.flags}"
            for m in sorted(fw.tq_meta, key=lambda m: m.id)]
    out.append(f"tq_cmd {len(tq.cmd)}")
    return out + [f"  {hx(off)} {hx(val)}" for off, val in tq.cmd]


def dump_text(fw: F.Firmware, boot: fwload.Boot, tq: fwload.TQBuild | None = None) -> str:
    """fwdump's stdout, rebuilt from the reference loader's result."""
    def hx(v: int) -> str:
        return f"0x{v:08x}"
    major, minor, build = fw.ddk
    out = [f"ddk {major}.{minor}.{build} options {hx(fw.build_options)}"]
    out += [f"buffer {b.id} heap {b.heap} rows {b.pds_rows} size {hx(b.size)} "
            f"align {hx(b.align)}" for b in fw.buffers]
    for name, writes in boot.scripts.items():
        out.append(f"{name} {len(writes)}")
        out += [f"  {hx(reg)} {hx(val)}" for reg, val in writes]
    out.append("dev_data " + " ".join(map(hx, boot.dev_data)))
    meta = (list(boot.kick.values()) + list(boot.struct_sizes.values()) +
            list(boot.edm_task_regs) + list(boot.clkgate.values()) + [boot.cache_control])
    out.append("meta " + " ".join(map(hx, meta)))
    if tq is not None:
        out += tq_text(fw, tq)
    return "\n".join(out) + "\n"


def py_load(raw: bytes, lay: fwload.Layout, fill: Fill = None, copy: Fill = None) -> Result:
    try:
        fw = F.unpack(raw)
        boot = fwload.load(fw, lay)
        tq = (fwload.fill(fw, lay, *fill) if fill is not None else
              fwload.copy(fw, lay, *copy) if copy is not None else None)
    except PY_ERRORS:
        return None
    images = {f"{i:02d}": b for i, b in boot.images.items()}
    if tq is not None:
        images.update({f"tq{i:02d}": b for i, b in tq.images.items()})
    return dump_text(fw, boot, tq), images


@pytest.fixture(scope="module")
def c_load(tmp_path_factory):
    subprocess.run(["make", "-s", "-B", "-C", str(KDIR), "fwdump"], check=True)
    work = tmp_path_factory.mktemp("cloader")
    count = iter(range(1 << 30))

    def run(raw: bytes, lay: fwload.Layout, fill: Fill = None, copy: Fill = None) -> Result:
        d = work / str(next(count))
        d.mkdir()
        (d / "fw").write_bytes(raw)
        (d / "layout.txt").write_text(layout_text(lay, fill, copy))
        p = subprocess.run([str(KDIR / "fwdump"), str(d / "fw"), str(d / "layout.txt"),
                            str(d / "out")], capture_output=True, text=True)
        if p.returncode:
            assert p.stderr.startswith("fwdump: "), p.stderr
            return None
        images = {f.stem: f.read_bytes() for f in (d / "out").glob("*.bin")}
        return p.stdout, images
    return run


def same(c_load, raw: bytes, lay: fwload.Layout, fill: Fill = None,
         copy: Fill = None) -> Result:
    py = py_load(raw, lay, fill, copy)
    assert c_load(raw, lay, fill, copy) == py
    return py


# --- synthetic firmware ------------------------------------------------------------------

def test_tiny_matches_python(c_load):
    assert same(c_load, F.pack(tiny()), TINY_LAYOUT) is not None


@pytest.mark.parametrize("corrupt", CORRUPTIONS + [unknown_section_type])
def test_rejects_damaged_files(c_load, corrupt):
    raw = corrupt(F.pack(tiny()))
    assert py_load(raw, TINY_LAYOUT) is None
    assert c_load(raw, TINY_LAYOUT) is None


@pytest.mark.parametrize("bad", BAD_LAYOUTS)
def test_rejects_bad_layouts(c_load, bad):
    fields = dict(addr=TINY_LAYOUT.addr, heap_base=TINY_LAYOUT.heap_base, pd_phys=0)
    lay = fwload.Layout(**{**fields, **bad})
    assert py_load(F.pack(tiny()), lay) is None
    assert c_load(F.pack(tiny()), lay) is None


def _recrc(raw: bytearray) -> bytes:
    struct.pack_into("<I", raw, 28, F.crc(bytes(raw[32:])))
    return bytes(raw)


def _section_past_end(raw: bytes) -> bytes:
    b = bytearray(raw)
    struct.pack_into("<I", b, 32 + 8, len(raw))              # first entry's size
    return _recrc(b)


def _table_past_end(raw: bytes) -> bytes:
    b = bytearray(raw)
    struct.pack_into("<H", b, 10, 0x1000)                    # section count
    return _recrc(b)


def _with(**changes) -> bytes:
    return F.pack(dataclasses.replace(tiny(), **changes))


def _patched(i: int, **changes) -> bytes:
    patches = tiny().patches
    patches[i] = dataclasses.replace(patches[i], **changes)
    return _with(patches=patches)


# Inputs both loaders reject, though fwload only notices them while loading.
REJECTED = {
    "section past end": lambda: _section_past_end(F.pack(tiny())),
    "table past end": lambda: _table_past_end(F.pack(tiny())),
    "word past buffer": lambda: _patched(3, offset=0x3E),
    "limm past buffer": lambda: _patched(0, offset=0x0C),
    "patch on unknown buffer": lambda: _patched(4, buffer=5),
    "unknown kind": lambda: _patched(6, kind=8),
    "rows of unknown buffer": lambda: _patched(2, target=4),
    "dev data index": lambda: _with(values={3: [F.Expr(0xFF, 0, F.K_CONST, 0, 24, 0, 1)]}),
}


@pytest.mark.parametrize("name", REJECTED)
def test_rejects_bad_records(c_load, name):
    raw = REJECTED[name]()
    assert py_load(raw, TINY_LAYOUT) is None
    assert c_load(raw, TINY_LAYOUT) is None


def test_rejects_missing_heap(c_load):
    lay = dataclasses.replace(TINY_LAYOUT, heap_base={2: 0xE8000000, 6: 0xE4000000})
    assert py_load(F.pack(tiny()), lay) is None
    assert c_load(F.pack(tiny()), lay) is None


def test_ukernel_below_code_base_without_code_words(c_load):
    raw = _with(patches=[e for e in tiny().patches if e.kind != F.K_CODE_WORD])
    lay = dataclasses.replace(TINY_LAYOUT, heap_base={**TINY_LAYOUT.heap_base, 2: 0xE8002000})
    assert py_load(raw, lay) is None
    assert c_load(raw, lay) is None


def test_address_carries_into_the_shift(c_load):
    # (addr + addend) >> 4 is taken before truncating to 32 bits.
    lay = dataclasses.replace(TINY_LAYOUT, addr={**TINY_LAYOUT.addr, 1: 0xFFFFFFC0})
    res = same(c_load, _patched(0, addend=0x80), lay)
    assert res is not None
    assert fwload.limm_get(struct.unpack_from("<Q", res[1]["00"], 8)[0]) == 0x10000004


# Malformed records fwload passes through but the C loader refuses: a kernel must not
# write past an allocation or guess at an encoding.
STRICTER = {
    "image larger than buffer": lambda: _with(images={0: bytes(24)}),
    "image of unknown buffer": lambda: _with(images={**tiny().images, 7: bytes(8)}),
    "unknown encoding": lambda: _patched(6, encoding=5),
    "duplicate buffer id": lambda: _with(buffers=tiny().buffers + [F.Buffer(9, 3, 0, 0x20, 32)]),
    "unknown list": lambda: _with(values={**tiny().values, 7: []}),
    "code base param": lambda: _with(values={1: [F.Expr(0xFF, F.ENC_CODE_BASE, F.K_CONST,
                                                        0x80, 0xA48, 0, 0)]}),
}


@pytest.mark.parametrize("name", STRICTER)
def test_c_is_stricter(c_load, name):
    raw = STRICTER[name]()
    assert py_load(raw, TINY_LAYOUT) is not None
    assert c_load(raw, TINY_LAYOUT) is None


def random_layout(rng: random.Random, fw: F.Firmware) -> fwload.Layout:
    """A valid layout: every buffer aligned, the ukernel a whole instruction above the
    code base, close enough for every code word's pc to fit."""
    heaps = {h: rng.randrange(0xC0000) << 12 for h in (2, 3, 4, 5, 6)}
    addr = {}
    for b in fw.buffers:
        if b.id == 0:
            addr[0] = heaps[2] + 8 * rng.randrange(0x40000)
        else:
            addr[b.id] = rng.randrange((0x100000000 - b.size) // b.align) * b.align
    return fwload.Layout(addr=addr, heap_base=heaps, pd_phys=rng.randrange(1 << 20) << 12)


def fuzz(c_load, fw: F.Firmware, seed: int, n: int = 50) -> None:
    rng = random.Random(seed)
    raw = F.pack(fw)
    for _ in range(n):
        lay = random_layout(rng, fw)
        assert same(c_load, raw, lay) is not None, lay


def test_tiny_random_layouts(c_load):
    fuzz(c_load, tiny(), seed=545)


# --- the real firmware -------------------------------------------------------------------

BLOB = fwcutter.default_input()


@pytest.fixture(scope="module")
def fw():
    if BLOB is None:
        pytest.skip("libsrv_init.so not available")
    return F.unpack(F.pack(fwcutter.cut(BLOB.read_bytes())))


@pytest.mark.parametrize("layout", LAYOUTS)
def test_real_firmware_matches_python(c_load, fw, layout):
    res = same(c_load, F.pack(fw), harness_layout(**LAYOUTS[layout]))
    assert res is not None and len(res[1]) == 15


def test_real_firmware_random_layouts(c_load, fw):
    fuzz(c_load, fw, seed=862890)


# --- format v2: transfer-queue templates -------------------------------------------------

def test_tiny_v2_matches_python(c_load):
    res = same(c_load, F.pack(tiny_v2()), TINY_V2_LAYOUT, FILL)
    assert res is not None and {"tq00", "tq02", "tq04"} <= set(res[1])
    assert "tq_cmd 4" in res[0]


def test_v2_without_fill_is_the_boot_part(c_load):
    res = same(c_load, F.pack(tiny_v2()), TINY_V2_LAYOUT)
    assert res is not None and "tq_" not in res[0]


@pytest.mark.parametrize("name", TQ_DAMAGED)
def test_rejects_damaged_v2(c_load, name):
    raw = TQ_DAMAGED[name]()
    assert py_load(raw, TINY_V2_LAYOUT, FILL) is None
    assert c_load(raw, TINY_V2_LAYOUT, FILL) is None


@pytest.mark.parametrize("name", BAD_TQ_LAYOUTS)
def test_rejects_bad_tq_layouts(c_load, name):
    lay = dataclasses.replace(TINY_V2_LAYOUT, **BAD_TQ_LAYOUTS[name])
    assert py_load(F.pack(tiny_v2()), lay, FILL) is None
    assert c_load(F.pack(tiny_v2()), lay, FILL) is None


@pytest.mark.parametrize("bad", BAD_FILLS)
def test_rejects_bad_fills(c_load, bad):
    assert py_load(F.pack(tiny_v2()), TINY_V2_LAYOUT, bad) is None
    assert c_load(F.pack(tiny_v2()), TINY_V2_LAYOUT, bad) is None


def test_fill_needs_a_v2_file(c_load):
    assert py_load(F.pack(tiny()), TINY_V2_LAYOUT, FILL) is None
    assert c_load(F.pack(tiny()), TINY_V2_LAYOUT, FILL) is None


def random_tq_layout(rng: random.Random, fw: F.Firmware, lay: fwload.Layout,
                     only: tuple[int, ...] | None = None) -> fwload.Layout:
    """Templates in their heaps: aligned, within a page if they must be, USE code in the
    first 512 KiB of its heap (where a DOUTU word can reach it). With only, just those."""
    heaps = dict(lay.heap_base)
    tq = {}
    for m in fw.tq_meta:
        if only is not None and m.id not in only:
            continue
        span = fwload.DOUTU_RANGE if m.heap == 4 else 0x1000000
        page = heaps[m.heap] + rng.randrange(span // 0x1000) * 0x1000
        room = 0x1000 - m.size if m.flags & F.TQ_NO_STRADDLE else 0x1000
        tq[m.id] = page + rng.randrange(room // m.align + 1) * m.align
    return dataclasses.replace(lay, tq_addr=tq)


def random_fill(rng: random.Random) -> tuple[int, ...]:
    w, h = rng.randrange(1, 0x2001), rng.randrange(1, 0x2001)
    x, y = rng.randrange(0x2001 - w), rng.randrange(0x2001 - h)
    if rng.random() < 0.3:
        x = y = 0
    stride = rng.randrange((4 * (x + w) + 7) // 8, 0x1001) * 8
    dest = rng.randrange((0x100000000 - stride * (y + h)) // 4) * 4
    return dest, w, h, stride, rng.randrange(1 << 32), x, y


def fuzz_tq(c_load, fw: F.Firmware, seed: int, n: int = 50) -> None:
    rng = random.Random(seed)
    raw = F.pack(fw)
    for _ in range(n):
        lay = random_tq_layout(rng, fw, random_layout(rng, fw))
        fill = random_fill(rng)
        assert same(c_load, raw, lay, fill) is not None, (lay, fill)


def test_tiny_v2_random_layouts_and_fills(c_load):
    fuzz_tq(c_load, tiny_v2(), seed=4)


UM = fwcutter.default_um_input()


@pytest.fixture(scope="module")
def fw2():
    if BLOB is None or UM is None:
        pytest.skip("libsrv_init.so / libsrv_um.so not available")
    return F.unpack(F.pack(fwcutter.cut(BLOB.read_bytes(), UM.read_bytes())))


def test_real_v2_recipe_matches_python(c_load, fw2):
    lay = harness_layout(**LAYOUTS["layout_base"])
    lay = dataclasses.replace(lay, tq_addr=RECIPE_LAYOUT.tq_addr)
    res = same(c_load, F.pack(fw2), lay, FILL)
    assert res is not None and len(res[1]) == 15 + 7
    assert res[1]["tq06"] == fwload.fill(fw2, lay, *FILL).images[6]


def test_real_v2_random_layouts_and_fills(c_load, fw2):
    fuzz_tq(c_load, fw2, seed=42)


# --- format v3: the copy -----------------------------------------------------------------

def test_tiny_v3_copy_matches_python(c_load):
    res = same(c_load, F.pack(tiny_v3()), TINY_V3_LAYOUT, copy=COPY)
    assert res is not None and {"tq07", "tq08"} <= set(res[1])
    assert "tq_params 0x00000000 0x00000000 0x001f003f" in res[0]


def test_tiny_v3_fill_builds_only_the_placed_templates(c_load):
    res = same(c_load, F.pack(tiny_v3()), TINY_V2_LAYOUT, FILL)
    assert res is not None and {k for k in res[1] if k.startswith("tq")} == {
        "tq00", "tq02", "tq04"}


@pytest.mark.parametrize("name", TQ_V3_DAMAGED)
def test_rejects_damaged_v3(c_load, name):
    raw = TQ_V3_DAMAGED[name]()
    assert py_load(raw, TINY_V3_LAYOUT, copy=COPY) is None
    assert c_load(raw, TINY_V3_LAYOUT, copy=COPY) is None


@pytest.mark.parametrize("bad", BAD_COPIES)
def test_rejects_bad_copies(c_load, bad):
    assert py_load(F.pack(tiny_v3()), TINY_V3_LAYOUT, copy=bad) is None
    assert c_load(F.pack(tiny_v3()), TINY_V3_LAYOUT, copy=bad) is None


UNPLACED = {
    "patch names an unplaced template": {i: a for i, a in TINY_V3_LAYOUT.tq_addr.items()
                                         if i != 0},
    "command names an unplaced template": {7: 0xF4000080, 8: 0xE4000080, 0: 0xF4000000,
                                           2: 0xF4000040},
    "copy template not placed": {i: a for i, a in TINY_V3_LAYOUT.tq_addr.items() if i != 7},
}


@pytest.mark.parametrize("name", UNPLACED)
def test_rejects_unplaced_templates(c_load, name):
    lay = dataclasses.replace(TINY_V3_LAYOUT, tq_addr=UNPLACED[name])
    assert py_load(F.pack(tiny_v3()), lay, copy=COPY) is None
    assert c_load(F.pack(tiny_v3()), lay, copy=COPY) is None


def test_copy_needs_a_v3_file(c_load):
    for fw in (tiny(), tiny_v2()):
        assert py_load(F.pack(fw), TINY_V3_LAYOUT, copy=COPY) is None
        assert c_load(F.pack(fw), TINY_V3_LAYOUT, copy=COPY) is None


def test_v3_reads_a_v2_file(c_load):
    assert same(c_load, F.pack(tiny_v2()), TINY_V2_LAYOUT, FILL) is not None


def random_copy(rng: random.Random) -> tuple[int, ...]:
    w, h = rng.randrange(1, 0x1001), rng.randrange(1, 0x1001)
    x, y = rng.randrange(0x2001 - w), rng.randrange(0x2001 - h)
    if rng.random() < 0.3:
        x = y = 0
    sstride = rng.randrange(w, 0x2001) * 4
    dstride = rng.randrange((4 * (x + w) + 7) // 8, 0x1001) * 8
    src = rng.randrange((0x100000000 - sstride * h) // 4) * 4
    dst = rng.randrange((0x100000000 - dstride * (y + h)) // 4) * 4
    return src, dst, w, h, sstride, dstride, x, y


def fuzz_copy(c_load, fw: F.Firmware, seed: int, n: int = 50) -> None:
    rng = random.Random(seed)
    raw = F.pack(fw)
    for _ in range(n):
        lay = random_tq_layout(rng, fw, random_layout(rng, fw))
        copy = random_copy(rng)
        assert same(c_load, raw, lay, copy=copy) is not None, (lay, copy)


def test_tiny_v3_random_layouts_and_copies(c_load):
    fuzz_copy(c_load, tiny_v3(), seed=3)


def test_real_v3_copy_recipe_matches_python(c_load, fw2):
    lay = harness_layout(**LAYOUTS["layout_base"])
    lay = dataclasses.replace(lay, tq_addr=COPY_RECIPE_LAYOUT.tq_addr)
    res = same(c_load, F.pack(fw2), lay, copy=COPY)
    assert res is not None and len(res[1]) == 15 + 7
    assert res[1]["tq08"] == fwload.copy(fw2, lay, *COPY).images[8]


def test_real_v3_random_copies(c_load, fw2):
    """Random copy-only layouts and copies on the real file."""
    rng = random.Random(7)
    raw = F.pack(fw2)
    for _ in range(50):
        lay = random_tq_layout(rng, fw2, random_layout(rng, fw2), fwload.COPY_TEMPLATES)
        copy = random_copy(rng)
        res = same(c_load, raw, lay, copy=copy)
        assert res is not None and "tq08" in res[1] and "tq00" not in res[1], (lay, copy)


def test_real_v2_file_still_loads(c_load):
    if BLOB is None or UM is None:
        pytest.skip("libsrv_init.so / libsrv_um.so not available")
    v2 = fwcutter.cut(BLOB.read_bytes(), UM.read_bytes(), version=2)
    lay = harness_layout(**LAYOUTS["layout_base"])
    lay = dataclasses.replace(lay, tq_addr=RECIPE_LAYOUT.tq_addr)
    res = same(c_load, F.pack(v2), lay, FILL)
    assert res is not None and len(res[1]) == 15 + 7
    assert py_load(F.pack(v2), lay, copy=COPY) is None
