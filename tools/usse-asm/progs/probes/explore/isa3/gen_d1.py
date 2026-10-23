"""Exploration d1: fdsx/fdsy on per-pixel values (coords)."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
P = [HDR]
# r11 = (1 + p/2048)^2 rounded (fmad r11, r3', r3', 0 with r3' = 1 + p/2048 = 0x3F800000 | p<<12)
PRE = ("or.skipinv r20, r1, #0x3F800000\nfmad r11, r20, r20, r7\n"
       "or.skipinv r21, r1, #0x40000000\nfmad r12, r21, r21, r7\nfmad r12, r12, r21, r7")
cases, labels = [], []
for op in ["fdsx r10, r11, r12", "fdsy r10, r11, r12", "fdsx r10, r12, r11", "fdsy r10, r12, r11",
           "fdsx r10, r11, r7", "fdsx r10, r7, r11", "fdsy r10, r11, r7", "fdsy r10, r7, r11",
           "fdsx r10, r11, r11", "fdsx r10, r11.neg, r12", "fdsx r10, r11, r12.neg",
           "fdsx r10, r11.abs, r12"]:
    cases.append([PRE, op, "r10"]); labels.append(op)
cases.append([PRE, "", "r11"]); labels.append("r11")
cases.append([PRE, "", "r12"]); labels.append("r12")
P.append(probe("x-fds", "fds", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True))
(pathlib.Path(__file__).parent / 'd1.toml').write_text("\n".join(P))
