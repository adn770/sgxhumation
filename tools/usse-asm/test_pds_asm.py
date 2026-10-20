"""pds-asm: assemble PDS text as pds-dis prints it, with a data segment and labels.

Expected words are those of tools/usse-dis/test_pds.py (refpds545 texts) and
of the transfer programs (design/sgx545_transfer.md §4.2.4, §4.4.3). Several
encodings print the same text (unused operand fields); where the word is not
the canonical one, the test compares decodes instead.
"""
import pytest

CANONICAL = {
    "movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]": 0x07000305,
    "movs douti, ds0[2], ds0[3], ds0[2], ds0[2]": 0x07040202,
    "movs doutt, ds0[4], ds0[5], ds1[2], ds1[3]": 0x07082364,
    "movs douta, ir1.l, ds1[0], ir1.l, ir1.l": 0x07840406,
    "movs doutu, ds1[1], ds0[48], ds0[48], ds0[48]": 0x07601805,
    "halt": 0xAF000000,
    "and ds1[48], ir1, ds1[2]": 0xCF820870,
    "tstz p0, ds1[48]": 0x8700C100,
    "(p0) bra 0x00000005": 0x90000005,
    "not ds1[48], ir1": 0xDF820070,
    "and ds1[48], ds0[4], ds1[48]": 0xCF08C070,
    "shl ds1[48], ir0, 3": 0xF7800370,
    "or ds0[48], ds0[6], ds1[48]": 0xC70CC030,
    "tstz p0, ir0": 0x87800000,
    "(p0) tstz p0, ir0": 0x80800000,
    "bra 0x00000026": 0x97000026,
    "xor ds1[48], ds0[48], ds1[48]": 0xD760C070,
    "(p0) xor ds1[48], ds0[48], ds1[48]": 0xD060C070,
}

OTHER = [
    "mov16 ds0[1], ds0[0].h", "mov32 ds0[0], tim", "mov64 ds0[0], mr23", "mov128 ds[0], ds[0], swap",
    "load 0x00000000, ds0[48-54]", "load 0x0007220C, ds1[56]", "store 0x00040EB0, ds0[55-59]",
    "(p0) mul ds1[26], ds0[4].h, ds1[16].h", "(p0) abs ds1[50], ds1[30]", "(if0) not ds0[13], tim",
    "rwfence", "rfence", "nop", "alum signed", "call 0x00001234", "(p0) shl ds1[26], ds0[1], 62",
    "shr ds1[63], ds0[0], 0", "shl ds0[0], ds0[0], 4294967232", "tstn , ds0[0]", "(alun) rtn",
    "movsa doutd, tim.l, ds1[2], tim.h, ds1[3]", "(aluz) sbc ds0[1], ds0[63], ds1[0]",
]


def test_canonical_words(pds_asm):
    assert pds_asm("\n".join(CANONICAL)) == list(CANONICAL.values())


def test_other_groups_round_trip(pds_asm, pds_dis):
    assert pds_dis(pds_asm("\n".join(OTHER))) == OTHER


def test_data_segment_and_labels(pds_asm, pds_dis):
    src = """
        .word 0x00000008        ; data dword 0
        .word 2
        tstz p0, ds1[48]
        (p0) bra skip           ; index counted from the first instruction
        halt
    skip:
        nop
        bra skip
    """
    words = pds_asm(src)
    assert words[:2] == [8, 2]
    assert pds_dis(words, 2)[2:] == ["tstz p0, ds1[48]", "(p0) bra 0x00000003", "halt", "nop",
                                     "bra 0x00000003"]


def test_listing_input(pds_asm):
    src = "0: 0x00000008\tdata\n1: 0x07000305\tmovs doutu, ds0[0], ds0[1], ds1[0], ds0[0]\n"
    assert pds_asm(src) == [8, 0x07000305]


def test_reserved_text_is_refused(pds_asm):
    for bad in ["movs ir1, ds0[0], ds0[0], ds0[0], ds0[0]",       # reserved port
                "movs doutu, ds0[0], ds0[2], ds0[0], ds0[0]",     # two ds0 pairs
                "add ds0[0], ir2, ds1[0]", "bra nowhere"]:
        assert pds_asm(bad, check=False).returncode == 1, bad


@pytest.mark.parametrize("text", ["shl ds0[0], ds0[0], 64", "load 0x00000002, ds0[48]"])
def test_unencodable_values_are_refused(pds_asm, text):
    assert pds_asm(text, check=False).returncode == 1
