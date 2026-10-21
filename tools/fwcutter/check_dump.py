#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Compare the SGX buffers a driver built with what fwload.py builds for the same layout.

The gma500 port exposes them in debugfs (dri/N/sgx/bufNN, dri/N/sgx/layout, the layout
in fwload's JSON form). Copy that directory off the device as bufNN.bin + layout.json.

    usage: check_dump.py <sgx545_ukernel.fw> <dump dir>
"""
from __future__ import annotations

import json
import pathlib
import sys
from dataclasses import dataclass

import facts
import fwformat as F
import fwload


@dataclass
class Row:
    buffer: int
    status: str                      # match, differ, missing
    first_diff: int | None = None    # byte offset of the first differing dword


@dataclass
class Report:
    rows: list[Row]

    @property
    def ok(self) -> bool:
        return all(r.status == "match" for r in self.rows)


def check(fw_path: pathlib.Path, dump: pathlib.Path) -> Report:
    fw = F.unpack(pathlib.Path(fw_path).read_bytes())
    layout = fwload.parse_layout(json.loads((dump / "layout.json").read_text()))
    rows = []
    for i, want in sorted(fwload.load(fw, layout).images.items()):
        path = dump / f"buf{i:02d}.bin"
        if not path.exists():
            rows.append(Row(i, "missing"))
            continue
        got = path.read_bytes()
        if got == want:
            rows.append(Row(i, "match"))
            continue
        n = min(len(got), len(want))
        diff = next((k for k in range(0, n, 4) if got[k:k + 4] != want[k:k + 4]), n & ~3)
        rows.append(Row(i, "differ", diff))
    return Report(rows)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    report = check(pathlib.Path(argv[0]), pathlib.Path(argv[1]))
    for r in report.rows:
        name = facts.BUFFER_NAMES[r.buffer] if r.buffer < len(facts.BUFFER_NAMES) else "?"
        where = f" at {r.first_diff:#x}" if r.first_diff is not None else ""
        print(f"{r.buffer:2d} {name:12s} {r.status}{where}")
    n = len(report.rows)
    print(f"all {n} buffers match" if report.ok else
          f"{sum(r.status != 'match' for r in report.rows)} of {n} buffers do not match")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
