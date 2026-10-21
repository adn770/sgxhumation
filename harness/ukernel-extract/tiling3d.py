#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The render-target layout the closed driver chooses by size (design/sgx545_3d.md §14.6).

layout(w, h) is the rule read from the size=WxH captures (sizes3d.txt): 32 x 16-pixel tiles,
grouped into n = 4 macrotiles across and down when w x h >= 640 x 480 pixels, else n = 2; a
macrotile spans the tiles over n, rounded up to a whole number of 4-tile blocks. From that:

- TEMTile1 (across) / TEMTile2 (down): the macrotile size in blocks (31:24), the second and
  third macrotile boundaries in tiles (19:12, 11:0), both the macrotile size when n = 2;
  TEMTile1 bit 31 set when n = 4;
- TEMTileStride: 16 per 4 x 4-tile block of a macrotile;
- the RT data set's MTileWidth: tiles across all macrotiles;
- the RT data's NumTileBlocksPerMT, MTRegionArraySize (12 bytes a tile) and TailSize (a
  square of 4-byte words, the power of two that covers the larger macrotile span);
- rgn_bytes: the region-header buffer (MTRegionArraySize per macrotile; the ZLS external-Z
  region headers have the same size).

check(var) compares the rule with every capture under var (state3d.extract_capture).

    usage: tiling3d.py <var dir>          # every capture against the rule
           tiling3d.py <width> <height>   # the layout of one size
"""
from __future__ import annotations

import pathlib
import sys

import state3d

PATHS = {
    "TEScreen": "k0.cmd.ta_regs.te_screen",
    "TEMTile1": "k0.cmd.ta_regs.te_mtile_1",
    "TEMTile2": "k0.cmd.ta_regs.te_mtile_2",
    "TEMTileStride": "k0.cmd.ta_regs.te_mtile_stride",
    "MTileWidth": "k0.rtds.mtile_width",
    "NumTileBlocksPerMT": "k0.rtds.hw_rt_data.num_tile_blocks_per_mt",
    "MTRegionArraySize": "k0.rtds.hw_rt_data.mt_region_array_size",
    "TailSize": "k0.rtds.hw_rt_data.tail_size",
}
SCREEN = "k0.cmd.ta_regs.mte_screen"


def _mtile(m: int, n: int) -> int:
    return (m // 4) << 24 | (2 * m if n == 4 else m) << 12 | (3 * m if n == 4 else m)


def layout(w: int, h: int) -> dict[str, int]:
    tx, ty = -(-w // 32), -(-h // 16)
    n = 4 if w * h >= 640 * 480 else 2
    mx, my = -(-(-(-tx // n)) // 4) * 4, -(-(-(-ty // n)) // 4) * 4
    blocks = (mx // 4) * (my // 4)
    side = 1
    while side < max(n * mx, n * my):
        side <<= 1
    return {
        "TEScreen": (ty - 1) << 12 | (tx - 1),
        "TEMTile1": _mtile(mx, n) | (1 << 31 if n == 4 else 0),
        "TEMTile2": _mtile(my, n),
        "TEMTileStride": 16 * blocks,
        "MTileWidth": n * mx,
        "NumTileBlocksPerMT": blocks,
        "MTRegionArraySize": 0xC0 * blocks,
        "TailSize": 4 * side * side,
        "rgn_bytes": 0xC0 * blocks * n * n,
    }


def observed(state: dict[str, int]) -> tuple[int, int, dict[str, int]]:
    """The target size of a capture's state words and the layout words it holds."""
    scr = state[SCREEN]
    words = {k: state[p] for k, p in PATHS.items() if p in state}
    return (scr & 0xFFFF) + 1, (scr >> 16) + 1, words


def check(var: pathlib.Path) -> tuple[int, list[str]]:
    """Every capture under var against the rule: (captures checked, mismatches)."""
    n, bad = 0, []
    for cap in sorted(var.iterdir()):
        if not (cap / "dump3d" / "kick00_in.bin").exists():
            continue
        w, h, words = observed(state3d.extract_capture(cap))
        want = layout(w, h)
        n += 1
        bad += [f"{cap.name} {w}x{h} {k}: {v:#x}, rule {want[k]:#x}"
                for k, v in words.items() if v != want[k]]
    return n, bad


def main(argv: list[str]) -> int:
    if len(argv) == 3:
        for k, v in layout(int(argv[1]), int(argv[2])).items():
            print(f"{k:20} {v:#x}")
        return 0
    if len(argv) != 2:
        print(__doc__.rsplit("usage:", 1)[1], file=sys.stderr)
        return 2
    n, bad = check(pathlib.Path(argv[1]))
    print("\n".join(bad))
    print(f"{n} captures, {len(bad)} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
