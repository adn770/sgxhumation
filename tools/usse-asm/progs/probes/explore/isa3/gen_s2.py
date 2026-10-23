"""Exploration s2: smp2d with T0 single-bit flips; flt16/c10 coords; LOD forms; state banks."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def h(x): return struct.unpack('<H', struct.pack('<e', x))[0]
def T(t0=0x00020E00, t1=0xCC00F00F, t3=0xA0800000):
    return f"mov r20, #0x{t0:08X}\nmov r21, #0x{t1:08X}\nmov r22, #0x5CAF1000\nmov r23, #0x{t3:08X}\n"
cases, labels = [], []
def c(lbl, pre, op, uv=(3.0/16, 5.5/16)):
    u, v = uv
    cases.append([pre + f"mov r24, #0x{f(u):08X}\nmov r25, #0x{f(v):08X}\nmov r26, #0x{f(0.0):08X}\n" + op + "\nwdf drc0", 0, 0, 0, 0, 0, 0, 0, 0])
    labels.append(lbl)
for b in list(range(0, 9)) + list(range(12, 17)) + list(range(27, 32)):
    c(f"T0 ^ bit {b}", T(t0=0x00020E00 ^ (1 << b)), "smp2d r10, r24, r20, drc0")
for b in range(0, 32, 1):
    if b in (12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11): continue
    c(f"T1 ^ bit {b}", T(t1=0xCC00F00F ^ (1 << b)), "smp2d r10, r24, r20, drc0")
for b in range(0, 32):
    c(f"T3 ^ bit {b}", T(t3=0xA0800000 ^ (1 << b)), "smp2d r10, r24, r20, drc0")
# coordinate formats: flt16 (two halves in r24), c10?
cases.append([T(0x00020E00) + f"mov r24, #0x{h(5.5/16) << 16 | h(3.5/16):08X}\nsmp2d r10, r24.flt16, r20, drc0\nwdf drc0", 0, 0, 0, 0, 0, 0, 0, 0]); labels.append("flt16 coords (3.5, 5.5)/16 lo/hi")
for lod, extra in [("bias", 0.0), ("replace", 0.0), ("grad", 0.0)]:
    c(f"smp2d{lod} 0", T(), f"smp2d{lod} r10, r24, r20, r26, drc0", uv=(3.5/16, 5.5/16))
P = [HDR, probe("x-smp2", "smp2", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 's2.toml').write_text("\n".join(P))
print(len(cases))
