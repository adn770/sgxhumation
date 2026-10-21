# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The first triangle's program header (kernel/gma500/sgxfive/sgx_tri_progs.h).

It must be what gen_tri_progs.py writes from tools/usse-asm/progs/ today, and the programs
must keep the instructions sgx_tri.c patches where it patches them.
"""
from __future__ import annotations

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_tri_progs as G     # noqa: E402


def test_generated_header_is_fresh():
    assert G.OUTPUT.read_text() == G.generate(), \
        "stale: run kernel/gen_tri_progs.py -o kernel/gma500/sgxfive/sgx_tri_progs.h"


def text(name: str, isa: str) -> list[str]:
    return [t for _w, t in G.listing(name, isa)]


def test_patched_slots():
    """The instruction slots sgx_tri.c patches (by index)."""
    vtx = text("use_vtx.s", "usse")
    assert vtx[0] == "phas #7, #0, pixel, vcull, parallel"      # + the placement index
    assert vtx[7].startswith("phas #0, #0, pixel, end")
    assert vtx[8].startswith("emitmtevtx.end")
    assert text("use_fill.s", "usse")[1] == "mov.skipinv.end o0, #0x00000000"
    eot = text("use_eot.s", "usse")
    assert eot[1:5] == ["mov.skipinv r0, #0x00000000"] * 4      # r0..r3, PBE words 0-3
    assert eot[6].startswith("emitpix2.end.freep")                # + the sideband
    # The PDS programs' data segments: the dwords sgx_tri.c writes exist.
    for name, data in (("pds_vtx.s", 8), ("pds_mte.s", 8), ("pds_prim.s", 4),
                       ("pds_sec.s", 4), ("pds_pixevent.s", 20)):
        assert text(name, "pds")[:data] == ["data"] * data
        assert text(name, "pds")[data] != "data"


def test_every_program_is_in_the_header():
    hdr = G.OUTPUT.read_text()
    for name, _isa in G.PROGS:
        assert re.search(rf"static const u(32|64) sgx_tri_{name[:-2]}\[\]", hdr), name
