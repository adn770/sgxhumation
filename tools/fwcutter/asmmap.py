#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Read what sgx-asm writes: an image per section and map.json (tools/usse-asm/README.md).

map.json names each patch's encoding and kind; here they become the firmware format's
numbers (format §4, §9.4) and the records fwformat.py packs, with `buffer` = the
section's id (a buffer id, or a template id for transfer-queue templates).
"""
from __future__ import annotations

import json
import pathlib
import subprocess
from dataclasses import dataclass

from fwformat import (ENC_CODE_BASE, ENC_EMITPIX_SB, ENC_USSE_LIMM, ENC_WORD32, K_ADDR,
                      K_ADDR_SHR4, K_CODE_WORD, K_CONST, K_DOUTU, K_HEAP, K_PARAM, K_PD_PHYS,
                      K_PDS_CTL, K_PDS_ROWS, Expr)

REPO = pathlib.Path(__file__).resolve().parent.parent.parent

ENCODINGS = {"word32": ENC_WORD32, "usse_limm": ENC_USSE_LIMM, "code_base": ENC_CODE_BASE,
             "emitpix_sb": ENC_EMITPIX_SB}
KINDS = {"const": K_CONST, "addr": K_ADDR, "addr_shr4": K_ADDR_SHR4, "heap": K_HEAP,
         "pd_phys": K_PD_PHYS, "code_word": K_CODE_WORD, "pds_ctl": K_PDS_CTL,
         "pds_rows": K_PDS_ROWS, "param": K_PARAM, "doutu": K_DOUTU}


class AsmError(RuntimeError):
    pass


@dataclass(frozen=True)
class Section:
    name: str
    isa: str
    id: int
    image: bytes
    data: int          # leading PDS data words


@dataclass(frozen=True)
class Symbol:
    name: str
    section: str
    index: int | None  # as branches count; None on PDS data
    offset: int


@dataclass(frozen=True)
class Program:
    sections: list[Section]
    symbols: list[Symbol]
    patches: list[Expr]

    def section(self, name: str) -> Section:
        return next(s for s in self.sections if s.name == name)


def load(out_dir: pathlib.Path) -> Program:
    """The program sgx-asm wrote to `out_dir` (-d)."""
    m = json.loads((out_dir / "map.json").read_text())
    sections = [Section(s["name"], s["isa"], s["id"], (out_dir / s["bin"]).read_bytes(), s["data"])
                for s in m["sections"]]
    for s, raw in zip(sections, m["sections"]):
        if len(s.image) != raw["size"]:
            raise AsmError(f"{raw['bin']}: {len(s.image)} bytes, the map says {raw['size']}")
    symbols = [Symbol(y["name"], y["section"], y["index"], y["offset"]) for y in m["symbols"]]
    patches = [Expr(p["id"], ENCODINGS[p["encoding"]], KINDS[p["kind"]], 0, p["offset"],
                    p["target"], p["addend"]) for p in m["patches"]]
    return Program(sections, symbols, patches)


def sgx_asm() -> pathlib.Path:
    """The sgx-asm binary, built with cargo on demand."""
    r = subprocess.run(["cargo", "build", "-q", "-p", "sgx-tools", "--bin", "sgx-asm"],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode:
        raise AsmError(f"cannot build sgx-asm:\n{r.stderr}")
    return REPO / "target" / "debug" / "sgx-asm"


def assemble(source: pathlib.Path, out_dir: pathlib.Path) -> Program:
    """Assemble `source` with sgx-asm into `out_dir` and read the result."""
    r = subprocess.run([str(sgx_asm()), "-d", str(out_dir), str(source)],
                       capture_output=True, text=True)
    if r.returncode:
        raise AsmError(f"{source}: {r.stderr.strip()}")
    return load(out_dir)
