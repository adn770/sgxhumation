"""Exploration c1: test channels: which lane(s) each channel tests (z test), pred.s."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
VALS = [0x00000000, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0x00FFFFFF, 0x01010101, 0x00FF00FF, 0xFF00FF00,
        0x0000FFFF, 0xFFFF0000]
CH = ["chan0", "chan1", "chan2", "chan3", "andall", "orall", "and02", "or02"]
P = [HDR]
for op, b in [("iadd8", 0), ("iadd16", 0), ("iadd32", 0), ("and", 0xFFFFFFFF), ("or", 0)]:
    cases, labels = [], []
    for ch in CH:
        for v in VALS:
            cases.append([f"{op}.testtaz.{ch} !c0, p0, r1, r2", v, b, 0]); labels.append(f"{ch} {v:08x}")
    P.append(probe(f"x-chan-{op}", "chan", "pred.s", {"P": "p0"}, ["OP", "A", "B", "C"], cases, {}, labels=labels))
(pathlib.Path(__file__).parent / 'c1.toml').write_text("\n".join(P))
