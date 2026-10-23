"""Exploration 2: filter weights (F32 conversion), mip chain LOD, pcf."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
T0, T1, T2, T3 = 0x00020E00, 0xCC00F00F, 0x5CAF1000, 0xA0800000
F32N = (T3 & 0xF87CFFFF) | 4 << 23 | 3 << 16          # F32 conversion, not normalized
cases, labels = [], []
def c(lbl, t0=T0, t1=T1, t3=T3, uv=(3.5, 5.5), op="smp2d r10, r24, r20, drc0", extra=(), wh=16):
    u, v = uv
    s = (f"mov r20, #0x{t0:08X}\nmov r21, #0x{t1:08X}\nmov r22, #0x{T2:08X}\nmov r23, #0x{t3:08X}\n"
         f"mov r24, #0x{f(u / wh):08X}\nmov r25, #0x{f(v / wh):08X}\n")
    for i, x in enumerate(extra):
        s += f"mov r{26 + i}, #0x{x:08X}\n"
    cases.append([s + op + "\nwdf drc0"] + [0] * 8); labels.append(lbl)
# 1: bilinear weights, F32 output: v = 5.5 + k/64 (rows 5 and 6 differ by 16 in B)
for k in range(0, 65, 4):
    c(f"linear F32 v = 5.5 + {k}/64", t0=T0 | 1 << 12, t3=F32N, uv=(3.5, 5.5 + k / 64))
for k in (1, 3, 5, 7):
    c(f"linear F32 u = 3.5 + {k}/16, v = 5.5 + {k}/16", t0=T0 | 1 << 12, t3=F32N,
      uv=(3.5 + k / 16, 5.5 + k / 16))
# 2: mip chain on twiddled 16x16: clamp field bits 20:17, mip filter bit 9, min linear bit 10
TW1 = 0x0C00F00F
for clamp in (12, 0, 1, 2):
    for lod in ((0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0) if clamp == 12 else (3.0,)):
        c(f"mips clamp {clamp} replace {lod}", t0=clamp << 17, t1=TW1, uv=(3.5, 5.5),
          op="smp2dreplace r10, r24, r20, r26, drc0", extra=(f(lod),))
for lod in (0.25, 0.5, 0.75, 1.5):
    c(f"mips mipfilter replace {lod}", t0=12 << 17 | 1 << 9, t1=TW1, uv=(3.5, 5.5),
      op="smp2dreplace r10, r24, r20, r26, drc0", extra=(f(lod),))
for b in (1.0, 2.0):
    c(f"mips bias {b}", t0=12 << 17, t1=TW1, uv=(3.5, 5.5),
      op="smp2dbias r10, r24, r20, r26, drc0", extra=(f(b),))
for g in (2.0, 4.0, 3.0):
    c(f"mips grad {g} texels", t0=12 << 17, t1=TW1, uv=(3.5, 5.5),
      op="smp2dgrad r10, r24, r20, r26, drc0", extra=(f(g / 16), 0, 0, f(g / 16)))
c("mips grad 4 texels in u only", t0=12 << 17, t1=TW1, uv=(3.5, 5.5),
  op="smp2dgrad r10, r24, r20, r26, drc0", extra=(f(4 / 16), 0, 0, 0))
c("mips min linear replace 1.0", t0=12 << 17 | 1 << 10, t1=TW1, uv=(3.0, 5.0),
  op="smp2dreplace r10, r24, r20, r26, drc0", extra=(f(1.0),))
c("mips replace integer 1", t0=12 << 17, t1=TW1, uv=(3.5, 5.5),
  op="smp2dreplace r10, r24, r20, r26, drc0", extra=(1,))
# 3: pcf, F32 texture (stride), reference in r26
for fmt, refs in ((18, (0.5, -1e10)), (9, (0.5,))):
    for ref in refs:
        for mode in range(8):
            c(f"pcf format {fmt} ref {ref} mode {mode}", t1=(T1 & 0xE0FFFFFF) | fmt << 24,
              t3=(T3 & 0x1FFFFFFF) | mode << 29, op="smp2d.pcf r10, r24, r20, drc0",
              extra=(f(ref),))
c("pcff16 format 18 ref 0.5 mode 1", t1=(T1 & 0xE0FFFFFF) | 18 << 24,
  t3=(T3 & 0x1FFFFFFF) | 1 << 29, op="smp2d.pcff16 r10, r24, r20, drc0", extra=(f(0.5),))
P = [HDR, "# Round 4 exploration 2: sampler.\n"]
P.append(probe("x-smp2", "sampler exploration 2", "dump.s", dparams(OUT="r10"),
               ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels))
pathlib.Path(sys.argv[1]).write_text("\n".join(P)); print(len(cases), "cases")
