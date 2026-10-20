#!/usr/bin/env python3
"""SGX545 kernel CCB commands: encoding, per-command payloads and the submit sequence.

A command is SGXMKIF_COMMAND (sgx_mkif_km.h), 32 bytes:
    +0  ui32ServiceAddress   EDM handler, from aui32HostKickAddr[cmd] (initinfo.py)
    +4  ui32CacheControl     SGXMKIF_CC_* invalidations the EDM does before the handler
    +8  ui32Data[6]          payload; the handler sees it in r6..r11

The EDM's CCB fetch (spec §13.10) reads CCBCtl.ReadOffset, loads the command at
CCB + off*32 into r4..r11, applies the cache-control bits, bumps ReadOffset mod 256
and jumps to r4. The host side (sgxutils.c SGXScheduleCCBCommand) is mirrored by
submit(): write the command at WriteOffset, advance WriteOffset and the event-kicker
word mod 256, then write EUR_CR_EVENT_KICK. SGX545 rev 1014 has no BRN26620, so the
plain KICK register (not KICK2) is used.

    usage: ccb.py <initinfo.bin> <ccb.bin>     # decode the commands in a CCB dump
"""
from __future__ import annotations

import struct
import sys

COMMAND_SIZE, CCB_ENTRIES = 32, 256

CC_INVAL_BIF_PT, CC_INVAL_BIF_PD, CC_INVAL_BIF_SL, CC_INVAL_DATA = 0x1, 0x2, 0x4, 0x8

POWERCMD_POWEROFF, POWERCMD_IDLE, POWERCMD_RESUME = 1, 2, 3
CTXSUSPCMD_SUSPEND, CTXSUSPCMD_RESUME = 1, 2
CLEANUPCMD_RT, CLEANUPCMD_RC, CLEANUPCMD_TC, CLEANUPCMD_2DC, CLEANUPCMD_PB = 1, 2, 3, 4, 5
HWPERF_RESET_COUNTERS, HWPERF_GRAPHICS_ON = 1 << 0, 1 << 1
HWPERF_PERIODIC_ON, HWPERF_MK_EXECUTION_ON = 1 << 2, 1 << 3

EUR_CR_EVENT_KICK, EUR_CR_EVENT_KICK_NOW_MASK = 0x0AC8, 0x1


def command(kick: dict[str, int], cmd: str, data: list[int], cache_control: int = 0) -> bytes:
    service = kick.get(cmd, 0)
    if not service:
        raise ValueError(f"no EDM handler for {cmd} in this build")
    if len(data) > 6:
        raise ValueError("SGXMKIF_COMMAND carries at most 6 data words")
    return struct.pack("<8I", service, cache_control, *data, *[0] * (6 - len(data)))


def decode(raw: bytes, kick: dict[str, int]) -> dict:
    service, cc, *data = struct.unpack("<8I", raw)
    names = [n for n, pc in kick.items() if pc and pc == service]
    return {"cmd": names[0] if names else f"{service:#x}", "cache_control": cc, "data": data}


# Payloads, as the GPL kernel driver fills ui32Data[] (sgxpower.c, sgxutils.c, sgxinit.c).
def power(powercmd: int) -> list[int]:
    return [0, powercmd]


def cleanup(cleanup_type: int, hw_data_addr: int) -> list[int]:
    return [cleanup_type, hw_data_addr]


def context_suspend(hw_context_addr: int, resume: bool) -> list[int]:
    return [hw_context_addr, CTXSUSPCMD_RESUME if resume else CTXSUSPCMD_SUSPEND]


def get_misc_info(misc_info_addr: int) -> list[int]:
    return [0, misc_info_addr]


def set_hwperf_status(status: int) -> list[int]:
    return [status]


def process_queues() -> list[int]:
    return []


def submit(kick: dict[str, int], cmd: str, data: list[int], write_offset: int, kicker: int,
           cache_control: int = 0) -> list[tuple[str, int, object]]:
    """The host's writes for one command, in order: (target, offset, value)."""
    return [
        ("ccb", write_offset * COMMAND_SIZE, command(kick, cmd, data, cache_control)),
        ("ccb_ctl.write_offset", 0, (write_offset + 1) % CCB_ENTRIES),
        ("event_kicker", 0, (kicker + 1) % CCB_ENTRIES),
        ("reg", EUR_CR_EVENT_KICK, EUR_CR_EVENT_KICK_NOW_MASK),
    ]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    import initinfo
    kick = initinfo.parse(open(argv[0], "rb").read()).kick
    raw = open(argv[1], "rb").read()
    for i in range(min(len(raw) // COMMAND_SIZE, CCB_ENTRIES)):
        entry = raw[i * COMMAND_SIZE:(i + 1) * COMMAND_SIZE]
        if any(entry):
            d = decode(entry, kick)
            words = " ".join(f"{w:#x}" for w in d["data"])
            print(f"[{i:3d}] {d['cmd']:16s} cc={d['cache_control']:#x}  {words}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
