#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Summarise one captured kick: every allocation with its role, cross references, and the
PDS and USSE programs it reaches, disassembled with tools/usse-dis.

Roles come from kick3d.walk() (the pointer fields of the kick structures and the VDM
control stream). Every object also gets the cross references found by scanning all
objects' dwords for device addresses: plain pointers, PDS base fields (address >> 4 in
bits 27:0, counted only when they land in a PDS heap) and, inside PDS heaps only, USSE
code-address words. Plain pointers can still be coincidences (a counter that happens to
equal an address); treat a lone reference as a lead, not a fact.

A USSE code-address word, as PDS programs hand it to `doutu`, holds the index of a
EUR_CR_USE_CODE_BASE register in bits 3:0 and an instruction index (8 bytes each) from
that base in bits 23:4. The bases are the ones srvinit's init script programs (spec
§13.5): 0..9 pixel, 10..13 vertex, 15 EDM.

PDS programs are a data segment followed by code. The data rows are four dwords,
ds0[2r], ds0[2r+1], ds1[2r], ds1[2r+1] (spec §13.8).

    usage: summary3d.py <kickNN_in.bin> <kickNN/> [initinfo.bin]
"""
from __future__ import annotations

import os
import pathlib
import struct
import subprocess
import sys
import tempfile

import kick3d

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent.parent / "tools" / "usse-dis"

# USE_CODE_BASE_n (harness heap layout, captured init script part 2).
DEFAULT_CODE_BASES = {**{n: 0xF4000000 + n * 0x80000 for n in range(10)},
                      **{10 + n: 0xFC000000 + n * 0x80000 for n in range(4)},
                      15: 0xE8000000}
USSE_HEAPS = ("PIXELSHADER", "VERTEXSHADER", "KERNEL_CODE")
PDS_HEAPS = ("PDSPIXEL_CODEDATA", "PDSVERTEX_CODEDATA")
PDS_HALT = 0xAF000000
PDS_EXEC_BASE = 0xE4000000      # EUR_CR_PDS_EXEC_BASE from the init script (spec §13.5)


def code_bases_from_initinfo(path) -> dict[int, int]:
    import initinfo
    info = initinfo.parse(pathlib.Path(path).read_bytes())
    bases = {}
    for off, val in info.scripts["part2"]:
        if off in initinfo.USE_CODE_BASE_REGS:
            n = (off - 0x0A0C) // 4
            _dm, addr = initinfo.decode_use_code_base(val)
            if addr or n not in bases:
                bases[n] = addr
    return {n: a for n, a in bases.items() if a}


def usse_address(word: int, bases: dict[int, int]) -> int | None:
    base = bases.get(word & 0xF)
    return None if base is None else base + ((word >> 4) & 0xFFFFF) * 8


def pds_banks(data: list[int]) -> tuple[list[int], list[int]]:
    ds0, ds1 = [], []
    for r in range(0, len(data) - 3, 4):
        ds0 += data[r:r + 2]
        ds1 += data[r + 2:r + 4]
    return ds0, ds1


def pds_code(words: list[int], data_dwords: int, limit: int = 256) -> list[int]:
    """The code words after the data segment: up to the last halt before a gap of zero
    words (two after a halt, where the next packed program may start, else four)."""
    code = words[data_dwords:data_dwords + limit]
    end = None
    for i, w in enumerate(code):
        if w == PDS_HALT:
            end = i + 1
            if code[i + 1:i + 3] in ([0, 0], [0], []):
                break
        if code[i:i + 4] == [0, 0, 0, 0] and end is not None:
            break
    return code[:end] if end is not None else code


def usse_code(raw: bytes, limit: int = 128) -> bytes:
    """USSE instructions up to the first pair of all-zero instructions."""
    out = []
    for i in range(0, min(len(raw), limit * 8) - 7, 8):
        insn = raw[i:i + 8]
        if insn == bytes(8) and raw[i + 8:i + 16] == bytes(8):
            break
        out.append(insn)
    return b"".join(out)


def xrefs(objs, data, bases=DEFAULT_CODE_BASES) -> dict[int, list[tuple[int, int]]]:
    """{target index: [(source index, byte offset)]} for dwords pointing into objects."""
    targets = sorted((o for o in objs if o["kind"] != 1), key=lambda o: o["dva"])

    def hit(v):
        for o in targets:
            if o["dva"] <= v < o["dva"] + o["mapped"]:
                return o
        return None

    refs: dict[int, list[tuple[int, int]]] = {}
    for src in targets:
        raw = data(src)
        for i, v in enumerate(struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4])):
            if not v:
                continue
            found = []
            for cand, kinds in ((v, None), ((v & 0x0FFFFFFF) << 4, PDS_HEAPS)):
                o = hit(cand)
                if o and (kinds is None or o["heap_name"] in kinds):
                    found.append(o)
            if src["heap_name"] in PDS_HEAPS and v >= 0x100 and not v & 0xF7000000:
                a = usse_address(v, bases)
                o = hit(a) if a is not None else None
                if o and o["heap_name"] in USSE_HEAPS:
                    found.append(o)
            for o in found:
                if o["index"] != src["index"]:
                    refs.setdefault(o["index"], [])
                    if (src["index"], 4 * i) not in refs[o["index"]]:
                        refs[o["index"]].append((src["index"], 4 * i))
    return refs


def pixel_pds_word(v: int) -> tuple[int, int] | None:
    """A pixel PDS program reference in MTE state [infer]: bits 31:26 the data size in
    16-byte rows, bits 15:0 the program address relative to PDS_EXEC_BASE >> 4. Seen as
    the special object word and in the state blocks the vertex PDS programs DMA."""
    rows = v >> 26
    if not rows or v & 0x03FF0000:
        return None
    return PDS_EXEC_BASE + ((v & 0xFFFF) << 4), 4 * rows


def pixel_pds_refs(objs, data) -> dict[int, tuple[int, str]]:
    """{program dva: (data dwords, where)} for pixel PDS references in PDS vertex objects
    whose target is a PDSPIXEL object holding code that halts after the data."""
    by_dva = sorted(objs, key=lambda o: o["dva"])
    found: dict[int, tuple[int, str]] = {}
    for src in by_dva:
        if src["heap_name"] != "PDSVERTEX_CODEDATA" or src["kind"] == 1:
            continue
        raw = data(src)
        for i, v in enumerate(struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4])):
            ref = pixel_pds_word(v)
            if not ref:
                continue
            dva, n = ref
            t = next((o for o in by_dva if o["dva"] <= dva < o["dva"] + o["mapped"]
                      and o["heap_name"] == "PDSPIXEL_CODEDATA"), None)
            if t is None or dva in found:
                continue
            tr = data(t)[dva - t["dva"]:]
            words = list(struct.unpack(f"<{len(tr) // 4}I", tr[:len(tr) // 4 * 4]))
            if PDS_HALT in pds_code(words, n, 64):
                found[dva] = (n, f"obj {src['index']}+{4 * i:#x}")
    return found


def _run(tool: str, raw: bytes, *args) -> list[str]:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(raw)
    try:
        out = subprocess.run([tool, f.name, *args], capture_output=True, text=True)
        return out.stdout.splitlines()
    finally:
        os.unlink(f.name)


def disassemble_pds(words: list[int], data_dwords: int, tool: str) -> list[str]:
    return _run(tool, struct.pack(f"<{len(words)}I", *words), str(data_dwords))


def disassemble_usse(raw: bytes, tool: str) -> list[str]:
    return _run(tool, raw)


def summary(kick_in_path, snap_dir, initinfo_path=None) -> str:
    snap = kick3d.Snapshot(snap_dir)
    kick_in = pathlib.Path(kick_in_path).read_bytes()
    bases = code_bases_from_initinfo(initinfo_path) if initinfo_path else DEFAULT_CODE_BASES
    k = kick3d.decode("PVRSRV_BRIDGE_IN_DOKICK", kick_in)
    roles = kick3d.walk(kick_in, snap.by_handle(k["sCCBKick.hCCBKernelMemInfo"]), snap)
    by_obj: dict[int, list[dict]] = {}
    for r in roles:
        o = snap.find(r["dva"])
        if o:
            by_obj.setdefault(o["index"], []).append(r)
    refs = xrefs(snap.objs, snap.data, bases)
    pds_tool, usse_tool = TOOLS / "pds-dis", TOOLS / "usse-dis"

    out = [f"kick {pathlib.Path(kick_in_path).name}: {len(snap.objs)} objects, "
           f"{len(roles)} roles", "", "== allocations"]
    for o in snap.objs:
        names = [f"{r['role']}" + (f" @+{r['dva'] - o['dva']:#x}" if r["dva"] != o["dva"] else "")
                 + (f" [{r['struct']}]" if r.get("struct") not in (None, "PDS", "VDM") else "")
                 for r in sorted(by_obj.get(o["index"], []), key=lambda r: r["dva"])]
        rs = refs.get(o["index"], [])
        ref = ", ".join(f"obj {s}+{off:#x}" for s, off in rs[:6]) + (" ..." if len(rs) > 6 else "")
        kind = {1: " sync", 2: " context"}.get(o["kind"], "")
        out.append(f"obj {o['index']:3d} {o['heap_name']:24s} {o['dva']:#010x} "
                   f"size={o['size']:#8x}{kind}  " + ("; ".join(names) or "-")
                   + (f"  <- {ref}" if ref else ""))

    out += ["", "== structures"]
    out.append(kick3d.report(kick_in, snap).split("\n\n", 1)[-1])

    out += ["", "== PDS programs"]
    progs = [(r["dva"], r["role"], r["pds_data_dwords"])
             for r in roles if r["struct"] == "PDS"]
    known = {p[0] for p in progs}
    for dva, (n, where) in pixel_pds_refs(snap.objs, snap.data).items():
        if dva not in known:
            progs.append((dva, f"pixel PDS program (MTE state word at {where}) [infer]", n))
    usse_entries: dict[int, str] = {}
    for dva, role, n in sorted(progs):
        o = snap.find(dva)
        if not o:
            continue
        raw = snap.data(o)[dva - o["dva"]:]
        words = list(struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4]))
        if n is None:
            nz = [w for w in words[:32]]
            out.append(f"\n{dva:#010x} {role}: data size not decoded; first words "
                       f"{kick3d._fmt(nz[:16])}")
            continue
        code = pds_code(words, n)
        ds0, ds1 = pds_banks(words[:n])
        out.append(f"\n{dva:#010x} {role}: {n} data dwords, {len(code)} instructions")
        out.append(f"  ds0 = {kick3d._fmt(ds0)}")
        out.append(f"  ds1 = {kick3d._fmt(ds1)}")
        for w in words[:n]:
            a = usse_address(w, bases) if w >= 0x100 and not w & 0xF7000000 else None
            t = snap.find(a) if a is not None else None
            if t and t["heap_name"] in USSE_HEAPS:
                usse_entries.setdefault(a, f"from PDS {dva:#010x} data word {w:#010x}")
        if pds_tool.exists():
            out += ["  " + line for line in disassemble_pds(words[:n] + code, n, str(pds_tool))
                    [n:]]

    out += ["", "== USSE programs"]
    for a, why in sorted(usse_entries.items()):
        o = snap.find(a)
        raw = usse_code(snap.data(o)[a - o["dva"]:])
        out.append(f"\n{a:#010x} {o['heap_name']} obj {o['index']}: {len(raw) // 8} "
                   f"instructions ({why})")
        if usse_tool.exists():
            out += ["  " + line for line in disassemble_usse(raw, str(usse_tool))]
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    print(summary(*argv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
