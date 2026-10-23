"""Exploration g5: gpiext writes from the multipliers (markers), reads through d and the adders."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
P = [HDR]
SET = "or.skipinv r9, r1, #0x5D000000\nmov i0, r3\nmov i1, r4\nmov i2, r5"
cases, labels = [], []
# m0 = src0*src2, m1 = src1*src2 ; src0 = r7 (0) or r6 (k3), src1 = r9 (k4) or r7, src2 = r8 (1.0)
F = "efo{g} r10= {d}, {w0}i0 = a0, {w1}i1 = m1, a0=m0+i0, a1=i1+m1, m0=src0*src2, m1=src1*src2, r7, {s1}, r8"
for g in ["", ".gpiext"]:
    for w1, s1, wl in [("!", "r7", "no write"), ("", "r9", "i1=m1=k4")]:
        for d, dl in [("i0", "d=i0"), ("i1", "d=i1"), ("a0", "a0=0+i0")]:
            op = F.format(g=g, d=d, w0="!", w1=w1, s1=s1)
            for rd in ["", "i0", "i1", "i2", "i3"]:
                if rd and d != "i0": continue
                cases.append([SET, op + (f"\nmov r10, {rd}" if rd else ""), "r10"])
                labels.append(f"efo{g} {wl}: {dl}" + (f"; then read {rd}" if rd else ""))
P.append(probe("x-gpiext-map5", "gpiext map 5", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True))
(pathlib.Path(__file__).parent / 'g5.toml').write_text("\n".join(P))
