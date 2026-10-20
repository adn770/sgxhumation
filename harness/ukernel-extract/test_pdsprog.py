"""Tests for pdsprog.py — PDS program / PDS-related register field decoding."""
import pathlib
import struct

import pytest

import pdsprog

# Data segment of the steady-state EDM event program (PDS #13), as captured.
PDS13_DATA = [0x2, 0, 0, 0x100, 0, 0, 0x200, 0x080100AF, 0, 0, 0x080079AF, 0x0800CCEF,
              0, 0, 0x0800206F, 0x00FFFFFF, 0, 0, 0x100, 0x00400090, 0, 0, 0x200, 0x00300400]


def test_banks_split_rows_of_four():
    ds0, ds1 = pdsprog.banks(PDS13_DATA)
    assert ds0[0] == 0x2
    assert ds1[1] == 0x100 and ds1[2] == 0x200
    assert ds1[3:7] == [0x080100AF, 0x080079AF, 0x0800CCEF, 0x0800206F]
    assert ds1[7] == 0x00FFFFFF and ds1[9] == 0x00400090 and ds1[11] == 0x00300400


def test_banks_rejects_partial_row():
    with pytest.raises(ValueError):
        pdsprog.banks([1, 2, 3])


def test_code_entry_decodes_usse_address_words():
    assert pdsprog.code_entry(0x080100AF, base_insn=0x200) == 0xE0A
    assert pdsprog.code_entry(0x08003ACF, base_insn=0x200) == 0x1AC
    assert pdsprog.code_entry(0x0800206F) == 0x206               # template: no relocation
    assert pdsprog.code_entry(0x07800406) is None                 # a PDS instruction


def test_emitpds_data_size():
    assert pdsprog.emitpds_data_size(0x00800100) == 1
    assert pdsprog.emitpds_data_size(0x01001000) == 2
    assert pdsprog.emitpds_data_size(0x01800100) == 3


def test_event_pds_registers():
    assert pdsprog.event_pds(0xE800E000, 0x1, 0x6) == {
        "exec": 0xE800E000, "data_size": 1, "attr_size": 1, "sd": 1, "rotate": 0}


CAP = pathlib.Path(__file__).parent / "extracted" / "sgx545_pds13_runtime.bin"


@pytest.mark.skipif(not CAP.exists(), reason="PDS #13 capture not present")
def test_pds13_capture_entries():
    words = list(struct.unpack("<64I", CAP.read_bytes()[:256]))
    ds0, ds1 = pdsprog.banks(words[:24])
    assert [pdsprog.code_entry(w, base_insn=0x200) for w in ds1[3:7]] == [0xE0A, 0x59A, 0xACE, 0x6]
