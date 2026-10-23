#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Regression-test the clean-room PDS decoder (pds-dis) against refpds545.

refpds545 (PVR_PSP2's PDS disassembler, built locally, refdisasm/README.md) is used
as a black-box oracle. Targets are the five EDM PDS programs captured from the
SGX545 srvinit, plus a fixed-seed random corpus covering every opcode, since
the captured programs use only a handful of instructions.

Invariant, as for oracle_check.py: a line we claim to decode must equal the
oracle, so WRONG must be 0. We may emit ".word" only where the oracle itself
reports a reserved encoding; anything else is counted as MISSED.
"""
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
REF = HERE / "refdisasm" / "refpds545"
OURS = HERE / "pds-dis"
CAP = ROOT / "harness/ukernel-extract/extracted"

# (program number, data-segment dwords) of the captured EDM PDS programs.
PROGRAMS = [(10, 4), (11, 8), (12, 4), (13, 24), (14, 12)]
RANDOM_PER_OPCODE = 4000


def run(tool, blob, ndata):
    out = subprocess.run([str(tool), str(blob), str(ndata)],
                         capture_output=True, text=True, check=True).stdout
    return [line.split("\t", 1)[1] for line in out.splitlines()]


def compare(blob, ndata):
    stats = {"ok": 0, "reserved": 0, "MISSED": 0, "WRONG": 0}
    bad = []
    for ref, ours in zip(run(REF, blob, ndata), run(OURS, blob, ndata)):
        if ours.startswith(".word"):
            key = "reserved" if "reserved" in ref else "MISSED"
        else:
            key = "ok" if ours == ref else "WRONG"
        stats[key] += 1
        if key in ("MISSED", "WRONG") and len(bad) < 10:
            bad.append(f"  {key}: ref={ref!r} ours={ours!r}")
    return stats, bad


def random_corpus(path, seed=1):
    rng = random.Random(seed)
    words = [(op << 27) | rng.getrandbits(27)
             for op in range(32) for _ in range(RANDOM_PER_OPCODE)]
    path.write_bytes(struct.pack(f"<{len(words)}I", *words))


def main():
    for tool in (REF, OURS):
        if not tool.exists():
            sys.exit(f"missing {tool} (refdisasm/README.md, then make)")
    targets = [(f"pds{n}", CAP / f"sgx545_pds{n}_runtime.bin", nd) for n, nd in PROGRAMS]
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        rnd = pathlib.Path(tmp) / "random.bin"
        random_corpus(rnd)
        targets.append(("random", rnd, 0))
        for label, blob, ndata in targets:
            if not blob.exists():
                print(f"{label:8s} skipped (no capture)")
                continue
            stats, bad = compare(blob, ndata)
            print(f"{label:8s} " + "  ".join(f"{k}={v}" for k, v in stats.items()))
            print(*bad, sep="\n") if bad else None
            failed |= stats["WRONG"] > 0 or stats["MISSED"] > 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
