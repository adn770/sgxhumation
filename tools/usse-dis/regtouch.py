#!/usr/bin/env python3
"""List the SGX registers a USSE program touches directly (its MMIO register map).

The EDM firmware reaches SGX registers with `ldr rD, #idx` (read) and `str #idx, src`
(write), where the immediate is a dword register index: register offset = idx * 4.
Accesses through a register-supplied address (`str r20, r18`) are not statically
resolvable and are skipped. Input is usse-dis or refdisasm output
("<idx>: 0x<ENC16>\\t<mnemonic>"); register names come from sgx*defs.h headers.

    usage: regtouch.py <disasm.txt> [--defs <hdr>...] [--r2 <out.r2>]
"""
from __future__ import annotations

import pathlib
import re
import sys
from dataclasses import dataclass

LINE = re.compile(r"^\s*(\d+):\s+0x[0-9A-Fa-f]{16}\s+(.*)$")
PRED = re.compile(r"^!?p\d+\s+")
LDR = re.compile(r"^ldr\s+\S+,\s+#(\d+)")
STR = re.compile(r"^str\s+#(\d+),")
DEFINE = re.compile(r"#define\s+(EUR_CR_\w+)\s+0x([0-9A-Fa-f]+)U?\s*$")
FIELD_MACRO = re.compile(r"(MASK|SHIFT|SIGNED|CLRMSK)$")
DEFAULT_DEFS = ("reference/PVR_PSP2/include/gpu_es4/eurasia/hwdefs/sgx545defs.h",
                "reference/PVR_PSP2/include/gpu_es4/eurasia/hwdefs/sgxdefs.h")


@dataclass(frozen=True)
class Access:
    insn: int      # instruction index
    kind: str      # "R" or "W"
    reg: int       # register byte offset


def scan(lines) -> list[Access]:
    out = []
    for ln in lines:
        m = LINE.match(ln)
        if not m:
            continue
        text = PRED.sub("", m.group(2).strip())
        if (r := LDR.match(text)):
            out.append(Access(int(m.group(1)), "R", int(r.group(1)) * 4))
        elif (w := STR.match(text)):
            out.append(Access(int(m.group(1)), "W", int(w.group(1)) * 4))
    return out


def summarize(accesses: list[Access]) -> dict[int, tuple[int, int]]:
    s: dict[int, list[int]] = {}
    for a in accesses:
        rw = s.setdefault(a.reg, [0, 0])
        rw[0 if a.kind == "R" else 1] += 1
    return {reg: tuple(rw) for reg, rw in sorted(s.items())}


def load_names(paths) -> dict[int, str]:
    names: dict[int, str] = {}
    for p in paths:
        for ln in pathlib.Path(p).read_text(errors="replace").splitlines():
            m = DEFINE.match(ln.strip())
            if m and not FIELD_MACRO.search(m.group(1)):
                names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def _name(reg: int, names: dict[int, str]) -> str:
    return names.get(reg, f"0x{reg:04x}")


def r2_comments(accesses: list[Access], names: dict[int, str]) -> str:
    verb = {"R": "read", "W": "write"}
    return "".join(f"CCu {verb[a.kind]} {_name(a.reg, names)} @ {a.insn * 8:#x}\n"
                   for a in accesses)


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    defs, r2_out, rest = [], None, argv[1:]
    while rest:
        flag = rest.pop(0)
        if flag == "--defs":
            while rest and not rest[0].startswith("--"):
                defs.append(rest.pop(0))
        elif flag == "--r2" and rest:
            r2_out = rest.pop(0)
    defs = defs or [d for d in DEFAULT_DEFS if pathlib.Path(d).exists()]
    names = load_names(defs)
    accesses = scan(pathlib.Path(argv[0]).read_text().splitlines())
    summary = summarize(accesses)
    print(f"{len(summary)} registers touched ({sum(r for r, _ in summary.values())} reads, "
          f"{sum(w for _, w in summary.values())} writes)")
    for reg, (r, w) in summary.items():
        print(f"  {reg:#06x} {_name(reg, names):42s} R{r:<3d} W{w}")
    if r2_out:
        pathlib.Path(r2_out).write_text(r2_comments(accesses, names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
