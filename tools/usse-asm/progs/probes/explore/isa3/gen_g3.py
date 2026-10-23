"""Exploration g3: gpiext reads and writes in the same instruction."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
P = [HDR]
SET = "mov i0, r3\nmov i1, r4\nmov i2, r5"
cases, labels = [], []
def c(lbl, op, out="r10"):
    cases.append([SET, op, out]); labels.append(lbl)
c("gpiext m1 read (=own i0?) while writing i1=a1=i1+i0", "efo.gpiext r10= a0, !i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8")
c("gpiext d=a1 (i1+i0 sum) while writing i1=a1", "efo.gpiext r10= a1, !i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8")
c("gpiext d=i0 while writing i1=a1", "efo.gpiext r10= i0, !i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8")
c("gpiext m0 read (=own i2?) while writing i0=a0 (src0+src2: r6+0)", "efo.gpiext r10= a1, i0 = a0, !i1 = a1, a0=src0+src2 a1=src1+src2, m0=src0*src2, m1=src1*src2, r6, r7, r7")
c("no gpiext: write i1, then mov read i0 of quad", "efo r10= a1, !i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8\nmov r10, i3")
c("gpiext twice: second m1 read", "efo.gpiext r11= a0, !i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8\nefo.gpiext r10= a0, !i0 = a0, !i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8")
c("gpiext repeat2 d=i0", "efo.gpiext.repeat2 r10= i0, !i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r7, r7, r8")
P.append(probe("x-gpiext-map3", "gpiext map 3", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True))
(pathlib.Path(__file__).parent / 'g3.toml').write_text("\n".join(P))
