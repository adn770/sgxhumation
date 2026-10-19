#!/usr/bin/env python3
"""Regression-test usse-dis against the SGX540 ground-truth disassembly.

Runs ./usse-dis over each 540 binary and compares our decoded mnemonic to the
oracle's, per instruction. Reports overall accuracy plus a per-opcode breakdown
so we can see exactly which classes still need field work. Exit non-zero if any
instruction we *claim* to decode (valid) disagrees with the oracle — a decoded
line must be correct, never plausibly wrong.
"""
import re, subprocess, sys, pathlib, collections

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
REF  = ROOT / "reference/sgx540-reversing/ukernel"
BINS = {"ukernel": "ukernel", "TA": "TA", "3D": "3D", "SPM": "SPM"}
DIS  = {"ukernel": "ukernel-disasm", "TA": "TA-disasm", "3D": "3D-disasm", "SPM": "spm-disasm"}
LINE = re.compile(r'0x([0-9A-Fa-f]{16})\s+(.*)')

def oracle(name):
    out = []
    for ln in (REF / DIS[name]).read_text(errors="replace").splitlines():
        m = LINE.search(ln)
        if m:
            out.append((int(m.group(1), 16), m.group(2).strip()))
    return out

def ours(name):
    r = subprocess.run([str(HERE / "usse-dis"), str(REF / BINS[name])],
                       capture_output=True, text=True)
    out = []
    for ln in r.stdout.splitlines():
        m = LINE.search(ln)
        if m:
            out.append((int(m.group(1), 16), m.group(2).strip()))  # raw, comment kept
    return out

total = good = decoded = wrong = 0
by_op = collections.Counter(); by_op_ok = collections.Counter()
mismatches = []
for name in BINS:
    o, u = oracle(name), ours(name)
    for (eo, mo), (eu, mu_raw) in zip(o, u):
        assert eo == eu, f"{name}: encoding stream misaligned {eo:x} vs {eu:x}"
        op = mo.split()[0]
        if op in ("p0", "!p0", "p1", "!p1"):
            op = mo.split()[1] if len(mo.split()) > 1 else op
        op = op.split('.')[0]
        total += 1; by_op[op] += 1
        is_ours = not (mu_raw.startswith(".long") or mu_raw == "" or "TODO" in mu_raw)
        mu = mu_raw.split(";")[0].strip()
        if is_ours:
            decoded += 1
            if mu == mo:
                good += 1; by_op_ok[op] += 1
            else:
                wrong += 1
                if len(mismatches) < 15:
                    mismatches.append(f"  0x{eo:016X}  oracle={mo!r}  ours={mu!r}")

print(f"instructions:        {total}")
print(f"attempted (decoded): {decoded}  ({100*decoded//max(total,1)}%)")
print(f"correct:             {good}")
print(f"WRONG (decoded≠oracle): {wrong}")
print("\nper-opcode (correct / total in oracle):")
for op, n in by_op.most_common(20):
    print(f"  {op:10s} {by_op_ok[op]:5d} / {n}")
if mismatches:
    print("\nfirst mismatches (decoded but wrong):")
    print("\n".join(mismatches))
sys.exit(1 if wrong else 0)
