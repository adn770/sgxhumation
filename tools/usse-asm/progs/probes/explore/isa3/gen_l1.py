"""Exploration l1: rlp (dump.s): r10 result, p0-p3 after into r11-r14 (as 0/1)."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def setp(bits):   # set p0-p3 from bits using and.testtanz on r7(0) / r8(1)
    return "\n".join(f"and.testtanz.chan0 !c0, p{k}, {'r8' if bits >> k & 1 else 'r7'}, {'r8' if bits >> k & 1 else 'r7'}" for k in range(4))
GETP = "\n" + "\n".join(f"mov r{11+k}, #0\np{k} mov r{11+k}, #1" for k in range(4))
cases, labels = [], []
for pb in [0b0000, 0b0001, 0b0010, 0b0100, 0b1000, 0b1111, 0b0101]:
    for pn in [0, 1, 2, 3]:
        for v in [0x00000000, 0x80000001, 0x12345678, 0xFFFFFFFF]:
            cases.append([setp(pb) + f"\nrlp r10, r1, p{pn}" + GETP, v, 0, 0, 0, 0, 0, 0, 1])
            labels.append(f"p={pb:04b} rlp p{pn} r1={v:08x}")
P = [HDR, probe("x-rlp", "rlp", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'l1.toml').write_text("\n".join(P))
