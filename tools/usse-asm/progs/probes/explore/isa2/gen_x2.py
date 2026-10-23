"""Exploration table 2: arsop, colour validation, int dots, fmad16, efo, fpma, fadm/fmsa/fsubflr,
single-source functions."""
import random, struct, sys, math
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
from gen_x1 import xp, eight2, eight3, EDGE, rand8, R, P, PAIRS, TRIPS, f, h
del P[1:]
INS3 = [EDGE, rand8(), rand8()]
S3 = ["zero", "asat", "s1", "s1a", "s0", "s0a", "s2", "s2a"]
def c(x): return x + R.choice(["", ".comp"])
# arsop
ops = []
for _ in range(24):
    x = c(R.choice(["zero", "s0a", "s1a", "s2a"])); y = c(R.choice(S3)); z = c(R.choice(S3))
    o = R.choice(["add", "sub"]); ao = R.choice(["add", "sub"])
    ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, {z}, {y}, {o}+arsop {x}, {ao}")
xp("x-arsop", "arsop", [eight3(o) for o in ops], INS3)
# zeros bias and random sop2
ops = ["sop2 {d}, {a}, {b}, zero.comp, zeros.comp, add+asop2 zero.comp, zeros.comp, add",
       "sop2 {d}, {a}, {b}, zero.comp, zeros.comp, sub+asop2 zero.comp, zeros.comp, sub",
       "sop2 {d}, {a}, {b}, s1, zeros, add+asop2 s1a, zeros, add",
       "sop2 {d}, {a}, {b}, zero.comp, zeros, sub+asop2 zero.comp, zeros, sub"]
C1 = ["zero", "s1", "s2", "s1a", "s2a", "asat", "s2scale"]
C2 = ["zero", "s1", "s2", "s1a", "s2a", "asat", "zeros"]
for _ in range(16):
    ops.append(f"sop2 {{d}}, {{a}}, {{b}}, {R.choice(['', 'comp, '])}{c(R.choice(C1))}, {c(R.choice(C2))}, "
               f"{R.choice(['add', 'sub', 'min', 'max'])}+asop2 {R.choice(['', 'comp, '])}"
               f"{c(R.choice(['zero', 's1a', 's2a', 's2scale']))}, {c(R.choice(['zero', 's1a', 's2a', 'zeros']))}, "
               f"{R.choice(['add', 'sub', 'min', 'max'])}{R.choice(['', ', neg'])}")
xp("x-sop2r", "sop2 random", [eight2(o) for o in ops], INS3)
# c10 readings of the colour ops
ops = ["sop2 {d}, {a}.c10, {b}, zero.comp, zero, add+asop2 zero.comp, zero, add",
       "sop2 {d}, {a}, {b}.c10, zero, zero.comp, add+asop2 zero, zero.comp, add",
       "sop2 {d}.c10, {a}, {b}, zero.comp, zero, add+asop2 zero.comp, zero, add",
       "sop2 {d}.c10, {a}.c10, {b}.c10, zero.comp, zero, add+asop2 zero.comp, zero, add",
       "sop2 {d}.c10, {a}.c10, {b}.c10, zero.comp, zero.comp, add+asop2 zero.comp, zero.comp, add",
       "sop2 {d}.c10, {a}.c10, {b}.c10, s1, zero, add+asop2 s1a, zero, add",
       "sop2 {d}.c10, {a}.c10, {b}.c10, zero.comp, zero.comp, sub+asop2 zero.comp, zero.comp, sub",
       "sop2 {d}.c10, {a}.c10, {b}.c10, s1.comp, zero, add+asop2 s1a.comp, zero, add"]
xp("x-c10", "colour ops on c10", [eight2(o) for o in ops], INS3)

# ---- integer dot products ---------------------------------------------------------------------
ops = []
for op in ["u8dot4", "u8dot4off", "u16dot4", "u16dot4off"]:
    for cx in ["Cx1", "Cx2", "Cx8"]:
        ops.append(f"{op} {{d}}, {cx}, Ax1, {{a}}, {{b}}")
    ops.append(f"{op} {{d}}, Cx1, Ax2, {{a}}, {{b}}")
    ops.append(f"{op} {{d}}, Cx1, Ax8, {{a}}, {{b}}")
    ops.append(f"{op} {{d}}, Cx1, Ax1, {{a}}.comp, {{b}}")
    ops.append(f"{op} {{d}}, Cx1, Ax1, {{a}}, {{b}}.comp")
