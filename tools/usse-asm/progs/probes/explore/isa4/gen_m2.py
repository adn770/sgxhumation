"""Exploration 2: bank-7 numbers per pixel (coords), br base, fdsx increments."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
SETI = "mov i0, r3\nmov i1, r4\nmov i2, r5"
cases, labels = [], []
for n in (4, 5, 6, 7, 8, 9, 11, 12, 15, 31, 63, 127):
    cases.append([SETI, f"mov r10, i{n}", "r10"]); labels.append(f"read i{n}")
for n in (4, 5, 6, 7):
    for k in (0, 1, 2):
        cases.append([SETI, f"mov i{n}, r6\nmov r10, i{k}", "r10"]); labels.append(f"write i{n}, read i{k}")
P = [HDR, "# Round 4 exploration: bank 7 per pixel.\n"]
P.append(probe("x-bank7", "bank 7 numbers past i3", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {},
               labels=labels, coords=True, pixels=True))
c2, l2 = [], []
ACC = "mov r10, #0\n{BR}\nor r10, r10, #1\nor r10, r10, #2\nor r10, r10, #4\nor r10, r10, #8\nor r10, r10, #16"
for k in (1, 2, 3, 4):
    c2.append([ACC.replace("{BR}", f"br #{k}")] + [0] * 8); l2.append(f"br #{k} before five ors")
c2.append(["mov r10, #0\nba #22\nor r10, r10, #1\nor r10, r10, #2\nor r10, r10, #4\nor r10, r10, #8\nor r10, r10, #16"] + [0] * 8)
l2.append("ba #22 (absolute; the br is instruction 18)")
for op in ("fdsx.repeat2 r10, r1, r3", "fdsx.repeat2.incs111 r10, r1, r3", "fdsx.repeat2.incs001 r10, r1, r3",
           "fdsx.repeat2.incs010 r10, r1, r3", "fdsx.repeat2.incs100 r10, r1, r3", "fdsy.repeat2.incs000 r10, r1, r3",
           "fdsy.repeat2.incs110 r10, r1, r3"):
    c2.append([op, f(1.0), f(2.0), f(4.0), f(8.0), f(16.0), f(32.0), 0, 0]); l2.append(op)
P.append(probe("x-m2", "br base and fdsx incs", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"),
               c2, {}, labels=l2))
pathlib.Path(sys.argv[1]).write_text("\n".join(P)); print(len(cases) + len(c2), "cases")
