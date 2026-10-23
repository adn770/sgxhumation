"""Exploration q1: dual issue (dump.s; i0-i2 read back into r15-r17)."""
import sys, pathlib, struct, subprocess
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
IN = [f(1.5), f(2.25), f(3.0), f(5.0), f(7.0), f(11.0), f(0.5), f(-2.0)]
SET = "mov i0, r4\nmov i1, r5\nmov i2, r6\n"
GET = "\nmov r15, i0\nmov r16, i1\nmov r17, i2"
prims = ["fmad r10, r1, r2, r3", "fadm r10, r1, r2, r3", "fmsa r10, r1, r2, r3", "fminmax r10, r1, r2, r3",
         "ima32 r10, r1, r2, r3", "mov r10, r1", "fddp r10, i0, r1, r2, r3", "fssq r10, r2, r3"]
secs = ["fmad i0, i0, i1, i2", "fmad16 i0, i0, i1, i2", "fmsa i0, i0, i1, i2", "fmaxmin i0, i0, i1, i2",
        "ima32 i0, i0, i1, i2", "fsubflr i0, i1, i2", "fadd i0, i1, i2", "fsub i0, i1, i2", "fmul i0, i1, i2",
        "frcp i0, i1", "frsq i0, i1", "fexp i0, i1", "flog i0, i1", "fsin i0, i1", "fcos i0, i1", "mov i0, i1"]
ops, labels = [], []
def ok(s):
    r = subprocess.run([str(pathlib.Path(__file__).parent.parents[3] / 'usse-asm'), '-o', '/dev/null', '-'], input=s.encode(), capture_output=True)
    return r.returncode == 0
for pr in prims:
    for se in secs:
        s = f"{pr}+{se}"
        if ok(s): ops.append(SET + s + GET); labels.append(s)
        else: print("no:", s)
extra = ["fmad r10, r1, r2, r3+fadd i1, i0, i2", "fmad r10, r1, r2, r3+fadd i2, i0, i1",
         "fmad r10, r1, r2, r3+fadd i0, r2, i2", "fmad r10, r1, r2, r3+fadd i0, i1, r3",
         "fmad r10, r1, r2, r3+fmad i0, r1, i1, i2", "fmad r10, r1, r2, r3+fmad i0, i0, r2, r3",
         "fmad r10, r1, r2, r3+mov i0, r1", "fmad r10, r1, r2, r3+mov i1, r2", "fmad r10, r1, r2, r3+mov i2, r3",
         "fmad r10, i0, r2, r3+fadd i0, i1, i2", "fmad r10, r1, i1, r3+mov i1, r1",
         "fmad r10, r1, r2, i2+fmul i2, i0, i1", "fmad.repeat2 r10, r1, r2, r3+fadd i0, i1, i2",
         "fmad.skipinv r10, r1, r2, r3+fadd i0, i1, i2", "mov r10, i0+mov i0, r1",
         "fmad r10, r1, r2, r3+fadd i0, i0, i0"]
for s in extra:
    if ok(s): ops.append(SET + s + GET); labels.append(s)
    else: print("no:", s)
cases = [[o] + IN for o in ops]
P = [HDR, probe("x-dual", "dual", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'q1.toml').write_text("\n".join(P))
print(len(cases))
