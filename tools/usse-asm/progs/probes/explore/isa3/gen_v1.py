import sys, pathlib, random, subprocess
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
R = random.Random(3545)
ops = ["adif r10, r1, r2", "adif.repeat2 r10, r1, r2", "adifsum r10, r3.low, r1, r2", "adifsum r10, r3.high, r1, r2",
       "bilin r10, r3, r1, r2, u8, planar, src01, dst01", "bilin r10, r3, r1, r2, u8, interleaved, src01, dst01",
       "bilin r10, r3, r1, r2, u8, planar, src23, dst23", "bilin r10, r3, r1, r2, u8, planar, src01, dst01, rnd",
       "bilin r10, r3, r1, r2, s8, planar, src01, dst01",
       "firv r10, r1, r2, r3, u8", "firv r10, r1, r2, r3, u8, iadd", "firv r10, r1, r2, r3, u8, scale",
       "firh r10, r1, r2, r3, u8, rep, fcs0, #0, #0", "firh r10, r1, r2, r3, u8, rep, fcs1, #0, #0"]
good = []
for o in ops:
    r = subprocess.run([str(pathlib.Path(__file__).parent.parents[3] / 'usse-asm'), '-o', '/dev/null', '-'], input=o.encode(), capture_output=True)
    if r.returncode: print("no:", o)
    else: good.append(o)
INS = [[0x10203040, 0x0F0E0D0C, 0x00800080, 0, 0, 0, 0, 0],
       [R.getrandbits(32) for _ in range(8)], [R.getrandbits(32) for _ in range(8)]]
cases, labels = [], []
for o in good:
    for k, ins in enumerate(INS):
        cases.append([o] + ins); labels.append(f"{o} [{k}]")
P = [HDR, probe("x-video", "video", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'v1.toml').write_text("\n".join(P))
print(len(cases)); print(INS)
