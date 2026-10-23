"""Exploration: the round 4 single-probe infer/open items (dump.s)."""
import sys, pathlib, struct, subprocess
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
ASM = str(pathlib.Path(__file__).parent.parents[3] / 'usse-asm')
def ok(t): return subprocess.run([ASM, '-o', '/dev/null', '-'], input=t + "\n", text=True,
                                 capture_output=True).returncode == 0
cases, labels, skipped = [], [], []
def c(lbl, op, ins=(0,) * 8):
    ins = list(ins) + [0] * (8 - len(ins))
    if all(ok(l) for l in op.split("\n") if l and not l.startswith(';')):
        cases.append([op] + ins); labels.append(lbl)
    else:
        skipped.append(lbl + ": " + op.replace("\n", "; "))
# br offset base
for k in (1, 2, 3):
    c(f"br #{k}", f"mov r10, #1\nbr #{k}\nmov r10, #2\nmov r10, #3\nmov r10, #4\nmov r10, #5\nmov r11, #6")
c("br.savelink #3 then read pclink", "mov r10, #1\nbr.savelink #3\nmov r10, #2\nmov r10, #3\nmov r10, #4\nmov r11, pclink")
# Pn predicate (value 7): p0..p3 set from r1..r4 (1 = true)
for st in ((0, 0, 0, 0), (1, 1, 1, 1), (1, 0, 0, 0), (0, 0, 0, 1)):
    t = "".join(f"and.testtanz.chan0 !c0, p{i}, r{i + 1}, r{i + 1}\n" for i in range(4))
    c(f"Pn mov with p0-p3 = {st}", t + "Pn mov r10, #7", [1 if s else 0 for s in st])
# bank 7 numbers past i3
for n in (3, 4, 5, 7, 8, 63, 127):
    c(f"mov r10, i{n} after i0-i2 = 1, 2, 3", f"mov i0, r1\nmov i1, r2\nmov i2, r3\nmov r10, i{n}", (1, 2, 3))
c("mov i4, r1 then i0-i2", "mov i0, r2\nmov i1, r2\nmov i2, r2\nmov i4, r1\nmov r10, i0\nmov r11, i1\nmov r12, i2\nmov r13, i4", (0x11, 0x22))
# index high: i.h and i.lh (r20.. by r[ih + #n]); r30-r37 hold 0x30-0x37
SETR = "".join(f"mov r{30 + k}, #0x{0x30 + k:X}\n" for k in range(8))
for k in (0, 1, 3):
    c(f"mov i.h, #{30 + k} (via r1); r10 = r[ih + #0]", SETR + f"mov i.h, r1\nmov r10, r[ih + #0]", (30 + k,))
    c(f"mov i.l, #{30 + k}; mov i.h, #0; r10 = r[il + #0] (control)", SETR + "mov i.l, r1\nmov i.h, r2\nmov r10, r[il + #0]", (30 + k, 0))
c("mov i.lh, r1 = 0x0021001E: r10 = r[il + #0], r11 = r[ih + #0]", SETR + "mov i.lh, r1\nmov r10, r[il + #0]\nmov r11, r[ih + #0]", (0x0021001E,))
c("mov i.h, r1 = 31; r[ih + #2]", SETR + "mov i.h, r1\nmov r10, r[ih + #2]", (31,))
c("mov i.h, r1 = 0x40020; r[ih + #0]", SETR + "mov i.h, r1\nmov r10, r[ih + #0]", (0x40000 | 30,))
# shift immediates >= 32 and ~
for op in ("shl", "shr", "asr", "rol"):
    for n in (31, 32, 33, 40):
        c(f"{op} r10, r1, #{n}", f"{op} r10, r1, #{n}", (0x80000003,))
for op in ("shl", "shr", "rol", "and", "or"):
    c(f"{op} r10, r1, ~r2", f"{op} r10, r1, ~r2", (0x80000003, 0xFFFFFFFC))
    c(f"{op} r10, r1, ~#3", f"{op} r10, r1, ~#3", (0x80000003,))
