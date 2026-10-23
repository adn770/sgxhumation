import sys, pathlib, random
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
R = random.Random(3547)
ops = ["sop2.repeat2 r10, r1, r2, s1, s2, add+asop2 s1a, s2a, add",
       "sop2.repeat4 r10, r1, r2, s1, s2, add+asop2 s1a, s2a, add",
       "fpma.repeat2 r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a",
       "fpma.sat r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a",
       "fpma.sat r10.c10, r1.c10, r2.c10, r3.c10, s0, s1, s2, s0a, s1a, s2a",
       "fpma r10.c10, r1.c10, r2.c10, r3.c10, s0, s1, s2, s0a, s1a, s2a",
       "fpma.sat r10.c10, r1.neg.c10, r2.c10, r3.c10, s0, s1, s2, s0a, s1a, s2a",
       "fpma r10.c10, r1.neg.c10, r2.c10, r3.c10, s0, s1, s2, s0a, s1a, s2a",
       "u8dot3.repeat2 r10, Cx1, Ax1, r1, r2", "u16dot4.repeat2 r10, Cx1, Ax1, r1, r2",
       "u8dot4.repeat3 r10, Cx1, Ax1, r1, r2"]
INS = [[R.getrandbits(32) for _ in range(8)] for _ in range(4)]
cases, labels = [], []
for o in ops:
    for k, ins in enumerate(INS):
        cases.append([o] + ins); labels.append(f"{o} [{k}]")
P = [HDR, probe("x-rep", "rep", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'r1.toml').write_text("\n".join(P))
