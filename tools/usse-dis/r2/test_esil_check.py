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
        if not cs or "mov.repeat" not in cs.text or "+" in cs.text:
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


def test_round2_kinds_are_generated():
    rng = random.Random(5)
    kinds = set()
    for _ in range(3000):
        cs = ec.gen_case(rng)
        if cs:
            w = cs.text.split()
            kinds.add((w[1] if w[0] in ec.PRED else w[0]).split(".")[0])
    assert {"sop2", "sop3", "lrp1", "lrp2", "sop2wm", "fpma", "efo", "fadm", "fmsa",
            "fsubflr", "statomic"} <= kinds
    assert {"u8dot3", "u8dot4", "u16dot3", "u16dot4", "u8dot4off", "u16dot3off"} <= kinds


def test_colour_models():
    # [HW] sop2-*, sop3-*, sop2wm: values read back on the laptop (round 2 exploration)
    assert ec.colour("sop2 r10, r1, r2, s1, s2, add+asop2 s1a, s2a, add",
                     0, 0x40C02080, 0x12345678) == 0x119B2179
    assert ec.colour("sop2 r10, r1, r2, comp, s1, s2, add+asop2 comp, s1a, s2a, add",
                     0, 0x40C02080, 0x12345678) == 0x313A3978
    assert ec.colour("sop2 r10, r1, r2, s2scale.comp, asat.comp, min+asop2 comp, zero.comp, "
                     "s1a, sub", 0, 0x80808080, 0x7F7F7F7F) == 0x3FFEFEFE
    assert ec.colour("sop2 r10, r1, r2, zero, zeros.comp, add+asop2 zero, zeros.comp, add",
                     0, 0x7F7F7F7F, 0x01FF00FE) == 0x007F007E
    assert ec.colour("sop3 r10, r1, r2, r3, s2a, s1.comp, sub, neg+alrp zero.comp, s2a.comp",
                     0x1940FAB5, 0xEB605F13, 0xE907F2D5) == 0xE800A2B1
    assert ec.colour("lrp1 r10, r1, r2, r3, s1, s2, s0+asop zero, zero, add",
                     0x80808080, 0x7F7F7F7F, 0x01FF00FE) == 0x00BF3FBF
    assert ec.colour("lrp2 r10, r1, r2, r3, s2a, s1a, s1, s2.comp+alrp zero, zero",
                     0xFFFFFFFF, 0x80808080, 0x7F7F7F7F) == 0x00808080
    assert ec.colour("sop3 r10, r1, r2, r3, zero.comp, zero.comp, sub+arsop zero.comp, sub",
                     0x7F7F7F7F, 0x01FF00FE, 0xFF00FF01) == 0xFEFF00FD
    assert ec.colour("sop2wm r10.bytemask0110, r1, r2, zero.comp, zero, add, add",
                     0, 0x12345678, 0, old=0xA0A1A2A3) == 0xA03456A3


def test_dot_models():
    # [HW] intdot-*: sums of byte products, scaled, rounded, saturated
    assert ec.idot("u8dot4 r10, Cx1, Ax1, r1, r2", 0x01FF00FE, 0xFF00FF01) == 0x02020202
    assert ec.idot("u8dot4 r10, Cx2, Ax1, r1, r2", 0x01020304, 0x05060708) == 0x00010101
    assert ec.idot("u8dot4off r10, Cx1, Ax1, r1, r2", 0x40C02080, 0x12345678) == 0x18181818
    assert ec.idot("u16dot4 r10, Cx1, Ax1, r1, r2", 0x7F7F7F7F, 0x01FF00FE) == 0x7F7F7F7F
    assert ec.idot("u16dot4off r10, Cx1, Ax1, r1, r2", 0, 0xFFFFFFFF) == 0x80028002
    assert ec.idot("u8dot3 r10, Cx1, r1, r2+aintrp2 r9, s1a, s2a", 0xFFFFFFFF, 0x80808080,
                   s0=0x40804080) == 0x20FFFFFF
    assert ec.idot("u8dot3 r10, Cx1, r1, r2+asub Ax1, zero.comp, s1a.comp, zero.comp, s2a",
                   0x12345678, 0, s0=0) == 0xED000000


