# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for tiling3d.py — the render-target layout the closed driver chooses by size.

The expected words are the closed driver's at those sizes (size=WxH captures, sizes3d.txt);
the capture test checks the rule against every capture present, and runs only when the
gitignored captures are there."""
import pathlib

import pytest

import tiling3d

HERE = pathlib.Path(__file__).resolve().parent
VAR = HERE / "extracted" / "gles2_tri" / "var"


def test_small_targets_keep_the_first_triangles_words():
    for w, h in ((1, 1), (64, 64), (100, 60), (256, 128)):
        lay = tiling3d.layout(w, h)
        assert (lay["TEMTile1"], lay["TEMTile2"], lay["TEMTileStride"]) == \
            (0x01004004, 0x01004004, 0x10)
        assert (lay["MTileWidth"], lay["NumTileBlocksPerMT"], lay["MTRegionArraySize"],
                lay["TailSize"], lay["rgn_bytes"]) == (8, 1, 0xC0, 0x100, 0x300)


def test_more_than_eight_tile_rows_grow_the_macrotiles():
    lay = tiling3d.layout(128, 192)
    assert lay["TEScreen"] == 0xB003
    assert (lay["TEMTile1"], lay["TEMTile2"], lay["TEMTileStride"]) == \
        (0x01004004, 0x02008008, 0x20)
    assert (lay["NumTileBlocksPerMT"], lay["MTRegionArraySize"], lay["TailSize"]) == \
        (2, 0x180, 0x400)


def test_four_macrotiles_from_vga_area():
    assert tiling3d.layout(640, 475)["TEMTile1"] == 0x0300C00C
    lay = tiling3d.layout(640, 480)
    assert (lay["TEMTile1"], lay["TEMTile2"], lay["TEMTileStride"]) == \
        (0x82010018, 0x02010018, 0x40)
    assert tiling3d.layout(4096, 74)["TEMTile1"] == 0x10040040
    assert tiling3d.layout(4096, 75)["TEMTile1"] == 0x88040060


def test_largest_target():
    lay = tiling3d.layout(4096, 4096)
    assert (lay["TEScreen"], lay["TEMTile1"], lay["TEMTile2"], lay["TEMTileStride"]) == \
        (0xFF07F, 0x88040060, 0x100800C0, 0x800)
    assert (lay["MTileWidth"], lay["NumTileBlocksPerMT"], lay["MTRegionArraySize"],
            lay["TailSize"], lay["rgn_bytes"]) == (0x80, 0x80, 0x6000, 0x40000, 0x60000)


def test_odd_sizes_round_macrotiles_to_whole_blocks():
    lay = tiling3d.layout(2049, 2049)
    assert (lay["TEMTile1"], lay["TEMTile2"], lay["NumTileBlocksPerMT"]) == \
        (0x8502803C, 0x0904806C, 0x2D)
    lay = tiling3d.layout(333, 777)
    assert (lay["TEMTile1"], lay["TEMTile2"], lay["TailSize"]) == \
        (0x02008008, 0x0701C01C, 0x4000)


def test_observed_words_by_path():
    state = {"k0.cmd.ta_regs.mte_screen": 0x00FF003F, "k0.rtds.mtile_width": 8,
             "k0.cmd.ta_regs.te_mtile_1": 0x01004004}
    w, h, words = tiling3d.observed(state)
    assert (w, h) == (64, 256)
    assert words == {"MTileWidth": 8, "TEMTile1": 0x01004004}


@pytest.mark.skipif(not VAR.is_dir(), reason="no differential captures (gitignored)")
def test_rule_matches_every_capture():
    n, bad = tiling3d.check(VAR)
    assert n > 0 and bad == []
