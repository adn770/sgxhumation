"""Exploration table 3: gpiext, fmad16 corners, aintrp s0, sop2wm min/max, leftovers."""
import random, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
from gen_x1 import xp, eight2, eight3, EDGE, rand8, R, P, PAIRS, TRIPS, f, h
del P[1:]
R.seed(5453)
EFIN = [f(3.0), f(5.0), f(7.0), f(11.0), f(13.0), f(17.0), 0, 0]
E = "efo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3"
ops = [f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E.replace('efo', 'efo.gpiext')}\nmov r15, i2\nmov r16, i0\nmov r17, i1",
       f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E.replace('efo', 'efo.gpiext').replace('m1=src1*src2', 'm1=src1*src2').replace('m0=src0*src2, m1=src1*src2', 'm0=src1*i0, m1=src2*i1')}\nmov r15, i2\nmov r16, i0\nmov r17, i1",
       f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E.replace('efo', 'efo.gpiext')}\n{E.replace('efo', 'efo.gpiext').replace('r10=', 'r11=')}\nmov r15, i2\nmov r16, i0\nmov r17, i1",
       f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E}\nmov r15, i2\nmov r16, i0\nmov r17, i1",
       f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E.replace('efo', 'efo.gpiext').replace('i0 = a0, i1 = a1', '!i0 = a0, !i1 = a1')}\nmov r15, i2\nmov r16, i0\nmov r17, i1",
       f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E.replace('efo', 'efo.gpiext').replace('r10= a0', 'r10= i0')}\nmov r15, i2\nmov r16, i0\nmov r17, i1",
       f"mov i0, r4\nmov i1, r5\nmov i2, r6\n{E.replace('efo', 'efo.gpiext').replace('r10= a0', 'r10= i1')}\nmov r15, i2\nmov r16, i0\nmov r17, i1"]
xp("x-gpiext", "efo gpiext", ops, [EFIN])
# fmad16 corners: one instruction per register slot
def hh(lo, hi): return h(lo) | h(hi) << 16
D = 2.0 ** -24
F16C = [hh(2.0 ** -8, 2.0 ** -10), hh(2.0 ** -8, 2.0 ** -6), hh(0.0, 0.0), 0x00010004, 0x3c003c00, 0x80008000, 0x04000400, hh(-0.0, 1.0)]
ops = ["fmad16 r10, r1, r2, r3", "fmad16 r10, r4, r5, r3", "fmad16 r10, r6, r7, r8", "fmad16 r10, r4, r6, r6"]
xp("x-fmad16c", "fmad16 denormals", ["\n".join([o.replace('r10', f'r{10+i}') for i, o in enumerate(ops)])], [F16C])
F32C = [f(1.0 + 2.0 ** -11), f(1.0 + 3 * 2.0 ** -11), f(1.0 + 2.0 ** -12), f(1.0 + 2.0 ** -11 + 2.0 ** -20),
        f(65520.0), f(65519.0), f(2.0 ** -25), f(2.0 ** -14 * 1.5)]
ops = "\n".join(f"fmad16 r{10+i}, r{1+i}.swzf32, r9, r0" for i in range(8))
xp("x-swzf32", "fmad16 swzf32 conversion", ["mov r9, #0x3c003c00\nmov r0, #0\n" + ops], [F32C, [f(-x) for x in [1.0 + 2.0 ** -11, 1.0 + 3 * 2.0 ** -11, 1.0 + 2.0 ** -12, 1.0 + 2.0 ** -11 + 2.0 ** -20, 65520.0, 65519.0, 2.0 ** -25, 2.0 ** -14 * 1.5]]])
def rh(): return h(R.uniform(-8, 8)) | h(R.uniform(-8, 8)) << 16
xp("x-fmad16r2", "fmad16 random", [eight3("fmad16 {d}, {a}, {b}, {c}")], [[rh() for _ in range(8)] for _ in range(24)])
# aintrp s0 byte, varied
ops = []
for al in ["aintrp1 r9, s1a, s2a", "aintrp2 r9, s1a, s2a"]:
    ops.append("mov r9, #0x40112233\n" + eight2(f"u8dot3 {{d}}, Cx1, {{a}}, {{b}}+{al}"))
    ops.append("mov r9, #0xc0804020\n" + eight2(f"u8dot3 {{d}}, Cx1, {{a}}, {{b}}+{al}"))