# test lanes: imul16 / iaddu16 / iadd8 / fmul tests, then p0 -> r11
for op, a, b in (("imul16", 0x00020003, 0x00010000), ("imul16", 0x00000000, 0x00050007),
                 ("imul16", 0x00030000, 0x00000004), ("imulu16", 0x00020003, 0x00010000),
                 ("iaddu16", 0xFFFF0001, 0x0001FFFF), ("iadd16", 0x0001FFFF, 0x00000001),
                 ("isub8", 0x01020304, 0x01020304), ("imul8", 0x00010203, 0x01010101),
                 ("fmul", f(2.0), f(-3.0)), ("fmul", f(0.0), f(-3.0)), ("fsub", f(1.0), f(1.0))):
    for t in ("testtaz.chan0", "testtaz.chan1", "testtanz.chan0", "testtanz.chan1", "testtsp.chan0"):
        c(f"{op}.{t} a=0x{a:08x} b=0x{b:08x}",
          f"mov r11, #0\n{op}.{t} r10, p0, r1, r2\np0 mov r11, #1", (a, b))
# efo with denormals
c("efo m0 = r1 x r3 (denormal x 1.0), a0 = m0 + m1, m1 = r2 x r3 (0)",
  "efo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3",
  (0x00400000, 0, f(1.0)))
c("efo a0 = src0 + src2 (denormal + 0)",
  "efo r10= a0, i0 = a0, i1 = a1, a0=src0+src2 a1=src1+src2, m0=src0*src2, r1, r2, r3",
  (0x00400000, 0, 0))
c("efo m0 = r1 x r3 (normal x 0.5 to a denormal)",
  "efo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3",
  (0x00800000, 0, f(0.5)))
# fdsx/fdsy incs
for n in ("fdsx.repeat2.incs111 r10, r1, r3", "fdsx.repeat2 r10, r1, r3", "fdsy.repeat2.incs011 r10, r1, r3"):
    c(n, n, (f(1.0), f(2.0), f(3.0), f(4.0)))
# c10 on sop3, lrp1, lrp2, sop2wm
for op in ("sop3 r10, r1.c10, r2.c10, r3.c10, zero, zero, add+asop zero.comp, zero, add",
           "sop3 r10, r1, r2, r3, zero, zero, add+asop zero.comp, zero, add",
           "lrp1 r10, r1.c10, r2.c10, r3.c10, zero.comp, zero, s0+asop s1.comp, s0a, add",
           "lrp1 r10, r1, r2, r3, zero.comp, zero, s0+asop s1.comp, s0a, add",
           "sop2wm r10.bytemask1111, r1.c10, r2.c10, zero.comp, zero.comp, add, sub",
           "sop2wm r10.bytemask0101, r1, r2, zero.comp, zero.comp, add, add",
           "sop2wm r10.bytemask1010, r1, r2, zero.comp, zero.comp, add, add"):
    c(op, op, (0x40302010, 0x04030201, 0x80808080))
# idiv
for a, b in ((100, 7), (0xFFFFFFFF, 3), (7, 0), (0x80000000, 2), (1000000, 1000)):
    c(f"idiv {a}/{b} + wdf", f"idiv r10, r1, r2, drc0\nwdf drc0", (a, b))
c("idiv 100/7 without wdf", "idiv r10, r1, r2, drc0", (100, 7))
c("idiv 100/7 into r10, r11 with repeat2", "idiv.repeat2 r10, r1, r2, drc0\nwdf drc0", (100, 7, 50, 3))
# ima16 8-bit formats
for b, cc, fa, fb in (("r2.low", "r3.low", "u8", "u8"), ("r2.high", "r3.high", "u8", "u8"),
                      ("r2.low", "r3.low", "s8", "s8"), ("r2.low", "r3.low", "o8", "o8"),
                      ("r2.low", "r3", "u8", "u16"), ("r2", "r3.low", "u16", "s8"),
                      ("r2.high", "r3.low", "s8", "o8")):
    op = f"ima16.u r10, r1, {b}, {cc}, {fa}, {fb}"
    c(op, op, (0x82FF0381, 0x7F05FE02, 0x00100020))
