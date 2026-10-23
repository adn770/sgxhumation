import sys, pathlib, struct, subprocess
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
IN = [f(1.5), f(2.25), f(3.0), f(5.0), f(7.0), f(11.0), f(0.5), f(-2.0)]
SET = "mov i0, r4\nmov i1, r5\nmov i2, r6\n"
GET = "\nmov r15, i0\nmov r16, i1\nmov r17, i2"
ops = ["fmad i0, r1, r2, r3+fadd i1, i0, i2", "fmad i1, r1, r2, r3+fadd i1, i0, i2",
       "fmad.repeat2 r10, r1, r2, r3+fadd i0, i0, i1", "fmad.repeat2 r10, r1, r2, r3+fmul i1, i1, i1",
       "and.testtanz.chan0 !c0, p0, r7, r7\np0 fmad r10, r1, r2, r3+fadd i0, i1, i2",
       "and.testtanz.chan0 !c0, p0, r7, r7\n!p0 fmad r10, r1, r2, r3+fadd i0, i1, i2",
       "fmad r10, r1.neg, r2, r3+fmad i0, r1, r2, r3", "fmad r10, r1, r2.neg, r3+fadd i0, r2, i2",
       "fmad r10, r1, r2, r3.neg+fadd i0, i1, r3", "fmad r10, r1.abs, r2, r3+mov i0, r1",
       "mov i0, r8\nfmad r10, r1, r2, r3+fadd i0, i1, i2", "fmad o0, r1, r2, r3+fadd i0, i1, i2",
       "fmad r10, r1, r2, r3+fmad i0, r1, r2, r3", "fmad i2, i0, i1, i2+fmad i0, i0, i1, i2",
       "fmad r10, r1, r2, r3+frcp i2, r2", "mov r10, r1+ima32 i0, i0, i1, i2",
       "mov r10, r1+ima32 i0, r1, r2, r3", "fmad r10, r1, r2, r3+fmad16 i0, i0, i1, i2",
       "mov r10, r1+fmad16 i0, i0, i1, i2"]
def ok(t):
    return subprocess.run([str(pathlib.Path(__file__).parent.parents[3] / 'usse-asm'),'-o','/dev/null','-'],input=t.split('\n')[-1].encode(),capture_output=True).returncode==0
bad=[o for o in ops if not ok(o)]; print('dropped', bad)
ops=[o for o in ops if o not in bad]
cases = [[SET + o + GET] + IN for o in ops]
P = [HDR, probe("x-dual2", "dual2", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=ops)]
(pathlib.Path(__file__).parent / 'q2.toml').write_text("\n".join(P))
