#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Word-by-word differences between two captured 3D frames (differential captures).

Each capture is a directory with dump3d/ (capture3d.sh runs tri once per variation in
variations3d.txt). state3d.extract_capture() names every state word of both kicks; this
tool compares two of them:

- a word that differs only because the object it points into moved (same object index and
  offset in both captures) is an address shift, counted but not listed;
- every other change is listed with the bits that flipped and, for the words state3d
  can decode, the fields that changed.

    usage: diff3d.py <base capture> <capture>...     # each against base
           diff3d.py --all <dir> [base]              # every capture in dir against base
"""
from __future__ import annotations

import dataclasses
import pathlib
import sys

import kick3d
import state3d


def symbol(v, snap) -> str | None:
    """'obj<index>+<offset>' when a 32-bit value points into a captured object."""
    if not isinstance(v, int) or v < 0x01000000 or v >> 32:
        return None
    o = snap.find(v)
    return None if o is None else f"obj{o['index']}+{v - o['dva']:#x}"


@dataclasses.dataclass
class Change:
    path: str
    a: int
    b: int
    xor: int
    bits: list[int]
    fields: dict


@dataclasses.dataclass
class Diff:
    changed: list[Change]
    moved: list[str]
    only_a: list[str]
    only_b: list[str]


def load(path):
    """(state words, {kick: Snapshot}) of one capture directory."""
    d = pathlib.Path(path) / "dump3d"
    snaps = {int(p.name[4:6]): kick3d.Snapshot(d / p.name.replace("_in.bin", ""))
             for p in sorted(d.glob("kick*_in.bin"))}
    return state3d.extract_capture(path), snaps


def _kick(path: str) -> int:
    return int(path.split(".", 1)[0][1:])


def diff(a: dict, snaps_a: dict, b: dict, snaps_b: dict, decoders=None) -> Diff:
    decoders = state3d.DECODERS if decoders is None else decoders
    out = Diff([], [], [k for k in a if k not in b], [k for k in b if k not in a])
    for k, va in a.items():
        if k not in b or b[k] == va:
            continue
        vb = b[k]
        n = _kick(k)
        sa, sb = symbol(va, snaps_a[n]), symbol(vb, snaps_b[n])
        if sa is not None and sa == sb:
            out.moved.append(k)
            continue
        x = va ^ vb
        fields = {}
        dec = decoders.get(state3d.word_kind(k))
        if dec:
            fa, fb = dec(va), dec(vb)
            fields = {f: (fa[f], fb.get(f)) for f in fa if fa[f] != fb.get(f)}
        out.changed.append(Change(k, va, vb, x, [i for i in range(64) if x >> i & 1], fields))
    return out


def _bits(bits: list[int]) -> str:
    runs, start = [], None
    for i, b in enumerate(bits):
        if start is None:
            start = b
        if i + 1 == len(bits) or bits[i + 1] != b + 1:
            runs.append(f"{b}:{start}" if b != start else f"{b}")
            start = None
    return " ".join(reversed(runs))


def format_diff(name: str, d: Diff, limit: int = 400) -> str:
    out = [f"== {name}: {len(d.changed)} words changed, {len(d.moved)} address shifts, "
           f"{len(d.only_a)} only in base, {len(d.only_b)} only here"]
    for c in d.changed[:limit]:
        line = f"  {c.path:44s} {c.a:#x} -> {c.b:#x}  bits {_bits(c.bits)}"
        if c.fields:
            line += "  " + ", ".join(f"{f}: {x} -> {y}" for f, (x, y) in c.fields.items())
        out.append(line)
    for tag, keys in (("only in base", d.only_a), ("only here", d.only_b)):
        if keys:
            out.append(f"  {tag}: " + " ".join(keys[:40]) + (" ..." if len(keys) > 40 else ""))
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[0] == "--all":
        root = pathlib.Path(argv[1])
        base_dir = root / (argv[2] if len(argv) > 2 else "base")
        others = sorted(p for p in root.iterdir() if (p / "dump3d").is_dir() and p != base_dir)
    elif len(argv) >= 2:
        base_dir, others = pathlib.Path(argv[0]), [pathlib.Path(p) for p in argv[1:]]
    else:
        print(__doc__.strip().splitlines()[-2].strip(), file=sys.stderr)
        return 2
    base = load(base_dir)
    for p in others:
        print(format_diff(p.name, diff(*base, *load(p))))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
