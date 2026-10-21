# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for summary3d.py — allocation roles, cross references and program extraction for a
captured kick. Synthetic inputs only, plus one check against the gitignored capture."""
import pathlib
import shutil
import struct

import pytest

import summary3d

HERE = pathlib.Path(__file__).resolve().parent
CAPTURE = HERE / "extracted" / "gles2_tri" / "dump3d"
PDS_DIS = HERE.parent.parent / "tools" / "usse-dis" / "pds-dis"

BASES = summary3d.DEFAULT_CODE_BASES


def test_default_code_bases_follow_the_init_script():
    # USE_CODE_BASE_0..9 pixel, 10..13 vertex, 15 EDM (harness heap layout, spec §13.5).
    assert BASES[0] == 0xF4000000 and BASES[9] == 0xF4480000
    assert BASES[10] == 0xFC000000 and BASES[13] == 0xFC180000
    assert BASES[15] == 0xE8000000 and 14 not in BASES


def test_usse_code_address_words():
    # low nibble = code base index, bits 23:4 = instruction index (8 bytes each)
    assert summary3d.usse_address(0x00010E0A, BASES) == 0xFC008700
    assert summary3d.usse_address(0x00000020, BASES) == 0xF4000010
    assert summary3d.usse_address(0x0000100E, BASES) is None           # no base 14


def test_pds_data_rows_are_deinterleaved():
    words = list(range(12))
    ds0, ds1 = summary3d.pds_banks(words)
    assert ds0 == [0, 1, 4, 5, 8, 9] and ds1 == [2, 3, 6, 7, 10, 11]


def test_pds_code_is_cut_after_the_last_halt():
    halt, other = 0xAF000000, 0x87000000
    words = [1, 2, 3, 4] + [other, halt, other, halt, 0, 0, 0, 0, 0x55]
    assert summary3d.pds_code(words, 4) == [other, halt, other, halt]
    assert summary3d.pds_code([1, 2, 3, 4, other, other], 4) == [other, other]
    # back-to-back programs: a halt followed by two zero words ends the program
    assert summary3d.pds_code([9, 0, 0, 0, other, halt, 0, 0, 7, 4, 0, 0, other, halt], 4) == [
        other, halt]


def test_usse_code_is_cut_at_two_zero_instructions():
    raw = struct.pack("<6Q", 1, 2, 0, 3, 0, 0) + b"\xff" * 8
    assert summary3d.usse_code(raw) == struct.pack("<4Q", 1, 2, 0, 3)


def test_xrefs_find_pointers_into_objects():
    objs = [
        {"index": 0, "dva": 0x08001000, "mapped": 0x1000, "heap_name": "GENERAL", "kind": 0},
        {"index": 1, "dva": 0xEC001000, "mapped": 0x1000, "heap_name": "PDSVERTEX", "kind": 0},
        {"index": 2, "dva": 0xE0001000, "mapped": 0x1000, "heap_name": "SYNCINFO", "kind": 1},
    ]
    data = {0: struct.pack("<4I", 0xEC001040, 7, 0x08001000, 0xE0001004),
            1: struct.pack("<2I", 0x0EC00104, 0x08001FFC), 2: b"\0" * 8}
    refs = summary3d.xrefs(objs, lambda o: data[o["index"]])
    assert refs[1] == [(0, 0x0)]                 # word 0 of obj 0 points into obj 1
    assert refs[0] == [(1, 0x4)]                 # obj 1 word 1; self-references are dropped
    assert 2 not in refs                         # sync pages are not scanned or targeted


@pytest.mark.skipif(not PDS_DIS.exists(), reason="pds-dis not built")
def test_pds_disassembly_marks_data_words():
    lines = summary3d.disassemble_pds([0x12345678, 0xAF000000], 1, str(PDS_DIS))
    assert "data" in lines[0] and "halt" in lines[1]


@pytest.mark.skipif(not CAPTURE.exists(), reason="gles2_tri capture not present")
def test_capture_summary_covers_every_object():
    text = summary3d.summary(CAPTURE / "kick00_in.bin", CAPTURE / "kick00", None)
    assert "VDM control stream" in text and "hw_render_details" in text
    for line in (CAPTURE / "kick00" / "objects.txt").read_text().splitlines():
        assert f"obj {int(line.split()[0]):3d}" in text


def test_usse_code_words_only_count_inside_pds_data():
    objs = [
        {"index": 0, "dva": 0x08001000, "mapped": 0x1000, "heap_name": "GENERAL", "kind": 0},
        {"index": 1, "dva": 0xEC001000, "mapped": 0x1000, "heap_name": "PDSVERTEX_CODEDATA",
         "kind": 0},
        {"index": 2, "dva": 0xFC008000, "mapped": 0x1000, "heap_name": "VERTEXSHADER", "kind": 0},
    ]
    word = 0x00010E0A                            # code base 10, instruction 0x10e0
    data = {0: struct.pack("<I", word), 1: struct.pack("<2I", 0, word), 2: b"\0" * 4}
    assert summary3d.xrefs(objs, lambda o: data[o["index"]]) == {2: [(1, 0x4)]}


def test_pixel_pds_words_in_mte_state():
    # bits 31:26 data size in 16-byte rows, low bits (address - PDS_EXEC_BASE) >> 4 [infer]
    assert summary3d.pixel_pds_word(0x04001800) == (0xE4018000, 4)
    assert summary3d.pixel_pds_word(0x0800041A) == (0xE40041A0, 8)
    assert summary3d.pixel_pds_word(0x00001800) is None          # no data rows
    assert summary3d.pixel_pds_word(0x04401800) is None          # stray middle bits
    assert summary3d.pixel_pds_word(0x3F800000) is None


def test_disassembled_pixel_pds_programs_come_from_pdsvertex_state(tmp_path):
    objs = [
        {"index": 0, "dva": 0xEC003000, "mapped": 0x1000, "heap_name": "PDSVERTEX_CODEDATA",
         "kind": 0},
        {"index": 1, "dva": 0xE4018000, "mapped": 0x1000, "heap_name": "PDSPIXEL_CODEDATA",
         "kind": 0},
        {"index": 2, "dva": 0x08001000, "mapped": 0x1000, "heap_name": "GENERAL", "kind": 0},
    ]
    # A program (4 data dwords, two of our own code words ending in halt) at +0, none at
    # +0x400; the GENERAL object is not state and is not scanned.
    prog = struct.pack("<6I", 0x80000, 0, 0, 0, 0x07000345, 0xAF000000)
    data = {0: struct.pack("<2I", 0x04001800, 0x04001840), 1: prog,
            2: struct.pack("<I", 0x04001800)}
    found = summary3d.pixel_pds_refs(objs, lambda o: data[o["index"]])
    assert found == {0xE4018000: (4, "obj 0+0x0")}