c("ima16.s r10, r1, r2, r3, u16, u16", "ima16.s r10, r1, r2, r3, u16, u16", (0x82FF0381, 0x7F05FE02, 0x00100020))
c("ima16.u.usat", "ima16.usat r10, r1, r2, r3, u16, u16", (0x82FF0381, 0x7F05FE02, 0x00100020))
c("ima16.u.rs2", "ima16.u.rs2 r10, r1, r2, r3, u16, u16", (0x82FF0381, 0x7F05FE02, 0x00100020))
# ima8
for op in ("ima8 r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a",
           "ima8.sat r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a",
           "ima8 r10, r1, r2, r3, s1, s1, s2, s1a, s1a, s2a",
           "ima8 r10, r1, r2, r3, s0.comp, s1, s2, s0a.comp, s1a, s2a"):
    c(op, op, (0x40302010, 0x04030201, 0x01020304))
# imae saturation, shift, z16/u16, carries
for op in ("imae.u r10, r1.low, r2.low, r3, u32", "imae.u r10, r1.high, r2.low, r3, u32",
           "imae.s r10, r1.low, r2.low, r3, u32", "imae.usat r10, r1.high, r2.high, r3, u32",
           "imae.ssat r10, r1.high, r2.high, r3, u32", "imae.u.rs4 r10, r1.low, r2.low, r3, u32",
           "imae.u r10, r1.low, r2.low, r3.low, u16", "imae.u r10, r1.low, r2.low, r3.high, z16",
           "imae.u r10, r1.high, r2.high, r3, u32, coe, i0\nmov r11, i0",
           "mov i0, r4\nimae.u r10, r1.low, r2.low, r3, u32, cie, i0",
           "mov i1, r4\nimae.u r10, r1.low, r2.low, r3, u32, cie, i1"):
    c(op, op, (0xFFFF0003, 0xFFFF0005, 0xFFFFFFF0, 1))
for op in ("ima32.u r10, r1, r2, r3", "ima32.s r10, r1, r2, r3",
           "mov i0, r4\nima32.u r10, r1, r2, r3, cie, i0", "mov i2, r4\nima32.u r10, r1, r2, r3, cie, i2"):
    c(op, op, (0xFFFFFFF0, 3, 0xFFFFFFFF, 1))
# .tpres, .partial, .syncs, .nosched
for op in ("frcp.tpres r10, r1", "frcp r10, r1", "frsq.tpres r10, r1", "fsqrt.tpres r10, r1",
           "and.partial r10, r1, r2", "or.partial r10, r1, r2", "mov.syncs r10, r1",
           "fmad.syncs r10, r1, r1, r2", "mov.nosched r10, r1"):
    c(op, op, (f(3.0), 0xF0F0F0F0))
# fmad as a move: x * 1.0 + 0
for x in (f(3.0), 0x00400000, 0x80000000, 0x7F800000, 0x7FC00001, 0xFF800000):
    c(f"fmad r10, r1, r8(1.0), r7(0) with 0x{x:08x}", "fmad r10, r1, r2, r3", (x, f(1.0), 0))
    c(f"fmad r10, r1, r2(1.0), r3(-0) with 0x{x:08x}", "fmad r10, r1, r2, r3", (x, f(1.0), 0x80000000))
# smlsi I1 / I3 on a three-source repeat: fmad.repeat2 r20, r10, r12, r14 (r10.. = floats)
for inc in ((1, 1, 1, 1), (1, 0, 1, 1), (1, 1, 1, 0), (1, 2, 1, 1), (1, 1, 1, 2), (1, 1, 0, 1)):
    c(f"smlsi #{inc} then fmad.repeat2 r10, r1, r3, r5",
      f"smlsi #{inc[0]}, #{inc[1]}, #{inc[2]}, #{inc[3]}, incrementmode, incrementmode, incrementmode, incrementmode, #0, #0, #0\nfmad.repeat2 r10, r1, r3, r5",
      (f(1.0), f(2.0), f(3.0), f(5.0), f(7.0), f(11.0), f(13.0), f(17.0)))
P = [HDR, "# Round 4 exploration: single-probe items.\n"]
P.append(probe("x-misc", "misc exploration", "dump.s", dparams(OUT="r10"),
               ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels))
pathlib.Path(sys.argv[1]).write_text("\n".join(P)); print(len(cases), "cases")
print("SKIPPED:\n  " + "\n  ".join(skipped))
