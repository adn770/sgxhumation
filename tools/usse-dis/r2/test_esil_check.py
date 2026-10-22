# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for esil_check.py: the survey tally, the token check and the cross-check model.

The last test runs the cross-check against the radare2 fork; it needs R2_PREFIX to point at
an install of the fork (the out-of-tree plugin has no ESIL) and is skipped otherwise."""
import random
import re
import subprocess

import pytest

import esil_check as ec


def test_base_mnemonic_drops_predicate_and_flags():
    assert ec.base_mnemonic("p1 and.testtaz.chan0 !c0, p0, r1, r2") == "and"
    assert ec.base_mnemonic("!p0 ba #0x00000102") == "ba"
    assert ec.base_mnemonic("ldr r1, #7, drc0") == "ldr"
    assert ec.base_mnemonic("Pn and r1, r2, r3") == "and"


def test_tally_counts_esil_per_base_and_skips_invalid():
    ops = [{"opcode": "mov r1, r2", "esil": "r2,r1,="},
           {"opcode": "mov i.l, r4", "esil": ""},
           {"opcode": "invalid"},
           {"opcode": "str #7, r1", "esil": "r1,0x10000001c,=[4]"}]
    st = ec.tally(ops)
    assert st == {"mov": [1, 1], "str": [1, 0]}
    assert ec.report(st).splitlines()[0] == "words 3: esil 2 (66.7%)"


def test_profile_regs_and_bad_tokens():
    arp = "=PC\tpc\ngpr\tr0\t.32\t0\t0\ngpr\tpa3\t.32\t4\t0\n"
    regs = ec.profile_regs(arp)
    assert regs == {"r0", "pa3"}
    assert ec.bad_tokens("pa3,0x100000014,=[4]", regs) == []
    assert ec.bad_tokens("r0,i.l,=", regs) == ["i.l"]


def test_alu_and_test_predicates():
    assert ec.alu("rol", 0x80000001, 4) == 0x18
    assert ec.alu("rol", 5, 0) == 5
    assert ec.alu("asr", 0x80000000, 4) == 0xF8000000
    assert ec.alu("iadd32", ec.M32, 1) == 0
    assert ec.test_pred("nat", 0x80000000) == 1
    assert ec.test_pred("nonz", 0) == 0
    assert ec.test_pred("noz", 0) == 1


def test_adds_take_a_16_bit_src1():
    # [HW] probes iadd32-value, iaddu32-value: sext16/zext16(src1) + src2
    assert ec.alu("iadd32", 0x12345678, 0x11111111) == 0x11116789
    assert ec.alu("iadd32", 0xFFFF, 1) == 0
    assert ec.alu("iaddu32", 0xFFFFFFFF, 2) == 0x10001
    assert ec.alu("iaddu32", 1, 0xFFFFFFFF) == 0


def test_register_shift_amounts():
    # [HW] probes shl-reg, shr-reg, asr-reg, rol-reg
    assert ec.alu("shl", 0x80000001, 32) == 0
    assert ec.alu("shl", 0x80000001, 0x100) == 0
    assert ec.alu("shr", 0x80000001, 0xFFFFFFFF) == 0
    assert ec.alu("asr", 0x80000001, 33) == ec.M32
    assert ec.alu("asr", 0x40000000, 64) == 0
    assert ec.alu("rol", 0x80000001, 33) == 3


def test_test_signs_and_joins():
    # [HW] sign-p-*: p is >= 0; join-t-*: t is true
    assert ec.test_pred("pat", 0) == 1 and ec.test_pred("pat", 0x80000000) == 0
    assert ec.test_pred("panz", 0) == 0 and ec.test_pred("paz", 0) == 1
    assert all(ec.test_pred(t, v) == 1 for t in ("not", "toz", "tonz", "tat", "pot")
               for v in (0, 1, 0x80000000))
    assert ec.test_pred("naz", 0) == 0 and ec.test_pred("tanz", 1) == 1
    # [HW] iaddu32-np: unsigned, n is never and p always true
    assert ec.test_pred("nat", 0x80000000, unsigned=True) == 0
    assert ec.test_pred("pat", 0x80000000, unsigned=True) == 1
    # [HW] isub16-*: chan0 tests the low lane: bit 15, its zero
    assert ec.test_pred("nat", 0x00008000, bits=16) == 1
    assert ec.test_pred("nat", 0x80000000, bits=16) == 0
    assert ec.test_pred("taz", 0x00010000, bits=16) == 1


def test_16_bit_lanes():
    # [HW] isub16-value, isub16-reg, ima16*: both lanes, wrapping
    assert ec.sub16(0x12340005, 0x00010001) == 0x12330004
    assert ec.sub16(0, 0x00010001) == ec.M32
    assert ec.sub16(0x00050007, 0x00010002) == 0x00040005
    assert ec.ima16(0x00020005, 0x00030003, 0x00010007, neg=True) == 0x00050008
    assert ec.ima16(0x0000FFFF, 0x00030003, 0, neg=False) == 0x0000FFFD
    assert ec.ima16(0x00020005, 0x00030004, 0x00010001, neg=False) == 0x00070015


def test_fmad_rounds_the_product_and_flushes_denormals():
    # [HW] fmad-round, fmad-fused, fmad-denormal, fmad-zero-sign, sweep-fmad
    assert ec.fmad(0x3F800800, 0x3F801800, 0) == 0x3F802002
    assert ec.fmad(0x3F800800, 0x3F800800, 0xBF801000) == 0
    assert ec.fmad(0x1C800000, 0x1C800000, 0) == 0
    assert ec.fmad(0x00000200, 0x71800000, 0) == 0
    assert ec.fmad(0x9C800000, 0x1C800000, 0x80000000) == 0x80000000
    assert ec.fmad(0x80000200, 0x3F800000, 0) == 0
    assert ec.flush(0x80000200) == 0x80000000 and ec.flush(0x00800000) == 0x00800000


def test_register_offsets_and_indexes():
    # [HW] ldad-reg-off*: bytes = (o >> 16) * (o & 0xffff), unsigned, 32-bit wrap
    assert ec.regoff(0x00040003) == 12
    assert ec.regoff(0x00010001) == 1
    assert ec.regoff(0xFFFFFFFF) == 0xFFFE0001
    # [HW] index-*: i.l counts registers from the bank's first, bits 10 and up ignored
    assert ec.index_reg("r", 0x40001, 0) == "r1"
    assert ec.index_reg("sa", 0x403, 2) == "sa5"
    assert ec.index_reg("r", 62, 3) is None          # r65: outside the profile


def test_register_file_window():
    assert ec.cr_addr(7) == 0x10000001C
    assert ec.cr_addr(0x3FFF) == 0x10000FFFC


def test_ldr_case_reads_the_window():
    rng = random.Random(0)
    for _ in range(200):
        cs = ec.gen_case(rng)
        if cs and cs.text.startswith("ldr"):
            break
    (addr, val), = cs.mem.items()
    dst = cs.text.split()[1].rstrip(",")
    assert cs.expect == {dst: val}
    assert addr >= ec.CR_BASE and (addr - ec.CR_BASE) % 4 == 0
    assert f"wv4 0x{val:x} @ 0x{addr:x}" in cs.setup()


def test_str_case_expects_the_window_word():
    rng = random.Random(1)
    for _ in range(400):
        cs = ec.gen_case(rng)
        if cs and cs.text.startswith("str"):
            break
    (key, val), = cs.expect.items()
    assert key.startswith("@0x1") and cs.reads() == [f"pv4 {key}"]


def test_mov_repeat_cases_stay_in_the_profile():
    rng = random.Random(2)
    seen = overlap = pred = 0
    for _ in range(3000):
        cs = ec.gen_case(rng)
        if not cs or "mov.repeat" not in cs.text:
            continue
        seen += 1
        pred += cs.text.startswith("p")
        n = int(cs.text.split("mov.repeat")[1].split()[0])
        assert len(cs.expect) == n
        regs = set(cs.expect) | {r for r in cs.regs if not r.startswith("p")}
        assert all(int(r.lstrip("opar")) < (64 if r[0] == "r" else 32) for r in regs)
        overlap += len(set(cs.expect) & set(r for r in cs.regs if not r.startswith("p"))) > 0
    assert seen > 50 and overlap > 5 and pred > 5


def test_sequential_repeat_model():
    # [HW] repeat-order: one iteration after the other
    regs = {"r1": 0x11, "r2": 0x22, "r3": 0x33}
    assert ec.repeat(regs, "r", 2, "r", 1, 2) == {"r2": 0x11, "r3": 0x11}


def test_new_kinds_are_generated():
    rng = random.Random(3)
    kinds = set()
    for _ in range(4000):
        cs = ec.gen_case(rng)
        if cs:
            w = cs.text.split()
            kinds.add((w[1] if w[0] in ec.PRED else w[0]).split(".")[0])
            if "[r" in cs.text and "+r" in cs.text:
                kinds.add("regoff")
            if "[il" in cs.text:
                kinds.add("index")
            if re.search(r" r\d+\.[xyzw]+,", cs.text):
                kinds.add("wmask")
    assert {"isub16", "isubu16", "ima16", "regoff", "index", "shl", "iadd32"} <= kinds
    assert {"fdp", "fddp", "fminmax", "fmaxmin", "movc", "movmsk", "wmask"} <= kinds
    assert any(k.startswith("pck") for k in kinds) and any(k.startswith("unpck") for k in kinds)


def test_minmax_models():
    # [HW] fminmax-*, fmaxmin-*: min(max(a, b), c), max(min(a, c), b); minNum/maxNum
    one, two, three = ec.f2u(1.0), ec.f2u(2.0), ec.f2u(3.0)
    assert ec.fminmax(three, one, two) == two
    assert ec.fminmax(two, three, one) == one
    assert ec.fmaxmin(one, three, two) == three
    assert ec.fmaxmin(three, one, two) == two
    assert ec.minmax(0x7FC00000, one, True) == one          # a NaN loses
    assert ec.minmax(0x80000000, 0, False) == 0x80000000    # -0 below +0
    assert ec.minmax(0x80000000, 0, True) == 0
    assert ec.minmax(0x00000005, 0x80000000, True) == 0     # flushed to +0


def test_dot_product_model():
    # [HW] fdp-count, fdp-rounding: products rounded, then the sums in order
    a = [ec.f2u(x) for x in (1, 2, 4, 8)]
    b = [ec.f2u(x) for x in (1, 16, 256, 4096)]
    assert ec.fdot(a, b) == ec.f2u(33825.0)
    assert ec.fdot([0x3F800800, 0x3F800800], [0x3F800800, 0xBF801000]) == 0xB9801000


def test_movc_model():
    # [HW] movc-*: a select per lane; i10 clears bits 30-31; flt with denormals as zero
    s1, s2 = 0x62222222, 0xB3333333
    assert ec.movc("i32", "tz", 0, s1, s2) == s1
    assert ec.movc("i16", "tnz", 0x0000FF80, s1, s2) == 0xB3332222
    assert ec.movc("i8", "tn", 0x0000FF80, s1, s2) == 0xB3332222
    assert ec.movc("i10", "tn", 0x40000200, s1, s2) == 0x33333222
    assert ec.movc("flt", "tz", 0x00000001, s1, s2) == s1
    assert ec.movc("flt", "tp|z", 0x7FC00000, s1, s2) == s2
    assert ec.movc("flt", "tp|z", 0x80000000, s1, s2) == s1


def test_movmsk_model():
    # [HW] movmsk-*: bit 0 / bit 7 of byte c, or a float or half at least 0.5
    assert ec.movmsk("mask", 0x0000FF00, 1) and not ec.movmsk("mask", 0x0000FF00, 0)
    assert ec.movmsk("u8", 0x80, 0) and not ec.movmsk("u8", 0x7F, 0)
    assert ec.movmsk("flt", 0x3F000000, 0) and not ec.movmsk("flt", 0x3EFFFFFF, 0)
    assert not ec.movmsk("flt", 0x7FC00000, 0)
    assert ec.movmsk("flt16", 0x3C000000, 2) and not ec.movmsk("flt16", 0x3C000000, 0)
    assert not ec.movmsk("flt16", 0x00003C00, 1)


def test_pack_models():
    # [HW] pack-*: lanes, byte masks, saturation, scales, rounding
    one, two = ec.f2u(1.0), ec.f2u(2.0)
    assert ec.pack("u8", one, two, False, "nearest", 0xF, 0) == 0x02010201
    assert ec.pack("u8", one, two, False, "nearest", 0b0110, 0xA0A1A2A3) == 0xA00201A3
    assert ec.pack("u8", one, two, False, "nearest", 0b1011, 0xA0A1A2A3) == 0x02A10201
    assert ec.pack("u16", ec.f2u(258.0), ec.f2u(772.0), False, "nearest", 0b1100, 0xA0A1A2A3) \
        == 0x0102A2A3
    assert ec.pack("u8", ec.f2u(0.5), ec.f2u(-0.5), True, "zero", 0xF, 0) == 0x00800080
    assert ec.pack("s8", ec.f2u(1.5), ec.f2u(-1.5), True, "nearest", 0xF, 0) == 0x817F817F
    assert ec.pack("o8", ec.f2u(0.75), ec.f2u(-0.75), True, "nearest", 0xF, 0) == 0x21DF21DF
    assert ec.pack("s8", ec.f2u(2.5), ec.f2u(-2.5), False, "down", 0xF, 0) == 0xFD02FD02
    assert ec.pack("c10", one, two, False, "nearest", 0xF, 0) == 0x0FF7F8FF
    # unpacks: correctly rounded quotients, -1 clamped; integer conversions
    assert ec.unpack_f32("u8", 0x20, True) == 0x3E008081
    assert ec.unpack_f32("s8", 0x80, True) == ec.f2u(-1.0)
    assert ec.unpack_f32("o8", 0x00, True) == ec.f2u(-1.0)
    assert ec.unpack_f32("s16", 0x8000, True) == ec.f2u(-1.0)
    assert ec.unpack_int("u8", "s8", 0x80) == 0x7F
    assert ec.unpack_int("u16", "s8", 0x80) == 0
    assert ec.unpack_int("s16", "s8", 0x80) == 0xFF80


def _fork_r2():
    try:
        out = subprocess.run([ec.tool("radare2"), "-q", "-a", "usse", "-b", "32", "-c",
                              "wx 0002202000102128; aoj 1", "malloc://8"],
                             capture_output=True, text=True).stdout
    except OSError:
        return False
    return '"esil":"r4,r1,=,r5,r2,="' in out


@pytest.mark.skipif(not _fork_r2(), reason="R2_PREFIX is not an install of the radare2 fork")
def test_cross_check_against_the_fork():
    n, bad = ec.cross(seed=7, count=300)
    assert n == 300
    assert bad == []
