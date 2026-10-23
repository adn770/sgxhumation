import sys, pathlib, struct, subprocess
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
IN = [f(1.5), f(2.25), f(3.0), f(5.0), f(7.0), f(11.0), 0, 1]
SET = "mov i0, r4\nmov i1, r5\nmov i2, r6\n"
GET = "\nmov r15, i0\nmov r16, i1\nmov r17, i2"
ops, labels = [], []
for p0 in (0, 1):
    for p1 in (0, 1):
        for pr in ["", "p0 ", "p1 ", "!p0 "]:
            t = (f"and.testtanz.chan0 !c0, p0, {'r8' if p0 else 'r7'}, {'r8' if p0 else 'r7'}\n"
                 f"and.testtanz.chan0 !c0, p1, {'r8' if p1 else 'r7'}, {'r8' if p1 else 'r7'}\n"
                 f"{pr}fmad r10, r1, r2, r3+fadd i0, i1, i2")
            ops.append(t); labels.append(f"p0={p0} p1={p1}: {pr}fmad+fadd")
# control: a plain predicated fmad (3-bit field)
for p0 in (0, 1):
    for pr in ["p0 ", "!p0 "]:
        t = f"and.testtanz.chan0 !c0, p0, {'r8' if p0 else 'r7'}, {'r8' if p0 else 'r7'}\n{pr}fmad r10, r1, r2, r3"
        ops.append(t); labels.append(f"control p0={p0}: {pr}fmad")
cases = [[SET + o + GET] + IN for o in ops]
P = [HDR, probe("x-dual3", "dual3", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'q3.toml').write_text("\n".join(P))