for op in ["u8dot3", "u8dot3off", "u16dot3", "u16dot3off"]:
    ax = ", Ax1" if op.startswith("u8") else ""
    for cx in ["Cx1", "Cx4"]:
        ops.append(f"{op} {{d}}, {cx}{ax}, {{a}}, {{b}}")
    ops.append(f"{op} {{d}}, Cx1{ax}, {{a}}.comp, {{b}}")
for op in ["u8dot3", "u8dot3off"]:
    for al in ["aadd Ax1, zero.comp, s1a, zero, s2a", "aadd Ax1, zero, s1a, zero.comp, s2a",
               "aadd Ax1, s1a, s1a, zero, s2a", "aadd Ax1, s2a, s1a, s2a, s2a", "aadd Ax2, zero.comp, s1a, zero.comp, s2a",
               "asub Ax1, zero.comp, s1a, zero.comp, s2a", "asub Ax1, zero.comp, s1a.comp, zero.comp, s2a",
               "aadd Ax1, s1a.comp, s1a, zero, s2a"]:
        ops.append(f"{op} {{d}}, Cx1, {{a}}, {{b}}+{al}")
    for al in ["aintrp1 r9, s1a, s2a", "aintrp2 r9, s1a, s2a", "aintrp1 r9, zero, s2a", "aintrp1 r9, s1a, s2a.comp",
               "aintrp2 r9, s2a, s1a", "aintrp1 r9, zero, zero.comp"]:
        ops.append(f"{op} {{d}}, Cx1, {{a}}, {{b}}+{al}")
INSD = [EDGE, [0x01020304, 0x05060708, 0x10203040, 0x80808080, 0xffffffff, 0x00ff00ff, 0xff00ff00, 0x7f7f7f7f],
        rand8(), rand8()]
xp("x-idot", "integer dot products", ["mov r9, #0x40804080\n" + eight2(o) for o in ops], INSD)

# ---- fmad16 -----------------------------------------------------------------------------------
def hh(lo, hi): return h(lo) | h(hi) << 16
F16 = [hh(1.5, 2.0), hh(3.0, -0.5), hh(0.25, 10.0), hh(-2.0, 4.0), hh(7.0, 0.125), hh(1.0, 1.0),
       hh(-1.0, 3.5), hh(5.0, 6.0)]
ops = ["fmad16 {d}, {a}, {b}, {c}", "fmad16 {d}, {a}.swzll, {b}, {c}", "fmad16 {d}, {a}.swzhh, {b}, {c}",
       "fmad16 {d}, {a}, {b}.swzll, {c}", "fmad16 {d}, {a}, {b}.swzhh, {c}", "fmad16 {d}, {a}, {b}, {c}.swzll",
       "fmad16 {d}, {a}, {b}, {c}.swzhh", "fmad16 {d}, {a}.swzf32, {b}, {c}", "fmad16 {d}, {a}, {b}.swzf32, {c}",
       "fmad16 {d}, {a}, {b}, {c}.swzf32", "fmad16 {d}, {a}, {b}.abs, {c}",
       "fmad16 {d}, {a}, {b}, {c}.neg", "fmad16 {d}, {a}.abs, {b}, {c}"]
def rh():
    return h(R.uniform(-4, 4)) | h(R.uniform(-4, 4)) << 16
F32 = [f(1.5), f(-2.25), f(3.0), f(0.1), f(100.0), f(-0.75), f(65504.0), f(1e-5)]
xp("x-fmad16", "fmad16", [eight3(o) for o in ops], [F16, F32, [rh() for _ in range(8)], [rh() for _ in range(8)]])
ops = ["fmad16.repeat2.incs000 r10, r1, r3, r5", "fmad16.repeat2.incs010 r10, r1, r3, r5",
       "fmad16.repeat2.incs111 r10, r1, r3, r5", "fmad16.repeat2.incs100 r10, r1, r3, r5"]
