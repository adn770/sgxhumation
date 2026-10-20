#!/usr/bin/env python3
"""Decode the SGX_BRIDGE_INIT_INFO that srvinit passes to SGX_DEVINITPART2.

The harness (fakedrm.c) dumps the 2068-byte IN buffer of that bridge call to
extracted/sgx545_initinfo.bin. It is the complete SGX545 boot payload: control-block
handles, the per-command EDM handler entry points (host kick addresses), the register
init scripts, EDM task / clock-gating registers, MKIF struct sizes and init memory.

The field table below is SGX_BRIDGE_INIT_INFO (DDK 1.7, services4/include/sgxinfo.h)
for *this* build's configuration — no SGX_FEATURE_2D_HARDWARE, no PTLA write-back
handle, SUPPORT_SGX_HWPERF on, no SGX_FEATURE_MP, no BRN-workaround handles — pinned
empirically: every meminfo handle field lands on a harness token, and the layout
consumes the buffer exactly.

    usage: initinfo.py <initinfo.bin> [--r2 <out.r2>]
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass

SIZE = 2068          # IN buffer: ui32BridgeFlags + hDevCookie + SGX_BRIDGE_INIT_INFO

HANDLE_NAMES = ("kernel_ccb", "kernel_ccb_ctl", "ccb_event_kicker",
                "host_ctl", "ta3d_ctl", "misc")
# aui32HostKickAddr[] is indexed by SGXMKIF_CMD_TYPE (sgxinfo.h).
KICK_NAMES = ("TA", "TRANSFER", "2D", "POWER", "CONTEXTSUSPEND", "CLEANUP", "GETMISCINFO",
              "PROCESS_QUEUES", "DATABREAKPOINT", "SETHWPERFSTATUS", "FLUSHPDCACHE")
# SGX_MISCINFO_STRUCT_SIZES without the two SGX_FEATURE_2D_HARDWARE fields.
STRUCT_SIZE_NAMES = ("CMDTA", "CMDTA_SHARED", "TRANSFERCMD", "TRANSFERCMD_SHARED",
                     "3DREGISTERS", "HWPBDESC", "HWRENDERCONTEXT", "HWRENDERDETAILS",
                     "HWRTDATA", "HWRTDATASET", "HWTRANSFERCONTEXT", "HOST_CTL", "COMMAND")
SCRIPT_PARTS = (("part1", 64), ("part2", 64), ("deinit", 16))   # SGX_MAX_[DE]INIT_COMMANDS
OP_ILLEGAL, OP_WRITE, OP_HALT = 0, 1, 2                          # SGX_INIT_OPERATION
DM_NAMES = ("VERTEX", "PIXEL", "RESERVED", "EDM")                # USE_CODE_BASE data master

# Names (sgx545defs.h / sgxdefs.h) for the registers the SGX545 init scripts touch.
REG_NAMES: dict[int, str] = {
    0x0000: "EUR_CR_CLKGATECTL", 0x0004: "EUR_CR_CLKGATECTL2",
    0x0110: "EUR_CR_EVENT_HOST_ENABLE2", 0x0130: "EUR_CR_EVENT_HOST_ENABLE",
    0x013C: "cr_pds_cache_host_enable", 0x04E8: "cr_isp_depth_sort",
    0x0800: "cr_tsp_config", 0x0804: "cr_cache_ctrl",
    0x0810: "cr_dcu_ictrl", 0x0814: "cr_dcu_pctrl", 0x0818: "cr_dcu_bypass",
    0x0A00: "cr_use_ctrl", 0x0A58: "cr_event_pds_enable",
    0x0A68: "EUR_CR_EVENT_OTHER_PDS_EXEC", 0x0A6C: "EUR_CR_EVENT_OTHER_PDS_DATA",
    0x0A70: "EUR_CR_EVENT_OTHER_PDS_INFO", 0x0A74: "EUR_CR_DMS_CTRL",
    0x0A78: "cr_pds_vcb_perf_ctrl", 0x0A7C: "cr_use_g0", 0x0A80: "cr_use_g1",
    0x0AAC: "cr_use_tmp_reg", 0x0AB8: "EUR_CR_PDS_EXEC_BASE",
    0x0ABC: "cr_pds_dms_ctrl_2", 0x0AC4: "EUR_CR_EVENT_KICKER",
    0x0ACC: "EUR_CR_EVENT_TIMER", 0x0B14: "EUR_CR_USE0_SERV_EVENT",
    0x0B20: "EUR_CR_USE1_SERV_EVENT", 0x0B30: "cr_use_moe",
    **{0x0A0C + 4 * n: f"EUR_CR_USE_CODE_BASE_{n}" for n in range(16)},
}
USE_CODE_BASE_REGS = range(0x0A0C, 0x0A0C + 4 * 16, 4)


@dataclass
class InitInfo:
    handles: dict[str, int]
    kick: dict[str, int]                      # command -> ukernel instruction index
    scripts: dict[str, list[tuple[int, int]]]  # part -> [(reg offset, value)]
    build_options: int
    struct_sizes: dict[str, int]
    hwperf_cb: int
    ta_sig: int
    td_sig: int
    edm_task_regs: tuple[int, int]
    clkgate: dict[str, int]
    cache_control: int
    dev_data: list[int]
    mem_handles: list[int]


def parse(buf: bytes) -> InitInfo:
    if len(buf) != SIZE:
        raise ValueError(f"expected {SIZE} bytes, got {len(buf)}")
    w = struct.unpack(f"<{SIZE // 4}I", buf)
    pos = 2                                   # skip ui32BridgeFlags, hDevCookie

    def take(n: int) -> list[int]:
        nonlocal pos
        v = list(w[pos:pos + n])
        pos += n
        return v

    handles = dict(zip(HANDLE_NAMES, take(len(HANDLE_NAMES))))
    kick = dict(zip(KICK_NAMES, take(len(KICK_NAMES))))
    scripts: dict[str, list[tuple[int, int]]] = {}
    for name, count in SCRIPT_PARTS:
        cmds = take(3 * count)
        writes = []
        for i in range(count):
            op, off, val = cmds[3 * i:3 * i + 3]
            if op in (OP_HALT, OP_ILLEGAL):   # script end (ILLEGAL = never written)
                break
            if op == OP_WRITE:
                writes.append((off, val))
        scripts[name] = writes
    (build_options,) = take(1)
    struct_sizes = dict(zip(STRUCT_SIZE_NAMES, take(len(STRUCT_SIZE_NAMES))))
    hwperf_cb, ta_sig, td_sig = take(3)
    edm0, edm1 = take(2)
    clkgate = dict(zip(("ctl", "ctl2", "status_reg", "status_mask"), take(4)))
    (cache_control,) = take(1)
    dev_data = take(24)                       # SGX_MAX_DEV_DATA
    mem_handles = take(18)                    # SGX_MAX_INIT_MEM_HANDLES
    assert pos == len(w), "field table does not cover the buffer"
    return InitInfo(handles, kick, scripts, build_options, struct_sizes, hwperf_cb, ta_sig,
                    td_sig, (edm0, edm1), clkgate, cache_control, dev_data, mem_handles)


def decode_use_code_base(value: int) -> tuple[str, int]:
    """EUR_CR_USE_CODE_BASE_n: ADDR[24:0] (128-byte units), DM[26:25] data master."""
    return DM_NAMES[(value >> 25) & 0x3], (value & 0x01FFFFFF) << 7


def reg_name(offset: int) -> str:
    return REG_NAMES.get(offset, f"0x{offset:04x}")


def r2_script(info: InitInfo) -> str:
    """radare2 commands that name the EDM handlers in the ukernel (loaded at 0)."""
    lines = []
    for cmd, insn in info.kick.items():
        if not insn:
            continue
        addr = insn * 8                       # USSE instructions are 8 bytes
        # The flag always sticks; af/afn may not if r2 already folded the address
        # into a neighbouring handler's function (e.g. GETMISCINFO inside edm.ta).
        lines += [f"f edm.{cmd.lower()} @ {addr:#x}",
                  f"af @ {addr:#x}", f"afn edm.{cmd.lower()} @ {addr:#x}",
                  f"CCu SGXMKIF_CMD_{cmd} handler (host kick address {insn:#x}) @ {addr:#x}"]
    return "\n".join(lines) + "\n"


def report(info: InitInfo) -> str:
    out = ["control blocks (handle = harness alloc token):"]
    out += [f"  {k:18s} {v:#010x}  alloc #{v & 0xFFFF}" for k, v in info.handles.items()]
    out.append("EDM handlers (aui32HostKickAddr -> ukernel instruction / byte offset):")
    out += [f"  {k:16s} {v:#06x}  @{v * 8:#07x}" if v else f"  {k:16s} -" for k, v in info.kick.items()]
    for part, writes in info.scripts.items():
        out.append(f"init script {part} ({len(writes)} writes):")
        for off, val in writes:
            extra = ""
            if off in USE_CODE_BASE_REGS:
                dm, addr = decode_use_code_base(val)
                extra = f"   DM={dm} code={addr:#010x}"
            out.append(f"  {off:#06x} {reg_name(off):30s} = {val:#010x}{extra}")
    out.append(f"build options      {info.build_options:#x}")
    out.append("MKIF struct sizes: " + ", ".join(f"{k}={v:#x}" for k, v in info.struct_sizes.items()))
    out.append(f"EDM task regs      {reg_name(info.edm_task_regs[0])}, {reg_name(info.edm_task_regs[1])}")
    out.append("clock gating       " + ", ".join(f"{k}={v:#x}" for k, v in info.clkgate.items()))
    out.append(f"cache control      {info.cache_control:#x}")
    out.append(f"hwperf/ta-sig/3d-sig buffers: {info.hwperf_cb:#x} {info.ta_sig:#x} {info.td_sig:#x}")
    out.append("init dev data      " + " ".join(f"{v:#x}" for v in info.dev_data))
    out.append("init mem handles   " + " ".join(f"{v:#x}" for v in info.mem_handles if v))
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    with open(argv[0], "rb") as f:
        info = parse(f.read())
    print(report(info))
    if len(argv) == 3 and argv[1] == "--r2":
        with open(argv[2], "w") as f:
            f.write(r2_script(info))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