xp("x-aintrp", "aintrp s0", ops, [EDGE, rand8()])
# sop2wm min/max with factors, random
W = ["zero", "asat", "s1", "s1a", "s2", "s2a"]
ops = []
for _ in range(16):
    x = R.choice(W) + R.choice(["", ".comp"]); y = R.choice(W) + R.choice(["", ".comp"])
    ops.append(f"sop2wm {{d}}.bytemask1111, {{a}}, {{b}}, {x}, {y}, {R.choice(['add', 'sub', 'min', 'max'])}, {R.choice(['add', 'sub', 'min', 'max'])}")
xp("x-sop2wmr", "sop2wm random", [eight2(o) for o in ops], [EDGE, rand8()])
# movmsk.smsk
S0V = [0, 1, 2, 3, 0x80, 0xff, 0x100, 0x8000, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff, 0x3f800000, 0xbf800000, 0x3f000000, 0x00000004]
ops = []
for fmt in ["mask", "u8", "flt", "flt16"]:
    mn = f"movmsk.{fmt}.smsk" if fmt in ("mask",) else (f"movmsk.smsk.{fmt}" if fmt == "u8" else f"movmsk.{fmt}.smsk")
    for c in ["r3.0", "r4.0"]:
        ops.append("\n".join(f"{mn} r{10+i}, r{1+i}, r2, {c}" if False else f"{mn} r{10+i}, r1, r2, {c}" for i in range(1)))
xp_ops = []
for fmt, mn in [("mask", "movmsk.mask.smsk"), ("u8", "movmsk.smsk.u8"), ("flt", "movmsk.flt.smsk"), ("flt16", "movmsk.flt16.smsk")]:
    for s2 in ["r9", "r0"]:
        xp_ops.append(f"mov r9, #0xffffffff\nmov r0, #0\nmov r18, #0x11223344\n" + "\n".join(f"{mn} r{10+i}, r{1+i}, r18, {s2}{'' if fmt == 'flt' else '.1'}" for i in range(8)))
xp("x-smsk", "movmsk smsk s0", xp_ops, [S0V[:8], S0V[8:]])
# statomic
ops = ["statomic r10, r1, r2, zero", "statomic r10, r1.1, r2.2, up", "statomic.scale r10, r1, r2, zero",
       "statomic r10, r3, r4, nearest", "statomic r10, r1.3, r2.3, up", "statomic r10, r5, r6, zero"]
xp("x-statomic", "statomic", [o + "\nmov r11, r10" for o in ops], [EDGE, rand8(), [f(1.0), f(0.5), f(-2.0), f(100.0), 0, 0, 0, 0]])
# c10 to 16-bit unpacks
C10 = [0x000000ff, 0x0003fc00, 0x3fc00000, 0x00000100, 0x000001fe, 0x00000200, 0x000003ff, 0x00000080]
ops = ["\n".join(f"{op} r{10+i}, r{1+i}.{l}, r{1+i}.{l + 1}, nearest" for i in range(8)) for op in ["unpcku16c10", "unpcks16c10", "unpcku16c10.scale", "unpcks16c10.scale"] for l in (0, 1)]
xp("x-unpc10", "c10 to 16-bit unpacks", ops, [C10, [R.getrandbits(30) for _ in range(8)]])
# all-NaN min/max
NANS = [0x7fc00000, 0x7fa00001, 0xffc00000, 0x7fffffff, 0xff800001, 0x7f800001, 0, 0]
ops = ["fminmax r10, r1, r2, r3\nfmaxmin r11, r1, r2, r3\nfminmax r12, r2, r1, r4\nfmaxmin r13, r4, r5, r6\nfminmax r14, r5, r5, r5\nfmaxmin r15, r6, r6, r6\nfminmax r16, r4, r4, r4\nfmaxmin r17, r2, r2, r2"]
xp("x-allnan", "min/max of NaNs", ops, [NANS])
# fadm/fmsa/fsubflr specials
SP = [0x7f800000, 0, 0x00400000, f(1.5), 0xff800000, 0x7fc00000, f(-2.5), f(3e38)]
ops = [eight3("fadm {d}, {a}, {b}, {c}"), eight3("fmsa {d}, {a}, {b}, {c}"), eight2("fsubflr {d}, {a}, {b}")]
xp("x-fspec", "fadm fmsa fsubflr specials", ops, [SP, [f(-x) if x < 0x80000000 else x for x in SP]])
open(pathlib.Path(__file__).parent / 'x3.toml', 'w').write("\n".join(P))
