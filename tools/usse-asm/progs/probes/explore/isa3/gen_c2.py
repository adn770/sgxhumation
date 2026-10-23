import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
VALS = [0x00000080, 0x00008000, 0x00800000, 0x80000000, 0x7F7F7F7F, 0x80808080, 0x00010001, 0x01000100]
CH = ["chan0", "chan1", "chan2", "chan3", "andall", "orall"]
P = [HDR]
for op, b in [("iadd8", 0), ("iadd16", 0), ("iadd32", 0)]:
    for t in ["testnat", "testpat", "testtanz", "testnanz"]:
        cases, labels = [], []
        for ch in CH:
            for v in VALS:
                cases.append([f"{op}.{t}.{ch} !c0, p0, r1, r2", v, b, 0]); labels.append(f"{ch} {v:08x}")
        P.append(probe(f"x-chan2-{op}-{t}", "chan", "pred.s", {"P": "p0"}, ["OP", "A", "B", "C"], cases, {}, labels=labels))
(pathlib.Path(__file__).parent / 'c2.toml').write_text("\n".join(P))
