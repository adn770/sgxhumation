#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Reference loader for sgx545_ukernel.fw (design/sgx545_fw_format.md §3-§5, §9, §10).

Given the file and the device addresses a driver chose, produce what the driver uploads
and programs: the 15 buffer images, patched; the init scripts and asInitDevData with
addresses filled in; and the named EDM metadata. For a v2 file and a solid fill, also the
seven transfer-queue programs, patched, and the fixed command words (fill()); for a v3 file
and a copy, the seven programs of the copy (copy()). A kernel driver does the same in C.

    usage: fwload.py <sgx545_ukernel.fw> <layout.json> [-o <dir>]
                     [--fill <dest>,<width>,<height>,<stride>,<colour>[,<x>,<y>]]
                     [--copy <src>,<dst>,<width>,<height>,<src_stride>,<dst_stride>[,<x>,<y>]]

layout.json: {"addr": {"<buffer name or id>": addr, ...},
              "heap_base": {"<heap id>": base, ...}, "pd_phys": addr,
              "tq_addr": {"<template name or id>": addr, ...}}
"""
from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
from dataclasses import dataclass, field

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


class ParamError(ValueError):
    pass


@dataclass
class Layout:
    addr: dict[int, int]             # buffer id -> device virtual address
    heap_base: dict[int, int]        # heap id -> base
    pd_phys: int = 0
    tq_addr: dict[int, int] = field(default_factory=dict)    # template id -> address


@dataclass
class TQBuild:
    params: list[int]
    images: dict[int, bytes]         # template id -> patched program
    cmd: list[tuple[int, int]]       # (SGXMKIF_TRANSFERCMD byte offset, value)


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
    return _scatter_get(_LIMM_FIELDS, insn)


def limm_set(insn: int, imm: int) -> int:
    return _scatter_set(_LIMM_FIELDS, insn, imm)


# emitpix2 sideband: (instruction bit, width, value bit), format §9.4.
_SB_FIELDS = ((22, 6, 0), (35, 6, 6), (54, 2, 12))


def _scatter_get(fields, insn: int) -> int:
    return sum(((insn >> pos) & ((1 << w) - 1)) << at for pos, w, at in fields)


def _scatter_set(fields, insn: int, v: int) -> int:
    for pos, w, at in fields:
        mask = (1 << w) - 1
        insn = (insn & ~(mask << pos)) | (((v >> at) & mask) << pos)
    return insn


def emitpix_sb_get(insn: int) -> int:
    return _scatter_get(_SB_FIELDS, insn)


def emitpix_sb_set(insn: int, sb: int) -> int:
    return _scatter_set(_SB_FIELDS, insn, sb)


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


# --- format v2/v3: transfer-queue templates (format §9, §10) -----------------------------

PAGE = 0x1000
DOUTU_RANGE = 0x80000                # USE code in the first 512 KiB of its heap (CBASE 0)
# The programs each operation runs (transfer §4.2.4, §4.4.3): they must be placed.
FILL_TEMPLATES = (facts.TQ_USE_FILL, facts.TQ_USE_SEC, facts.TQ_USE_EOT, facts.TQ_USE_EOR,
                  facts.TQ_PDS_PRIM, facts.TQ_PDS_SEC, facts.TQ_PDS_PIXEVENT)
COPY_TEMPLATES = (facts.TQ_USE_COPY, facts.TQ_USE_SEC, facts.TQ_USE_EOT, facts.TQ_USE_EOR,
                  facts.TQ_PDS_PRIM_1SRC, facts.TQ_PDS_SEC, facts.TQ_PDS_PIXEVENT)
TEX_MAX = 0x1000                     # 12-bit WIDTH/HEIGHT in T1, no *EXT bits in T3


def fill_params(dest: int, width: int, height: int, stride: int, colour: int,
                x: int = 0, y: int = 0) -> list[int]:
    """The PARAM values of an ARGB8888 fill (format §9.6).

    The surface's pixel (0, 0) is at dest; the filled rectangle is width x height pixels at
    (x, y). The PBE clips to that rectangle, so pixels around it are left alone.
    """
    if not (1 <= width and 1 <= height and x + width <= 0x2000 and y + height <= 0x2000):
        raise ParamError("the rectangle must lie within 8192 x 8192")
    if x < 0 or y < 0:
        raise ParamError("x and y must not be negative")
    if stride % 8 or not 4 * (x + width) <= stride <= 0x8000:
        raise ParamError("stride must be a multiple of 8 bytes in [4 * (x + width), 0x8000]")
    if dest % 4 or dest + stride * (y + height - 1) + 4 * (x + width) > 1 << 32:
        raise ParamError("the surface must be 4-byte aligned and below 4 GiB")
    return [colour & 0xFFFFFFFF, x | y << 16, (x + width - 1) | (y + height - 1) << 16,
            0x8000 | (stride // 8 - 1) << 16, dest, 0x200, 0, 0, 0]


def copy_params(src: int, dst: int, width: int, height: int, src_stride: int,
                dst_stride: int, x: int = 0, y: int = 0) -> list[int]:
    """The PARAM values of an ARGB8888 copy (format §10.4).

    The source is the linear width x height texture whose first texel is at src, src_stride
    bytes per line; it lands 1:1 on the width x height rectangle at (x, y) of the linear
    destination surface whose pixel (0, 0) is at dst. Parameters 1-5 are the PBE words of
    that rectangle (as for a fill), 6-8 the texture words T0, T1 and T2.
    """
    if not (1 <= width <= TEX_MAX and 1 <= height <= TEX_MAX):
        raise ParamError(f"the copy must be 1 to {TEX_MAX} pixels in each direction")
    if src_stride % 4 or not 4 * width <= src_stride <= 0x8000:
        raise ParamError("src_stride must be a multiple of 4 bytes in [4 * width, 0x8000]")
    if src % 4 or src + src_stride * (height - 1) + 4 * width > 1 << 32:
        raise ParamError("the source must be 4-byte aligned and below 4 GiB")
    pbe = fill_params(dst, width, height, dst_stride, 0, x, y)
    s = src_stride // 4 - 1                         # the stride in dwords, minus one
    t0 = (s & 7) << 9 | (s >> 3) << 17              # STRIDELO [11:9], STRIDEHI [26:17]
    t1 = 0xCC000000 | (width - 1) << 12 | (height - 1)   # NP2_STRIDE_EXT, B8G8R8A8
    return [0, *pbe[1:6], t0, t1, src]


def _tq_meta(fw: F.Firmware) -> dict[int, F.TQMeta]:
    return {m.id: m for m in fw.tq_meta}


def tq_check(fw: F.Firmware, layout: Layout, need: tuple[int, ...] = ()) -> None:
    """Each placed template aligned, in its heap, within a page if NO_STRADDLE; each
    template in need that the file has placed (format §10.3)."""
    if not fw.tq_meta:
        raise F.FormatError("the file has no transfer-queue templates (format v2)")
    for m in fw.tq_meta:
        if m.id not in layout.tq_addr:
            if m.id in need:
                raise LayoutError(f"no address for template {m.id}")
            continue
        if m.heap not in layout.heap_base:
            raise LayoutError(f"no base for heap {m.heap}")
        a = layout.tq_addr[m.id]
        if a % m.align or a < layout.heap_base[m.heap] or a + m.size > 1 << 32:
            raise LayoutError(f"template {m.id} misplaced at {a:#x}")
        if m.flags & F.TQ_NO_STRADDLE and a % PAGE + m.size > PAGE:
            raise LayoutError(f"template {m.id} at {a:#x} straddles a page")


def tq_evaluate(e: F.Expr, layout: Layout, fw: F.Firmware, params: list[int]) -> int:
    k = e.kind
    if k == F.K_CONST:
        v = e.addend
    elif k in (F.K_ADDR, F.K_DOUTU) and e.target not in layout.tq_addr:
        raise LayoutError(f"no address for template {e.target}")
    elif k == F.K_ADDR:
        v = layout.tq_addr[e.target] + e.addend
    elif k == F.K_HEAP:
        if e.target not in layout.heap_base:
            raise LayoutError(f"no base for heap {e.target}")
        v = layout.heap_base[e.target] + e.addend
    elif k == F.K_PARAM:
        v = params[e.target] + e.addend
    elif k == F.K_DOUTU:
        m = _tq_meta(fw)[e.target]
        off = layout.tq_addr[e.target] + e.addend - layout.heap_base[m.heap]
        if off < 0 or off >= DOUTU_RANGE or off % 8:
            raise LayoutError(f"template {e.target} is not a DOUTU target ({off:#x})")
        v = ((off << 1) & 0xFFFF0) | ((off >> 19) & 0xF) | ((m.temps + 3) >> 2) << 24
    else:
        raise F.FormatError(f"kind {k} in a transfer-queue record")
    return v & 0xFFFFFFFF


def build_tq(fw: F.Firmware, layout: Layout, params: list[int],
             need: tuple[int, ...] = ()) -> TQBuild:
    """Copy every placed template, zero-padded, apply its TQ_PATCHES in order, evaluate
    TQ_CMD. Templates without an address are left out, with their patches (format §10.3)."""
    if len(params) != F.TQ_NUM_PARAMS:
        raise ParamError(f"{F.TQ_NUM_PARAMS} parameters expected")
    tq_check(fw, layout, need)
    images = {m.id: bytearray(fw.tq_images.get(m.id, b"").ljust(m.size, b"\0"))
              for m in fw.tq_meta if m.id in layout.tq_addr}
    for e in fw.tq_patches:
        if e.buffer not in images:
            continue
        v = tq_evaluate(e, layout, fw, params)
        img = images[e.buffer]
        if e.encoding == F.ENC_WORD32:
            struct.pack_into("<I", img, e.offset, v)
        else:
            (insn,) = struct.unpack_from("<Q", img, e.offset)
            put = limm_set if e.encoding == F.ENC_USSE_LIMM else emitpix_sb_set
            struct.pack_into("<Q", img, e.offset, put(insn, v))
    cmd = [(e.offset, tq_evaluate(e, layout, fw, params))
           for e in fw.values.get(F.V_TQ_CMD, [])]
    return TQBuild(list(params), {i: bytes(b) for i, b in images.items()}, cmd)


def fill(fw: F.Firmware, layout: Layout, dest: int, width: int, height: int, stride: int,
         colour: int, x: int = 0, y: int = 0) -> TQBuild:
    """The patched programs and command words of a solid fill (format §9)."""
    return build_tq(fw, layout, fill_params(dest, width, height, stride, colour, x, y),
                    FILL_TEMPLATES)


def copy(fw: F.Firmware, layout: Layout, src: int, dst: int, width: int, height: int,
         src_stride: int, dst_stride: int, x: int = 0, y: int = 0) -> TQBuild:
    """The patched programs and command words of an ARGB8888 copy (format §10): the
    params of copy_params(), the seven programs of COPY_TEMPLATES (and any other template
    the layout places) and the same fixed command words as a fill."""
    if not {facts.TQ_USE_COPY, facts.TQ_PDS_PRIM_1SRC} <= {m.id for m in fw.tq_meta}:
        raise F.FormatError("the file has no copy templates (format v3)")
    params = copy_params(src, dst, width, height, src_stride, dst_stride, x, y)
    return build_tq(fw, layout, params, COPY_TEMPLATES)


def parse_layout(doc: dict) -> Layout:
    def bid(key: str) -> int:
        return facts.BUFFER_NAMES.index(key) if key in facts.BUFFER_NAMES else int(key, 0)

    def tid(key: str) -> int:
        return facts.TQ_NAMES.index(key) if key in facts.TQ_NAMES else int(key, 0)
    return Layout(addr={bid(k): int(str(v), 0) for k, v in doc["addr"].items()},
                  heap_base={int(k, 0): int(str(v), 0) for k, v in doc["heap_base"].items()},
                  pd_phys=int(str(doc.get("pd_phys", 0)), 0),
                  tq_addr={tid(k): int(str(v), 0) for k, v in doc.get("tq_addr", {}).items()})


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="fwload.py", description=__doc__.splitlines()[0])
    ap.add_argument("fw")
    ap.add_argument("layout")
    ap.add_argument("-o", "--outdir", help="write the patched buffer images here")
    op = ap.add_mutually_exclusive_group()
    op.add_argument("--fill", help="dest,width,height,stride,colour[,x,y]: build a solid fill")
    op.add_argument("--copy", help="src,dst,width,height,src_stride,dst_stride[,x,y]: build "
                                   "a copy")
    args = ap.parse_args(argv)
    try:
        fw = F.unpack(pathlib.Path(args.fw).read_bytes())
        layout = parse_layout(json.loads(pathlib.Path(args.layout).read_text()))
        boot = load(fw, layout) if layout.addr else None
        what, build = ("fill", fill) if args.fill else ("copy", copy)
        spec = args.fill or args.copy
        tq = build(fw, layout, *(int(x, 0) for x in spec.split(","))) if spec else None
    except (F.FormatError, LayoutError, ParamError, KeyError, ValueError, TypeError) as e:
        print(f"fwload: {e}", file=sys.stderr)
        return 1
    out = pathlib.Path(args.outdir) if args.outdir else None
    if tq:
        print(f"{what} command words ({len(tq.cmd)}):")
        print("\n".join(f"  {off:#05x} = {val:#010x}" for off, val in tq.cmd))
        if out:
            out.mkdir(parents=True, exist_ok=True)
            for i, img in tq.images.items():
                (out / f"tq{i}_{facts.TQ_NAMES[i]}.bin").write_bytes(img)
    if boot is None:
        return 0
    for name, writes in boot.scripts.items():
        print(f"init script {name} ({len(writes)} writes):")
        print("\n".join(f"  {reg:#06x} = {val:#010x}" for reg, val in writes))
    print("dev data " + " ".join(f"{v:#x}" for v in boot.dev_data))
    print("kick     " + ", ".join(f"{k}={v:#x}" for k, v in boot.kick.items() if v))
    if out:
        out.mkdir(parents=True, exist_ok=True)
        for i, img in boot.images.items():
            (out / f"{i:02d}_{facts.BUFFER_NAMES[i]}.bin").write_bytes(img)
        print(f"wrote {len(boot.images)} buffer images to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