def test_fpma_and_float_models():
    # [HW] fpma, fadm-fmsa-fsubflr, efo-dataflow (d = i0 is the value before the write)
    assert ec.fpma("fpma r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a",
                   0x80808080, 0x7F7F7F7F, 0x01FF00FE) == 0x80FF80FF
    f = ec.f2u
    assert ec.fmisc("fadm", f(3.0), f(5.0), f(7.0)) == f(56.0)
    assert ec.fmisc("fmsa", f(3.0), f(5.0), f(7.0)) == f(44.0)
    assert ec.fmisc("fsubflr", f(0.5), f(2.75)) == f(-1.5)
    assert ec.fmisc("fsubflr", f(-1.25), f(-3.5)) == f(2.75)
    regs = {"r1": f(3.0), "r2": f(5.0), "r3": f(7.0), "i0": f(11.0), "i1": f(13.0),
            "i2": f(17.0)}
    out = ec.efo("efo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, "
                 "m1=src1*src2, r1, r2, r3", regs)
    assert out == {"r10": f(56.0), "i0": f(56.0), "i1": f(24.0)}
    out = ec.efo("efo r10= i0, i0 = m0, !i1 = m1, a0=m0+m1, a1=i1+i0, m0=src1*i0, "
                 "m1=src2*i1, r1, r2, r3", regs)
    assert out == {"r10": f(11.0), "i0": f(55.0)}


def test_statomic_and_smsk_models():
    # [HW] statomic, movmsk-smsk2
    assert ec.statomic(0x80, 0x7F) == 0x7F007F00
    assert ec.statomic(0x03, 0xF9) == 0x00030003
    assert ec.smsk("u8", 1, 0x00008000, 1) and not ec.smsk("u8", 2, 0x00008000, 1)
    assert ec.smsk("mask", 2, 0x00000100, 1)


def test_round3_kinds_are_generated():
    rng = random.Random(9)
    kinds = set()
    for _ in range(4000):
        cs = ec.gen_case(rng)
        if cs:
            w = cs.text.split()
            m = (w[1] if w[0] in ec.PRED else w[0])
            kinds.add(m.split(".")[0] + ("+" if "+" in cs.text and m.startswith("f") or
                                         m.startswith(("mov", "ima32")) and "+" in cs.text else ""))
            if ".test" in m:
                kinds.add(m.split(".")[-1])
    assert {"rlp", "adif", "adifsum", "unpcku16c10", "unpcks16c10"} <= kinds
    assert {"chan1", "chan2", "chan3", "andall", "orall", "and02", "or02"} <= kinds
    assert {"fmad+", "mov+", "fminmax+", "fssq+"} <= kinds


def test_round3_models():
    # [HW] rlp, rlp-high-predicates
    assert ec.rlp(0x80000001, 1) == (0x00000003, 1)
    assert ec.rlp(0x12345678, 1) == (0x2468ACF1, 0)
    # [HW] test-channels: lanes, absent channels positive non-zero
    assert ec.test_chan("taz", "chan1", "and", 0) == 0
    assert ec.test_chan("tanz", "chan3", "and", 0) == 1
    assert ec.test_chan("taz", "orall", "and", 0) == 1
    assert ec.test_chan("pat", "andall", "and", 0) == 1
    assert ec.test_chan("taz", "chan2", "isub16", 0x00000005) == 1
    assert ec.test_chan("nat", "or02", "isub16", 0x80000001) == 1
    assert ec.test_chan("taz", "and02", "isub16", 0x00000005) == 0
    # [HW] video-adif
    assert ec.adif(0x10203040, 0x0F0E0D0C) == 0x01122334
    assert ec.adifsum(0x10203040, 0x0F0E0D0C, 0x00800080, False) == 0xEA
    # [HW] unpack-c10-16
    assert ec.c10u16(0x0FF) == 0xFFFE and ec.c10u16(0x100) == 1 and ec.c10u16(0x080) == 0
    # [HW] dual-issue: both read before either writes, the secondary wins
    f = ec.f2u
    R = {"r1": f(1.5), "r2": f(2.25), "r3": f(3.0), "i0": f(5.0), "i1": f(7.0), "i2": f(11.0)}
    assert ec.dual("fmad r10, r1, r2, r3+fadd i0, i1, i2", R) == {"r10": f(6.375), "i0": f(18.0)}
    assert ec.dual("mov r10, i0+mov i0, r1", R) == {"r10": f(5.0), "i0": f(1.5)}
    assert ec.dual("fmad i1, r1, r2, r3+fadd i1, i0, i2", R) == {"i1": f(16.0)}
    assert ec.dual("fssq r10, r2, r3+fsub i2, i1, i2", R) == {"r10": f(14.0625), "i2": f(-4.0)}
    assert ec.dual("fmad.repeat2 r10, r1, r2, r3+fadd i0, i0, i1", {**R, "r4": f(5.0)}) == \
        {"r10": f(6.375), "r11": f(11.75), "i0": f(19.0)}


