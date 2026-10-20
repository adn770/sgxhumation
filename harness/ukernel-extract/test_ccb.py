"""Tests for ccb.py — SGX545 kernel CCB command encoding and the submit sequence.

The layouts are the host side (sgx_mkif_km.h, sgxutils.c) as confirmed against the
EDM's CCB fetch and handlers (spec §13.10): the EDM loads dword 0 into r4 and
dwords 1-7 into r5-r11, so a handler sees ui32Data[0..5] in r6-r11.
"""
import pathlib
import struct

import pytest

import ccb
import initinfo

KICK = {"TA": 0x11D, "TRANSFER": 0x103, "2D": 0, "POWER": 0xD4, "CONTEXTSUSPEND": 0xEB,
        "CLEANUP": 0x13A, "GETMISCINFO": 0x173, "PROCESS_QUEUES": 0x156,
        "DATABREAKPOINT": 0, "SETHWPERFSTATUS": 0x157, "FLUSHPDCACHE": 0}


def test_command_is_32_bytes_service_cache_data():
    raw = ccb.command(KICK, "POWER", [0, ccb.POWERCMD_IDLE], cache_control=ccb.CC_INVAL_DATA)
    assert len(raw) == 32
    assert struct.unpack("<8I", raw) == (0xD4, 0x8, 0, 2, 0, 0, 0, 0)


def test_command_rejects_missing_handler_and_long_payload():
    with pytest.raises(ValueError):
        ccb.command(KICK, "2D", [])                 # no 2D core: kick address 0
    with pytest.raises(ValueError):
        ccb.command(KICK, "POWER", [0] * 7)


def test_payload_builders_match_the_kernel_driver():
    assert ccb.power(ccb.POWERCMD_RESUME) == [0, 3]
    assert ccb.cleanup(ccb.CLEANUPCMD_RC, 0xD0012000) == [2, 0xD0012000]
    assert ccb.context_suspend(0xD0034000, resume=True) == [0xD0034000, 2]
    assert ccb.context_suspend(0xD0034000, resume=False) == [0xD0034000, 1]
    assert ccb.get_misc_info(0xF0009000) == [0, 0xF0009000]
    assert ccb.set_hwperf_status(ccb.HWPERF_RESET_COUNTERS | ccb.HWPERF_GRAPHICS_ON) == [3]
    assert ccb.process_queues() == []


def test_submit_sequence():
    steps = ccb.submit(KICK, "PROCESS_QUEUES", [], write_offset=255, kicker=7,
                       cache_control=ccb.CC_INVAL_BIF_PD)
    assert steps == [
        ("ccb", 255 * 32, struct.pack("<8I", 0x156, 0x2, 0, 0, 0, 0, 0, 0)),
        ("ccb_ctl.write_offset", 0, 0),           # wraps mod 256
        ("event_kicker", 0, 8),
        ("reg", ccb.EUR_CR_EVENT_KICK, ccb.EUR_CR_EVENT_KICK_NOW_MASK),
    ]


def test_decode_names_handler_and_fields():
    raw = ccb.command(KICK, "CLEANUP", ccb.cleanup(ccb.CLEANUPCMD_TC, 0xD0001000))
    d = ccb.decode(raw, KICK)
    assert d["cmd"] == "CLEANUP" and d["cache_control"] == 0
    assert d["data"] == [3, 0xD0001000, 0, 0, 0, 0]


CAPTURE = pathlib.Path(__file__).parent / "extracted" / "sgx545_initinfo.bin"


@pytest.mark.skipif(not CAPTURE.exists(), reason="SGX545 init-info capture not present")
def test_kick_table_matches_capture():
    assert initinfo.parse(CAPTURE.read_bytes()).kick == KICK
