"""Exploration 3: weight precision, address modes, formats on two texels, conversions."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
T0, T1, T2, T3 = 0x00020E00, 0xCC00F00F, 0x5CAF1000, 0xA0800000
T1H32 = 0xCC00F01F                                      # 16 wide, 32 high
F32N = (T3 & 0xF87CFFFF) | 4 << 23 | 3 << 16
cases, labels = [], []
def c(lbl, t0=T0, t1=T1, t3=T3, uv=(3.5, 5.5), op="smp2d r10, r24, r20, drc0", extra=(),
      w=16, h=16):
    u, v = uv
    s = (f"mov r20, #0x{t0:08X}\nmov r21, #0x{t1:08X}\nmov r22, #0x{T2:08X}\nmov r23, #0x{t3:08X}\n"
         f"mov r24, #0x{f(u / w):08X}\nmov r25, #0x{f(v / h):08X}\n")
    for i, x in enumerate(extra):
        s += f"mov r{26 + i}, #0x{x:08X}\n"
    cases.append([s + op + "\nwdf drc0"] + [0] * 8); labels.append(lbl)
for k in (1, 2, 3, 5, 7, 9, 15, 17, 31, 33, 63, 65, 127, 129, 200, 255):
    c(f"linear F32 32-high v = 15.5 + {k}/256 at u 15.5", t0=T0 | 1 << 12, t1=T1H32, t3=F32N,
      uv=(15.5, 15.5 + k / 256), h=32)
for k in (1, 3, 9, 33):
    c(f"linear raw 32-high v = 15.5 + {k}/256 at u 15.5", t0=T0 | 1 << 12, t1=T1H32,
      uv=(15.5, 15.5 + k / 256), h=32)
TW = 0x001E0000; TW1 = 0x0C00F00F
for mode in range(4):
    for u in (33.5, -17.5, 40.5, 31.5, 16.5, -0.5):
        c(f"twiddled mode {mode} u {u}", t0=TW | mode << 6 | mode << 3, t1=TW1, uv=(u, 0.5))
for mode in (4, 5, 6, 7):
    c(f"twiddled mode {mode} u 17.5", t0=TW | mode << 6 | mode << 3, t1=TW1, uv=(17.5, 0.5))
c("twiddled 8x8 at (5.5, 3.5)", t0=TW, t1=0x0C007007, uv=(5.5, 3.5), w=8, h=8)
c("twiddled 16x4 at (5.5, 3.5)", t0=TW, t1=0x0C00F003, uv=(5.5, 3.5), w=16, h=4)
c("twiddled 4x16 at (3.5, 5.5)", t0=TW, t1=0x0C00300F, uv=(3.5, 5.5), w=4, h=16)
c("twiddled S mode bits 2:0 = 2 at u 17.5", t0=TW | 2, t1=TW1, uv=(17.5, 0.5))
for fmt in (0, 1, 7, 8, 9, 10, 11):
    for x in (0, 1, 2, 3):
        c(f"format {fmt} texel ({x}, 5)", t1=(T1 & 0xE0FFFFFF) | fmt << 24, uv=(x + 0.5, 5.5))
for fmt in (2, 3, 4, 5, 6, 14, 20, 30):
    for xy in ((2, 5), (3, 5), (15, 15), (14, 15)):
        c(f"format {fmt} texel {xy}", t1=(T1 & 0xE0FFFFFF) | fmt << 24, uv=(xy[0] + .5, xy[1] + .5))
for fmt in (12, 13, 5, 0, 7, 9, 11, 18):
    for conv in (1, 2, 3):
        c(f"format {fmt} conversion {conv} texel (15, 15)", t1=(T1 & 0xE0FFFFFF) | fmt << 24,
          t3=(T3 & 0xF87CFFFF) | conv << 16, uv=(15.5, 15.5))
    c(f"format {fmt} conversion 3 normalized texel (15, 15)", t1=(T1 & 0xE0FFFFFF) | fmt << 24,
      t3=(T3 & 0xF87CFFFF) | 3 << 16 | 1 << 15, uv=(15.5, 15.5))
c("pcff16 U16 ref 0x3F003C00 mode 4", t1=(T1 & 0xE0FFFFFF) | 9 << 24,
  t3=(T3 & 0x1FFFFFFF) | 4 << 29, op="smp2d.pcff16 r10, r24, r20, drc0", extra=(0x3F003C00,))
c("pcf U16 ref 0x3F003C00 mode 4", t1=(T1 & 0xE0FFFFFF) | 9 << 24,
  t3=(T3 & 0x1FFFFFFF) | 4 << 29, op="smp2d.pcf r10, r24, r20, drc0", extra=(0x3F003C00,))
c("pcf U16 ref 0.5 mode 4 linear between 0x0451 and 0xD000", t0=T0 | 1 << 12,
  t1=(T1 & 0xE0FFFFFF) | 9 << 24, t3=(T3 & 0x1FFFFFFF) | 4 << 29, uv=(3.0, 5.5),
  op="smp2d.pcf r10, r24, r20, drc0", extra=(f(0.5),))
c("pcf B8G8R8A8 ref 0.5 mode 4", t3=(T3 & 0x1FFFFFFF) | 4 << 29,
  op="smp2d.pcf r10, r24, r20, drc0", extra=(f(0.5),))
P = [HDR, "# Round 4 exploration 3: sampler.\n"]
P.append(probe("x-smp3", "sampler exploration 3", "dump.s", dparams(OUT="r10"),
               ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels))
pathlib.Path(sys.argv[1]).write_text("\n".join(P)); print(len(cases), "cases")
