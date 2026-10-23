# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The assembled program source (src/prog_asm.c) against the Python reference.

For the same placement and parameters, every program sgxfive_prog_asm_create() builds must
equal fwload.build_tq() word for word: on a firmware whose templates are our assembled
programs (no closed input needed) and, with the user's libsrv_init.so and libsrv_um.so, on
the cut v3 file as well. The generated tables (src/prog_asm_gen.h) must be what
tools/gen_progs.py writes from asm/transfer/ and tools/fwcutter/facts.py today.
Run through `make check`, which builds build/prog_dump.
"""
from __future__ import annotations

import os
import pathlib
import random
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "fwcutter"))
import facts      # noqa: E402
import fwcutter   # noqa: E402
import fwformat as F   # noqa: E402
import fwload     # noqa: E402
import ours       # noqa: E402

DUMP = os.environ.get("SGXFIVE_PROG_DUMP", str(HERE.parent / "build" / "prog_dump"))
GEN = HERE.parent / "tools" / "gen_progs.py"
GENERATED = HERE.parent / "src" / "prog_asm_gen.h"
# fw_format heap ids (sgxapi_km.h) -> DRM_SGXFIVE_HEAP_*: PIXELSHADER, PDSPIXEL
DRM_HEAP = {4: 4, 6: 2}


@pytest.fixture(scope="module", autouse=True)
def tool_exists():
    if not pathlib.Path(DUMP).exists():
        pytest.skip(f"{DUMP} not built (run make -C userspace/libsgxfive check)")


def ours_fw() -> F.Firmware:
    """The transfer-queue part of a firmware file built from our source alone."""
    return F.Firmware(ddk=(1, 7, 862890), build_options=0, images={}, buffers=[],
                      patches=[], values={F.V_TQ_CMD: list(facts.TQ_CMD)},
                      tq_images=ours.templates(), tq_meta=list(facts.TQ_META),
                      tq_patches=list(facts.TQ_PATCHES), tq_ours=set(ours.PROGS))


def dump(lay: fwload.Layout, params: list[int]) -> tuple[dict[int, str], dict[int, bytes | int]]:
    """prog_dump asm: the meta lines and the images (or errno) by program id."""
    text = [f"heap {DRM_HEAP[h]} {b:#x}" for h, b in lay.heap_base.items()]
    text += [f"addr {i} {a:#x}" for i, a in lay.tq_addr.items()]
    text += [f"param {i} {v:#x}" for i, v in enumerate(params)]
    out = subprocess.run([DUMP, "asm"], input="\n".join(text) + "\n", capture_output=True,
                         text=True, check=True).stdout
    meta, progs = {}, {}
    for line in out.splitlines():
        kind, pid, rest = line.split(" ", 2)
        if kind == "meta":
            meta[int(pid)] = rest
        elif rest.startswith("error "):
            progs[int(pid)] = int(rest.split()[1])
        else:
            progs[int(pid)] = bytes.fromhex(rest)
    return meta, progs


def same_as_python(fw: F.Firmware, lay: fwload.Layout, params: list[int]) -> None:
    meta, progs = dump(lay, params)
    want = fwload.build_tq(fw, lay, params).images
    assert progs == want
    for m in fw.tq_meta:
        if m.id in lay.tq_addr:
            assert meta[m.id] == (f"heap {DRM_HEAP[m.heap]} temps {m.temps} attrs "
                                  f"{m.pds_attrs} size {m.size} align {m.align} data "
                                  f"{m.pds_data} flags {m.flags}")


def random_layout(rng: random.Random) -> fwload.Layout:
    """Every template aligned, in its heap, within a page; USE code in the DOUTU window."""
    heaps = {4: rng.randrange(0x800) << 20, 6: rng.randrange(0x800) << 20}
    tq = {}
    for m in facts.TQ_META:
        span = fwload.DOUTU_RANGE if m.heap == 4 else 0x1000000
        page = heaps[m.heap] + rng.randrange(span // 0x1000) * 0x1000
        tq[m.id] = page + rng.randrange((0x1000 - m.size) // m.align + 1) * m.align
    return fwload.Layout(addr={}, heap_base=heaps, tq_addr=tq)


def random_params(rng: random.Random) -> list[int]:
    return [rng.randrange(1 << 32) for _ in range(F.TQ_NUM_PARAMS)]


def test_generated_tables_are_current(tmp_path):
    out = tmp_path / "prog_asm_gen.h"
    subprocess.run([sys.executable, str(GEN), "-o", str(out)], check=True)
    assert out.read_text() == GENERATED.read_text(), "run make gen"


def test_recipes_match_python():
    from_src = ours_fw()
    lay = fwload.Layout(addr={}, heap_base={4: 0xF4000000, 6: 0xE4000000},
                        tq_addr={0: 0xF4000000, 1: 0xF4000010, 2: 0xF4000020, 3: 0xF4000060,
                                 4: 0xE4000000, 5: 0xE4000020, 6: 0xE4000040,
                                 7: 0xF4000080, 8: 0xE4000100})
    same_as_python(from_src, lay, fwload.fill_params(0xF0200000, 64, 32, 256, 0xFF336699))
    same_as_python(from_src, lay, fwload.copy_params(0xF0300000, 0xF0200000, 64, 32, 256,
                                                     256))


def test_random_layouts_and_params_match_python():
    rng = random.Random(545)
    from_src = ours_fw()
    for _ in range(100):
        same_as_python(from_src, random_layout(rng), random_params(rng))


def test_unplaced_doutu_target_fails_like_python():
    lay = random_layout(random.Random(1))
    del lay.tq_addr[facts.TQ_USE_FILL]
    params = random_params(random.Random(2))
    with pytest.raises(fwload.LayoutError):
        fwload.build_tq(ours_fw(), lay, params)
    assert dump(lay, params)[1][facts.TQ_PDS_PRIM] == 2      # ENOENT


BLOB, UM = fwcutter.default_input(), fwcutter.default_um_input()


@pytest.mark.skipif(BLOB is None or UM is None,
                    reason="libsrv_init.so / libsrv_um.so not available")
def test_random_layouts_match_the_cut_file():
    cut = fwcutter.cut(BLOB.read_bytes(), UM.read_bytes())
    rng = random.Random(862890)
    for _ in range(50):
        same_as_python(cut, random_layout(rng), random_params(rng))
