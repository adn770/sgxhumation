#!/usr/bin/env python3
"""Reference loader for sgx545_ukernel.fw (design/sgx545_fw_format.md §3-§5).

Given the file and the device addresses a driver chose, produce what the driver uploads
and programs: the 15 buffer images, patched; the init scripts and asInitDevData with
addresses filled in; and the named EDM metadata. A kernel driver does the same in C.

    usage: fwload.py <sgx545_ukernel.fw> <layout.json> [-o <dir>]

layout.json: {"addr": {"<buffer name or id>": addr, ...},
              "heap_base": {"<heap id>": base, ...}, "pd_phys": addr}
"""
from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
from dataclasses import dataclass

import facts
import fwformat as F

CODE_PC_BITS = 19                    # pc field of a PDS code word, [22:4]
_LIMM_FIELDS = ((0, 21, 0), (36, 5, 21), (44, 6, 26))   # (insn bit, width, imm bit)
SCRIPT_NAMES = {F.V_PART1: "part1", F.V_PART2: "part2", F.V_DEINIT: "deinit"}
KICK_NAMES = ("TA", "TRANSFER", "2D", "POWER", "CONTEXTSUSPEND", "CLEANUP", "GETMISCINFO",
              "PROCESS_QUEUES", "DATABREAKPOINT", "SETHWPERFSTATUS", "FLUSHPDCACHE")
STRUCT_SIZE_NAMES = ("CMDTA", "CMDTA_SHARED", "TRANSFERCMD", "TRANSFERCMD_SHARED",
                     "3DREGISTERS", "HWPBDESC", "HWRENDERCONTEXT", "HWRENDERDETAILS",
                     "HWRTDATA", "HWRTDATASET", "HWTRANSFERCONTEXT", "HOST_CTL", "COMMAND")


class LayoutError(ValueError):
    pass


@dataclass
class Layout:
    addr: dict[int, int]             # buffer id -> device virtual address
    heap_base: dict[int, int]        # heap id -> base
    pd_phys: int = 0


@dataclass
class Boot:
    layout: Layout
    images: dict[int, bytes]
    scripts: dict[str, list[tuple[int, int]]]
    dev_data: list[int]
    kick: dict[str, int]
    struct_sizes: dict[str, int]
    edm_task_regs: tuple[int, int]
    clkgate: dict[str, int]
    cache_control: int


def limm_get(insn: int) -> int:
    return sum(((insn >> pos) & ((1 << w) - 1)) << at for pos, w, at in _LIMM_FIELDS)


def limm_set(insn: int, imm: int) -> int:
    for pos, w, at in _LIMM_FIELDS:
        mask = (1 << w) - 1
        insn = (insn & ~(mask << pos)) | (((imm >> at) & mask) << pos)
    return insn


def _uk_off(fw: F.Firmware, layout: Layout) -> int:
    delta = layout.addr[facts.UKERNEL] - layout.heap_base[facts.KERNEL_CODE]
    if delta < 0 or delta % 8:
        raise LayoutError("the ukernel must sit at a whole instruction above the code base")
    return delta // 8


def evaluate(e: F.Expr, layout: Layout, fw: F.Firmware) -> int:
    rows = {b.id: b.pds_rows for b in fw.buffers}
    k = e.kind
    if k == F.K_CONST:
        v = e.addend
    elif k == F.K_ADDR:
        v = layout.addr[e.target] + e.addend
    elif k == F.K_ADDR_SHR4:
        v = (layout.addr[e.target] + e.addend) >> 4
    elif k == F.K_HEAP:
        v = layout.heap_base[e.target] + e.addend
    elif k == F.K_PD_PHYS:
        v = layout.pd_phys + e.addend
    elif k == F.K_CODE_WORD:
        pc = e.target + _uk_off(fw, layout)
        if pc >> CODE_PC_BITS:
            raise LayoutError(f"code word target {pc:#x} does not fit {CODE_PC_BITS} bits")
        v = e.addend | (pc << 4)
    elif k == F.K_PDS_CTL:
        v = (rows[e.target] << 23) | e.addend
    elif k == F.K_PDS_ROWS:
        v = rows[e.target]
    else:
        raise F.FormatError(f"unknown expression kind {k}")
    return v & 0xFFFFFFFF


