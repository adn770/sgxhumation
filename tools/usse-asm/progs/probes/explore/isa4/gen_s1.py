"""Exploration: sampler filters, address modes, twiddled 2D, formats, conversions, LOD, pcf."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
T0, T1, T2, T3 = 0x00020E00, 0xCC00F00F, 0x5CAF1000, 0xA0800000
cases, labels = [], []
def c(lbl, t0=T0, t1=T1, t3=T3, uv=(3.5, 5.5), op="smp2d r10, r24, r20, drc0", extra=(), wh=16):
    u, v = uv
    s = (f"mov r20, #0x{t0:08X}\nmov r21, #0x{t1:08X}\nmov r22, #0x{T2:08X}\nmov r23, #0x{t3:08X}\n"
         f"mov r24, #0x{f(u / wh):08X}\nmov r25, #0x{f(v / wh):08X}\n")
    for i, x in enumerate(extra):
        s += f"mov r{26 + i}, #0x{x:08X}\n"
    cases.append([s + op + "\nwdf drc0"] + [0] * 8); labels.append(lbl)
# A: mag filter on the stride texture
for m in (1, 2, 3):
    for uv in ((3.0, 5.5), (3.25, 5.5), (3.5, 5.0), (3.75, 5.75), (0.25, 0.5), (15.9, 15.9)):
        c(f"stride magfilter {m} at {uv}", t0=T0 | m << 12, uv=uv)
# B: address modes on the stride texture (U bits 8:6, V 5:3, S 2:0) at u = 17.5 / -1.5
for mode in (1, 2, 3, 4, 5, 6, 7):
    c(f"stride U/V mode {mode} at (17.5, -1.5)", t0=T0 | mode << 6 | mode << 3, uv=(17.5, -1.5))
# C: twiddled 2D (TEXTYPE 0), 16x16, not mipmapped (bits 20:17 = 15), clamp/repeat
TW0 = 0x001E0000
for uv in ((0.5, 0.5), (1.5, 0.5), (0.5, 1.5), (3.5, 5.5), (15.5, 15.5), (7.5, 9.5)):
    c(f"twiddled point at {uv}", t0=TW0, t1=0x0C00F00F, uv=uv)
for mode in (0, 1, 2, 3):
    c(f"twiddled U/V mode {mode} at (17.5, -1.5)", t0=TW0 | mode << 6 | mode << 3,
      t1=0x0C00F00F, uv=(17.5, -1.5))
for mag, mn in ((1, 0), (0, 1), (1, 1)):
    c(f"twiddled mag {mag} min {mn} at (3.0, 5.5)", t0=TW0 | mag << 12 | mn << 10,
      t1=0x0C00F00F, uv=(3.0, 5.5))
# D: formats 0-31 on the stride texture at texel (3, 5), raw
for fmt in range(32):
    c(f"stride format {fmt} at (3.5, 5.5)", t1=(T1 & 0xE0FFFFFF) | fmt << 24)
# E: T3 conversion (bits 17:16) and data size (bits 26:23) with format 12
for conv in (1, 2, 3):
    for size in (1, 2, 4):
        c(f"format 12 conversion {conv} size {size}",
          t3=(T3 & 0xF87CFFFF) | size << 23 | conv << 16)
c("format 12 T3 bit 15 (normalize)", t3=T3 | 1 << 15)
c("format 12 conversion 3 size 4 bit 15", t3=(T3 & 0xF87CFFFF) | 4 << 23 | 3 << 16 | 1 << 15)
# F: LOD on twiddled 16x16 without mipmaps, mag point, min linear, at (3.0, 5.5)
TWL = TW0 | 1 << 10
for lod in (0.0, 1.0, -1.0, 2.0):
    c(f"twiddled mag point min linear: smp2dreplace {lod}", t0=TWL, t1=0x0C00F00F,
      uv=(3.0, 5.5), op="smp2dreplace r10, r24, r20, r26, drc0", extra=(f(lod),))
    c(f"twiddled mag point min linear: smp2dbias {lod}", t0=TWL, t1=0x0C00F00F,
      uv=(3.0, 5.5), op="smp2dbias r10, r24, r20, r26, drc0", extra=(f(lod),))
for g in (0.0, 1 / 16, 2 / 16, 4 / 16):
    c(f"twiddled mag point min linear: smp2dgrad {g * 16} texels", t0=TWL, t1=0x0C00F00F,
      uv=(3.0, 5.5), op="smp2dgrad r10, r24, r20, r26, drc0", extra=(f(g), 0, 0, f(g)))
# G: pcf with each compare mode, reference 0.5 in r26 (and the coordinate's third)
for mode in range(8):
    c(f"smp2d.pcf compare mode {mode}", t3=(T3 & 0x1FFFFFFF) | mode << 29,
      op="mov r26, #0x3F000000\nsmp2d.pcf r10, r24, r20, drc0")
P = [HDR, "# Round 4 exploration: sampler state.\n"]
P.append(probe("x-smp", "sampler exploration", "dump.s", dparams(OUT="r10"),
               ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels))
pathlib.Path(sys.argv[1]).write_text("\n".join(P)); print(len(cases), "cases")
