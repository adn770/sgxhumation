# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for diff3d.py — word-by-word differences between two captured 3D frames.

Synthetic state dictionaries and object lists only; the capture test runs only when the
gitignored differential captures are present."""
import pathlib

import pytest

import diff3d

HERE = pathlib.Path(__file__).resolve().parent
VAR = HERE / "extracted" / "gles2_tri" / "var"


class Snap:
    def __init__(self, objs):
        self.objs = [{"index": i, "dva": d, "mapped": n} for i, (d, n) in enumerate(objs)]

    def find(self, dva):
        return next((o for o in self.objs if o["dva"] <= dva < o["dva"] + o["mapped"]), None)


def test_symbol_names_an_address_by_object_and_offset():
    snap = Snap([(0x08001000, 0x1000), (0x08087000, 0x2000)])
    assert diff3d.symbol(0x08087010, snap) == "obj1+0x10"
    assert diff3d.symbol(0x3F800000, snap) is None
    assert diff3d.symbol(0x10, Snap([(0, 0x1000)])) is None       # small values are not
    assert diff3d.symbol(1 << 40, snap) is None                    # USSE instructions


def test_diff_hides_moved_addresses_and_reports_bits():
    a = {"k0.x": 0x08002000, "k0.y": 0x01D00200, "k0.z": 5, "k0.only_a": 1}
    b = {"k0.x": 0x08003000, "k0.y": 0x01D40200, "k0.z": 5, "k0.only_b": 2}
    sa = {0: Snap([(0x08001000, 0x1000), (0x08002000, 0x1000)])}
    sb = {0: Snap([(0x08001000, 0x2000), (0x08003000, 0x1000)])}
    d = diff3d.diff(a, sa, b, sb)
    assert d.moved == ["k0.x"]                    # obj1+0x0 in both: an address shift
    assert [(r.path, r.a, r.b) for r in d.changed] == [("k0.y", 0x01D00200, 0x01D40200)]
    assert d.changed[0].xor == 0x00040000 and d.changed[0].bits == [18]
    assert d.only_a == ["k0.only_a"] and d.only_b == ["k0.only_b"]


def test_diff_decodes_known_words():
    a, b = {"k0.vdm.b2.mte.ISPA": 0x01D00200}, {"k0.vdm.b2.mte.ISPA": 0x01B00200}
    d = diff3d.diff(a, {0: Snap([])}, b, {0: Snap([])},
                    decoders={"ISPA": lambda v: {"f": v >> 20 & 0xF}})
    assert d.changed[0].fields == {"f": (0xD, 0xB)}


def test_format_lists_changes():
    a, b = {"k0.y": 0x10}, {"k0.y": 0x30}
    text = diff3d.format_diff("v", diff3d.diff(a, {0: Snap([])}, b, {0: Snap([])}))
    assert "k0.y" in text and "0x10 -> 0x30" in text and "bits 5" in text


@pytest.mark.skipif(not (VAR / "base2" / "dump3d" / "kick00_in.bin").exists(),
                    reason="differential captures not present")
def test_two_identical_runs_have_the_same_state():
    base, base2 = diff3d.load(VAR / "base"), diff3d.load(VAR / "base2")
    d = diff3d.diff(*base, *base2)
    assert not d.changed and not d.moved and not d.only_a and not d.only_b
