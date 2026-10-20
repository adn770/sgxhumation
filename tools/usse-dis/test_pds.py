"""Tests for the clean-room PDS decoder (pds.c via the pds-dis CLI).

Expected texts are refpds545 output for the same words, so these pin oracle
compatibility without needing the oracle; the last test runs the full
pds_oracle_check.py when refpds545 has been built.
"""
import pathlib
import struct
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent


@pytest.fixture(scope="module")
def dis(tmp_path_factory):
    subprocess.run(["make", "-s", "-C", str(HERE), "pds-dis"], check=True)
    blob = tmp_path_factory.mktemp("pds") / "words.bin"

    def run(*words):
        blob.write_bytes(struct.pack(f"<{len(words)}I", *words))
        out = subprocess.run([str(HERE / "pds-dis"), str(blob), "0"],
                             capture_output=True, text=True, check=True).stdout
        return [line.split("\t", 1)[1] for line in out.splitlines()]
    return run


# Every instruction form used by the captured EDM PDS programs #10-#14.
CAPTURED = {
    0x07000305: "movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]",
    0x07003885: "movs doutu, ds1[3], ds0[0], ds0[1], ds0[0]",
    0x07000203: "movs doutd, ds0[0], ds0[1], ds0[0], ds0[0]",
    0x07800406: "movs douta, ir0.l, ds1[0], ir0.l, ir0.l",
    0x07882406: "movs douta, tim.l, ds1[2], tim.l, tim.l",
    0xAF000000: "halt",
    0x87800000: "tstz p0, ir0",
    0x8700C100: "tstz p0, ds1[48]",
    0x80800000: "(p0) tstz p0, ir0",
    0x90000003: "(p0) bra 0x00000003",
    0x97000026: "bra 0x00000026",
    0xCF821C30: "and ds0[48], ir1, ds1[7]",
    0xCF602070: "and ds1[48], ds0[48], ds1[8]",
    0xD760C070: "xor ds1[48], ds0[48], ds1[48]",
    0xD060C070: "(p0) xor ds1[48], ds0[48], ds1[48]",
}


def test_captured_program_forms(dis):
    assert dis(*CAPTURED) == list(CAPTURED.values())


OTHER = {
    0x0F000101: "mov16 ds0[1], ds0[0].h",
    0x17000000 | (0x83 << 9): "mov32 ds0[0], tim",
    0x1F000000 | (0x41 << 10): "mov64 ds0[0], mr23",
    0x27000100: "mov128 ds[0], ds[0], swap",
    0x37000000: "load 0x00000000, ds0[48-54]",
    0x31E1C883: "load 0x0007220C, ds1[56]",
    0x3D9D03AC: "store 0x00040EB0, ds0[55-59]",
    0x6009435A: "(p0) mul ds1[26], ds0[4].h, ds1[16].h",
    0x689CF972: "(p0) abs ds1[50], ds1[30]",
    0xDB859E8D: "(if0) not ds0[13], tim",
    0x7F000001: "rwfence",
    0xB05F8654: "nop",
    0xBF000001: "alum signed",
    0x9F000000 | 0x1234: "call 0x00001234",
    0xF0013E5A: "(p0) shl ds1[26], ds0[1], 62",
    0xFF0000FF: "shr ds1[63], ds0[0], 0",
    0x8F000004: "tstn , ds0[0]",
}


def test_other_groups(dis):
    assert dis(*OTHER) == list(OTHER.values())


def test_shift_amount_is_signed(dis):
    assert dis(0xF7004000) == ["shl ds0[0], ds0[0], 4294967232"]      # -64


@pytest.mark.parametrize("word", [
    0x705E4D75,             # reserved arithmetic type
    0x07000000 | 11,        # movs to a reserved destination
    0x07900000,             # movs from a reserved input-register base
    0x47000000 | (0x43 << 17),   # add from a reserved source register
])
def test_reserved_encodings_are_not_guessed(dis, word):
    assert dis(word) == [f".word 0x{word:08X}"]


@pytest.mark.skipif(not (HERE / "refdisasm" / "refpds545").exists(),
                    reason="refpds545 oracle not built")
def test_oracle_check_has_no_wrong_or_missed():
    r = subprocess.run(["python3", str(HERE / "pds_oracle_check.py")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout


def test_analysis_api(tmp_path):
    exe = tmp_path / "test_pds_api"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(HERE), "-o", str(exe),
                    str(HERE / "test_pds_api.c"), str(HERE / "pds.c")], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout
