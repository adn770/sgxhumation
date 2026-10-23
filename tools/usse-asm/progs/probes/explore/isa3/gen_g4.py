"""Exploration g4: across tasks/clients: A writes i0-i2 per pixel (tags k3,k4,k2'), B reads without writing."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
P = [HDR]
A = ("or.skipinv r9, r1, #0x5D000000", "mov i0, r6\nmov i1, r9\nmov i2, r3", "r10")
cases, labels = [], []
reads = [("mov read i0", "mov r10, i0"), ("mov read i1", "mov r10, i1"), ("mov read i2", "mov r10, i2"),
         ("mov read i3", "mov r10, i3"),
         ("gpiext m0 (i0->?)", "efo.gpiext r10= a0, !i0 = a0, !i1 = a1, a0=m0+src0, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r8, r8"),
         ("gpiext m1 (i1->?)", "efo.gpiext r10= a0, !i0 = a0, !i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8")]
for lbl, op in reads:
    cases.append([A[0], A[1], "r3"]); labels.append("A: i0=k3 i1=k4 i2=k0")
    cases.append(["", op, "r10"]); labels.append("B (no writes): " + lbl)
P.append(probe("x-gpiext-xtask", "across tasks", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True))
(pathlib.Path(__file__).parent / 'g4.toml').write_text("\n".join(P))