xp("x-fmad16r", "fmad16 repeat", ops, [F16])
SPEC16 = [hh(65504.0, 2.0), hh(6.1e-5, 0.5), 0x7c00fc00, 0x7e000000, 0x00010003, 0x3c003c00, 0x80000000, 0x7bff7bff]
xp("x-fmad16s", "fmad16 specials", [eight3("fmad16 {d}, {a}, {b}, {c}")], [SPEC16])

# ---- efo --------------------------------------------------------------------------------------
EFIN = [f(3.0), f(5.0), f(7.0), f(11.0), f(13.0), 0, 0, 0]
MS = ["m0=src0*src2, m1=src1*src2", "m0=src2*src1, m1=src2*src2", "m0=src1*src0, m1=src2*src2",
      "m0=src1*i0, m1=src2*i1"]
AD = ["a0=m0+m1, a1=i1+i0", "a0=m0+src0, a1=i1+i0", "a0=m0+i0, a1=i1+m1", "a0=src0+src2 a1=src1+src2"]
def adv(a, sub, neg):
    a0, a1 = a.split(" a1=") if " a1=" in a and "," not in a else a.split(", a1=")
    sep = ", " if "," in a else " "
    if sub: a0 = a0.replace("+", "-", 1)
    if neg: a1 = "-" + a1
    return f"{a0}{sep}a1={a1}"
ops = []
for m in MS:
    for a in AD:
        for sub, neg in [(0, 0), (1, 1)]:
            for dsel in ["i0", "i1", "a0", "a1"]:
                ops.append(f"mov i0, r4\nmov i1, r5\nefo r10= {dsel}, i0 = a0, i1 = a1, {adv(a, sub, neg)}, {m}, r1, r2, r3\nmov r16, i0\nmov r17, i1")
for iw in ["i0 = a0, i1 = a1", "i0 = a1, i1 = a0", "i0 = m0, i1 = m1", "i0 = a0, i1 = m1"]:
    for w0, w1 in [("", ""), ("!", ""), ("", "!"), ("!", "!")]:
        x, y = iw.split(", ")
        ops.append(f"mov i0, r4\nmov i1, r5\nefo r10= a0, {w0}{x}, {w1}{y}, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3\nmov r16, i0\nmov r17, i1")
for negs in ["r1.neg, r2, r3", "r1, r2.neg, r3", "r1, r2, r3.neg"]:
    ops.append(f"mov i0, r4\nmov i1, r5\nefo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, {negs}\nmov r16, i0\nmov r17, i1")
ops.append("mov i0, r4\nmov i1, r5\nefo.gpiext r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3\nmov r16, i0\nmov r17, i1")
ops.append("mov i0, r4\nmov i1, r5\nefo.repeat2 r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3\nmov r16, i0\nmov r17, i1")
ops.append("mov i0, r4\nmov i1, r5\nefo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3\n"
           "efo r11= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3\nmov r16, i0\nmov r17, i1")
xp("x-efo", "efo dataflow", ops, [EFIN])
def rf(): return f(R.uniform(1, 2) * R.choice([1, -1]) * 2.0 ** R.randint(-3, 3))
ops = ["mov i0, r4\nmov i1, r5\nefo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3\nmov r16, i0\nmov r17, i1",
       "mov i0, r4\nmov i1, r5\nefo r10= a0, i0 = a0, i1 = a1, a0=m0+i0, a1=i1+m1, m0=src1*src0, m1=src2*src2, r1, r2, r3\nmov r16, i0\nmov r17, i1"]
xp("x-efor", "efo rounding", ops, [[rf() for _ in range(5)] + [0, 0, 0] for _ in range(24)])

