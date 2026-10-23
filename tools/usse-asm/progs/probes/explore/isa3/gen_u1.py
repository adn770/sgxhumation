import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
cases, labels = [], []
for base in range(0, 1024, 8):
    op = "\n".join(f"unpcku16c10 r{10+k}, r{1+k}.0, r{1+k}.1, nearest" for k in range(8))
    cases.append([op] + [base + k for k in range(8)]); labels.append(f"u lane0 {base}")
for base in range(0, 1024, 32):
    op = "\n".join(f"unpcku16c10 r{10+k}, r{1+k}.1, r{1+k}.0, nearest" for k in range(8))
    cases.append([op] + [(base + 4 * k) << 10 for k in range(8)]); labels.append(f"u lane1 sel .1 {base}")
for base in range(0, 1024, 32):
    op = "\n".join(f"unpcks16c10 r{10+k}, r{1+k}.0, r{1+k}.1, nearest" for k in range(8))
    cases.append([op] + [(base + 4 * k) | (0x155 << 10) for k in range(8)]); labels.append(f"s lane0 + lane1=0x155 {base}")
P = [HDR, probe("x-c10u", "c10u", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'u1.toml').write_text("\n".join(P))
print(len(cases))
