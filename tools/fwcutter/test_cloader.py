"""Tests for the C port of the loader (kernel/sgx_fw/, via the fwdump harness).

fwload.py is the reference: for every input, fwdump must print exactly what fwload.load()
yields and write the same buffer images, or reject the input where fwload rejects it.
The real-blob cases cut the user's libsrv_init.so at runtime, as test_fwload.py does.
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
from test_fwformat import CORRUPTIONS, unknown_section_type
from test_fwload import BAD_LAYOUTS, LAYOUTS, TINY_LAYOUT, harness_layout, tiny

HERE = pathlib.Path(__file__).resolve().parent
KDIR = HERE.parent.parent / "kernel" / "sgx_fw"
# What fwload rejects with, besides its own errors: a missing key, a bad index, a store
# past the end of an image.
PY_ERRORS = (F.FormatError, fwload.LayoutError, KeyError, IndexError, struct.error)

Result = tuple[str, dict[int, bytes]] | None


def layout_text(lay: fwload.Layout) -> str:
    lines = [f"addr {i} {a:#x}" for i, a in lay.addr.items()]
    lines += [f"heap {h} {b:#x}" for h, b in lay.heap_base.items()]
    return "\n".join(lines + [f"pd_phys {lay.pd_phys:#x}", ""])


def dump_text(fw: F.Firmware, boot: fwload.Boot) -> str:
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
    return "\n".join(out) + "\n"


def py_load(raw: bytes, lay: fwload.Layout) -> Result:
    try:
        fw = F.unpack(raw)
        boot = fwload.load(fw, lay)
    except PY_ERRORS:
        return None
    return dump_text(fw, boot), boot.images


@pytest.fixture(scope="module")
def c_load(tmp_path_factory):
    subprocess.run(["make", "-s", "-B", "-C", str(KDIR), "fwdump"], check=True)
    work = tmp_path_factory.mktemp("cloader")
    count = iter(range(1 << 30))

    def run(raw: bytes, lay: fwload.Layout) -> Result:
        d = work / str(next(count))
        d.mkdir()
        (d / "fw").write_bytes(raw)
        (d / "layout.txt").write_text(layout_text(lay))
        p = subprocess.run([str(KDIR / "fwdump"), str(d / "fw"), str(d / "layout.txt"),
                            str(d / "out")], capture_output=True, text=True)
        if p.returncode:
            assert p.stderr.startswith("fwdump: "), p.stderr
            return None
        images = {int(f.stem): f.read_bytes() for f in (d / "out").glob("*.bin")}
        return p.stdout, images
    return run


def same(c_load, raw: bytes, lay: fwload.Layout) -> Result:
    py = py_load(raw, lay)
    assert c_load(raw, lay) == py
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
    assert fwload.limm_get(struct.unpack_from("<Q", res[1][0], 8)[0]) == 0x10000004


# Malformed records fwload passes through but the C loader refuses: a kernel must not
# write past an allocation or guess at an encoding.
STRICTER = {
    "image larger than buffer": lambda: _with(images={0: bytes(24)}),
    "image of unknown buffer": lambda: _with(images={**tiny().images, 7: bytes(8)}),
    "unknown encoding": lambda: _patched(6, encoding=3),
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
