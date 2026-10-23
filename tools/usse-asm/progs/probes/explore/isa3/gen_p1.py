import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
GET = "\nmov r14, i0\nmov r15, i1\nmov r16, i2\nmov r17, #0\np0 or r17, r17, #1\np1 or r17, r17, #2\np2 or r17, r17, #4\np3 or r17, r17, #8"
SET = "mov i0, r7\nmov i1, r7\nmov i2, r7\nand.testtanz.chan0 !c0, p0, r8, r8\nand.testtanz.chan0 !c0, p1, r8, r8\nand.testtanz.chan0 !c0, p2, r8, r8\nand.testtanz.chan0 !c0, p3, r8, r8\n"
cases, labels = [], []
for cp in range(8):
    for a, b in [(1.0, 2.0), (-1.0, 2.0), (0.0, 0.0)]:
        cases.append([SET + f"fdpc r10, cp{cp}, r1, r2" + GET, f(a), f(1.0), 0, 0, 0, 0, f(5.0), 1]); labels.append(f"fdpc cp{cp} {a}*1")
        cases.append([SET.replace("r8, r8", "r7z, r7z").replace("r7z", "r9") + f"fdpc r10, cp{cp}, r1, r2" + GET, f(a), f(1.0), 0, 0, 0, 0, f(5.0), 1]); labels.append(f"fdpc cp{cp} {a}*1 preds clear")
P = [HDR, probe("x-fdpc", "fdpc", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'p1.toml').write_text("\n".join(P))