def test_round4_kinds_are_generated():
    rng = random.Random(11)
    kinds = set()
    for _ in range(6000):
        cs = ec.gen_case(rng)
        if cs:
            w = cs.text.split()
            m = (w[1] if w[0] in ec.PRED else w[0])
            kinds.add(m.split(".")[0] + ("+" + cs.text.split("+")[1].split()[0]
                                         if "+" in cs.text else ""))
            if w[0] == "Pn":
                kinds.add("Pn")
    assert {"idiv", "ima8", "imae", "ima16", "Pn"} <= kinds
    assert {"mov+ima32", "fmad+ima32", "fadm+fmul", "fssq+fmad", "fmad+fmsa", "fminmax+fadd",
            "ima32+ima32"} <= kinds


def test_round4_models():
    # [HW] shift-immediates
    assert ec.alu("shl", 0x80000003, ec.shift_amount("#33")) == 0
    assert ec.alu("rol", 0x80000003, ec.shift_amount("#33")) == 7
    assert ec.alu("rol", 0x80000003, ec.shift_amount("~#3")) == 0x38000000
    # [HW] idiv
    assert ec.idiv(100, 7) == (14, 2) and ec.idiv(7, 0) == (ec.M32, ec.M32)
    # [HW] imae-modes
    assert ec.imae("usat", 0xFFFF, 0xFFFF, 0xFFFFFFF0) == ec.M32
    assert ec.imae("ssat", -1, -1, -16) == 0xFFFFFFF1
    assert ec.imae("u", 3, 5, 0xFFFFFFF0, rs=4) == 0x0FFFFFFF
    assert ec.imae("u", 0xFFFF, 0xFFFF, 0xFFFFFFF0, coe=True) == (0xFFFDFFF1, 1)
    assert ec.imae("u", 3, 5, 0xFFFFFFF0, cin=1) == 0
    # [HW] ima16-8bit
    a, b, c = 0x82FF0381, 0x7F05FE02, 0x00100020
    assert ec.ima16x(a, b, c, "low", "low", "u8", "u8") == 0x8F0B0722
    assert ec.ima16x(a, b, c, "high", "high", "u8", "u8") == 0xFC8179FE
    assert ec.ima16x(a, b, c, "low", "low", "o8", "o8") == 0x0F0B4622
    assert ec.ima16x(a, b, c, "high", "low", "s8", "o8") == 0xFC11F89E
    assert ec.ima16x(a, b, c, None, "low", "u16", "s8") == 0x100B0522
    # [HW] ima8
    sel = ["s0", "s1", "s2"]
    assert ec.ima8(0x40302010, 0x04030201, 0x01020304, sel, sel, False) == 0x44362614
    assert ec.ima8(0x40302010, 0x04030201, 0x01020304, ["s0.comp", "s1", "s2"],
                   ["s0.comp", "s1", "s2"], False) == 0xC3D5E5F3
    assert ec.ima8(0xFF, 0x10, 0x10, sel, sel, True) == 0xFF
    # [HW] dual-issue-ima32, dual-issue-shared
    f = ec.f2u
    R = {"r1": 0x00040009, "r2": 0x00060003, "r3": 0x000A000B, "i0": 0x00050002,
         "i1": 0x0009000D, "i2": 0x00200300}
    assert ec.dual("mov r10, r1+ima32 i0, i0, i1, i2", R)["i0"] == 0x004D031A
    R = {"r1": f(1.25), "r2": f(3.5), "r3": f(2.5), "i0": f(4.5), "i1": f(6.0), "i2": f(9.0)}
    assert ec.dual("fadm r10, r1, r2, r3+fmul i0, i1, i2", R)["i0"] == f(8.75)
    assert ec.dual("fssq r11, r2, r3+fmad i0, i0, i1, i2", R)["i0"] == f(15.25)
    assert ec.dual("fmad r10, r1, r2, r3+fmsa i0, i0, i1, i2", R)["i0"] == f(6.875)
    assert ec.dual("fminmax r11, r1, r2, r3+fadd i0, i1, i2", R)["i0"] == f(1.0)


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
