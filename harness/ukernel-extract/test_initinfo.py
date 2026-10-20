"""Tests for initinfo.py — the SGX_BRIDGE_INIT_INFO decoder.

Synthetic buffers pin the layout with hard-coded dword indices (independent of the
decoder's own field table); the last test checks the real SGX545 capture if present.
"""
import pathlib
import struct

import pytest

import initinfo

NDW = 517  # 2068-byte SGX_DEVINITPART2 IN buffer, in dwords


def make(vals: dict[int, int]) -> bytes:
    w = [0] * NDW
    for i, v in vals.items():
        w[i] = v
    return struct.pack(f"<{NDW}I", *w)


def test_rejects_wrong_size():
    with pytest.raises(ValueError):
        initinfo.parse(b"\0" * 2064)


def test_control_block_handles():
    info = initinfo.parse(make({2: 0x4D410000, 3: 0x4D410001, 4: 0x4D410002,
                                5: 0x4D410004, 6: 0x4D410003, 7: 0x4D410005}))
    assert info.handles == {
        "kernel_ccb": 0x4D410000, "kernel_ccb_ctl": 0x4D410001,
        "ccb_event_kicker": 0x4D410002, "host_ctl": 0x4D410004,
        "ta3d_ctl": 0x4D410003, "misc": 0x4D410005,
    }


def test_host_kick_addresses_by_command():
    info = initinfo.parse(make({8: 0x11D, 9: 0x103, 11: 0xD4, 18: 0x77}))
    assert list(info.kick) == [
        "TA", "TRANSFER", "2D", "POWER", "CONTEXTSUSPEND", "CLEANUP", "GETMISCINFO",
        "PROCESS_QUEUES", "DATABREAKPOINT", "SETHWPERFSTATUS", "FLUSHPDCACHE",
    ]
    assert info.kick["TA"] == 0x11D
    assert info.kick["TRANSFER"] == 0x103
    assert info.kick["2D"] == 0
    assert info.kick["POWER"] == 0xD4
    assert info.kick["FLUSHPDCACHE"] == 0x77


def test_scripts_parse_writes_and_stop_at_halt():
    vals = {19: 1, 20: 0xA58, 21: 0, 22: 1, 23: 0xACC, 24: 5, 25: 2,
            28: 1, 29: 0xDEAD, 30: 1}            # write after HALT must be ignored
    vals.update({211: 1, 212: 0x818, 213: 0x7FF, 214: 2})   # part2 base = 19 + 64*3
    vals.update({403: 1, 404: 0x0, 405: 0, 406: 2})         # deinit base = 211 + 64*3
    info = initinfo.parse(make(vals))
    assert info.scripts["part1"] == [(0xA58, 0), (0xACC, 5)]
    assert info.scripts["part2"] == [(0x818, 0x7FF)]
    assert info.scripts["deinit"] == [(0x0, 0)]


def test_tail_fields():
    t = 451
    vals = {t: 0x60218, t + 12: 0x44, t + 13: 0x20,
            t + 14: 0x4D410006, t + 15: 0x4D410007, t + 16: 0x4D410008,
            t + 17: 0xB14, t + 18: 0xB20, t + 19: 0xA6AA, t + 20: 0xAAAAAA,
            t + 21: 0x8, t + 22: 0xFFFFFFDF, t + 23: 0x2,
            t + 24 + 12: 0xE4000000, t + 48: 0x4D410009, t + 65: 0x4D41001F}
    info = initinfo.parse(make(vals))
    assert info.build_options == 0x60218
    assert info.struct_sizes["HOST_CTL"] == 0x44
    assert info.struct_sizes["COMMAND"] == 0x20
    assert len(info.struct_sizes) == 13
    assert (info.hwperf_cb, info.ta_sig, info.td_sig) == (0x4D410006, 0x4D410007, 0x4D410008)
    assert info.edm_task_regs == (0xB14, 0xB20)
    assert info.clkgate == {"ctl": 0xA6AA, "ctl2": 0xAAAAAA,
                            "status_reg": 0x8, "status_mask": 0xFFFFFFDF}
    assert info.cache_control == 0x2
    assert len(info.dev_data) == 24 and info.dev_data[12] == 0xE4000000
    assert len(info.mem_handles) == 18
    assert info.mem_handles[0] == 0x4D410009 and info.mem_handles[17] == 0x4D41001F


def test_use_code_base_decode():
    assert initinfo.decode_use_code_base(0x07D00000) == ("EDM", 0xE8000000)
    assert initinfo.decode_use_code_base(0x03E80000) == ("PIXEL", 0xF4000000)
    assert initinfo.decode_use_code_base(0x01F80000) == ("VERTEX", 0xFC000000)


def test_register_names():
    assert initinfo.reg_name(0x0A0C) == "EUR_CR_USE_CODE_BASE_0"
    assert initinfo.reg_name(0x0B14) == "EUR_CR_USE0_SERV_EVENT"
    assert initinfo.reg_name(0x1234) == "0x1234"


def test_r2_script_labels_edm_handlers():
    s = initinfo.r2_script(initinfo.parse(make({8: 0x11D, 11: 0xD4})))
    assert "f edm.ta @ 0x8e8" in s          # flag: sticks even inside another function
    assert "afn edm.ta @ 0x8e8" in s        # ukernel insn 0x11d * 8 bytes
    assert "afn edm.power @ 0x6a0" in s     # 0xd4 * 8
    assert "edm.2d" not in s                # zero kick address = no handler


CAPTURE = pathlib.Path(__file__).parent / "extracted" / "sgx545_initinfo.bin"


@pytest.mark.skipif(not CAPTURE.exists(), reason="SGX545 init-info capture not present")
def test_against_sgx545_capture():
    info = initinfo.parse(CAPTURE.read_bytes())
    assert info.kick["TA"] == 0x11D and info.kick["2D"] == 0
    assert len(info.scripts["part2"]) == 38
    assert info.edm_task_regs == (0xB14, 0xB20)
    assert info.struct_sizes["HOST_CTL"] == 0x44
    part2 = dict(info.scripts["part2"])
    assert initinfo.decode_use_code_base(part2[0x0A48]) == ("EDM", 0xE8000000)
    # Harness allocations get deterministic device addresses (fakedrm.c bump allocator):
    assert part2[0x0A68] == 0xE800E000      # EVENT_OTHER_PDS_EXEC -> PDS program, alloc #10
    assert part2[0x0AC4] == 0xF0004000      # EVENT_KICKER -> event-kicker word, alloc #2
