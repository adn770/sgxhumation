# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for state3d.py — the fixed-function state words of a captured 3D frame.

Synthetic words only: PDS instructions and USSE moves are built from the field layouts
(our decoders' own, as tools/usse-dis documents them), and state blocks from the presence
word. The capture tests at the end run only when the gitignored captures are present.
"""
import pathlib
import struct

import pytest

import kick3d
import state3d

HERE = pathlib.Path(__file__).resolve().parent
VAR = HERE / "extracted" / "gles2_tri" / "var"


def movs(dest: int, srcs, op: int = 0, b0: int = 0, b1: int = 0, pred: int = 7) -> int:
    """A PDS movs: dest port in 3:0, four 2-bit source selectors (6:5 last ... 12:11
    first; bit 1 picks ds1[b1*2 + bit 0], else ds0[b0*2 + bit 0]), ds0 pair in 22:18,
    ds1 pair in 17:13, predicate in 26:24, opcode in 31:27."""
    w = op << 27 | pred << 24 | b0 << 18 | b1 << 13 | dest
    for s, sel in enumerate(reversed(srcs)):
        w |= sel << (5 + 2 * s)
    return w


DOUTD, DOUTU, DOUTI = 3, 5, 2
HALT = 0xAF000000


def test_pds_movs_operands():
    # movs doutd, ds0[2], ds0[3], ds0[2], ds0[2]
    w = movs(DOUTD, [0, 1, 0, 0], b0=1)
    assert state3d.pds_movs(w) == ("doutd", [("ds0", 2), ("ds0", 3), ("ds0", 2), ("ds0", 2)])
    # movs doutu, ds0[4], ds0[5], ds1[0], ds1[1]
    w = movs(DOUTU, [0, 1, 2, 3], b0=2, b1=0)
    assert state3d.pds_movs(w) == ("doutu", [("ds0", 4), ("ds0", 5), ("ds1", 0), ("ds1", 1)])
    assert state3d.pds_movs(HALT) is None


def test_data_segment_index_of_bank_registers():
    # rows of four dwords: ds0[2r], ds0[2r+1], ds1[2r], ds1[2r+1]
    assert state3d.data_index("ds0", 0) == 0 and state3d.data_index("ds0", 3) == 5
    assert state3d.data_index("ds1", 0) == 2 and state3d.data_index("ds1", 5) == 11


def test_pds_outputs_resolve_data_words():
    data = [0xEC003000, 0x02C0000B, 0, 0, 0, 0, 0, 0, 0x00010E0A, 0, 0, 0]
    code = [movs(DOUTD, [0, 1, 0, 0]), movs(DOUTU, [0, 1, 2, 0], b0=2), HALT]
    outs = state3d.pds_outputs(data + code, len(data))
    assert outs == [("doutd", [0xEC003000, 0x02C0000B, 0xEC003000, 0xEC003000]),
                    ("doutu", [0x00010E0A, 0, 0, 0x00010E0A])]


def test_doutd_control_word():
    # burst size - 1 in 3:0, lines - 1 in 7:4, attribute offset in 19:8, stride in 29:22
    assert state3d.doutd(0x02C0000B) == {"dwords": 12, "lines": 1, "offset": 0, "stride": 11}
    assert state3d.doutd(0x02000018) == {"dwords": 18, "lines": 2, "offset": 0, "stride": 8}
    assert state3d.doutd(0x403)["offset"] == 4


def test_pixel_pds_pointer():
    # data size in 16-byte rows (31:26), program offset from the PDS exec base >> 4 (23:0)
    assert state3d.pixel_pds_ptr(0x0800041A) == (0xE40041A0, 32)
    assert state3d.pixel_pds_ptr(0x04001800) == (0xE4018000, 16)


def test_usse_limm():
    def limm(reg, v):
        lo = (v & 0x1FFFFF) | reg << 21
        hi = 0xFCA00000 | ((v >> 21) & 0x1F) << 4 | (v >> 26) << 12
        return hi << 32 | lo
    assert state3d.usse_limm(limm(1, 0x003F003F)) == (1, 0x003F003F)
    assert state3d.usse_limm(limm(3, 0x08087000)) == (3, 0x08087000)
    assert state3d.usse_limm(0) is None


def test_emitpix2_sideband():
    # value bits 5:0 at e[27:22], 11:6 at e[40:35], 13:12 at e[55:54]
    sb = 0x2603
    e = 0xFB24000000208180 | (sb & 0x3F) << 22 | ((sb >> 6) & 0x3F) << 35 \
        | ((sb >> 12) & 3) << 54
    assert state3d.emitpix2_sideband(e) == sb


def test_mte_state_block_follows_the_presence_word():
    words = [0x0000D4C3, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB]
    names = [n for n, _ in state3d.decode_mte_state(words)]
    assert names == ["present", "isp0", "isp1", "pds_sec", "pds_size", "pds_pri",
                     "clip0", "clip1", "outsel", "mtectl", "coordsize", "coordfmt"]
    vp = [0x0000DF47] + list(range(1, 19))
    d = dict(state3d.decode_mte_state(vp))
    assert d["isp2"] == 3 and d["vp0"] == 7 and d["vp5"] == 12 and d["wrap"] == 13
    assert d["wclamp"] == 15 and d["coordfmt"] == 18
    assert dict(state3d.decode_mte_state([0x2000, 0x10001])) == {
        "present": 0x2000, "term": 0x10001}


def test_mte_state_block_stops_at_an_unknown_presence_bit():
    d = state3d.decode_mte_state([0x00010001, 5, 6])
    assert d[:2] == [("present", 0x00010001), ("isp0", 5)]
    assert d[2][0] == "rest0"


def test_douta_writes_build_an_attribute_block():
    # douta value, word 1 with the attribute offset in 19:8: the state the kick-1 stream's
    # last-task program hands to its USSE task
    assert state3d.douta_block([(0x2000, 0), (0x10001, 0x100), (0x10001, 0x100)]) == [
        0x2000, 0x10001]


def test_flatten():
    assert state3d.flatten("a", {"x": 1, "l": [2, [3, 4]]}) == {
        "a.x": 1, "a.l[0]": 2, "a.l[1][0]": 3, "a.l[1][1]": 4}


# --- bitfields pinned by the differential captures (variations3d.txt) ----------------

def test_isp0_fields():
    # base triangle: depth test off = compare always + no depth write
    f = state3d.decode_isp0(0x01D00200)
    assert f["dcmp"] == "always" and f["dwrite_off"] == 1 and f["colour_off"] == 0
    assert f["pass"] == 0 and f["objtype"] == "triangle"
    assert state3d.decode_isp0(0x00400200)["dcmp"] == "less"             # depth=less
    assert state3d.decode_isp0(0x00500200)["dwrite_off"] == 1             # depthmask=0
    assert state3d.decode_isp0(0x01F00200)["colour_off"] == 1             # mask=0000
    assert state3d.decode_isp0(0x03D00200)["pass"] == 1                   # blending
    assert state3d.decode_isp0(0x01D08200)["objtype"] == "line"
    assert state3d.decode_isp0(0x01D18200)["objtype"] == "point"
    f = state3d.decode_isp0(0x01D00355)                                    # stencil ref 85
    assert f["sref"] == 85 and f["isp2_present"] == 1


def test_isp1_and_isp2_fields():
    assert state3d.decode_isp1(0x10E0)["cmask"] == "RGB"                 # mask=1110
    assert state3d.decode_isp1(0x1080)["cmask"] == "R"
    f = state3d.decode_isp2(0x0E5DFFFF)                                    # zero,incr,invert
    assert (f["scmp"], f["sfail"], f["zfail"], f["zpass"]) == (
        "always", "zero", "incr", "invert")
    f = state3d.decode_isp2(0x0F37FFFF)
    assert (f["sfail"], f["zfail"], f["zpass"]) == ("decr", "incrwrap", "decrwrap")
    f = state3d.decode_isp2(0x0E020FF0)                                    # masks 0f, f0
    assert f["cmpmask"] == 0x0F and f["wmask"] == 0xF0 and f["zpass"] == "replace"
    assert state3d.decode_isp2(0x0202FFFF)["scmp"] == "less"


def test_tile_rectangles():
    # region clip: 32 x 16 pixel tiles, min in 31:16 (bit 31 of word 0 set), max in 15:0
    assert state3d.decode_clip0(0x80010001) == {"on": 1, "xmin": 1, "xmax": 1}
    assert state3d.decode_clip1(0x00010003) == {"ymin": 1, "ymax": 3}
    assert state3d.decode_term(0x00030001) == {"xmax32": 3, "ymax32": 1}
    assert state3d.decode_te_screen(0x3003) == {"xtiles": 4, "ytiles": 4}
    assert state3d.decode_mte_screen(0x003B0063) == {"width": 100, "height": 60}


def test_mte_control_and_pds_size():
    assert state3d.decode_mtectl(0x01555622)["cull"] == "cw"
    assert state3d.decode_mtectl(0x01555621)["cull"] == "ccw"
    assert state3d.decode_mtectl(0x01555620)["cull"] == "none"
    f = state3d.decode_pds_size(0x160F0001)
    assert f["pri_attr_rows"] == 1 and f["task"] == 3 and f["sec_attr_rows"] == 3


def test_pbe_words():
    assert state3d.decode_pbe1(0x003B0063) == {"xmax": 99, "ymax": 59}
    assert state3d.decode_pbe2(0x00338000) == {"count": 0x80, "stride_px": 104}
    f = state3d.decode_sideband(0x603)
    assert f == {"packmode": 3, "tile_relative": 1, "dither": 1}


def test_decoders_are_registered_by_word_kind():
    assert state3d.word_kind("k0.vdm.b2.mte.isp0") == "isp0"
    for kind in ("isp0", "isp1", "isp2", "pds_size", "clip0", "clip1", "mtectl", "term",
                 "pbe0", "pbe1", "pbe2", "sideband", "te_screen", "mte_screen"):
        assert kind in state3d.DECODERS
    assert state3d.DECODERS["vp1"](0x41000000) == {"float": 8.0}


@pytest.mark.skipif(not (VAR / "base" / "dump3d" / "kick00_in.bin").exists(),
                    reason="differential captures not present")
def test_capture_base_state():
    s = state3d.extract_capture(VAR / "base")
    # kick 0: the clear's MTE state block (VDM block 0) and the triangle's (block 2)
    assert s["k0.vdm.b0.mte.present"] == 0xD4C3
    assert s["k0.vdm.b2.mte.present"] == 0xDF43
    assert s["k0.vdm.b2.mte.vp0"] == 0x42000000                 # 32.0
    assert s["k1.eot.pbe0"] == 0 and s["k1.eot.pbe1"] == 0x003F003F
    assert s["k1.eot.sideband"] == 0x603
    assert s["k1.rd.regs_3d.isp_bg_obj_depth"] == 0x3F800000
    # the end-of-scene stream's last task builds a terminate state block with douta
    assert s["k1.vdm.b1.mte.present"] == 0x2000 and s["k1.vdm.b1.mte.term"] == 0x10001


@pytest.mark.skipif(not (VAR / "varying" / "dump3d" / "kick00_in.bin").exists(),
                    reason="differential captures not present")
def test_capture_pixel_pds_pointers_reach_whole_programs():
    # Every pixel PDS pointer of every capture lands on a PDSPIXEL program whose code,
    # after the data size the pointer gives, ends in a halt.
    n = 0
    for cap in sorted(VAR.iterdir()):
        if not (cap / "dump3d").is_dir():
            continue
        s = state3d.extract_capture(cap)
        for k, v in s.items():
            if k.endswith((".pds_sec", ".pds_pri")):
                prog = f"{k.rsplit('.', 1)[0]}.{k[-3:]}.c"
                code = [s[c] for c in s if c.startswith(prog)]
                n += 1
                assert code and code[-1] == 0xAF000000, (cap.name, k)
    assert n > 100
