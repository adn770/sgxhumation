"""Exploration g2: i3 writes, gpiext single writes, read all of i0-i3."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
P = [HDR]
SET = "or.skipinv r9, r1, #0x5D000000\nmov i0, r3\nmov i1, r4\nmov i2, r5"
cases, labels = [], []
def c(lbl, op, out="r10"):
    for rd in ["i0", "i1", "i2", "i3"]:
        cases.append([SET, op + f"\nmov r10, {rd}", out]); labels.append(f"{lbl}; read {rd}")
c("mov i3, r6", "mov i3, r6")
W = "efo{g} r11= a0, {w0}i0 = a0, {w1}i1 = a1, a0=src0+src2 a1=src1+src2, m0=src0*src2, m1=src1*src2, r6, r9, r7"
for g in ["", ".gpiext"]:
    c(f"efo{g} write i1=a1 only (k4)", W.format(g=g, w0="!", w1=""))
    c(f"efo{g} write i0=a0 only (k3)", W.format(g=g, w0="", w1="!"))
c("efo.gpiext write both", W.format(g=".gpiext", w0="", w1=""))
c("efo.gpiext write i0=m0 i1=m1 (m0=src0*src2=k3*1, m1=src1*src2=k4*1)",
  "mov r12, #0x3F800000\nefo.gpiext r11= a0, i0 = m0, i1 = m1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r6, r9, r12")
P.append(probe("x-gpiext-map2", "gpiext map 2", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True))
(pathlib.Path(__file__).parent / 'g2.toml').write_text("\n".join(P))
