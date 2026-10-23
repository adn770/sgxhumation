"""Exploration s1: smp with state words in r20-r23 (dump.s)."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
T = "mov r20, #0x00020E00\nmov r21, #0xCC00F00F\nmov r22, #0x5CAF1000\nmov r23, #0xA0800000\n"
cases, labels = [], []
def c(lbl, uv, op):
    u, v = uv
    cases.append([T + f"mov r24, #0x{f(u):08X}\nmov r25, #0x{f(v):08X}\nmov r26, #0x{f(0.0):08X}\n" + op + "\nwdf drc0", 0, 0, 0, 0, 0, 0, 0, 0])
    labels.append(lbl)
for uv in [(0.5/16, 0.5/16), (3.5/16, 5.5/16), (15.5/16, 15.5/16), (3.5, 5.5), (1.0/16, 1.0/16), (17.5/16, 2.5/16)]:
    c(f"smp2d uv={uv}", uv, "smp2d r10, r24, r20, drc0")
c("smp1d", (3.5/16, 5.5/16), "smp1d r10, r24, r20, drc0")
c("smp2d.repeat2", (3.5/16, 5.5/16), "smp2d.repeat2 r10, r24, r20, drc0")
c("smp2d r10.xy", (3.5/16, 5.5/16), "smp2d r10.xy, r24, r20, drc0")
P = [HDR, probe("x-smp", "smp", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 's1.toml').write_text("\n".join(P))