def _encode(e: F.Expr, v: int) -> int:
    if e.encoding == F.ENC_CODE_BASE:
        return ((v >> 7) & 0x1FFFFFF) | (e.param << 25)
    return v


def _check(fw: F.Firmware, layout: Layout) -> None:
    for b in fw.buffers:
        if b.id not in layout.addr:
            raise LayoutError(f"no address for buffer {facts.BUFFER_NAMES[b.id]}")
        if layout.addr[b.id] % b.align:
            raise LayoutError(f"{facts.BUFFER_NAMES[b.id]} must be {b.align}-byte aligned")
    _uk_off(fw, layout)


def load(fw: F.Firmware, layout: Layout) -> Boot:
    _check(fw, layout)
    images = {b.id: bytearray(fw.images.get(b.id, b"").ljust(b.size, b"\0")) for b in fw.buffers}
    for e in fw.patches:
        v = evaluate(e, layout, fw)
        img = images[e.buffer]
        if e.encoding == F.ENC_USSE_LIMM:
            (insn,) = struct.unpack_from("<Q", img, e.offset)
            struct.pack_into("<Q", img, e.offset, limm_set(insn, v))
        else:
            struct.pack_into("<I", img, e.offset, _encode(e, v))

    def values(lid: int) -> list[tuple[int, int]]:
        return [(e.offset, _encode(e, evaluate(e, layout, fw))) for e in fw.values.get(lid, [])]

    dev_data = [0] * 24
    for i, v in values(F.V_DEV_DATA):
        dev_data[i] = v
    m = fw.meta
    return Boot(
        layout=layout,
        images={i: bytes(b) for i, b in images.items()},
        scripts={name: values(lid) for lid, name in SCRIPT_NAMES.items()},
        dev_data=dev_data,
        kick=dict(zip(KICK_NAMES, m[0:11])),
        struct_sizes=dict(zip(STRUCT_SIZE_NAMES, m[11:24])),
        edm_task_regs=(m[24], m[25]),
        clkgate=dict(zip(("ctl", "ctl2", "status_reg", "status_mask"), m[26:30])),
        cache_control=m[30],
    )


def parse_layout(doc: dict) -> Layout:
    def bid(key: str) -> int:
        return facts.BUFFER_NAMES.index(key) if key in facts.BUFFER_NAMES else int(key, 0)
    return Layout(addr={bid(k): int(str(v), 0) for k, v in doc["addr"].items()},
                  heap_base={int(k, 0): int(str(v), 0) for k, v in doc["heap_base"].items()},
                  pd_phys=int(str(doc.get("pd_phys", 0)), 0))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="fwload.py", description=__doc__.splitlines()[0])
    ap.add_argument("fw")
    ap.add_argument("layout")
    ap.add_argument("-o", "--outdir", help="write the patched buffer images here")
    args = ap.parse_args(argv)
    try:
        fw = F.unpack(pathlib.Path(args.fw).read_bytes())
        boot = load(fw, parse_layout(json.loads(pathlib.Path(args.layout).read_text())))
    except (F.FormatError, LayoutError, KeyError) as e:
        print(f"fwload: {e}", file=sys.stderr)
        return 1
    for name, writes in boot.scripts.items():
        print(f"init script {name} ({len(writes)} writes):")
        print("\n".join(f"  {reg:#06x} = {val:#010x}" for reg, val in writes))
    print("dev data " + " ".join(f"{v:#x}" for v in boot.dev_data))
    print("kick     " + ", ".join(f"{k}={v:#x}" for k, v in boot.kick.items() if v))
    if args.outdir:
        out = pathlib.Path(args.outdir)
        out.mkdir(parents=True, exist_ok=True)
        for i, img in boot.images.items():
            (out / f"{i:02d}_{facts.BUFFER_NAMES[i]}.bin").write_bytes(img)
        print(f"wrote {len(boot.images)} buffer images to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
