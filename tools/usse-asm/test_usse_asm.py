# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""usse-asm: assemble USSE text as usse-dis prints it.

The words below are the instruction forms of the transfer programs, published
one by one in design/sgx545_transfer.md §4.2.4 and §4.4.3, plus field variants
pinned in tools/usse-dis/test_forms.py against refdisasm545.
"""
import subprocess

from conftest import HERE

TRANSFER = {
    "phas #0, #0, pixel, end, parallel": 0xFA44070000000000,
    "mov.skipinv.end o0, #0x00000000": 0xFCA4000100000000,
    "mov.skipinv.end o0, #0xFF336699": 0xFCA7F19100136699,
    "mov.skipinv r0, #0x00000000": 0xFCA0000000000000,
    "mov.skipinv r3, #0x00000000": 0xFCA0000000600000,
    "emitpix1.freep #0 /* incp */, r0, r1": 0xFB20010000200080,
    "emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000000": 0xFB24000000208180,
    "emitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200": 0xFB26004080200000,
    "nop.end": 0xF804014000000000,
    "mov.skipinv.end o0, pa0": 0x28851001A0000000,
}


def test_transfer_instructions(usse_asm):
    assert usse_asm("\n".join(TRANSFER)) == list(TRANSFER.values())


# The vertex-side forms of the first triangle's programs (progs/use_vtx.s,
# use_mtest*.s). The emits that do not print one of their operands take the encoding
# the closed compilers use for it, immediate 0 (design/sgx545_3d.md 9.2).
VERTEX = {
    "phas #7, #0, pixel, vcull, parallel": 0xFA44020000000007,
    "nop.nosched": 0xF800094000000000,
    "wdf drc0": 0xF920000000000000,
    "mov.skipinv.repeat4 o0, pa0": 0x28A13001A0000000,
    "mov.skipinv.repeat16 o0, pa0": 0x28A1F001A0000000,
    "mov.skipinv.repeat2 o16, pa16": 0x28A11001A2000800,
    "emitvcbst #0 /* incp */, #0": 0xFB23C000A0000000,
    "wop #0": 0xFF20000000000000,
    "emitmtest.end.freep #0 /* incp */, #0, #0": 0xFB2F4000A0200000,
    "emitvcbvtx.end.twopart #0 /* incp */, #0": 0xFB27D400A0000000,
    "emitmtevtx.end.twopart.freep #0 /* incp */, #0, #0": 0xFB275400A0200000,
}


def test_vertex_instructions(usse_asm):
    assert usse_asm("\n".join(VERTEX)) == list(VERTEX.values())


def test_more_forms_round_trip(usse_asm, usse_dis):
    texts = ["phas #16, #4, pixel, end, perinstance", "phas r1, r1",
             "p3 mov.end o0, #0x00000000", "mov.skipinv.end i.lh, #0x00000000",
             "mov.skipinv.end o[il + #0], #0x00000000", "mov.end.repeat2 r0, r0",
             "mov.skipinv.end o0.xy, pa0", "nop.syncs.nosched.end.tog.trigger",
             "!p0 and r2, r2, ~#0x00040000", "ldad.bpcache.fetch4 r3, [r2,+#1], drc0",
             "stad.bpcache.fetch2 [r1,#82++], r5", "br -#0x00000010", "ba.savelink #0x00000123",
             "ldr r9, #372, drc0", "str #372, i2", "ima32.u r0, r0, r0, r0"]
    assert usse_dis(usse_asm("\n".join(texts))) == texts


def test_lenient_spelling(usse_asm):
    src = """
        ; a comment line
        mov.skipinv.end   o0 ,#0xff336699     ; lower-case hex, odd blanks
        mov.skipinv r3, #0                    ; decimal for a hex field
        emitpix1.freep #0, r0, r1             /* the incp comment may go */
    """
    assert usse_asm(src) == [0xFCA7F19100136699, 0xFCA0000000600000, 0xFB20010000200080]


def test_listing_input(usse_asm):
    """usse-dis output assembles back: the index and hex columns are ignored."""
    src = "     0: 0xFA44070000000000\tphas #0, #0, pixel, end, parallel\n" \
          "     1: 0x0000000000000000\tnop.end\n"
    assert usse_asm(src) == [0xFA44070000000000, 0xF804014000000000]


def test_raw_words(usse_asm):
    assert usse_asm(".long 0x0123456789ABCDEF") == [0x0123456789ABCDEF]


def test_listing_output(usse_asm, tmp_path):
    (tmp_path / "l.s").write_text("nop.end\n")
    r = subprocess.run([str(HERE / "usse-asm"), "-l", "-o", str(tmp_path / "l.bin"),
                        str(tmp_path / "l.s")], capture_output=True, text=True, check=True)
    assert r.stdout == "     0: 0xF804014000000000\tnop.end\n"


def test_errors_name_the_line(usse_asm):
    r = usse_asm("nop.end\nfrobnicate r0\n", check=False)
    assert r.returncode == 1
    assert ":2: cannot assemble 'frobnicate r0'" in r.stderr


def test_out_of_range_is_refused(usse_asm):
    assert usse_asm("mov r128, #0", check=False).returncode == 1
    assert usse_asm("phas #262144, #0, pixel, end, parallel", check=False).returncode == 1


def test_canonical_choice_is_stable(usse_asm, usse_dis):
    """mov r0, r0 has a write-mask and a repeat encoding; the first form wins."""
    (w,) = usse_asm("mov r0, r0")
    assert usse_dis([w]) == ["mov r0, r0"]
    assert not w >> 53 & 1 and w >> 44 & 0xF == 1      # mask form, x only
    assert usse_asm("mov r0, r0") == [w]


def test_labels(usse_asm):
    """ba takes a label as an absolute instruction index; br only a number."""
    src = "start:\n nop\n ba #start\n ba.savelink #end\nend:\n nop.end\n"
    assert usse_asm(src)[1:3] == [0xF800000000000000, 0xF800020000000003]
    assert usse_asm("x:\n br #x\n", check=False).returncode == 1