# ---- fpma -------------------------------------------------------------------------------------
ops = ["fpma {d}, {a}, {b}, {c}, s0, s1, s2, s0a, s1a, s2a", "fpma {d}, {a}, {b}, {c}, s1, s1, s2, s1a, s1a, s2a",
       "fpma {d}, {a}, {b}, {c}, s0a, s1a, s2a, s0a, s1a, s2a", "fpma {d}, {a}, {b}, {c}, s1a, s1, s2, s0a, s1a, s2a",
       "fpma {d}, {a}, {b}, {c}, s0.comp, s1, s2, s0a.comp, s1a, s2a", "fpma {d}, {a}, {b}, {c}, s0, s1.comp, s2, s0a, s1a.comp, s2a",
       "fpma {d}, {a}, {b}, {c}, s0, s1, s2.comp, s0a, s1a, s2a.comp", "fpma.sat {d}, {a}, {b}, {c}, s0, s1, s2, s0a, s1a, s2a",
       "fpma {d}, {a}.neg, {b}, {c}, s0, s1, s2, s0a, s1a, s2a",
       "fpma {d}.c10, {a}.c10, {b}.c10, {c}.c10, s0, s1, s2, s0a, s1a, s2a",
       "fpma.sat {d}.c10, {a}.c10, {b}.c10, {c}.c10, s0, s1, s2, s0a, s1a, s2a",
       "fpma {d}, {a}, {b}, {c}, s0.comp, s1.comp, s2.comp, s0a.comp, s1a.comp, s2a.comp"]
xp("x-fpma", "fpma", [eight3(o) for o in ops], INS3 + [rand8()])

# ---- fadm / fmsa / fsubflr ----------------------------------------------------------------------
FI = [f(3.0), f(5.0), f(7.0), f(0.5), f(2.75), f(-1.25), f(-3.5), f(10.0)]
ops = []
for op in ["fadm", "fmsa"]:
    ops += [f"{op} {{d}}, {{a}}, {{b}}, {{c}}", f"{op} {{d}}, {{a}}, {{b}}.neg, {{c}}",
            f"{op} {{d}}, {{a}}, {{b}}, {{c}}.neg", f"{op} {{d}}, {{a}}.abs, {{b}}, {{c}}"]
ops += ["fsubflr {d}, {a}, {b}", "fsubflr {d}, {a}.neg, {b}", "fsubflr {d}, {a}, {b}.neg", "fsubflr {d}, {a}.abs, {b}"]
def eight(tmpl):
    return eight3(tmpl) if "{c}" in tmpl else eight2(tmpl)
FR = [[f(R.uniform(-8, 8)) for _ in range(8)] for _ in range(3)]
xp("x-fadm", "fadm fmsa fsubflr", [eight(o) for o in ops], [FI] + FR)

# ---- single-source functions --------------------------------------------------------------------
def fill8(vals):
    return [vals[i:i + 8] for i in range(0, len(vals), 8)]
def one(op):
    return "\n".join(f"{op} r{10 + i}, r{1 + i}" for i in range(8))
LOGIN = [f(x) for x in [1.0, 2.0, 4.0, 0.5, 0.25, 1024.0, 3.0, 10.0, 1.5, 0.1, 1e-10, 1e10, 2.0**127, 2.0**-126,
                        1.0000001, 0.99999994]] + [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, f(-1.0),
                        0x00400000, 0x7f7fffff] + [f(R.uniform(0.001, 1000)) for _ in range(40)] + \
        [f(2.0 ** R.uniform(-126, 127)) for _ in range(40)]
EXPIN = [f(x) for x in [0.0, 1.0, 2.0, -1.0, 0.5, -0.5, 10.0, -10.0, 127.0, 128.0, -126.0, -127.0, -150.0, 0.25,
                        1e-7, -1e-7]] + [0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, f(126.9999), f(-149.0),
                        0x00400000, f(3.14159)] + [f(R.uniform(-10, 10)) for _ in range(40)] + \
        [f(R.uniform(-130, 130)) for _ in range(40)]
TRIG = [f(x) for x in [0.0, 0.25, 0.5, 1.0, 0.125, 1.5707964, 3.1415927, 2.0, -0.25, 0.75, 4.0, 100.0, 1e-5,
                       0.0625, 6.2831855, 1000.0]] + [0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, f(1e6),
                       f(-1.0), 0x00400000, f(0.375)] + [f(R.uniform(-1, 1)) for _ in range(40)] + \
       [f(R.uniform(-8, 8)) for _ in range(40)]
xp("x-flog", "flog sweep", [one("flog")], fill8(LOGIN))
xp("x-fexp", "fexp sweep", [one("fexp")], fill8(EXPIN))
xp("x-fsin", "fsin sweep", [one("fsin")], fill8(TRIG))
xp("x-fcos", "fcos sweep", [one("fcos")], fill8(TRIG))
open(pathlib.Path(__file__).parent / 'x2.toml', 'w').write("\n".join(P))
