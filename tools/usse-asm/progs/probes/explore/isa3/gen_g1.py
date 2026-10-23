"""Exploration g1: per-pixel i0/i1/i2 and what efo/efo.gpiext read (coords + pixels)."""
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
P = [HDR]
SET = "or.skipinv r9, r1, #0x5D000000\nmov i0, r3\nmov i1, r4\nmov i2, r5"
E = "efo{g} r10= {d}, {w0}i0 = {a}, {w1}i1 = {b}, a0={a0}, a1={a1}, m0={m0}, m1={m1}, {s0}, {s1}, {s2}"
def efo(g, d, a0, a1, m0, m1, s, w="!!", wa="a0", wb="a1"):
    E2 = E.replace("a0={a0}, a1={a1}", "a0={a0} a1={a1}") if a0.startswith("src") else E
    return E2.format(g=g, d=d, w0="!" if w[0] == "!" else "", w1="!" if w[1] == "!" else "",
                    a=wa, b=wb, a0=a0, a1=a1, m0=m0, m1=m1, s0=s[0], s1=s[1], s2=s[2])
cases, labels = [], []
def c(lbl, pre, op, out="r10"):
    cases.append([pre, op, out]); labels.append(lbl)
for r in ["i0", "i1", "i2", "i3"]:
    c(f"mov read {r}", SET, f"mov r10, {r}")
for g in ["", ".gpiext"]:
    c(f"efo{g} d=i0", SET, efo(g, "i0", "m0+m1", "i1+i0", "src0*src2", "src1*src2", ["r7", "r7", "r7"]))
    c(f"efo{g} d=i1", SET, efo(g, "i1", "m0+m1", "i1+i0", "src0*src2", "src1*src2", ["r7", "r7", "r7"]))
    c(f"efo{g} a0=m0+i0 (m0=0)", SET, efo(g, "a0", "m0+i0", "i1+m1", "src0*src2", "src1*src2", ["r7", "r7", "r7"]))
    c(f"efo{g} a1=i1+m1 (m1=0)", SET, efo(g, "a1", "m0+i0", "i1+m1", "src0*src2", "src1*src2", ["r7", "r7", "r7"]))
    c(f"efo{g} m0=src1*i0 (1.0), a0=m0+src0 (0)", SET, efo(g, "a0", "m0+src0", "i1+i0", "src1*i0", "src2*i1", ["r7", "r8", "r8"]))
    c(f"efo{g} m1=src2*i1 (1.0), a0=m0+m1 (m0=src1*src0=0)", SET, efo(g, "a0", "m0+m1", "i1+i0", "src1*i0", "src2*i1", ["r7", "r7", "r8"]))
    # writes: i0 = a0 = src0+src2 = r6 (k=3 marker) + 0, i1 = a1 = src1+src2 = r5? use r6 / r4
    for rd in ["i0", "i1", "i2"]:
        c(f"efo{g} write i0=a0=r6+0, i1=a1=r3'... then mov read {rd}", SET,
          efo(g, "a0", "src0+src2", "src1+src2", "src0*src2", "src1*src2", ["r6", "r9", "r7"], w="  ", wa="a0", wb="a1") + f"\nmov r10, {rd}")
P.append(probe("x-gpiext-map", "gpiext map", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True))
pathlib = __import__('pathlib')
(pathlib.Path(__file__).parent / 'g1.toml').write_text("\n".join(P))
