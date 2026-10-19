#!/usr/bin/env python3
"""Regression-test the clean-room usse-dis against the AUTHORITATIVE refdisasm.

refdisasm (PVR_PSP2's own `ref_usse_disasm`, built via refdisasm/build.sh) is ground
truth. We disassemble each binary with both refdisasm and our usse-dis and
compare per instruction. The primary target is our extracted SGX545 microkernel;
the SGX540 binaries are extra ISA coverage.

A line we *claim* to decode must equal refdisasm — WRONG must be 0. A line we
can't decode prints "; TODO" (counted as not-attempted, never as wrong).

Requires the refdisasm binaries (run refdisasm/build.sh first).
"""
import re, subprocess, sys, pathlib, collections

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
RD   = HERE / "refdisasm"
LINE = re.compile(r'0x([0-9A-Fa-f]{16})\s+(.*)')

# (label, refdisasm binary, target blob)
TARGETS = [
    ("545-ukernel", RD/"refdisasm545", ROOT/"harness/ukernel-extract/extracted/sgx545_ukernel_template.bin"),
    ("540-ukernel", RD/"refdisasm540", ROOT/"reference/sgx540-reversing/ukernel/ukernel"),
    ("540-TA",      RD/"refdisasm540", ROOT/"reference/sgx540-reversing/ukernel/TA"),
    ("540-3D",      RD/"refdisasm540", ROOT/"reference/sgx540-reversing/ukernel/3D"),
    ("540-SPM",     RD/"refdisasm540", ROOT/"reference/sgx540-reversing/ukernel/SPM"),
]

def run(binpath, blob):
    r = subprocess.run([str(binpath), str(blob)], capture_output=True, text=True)
    out = []
    for ln in r.stdout.splitlines():
        m = LINE.search(ln)
        if m:
            out.append((int(m.group(1), 16), m.group(2).strip()))
    return out

if not (RD/"refdisasm545").exists():
    print("refdisasm not built — run refdisasm/build.sh first", file=sys.stderr); sys.exit(2)

grand_t = grand_dec = grand_ok = grand_wrong = 0
by_op = collections.Counter(); by_op_ok = collections.Counter()
mism = []
for label, rdbin, blob in TARGETS:
    if not blob.exists(): continue
    truth = run(rdbin, blob)
    ours  = run(HERE/"usse-dis", blob)
    t = dec = ok = wrong = 0
    for (eo, mo), (eu, mu_raw) in zip(truth, ours):
        if eo != eu: break
        t += 1
        op = mo.split()[0]
        if op in ("p0","!p0","p1","!p1","p2","!p2","p3"): op = (mo.split()[1:2] or [op])[0]
        op = op.split('.')[0]
        by_op[op] += 1
        is_ours = not (mu_raw.startswith(".long") or mu_raw == "" or "TODO" in mu_raw)
        mu = mu_raw.split(";")[0].strip()
        if is_ours:
            dec += 1
            if mu == mo: ok += 1; by_op_ok[op] += 1
            else:
                wrong += 1
                if len(mism) < 20: mism.append(f"  [{label}] 0x{eo:016X}  truth={mo!r}  ours={mu!r}")
    print(f"{label:12s} {dec:5d}/{t:<5d} decoded ({100*dec//max(t,1):3d}%)  correct {ok}  WRONG {wrong}")
    grand_t += t; grand_dec += dec; grand_ok += ok; grand_wrong += wrong

print(f"\nTOTAL: {grand_dec}/{grand_t} decoded ({100*grand_dec//max(grand_t,1)}%), "
      f"correct {grand_ok}, WRONG {grand_wrong}")
print("\nper-opcode (correct/total across all targets):")
for op, nn in by_op.most_common(24):
    print(f"  {op:12s} {by_op_ok[op]:6d} / {nn}")
if mism:
    print("\nfirst WRONG (decoded != authoritative refdisasm):")
    print("\n".join(mism))
sys.exit(1 if grand_wrong else 0)
