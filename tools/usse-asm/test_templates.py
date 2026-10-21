# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The nine transfer templates, assembled from our own source in progs/.

Two checks. Always: each program disassembles to the instructions documented
in design/sgx545_transfer.md §4.2.4/§4.4.3 and design/sgx545_fw_format.md
§9.2. With the user's libsrv_um.so (found as tools/fwcutter finds it): the
assembled bytes equal the template that fwcutter cuts from the library, bit
for bit. No closed bytes are stored here; the comparison is made at test time.
"""
import sys

import pytest

from conftest import HERE

sys.path.insert(0, str(HERE.parent / "fwcutter"))
import facts      # noqa: E402
import fwcutter   # noqa: E402

PHAS = "phas #0, #0, pixel, end, parallel"
KICK = ["movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]", "halt"]

# template id -> (source, ISA, data dwords, documented disassembly of the code)
PROGRAMS = {
    facts.TQ_USE_FILL: ("use_fill.s", "usse", 0, [PHAS, "mov.skipinv.end o0, #0x00000000"]),
    facts.TQ_USE_SEC: ("use_sec.s", "usse", 0, [PHAS, "nop.end"]),
    facts.TQ_USE_EOT: ("use_eot.s", "usse", 0, [PHAS] + ["mov.skipinv r0, #0x00000000"] * 4 + [
        "emitpix1.freep #0 /* incp */, r0, r1",
        "emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000000"]),
    facts.TQ_USE_EOR: ("use_eor.s", "usse", 0, [
        PHAS, "mov.skipinv r0, #0x00000000",
        "emitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200"]),
    facts.TQ_PDS_PRIM: ("pds_prim.s", "pds", 4, KICK),
    facts.TQ_PDS_SEC: ("pds_sec.s", "pds", 4, KICK),
    facts.TQ_PDS_PIXEVENT: ("pds_pixevent.s", "pds", 20, [
        "and ds1[48], ir1, ds1[2]", "tstz p0, ds1[48]", "(p0) bra 0x00000005",
        "movs doutu, ds0[0], ds0[1], ds0[0], ds0[0]", "halt",
        "and ds1[48], ir1, ds1[3]", "tstz p0, ds1[48]", "(p0) bra 0x00000018",
        "not ds1[48], ir1", "and ds1[48], ds0[4], ds1[48]", "tstz p0, ds1[48]",
        "(p0) bra 0x0000001B", "movs douta, ir1.l, ds1[0], ir1.l, ir1.l",
        "shl ds1[48], ir0, 3", "and ds1[48], ds0[5], ds1[48]", "or ds0[48], ds0[6], ds1[48]",
        "shl ds1[48], ir0, 3", "and ds1[48], ds0[7], ds1[48]", "or ds0[48], ds0[48], ds1[48]",
        "shl ds1[48], ir1, 20", "and ds1[48], ds0[8], ds1[48]", "or ds0[48], ds0[48], ds1[48]",
        "movs doutu, ds1[1], ds0[48], ds0[48], ds0[48]", "halt",
        "and ds1[48], ir1, ds1[4]", "tstz p0, ds1[48]", "(p0) bra 0x0000001C",
        "movs doutu, ds0[2], ds0[3], ds0[2], ds0[2]", "halt"]),
    facts.TQ_USE_COPY: ("use_copy.s", "usse", 0, [PHAS, "mov.skipinv.end o0, pa0"]),
    facts.TQ_PDS_PRIM_1SRC: ("pds_prim_1src.s", "pds", 12, [
        "movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]", "movs douti, ds0[2], ds0[3], ds0[2], ds0[2]",
        "movs doutt, ds0[4], ds0[5], ds1[2], ds1[3]", "halt"]),
}

# The template constants of the pixel-event data segment (transfer §4.2.4).
PIXEVENT_DATA = [0, 0, 0, 0, 0, 0, 8, 2, 0x100, 0xFF8, 4, 0, 0, 0x3FF000, 0, 0, 0x400000, 0, 0, 0]

UM = fwcutter.default_um_input()


def assemble(name, isa, usse_asm, pds_asm):
    src = (HERE / "progs" / name).read_text()
    return usse_asm(src) if isa == "usse" else pds_asm(src)


def test_every_template_has_a_program():
    assert sorted(PROGRAMS) == list(range(len(facts.TQ_NAMES)))


@pytest.mark.parametrize("tid", sorted(PROGRAMS), ids=lambda t: facts.TQ_NAMES[t])
def test_program_matches_documented_disassembly(tid, usse_asm, pds_asm, usse_dis, pds_dis):
    name, isa, data, code = PROGRAMS[tid]
    words = assemble(name, isa, usse_asm, pds_asm)
    meta = {m.id: m for m in facts.TQ_META}[tid]
    if isa == "usse":
        assert usse_dis(words) == code
        assert len(words) * 8 == meta.size
    else:
        assert pds_dis(words, data)[data:] == code
        assert len(words) * 4 == meta.size and data * 4 == meta.pds_data
        assert all(w == 0 for w in words[:data]) or tid == facts.TQ_PDS_PIXEVENT
    if tid == facts.TQ_PDS_PIXEVENT:
        assert words[:data] == PIXEVENT_DATA


@pytest.mark.skipif(UM is None, reason="libsrv_um.so not available")
@pytest.mark.parametrize("tid", sorted(PROGRAMS), ids=lambda t: facts.TQ_NAMES[t])
def test_program_reproduces_the_cut_template(tid, usse_asm, pds_asm):
    blob = UM.read_bytes()
    known = fwcutter.identify(blob, "um")
    (cut,) = [t for t in known.templates if t.template == tid]
    name, isa, _, _ = PROGRAMS[tid]
    words = assemble(name, isa, usse_asm, pds_asm)
    width = 8 if isa == "usse" else 4
    ours = b"".join(w.to_bytes(width, "little") for w in words)
    assert ours == fwcutter.cut_template(known, cut, blob)
