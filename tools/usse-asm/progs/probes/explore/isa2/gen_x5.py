import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
from gen_x1 import xp, P, EDGE, rand8
del P[1:]
ops = ["mov r9, #0x40112233\n" + "\n".join([
    "u8dot3 r10, Cx1, r1.comp, r2+aadd Ax1, zero.comp, s1a, zero, s2a",
    "u8dot3 r11, Cx1, r1, r2.comp+aadd Ax1, zero, s1a, zero.comp, s2a",
    "u8dot3 r12, Cx1, r1.comp, r2+aintrp1 r9, s1a, zero",
    "u8dot3 r13, Cx1, r1, r2.comp+aintrp1 r9, zero, s2a",
    "u8dot3 r14, Cx1, r1.comp, r2.comp+aadd Ax1, s1a, s1a, zero, s2a",
    "u8dot3off r15, Cx1, r1.comp, r2+aintrp2 r9, s1a, s2a",
    "u8dot3 r16, Cx1, r1.comp, r2+asub Ax1, zero.comp, s1a.comp, zero, s2a",
    "u8dot3 r17, Cx1, r1, r2.comp+aadd Ax1, s2a, s1a, zero, s2a"])]
xp("x-dotcomp", "dot alpha with comp sources", ops, [[0x12345678, 0xc0805020, 0, 0, 0, 0, 0, 0], [0xf0e0d0c0, 0x3080a0b0, 0, 0, 0, 0, 0, 0]])
open(pathlib.Path(__file__).parent / 'x5.toml', 'w').write("\n".join(P))
