"""Exploration: ima32 as a dual-issue secondary; unit-sharing pairs with new values."""
import sys, pathlib, struct
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
SET = "mov i0, r4\nmov i1, r5\nmov i2, r6\n"
GET = "\nmov r15, i0\nmov r16, i1\nmov r17, i2"
INT = [0x00020003, 0x00050007, 0x000B000D, 0x00030005, 0x00070011, 0x00100100, 0, 0]
FL = [f(1.5), f(2.75), f(3.25), f(5.5), f(7.0), f(13.0), 0, 0]
cases, labels = [], []
prims = ["mov r10, r1", "fmad r10, r1, r2, r3", "fadm r10, r1, r2, r3", "fmsa r10, r1, r2, r3",
         "fminmax r10, r1, r2, r3", "fssq r10, r2, r3", "ima32 r10, r1, r2, r3"]
for p in prims:
    for s in ("ima32 i0, i0, i1, i2", "ima32 i0, i2, i1, i0", "ima32 i0, i0, i1, r3", "ima32 i0, r1, i1, i2"):
        for nm, inp in (("int", INT), ("float", FL)):
            cases.append([SET + p + "+" + s + GET] + inp); labels.append(f"{p}+{s} [{nm}]")
conf = ["fmad r10, r1, r2, r3+fmad16 i0, i0, i1, i2", "fmad r10, r1, r2, r3+fmsa i0, i0, i1, i2",
        "fmad r10, r1, r2, r3+fmaxmin i0, i0, i1, i2", "fadm r10, r1, r2, r3+fmad i0, i0, i1, i2",
        "fadm r10, r1, r2, r3+fmul i0, i1, i2", "fminmax r10, r1, r2, r3+fadd i0, i1, i2",
        "fminmax r10, r1, r2, r3+fmad i0, i0, i1, i2", "fssq r10, r2, r3+fmul i0, i1, i2",
        "fddp r10, i0, r1, r2, r3+fadd i0, i1, i2", "fddp r10, i0, r1, r2, r3+fadd i1, i1, i2",
        "fadm r10, r1, r2, r3+fsub i0, i1, i2", "fmsa r10, r1, r2, r3+fmul i0, i1, i2",
        "fmsa r10, r1, r2, r3+fmad i0, i0, i1, i2", "fssq r10, r2, r3+fmad i0, i0, i1, i2",
        "fminmax r10, r1, r2, r3+fsub i0, i1, i2", "fmad r10, r1, r2, r3+fminmax i0, i0, i1, i2",
        "fadm r10, r1, r2, r3+fmsa i0, i0, i1, i2", "fmsa r10, r1, r2, r3+fmsa i0, i0, i1, i2"]
for c in conf:
    cases.append([SET + c + GET] + FL); labels.append(c)
import subprocess
def ok(t): return subprocess.run([str(pathlib.Path(__file__).parent.parents[3] / 'usse-asm'), '-o', '/dev/null', '-'], input=t,
                                 text=True, capture_output=True).returncode == 0
keep = [i for i, c in enumerate(cases) if ok(c[0].split("\n")[3] + "\n")]
print("skipped", [labels[i] for i in range(len(cases)) if i not in keep])
cases = [cases[i] for i in keep]; labels = [labels[i] for i in keep]
P = [HDR, "# Round 4 exploration: dual issue.\n"]
P.append(probe("x-dual", "dual exploration", "dump.s", dparams(OUT="r10"),
               ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels))
pathlib.Path(sys.argv[1]).write_text("\n".join(P)); print(len(cases), "cases")
