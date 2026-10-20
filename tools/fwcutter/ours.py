#!/usr/bin/env python3
"""Our own transfer-queue templates: tools/usse-asm/progs/ assembled (format §11).

The nine programs of a fill and a copy, written as our own assembly source, assemble bit for
bit to the templates fwcutter cuts from libsrv_um.so. This module assembles them with
usse-asm / pds-asm (built on demand with make), so the firmware file and libsgxfive can carry
them without the closed library. Their TQ_META and TQ_PATCHES records are the facts.py ones:
the sources keep the patched fields where the templates have them.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

import facts

ASM_DIR = pathlib.Path(__file__).resolve().parent.parent / "usse-asm"
PROGS_DIR = ASM_DIR / "progs"

# template id -> (source file in PROGS_DIR, ISA)
PROGS = {
    facts.TQ_USE_FILL: ("use_fill.s", "usse"),
    facts.TQ_USE_SEC: ("use_sec.s", "usse"),
    facts.TQ_USE_EOT: ("use_eot.s", "usse"),
    facts.TQ_USE_EOR: ("use_eor.s", "usse"),
    facts.TQ_PDS_PRIM: ("pds_prim.s", "pds"),
    facts.TQ_PDS_SEC: ("pds_sec.s", "pds"),
    facts.TQ_PDS_PIXEVENT: ("pds_pixevent.s", "pds"),
    facts.TQ_USE_COPY: ("use_copy.s", "usse"),
    facts.TQ_PDS_PRIM_1SRC: ("pds_prim_1src.s", "pds"),
}
WORD = {"usse": 8, "pds": 4}


class SourceError(RuntimeError):
    pass


_built = False


def _tool(isa: str) -> pathlib.Path:
    global _built
    if not _built:
        r = subprocess.run(["make", "-s", "-C", str(ASM_DIR)], capture_output=True, text=True)
        if r.returncode:
            raise SourceError(f"cannot build the assembler in {ASM_DIR}:\n{r.stderr}")
        _built = True
    return ASM_DIR / ("usse-asm" if isa == "usse" else "pds-asm")


def _run(tid: int) -> tuple[bytes, str]:
    name, isa = PROGS[tid]
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "out.bin"
        r = subprocess.run([str(_tool(isa)), "-l", "-o", str(out), str(PROGS_DIR / name)],
                           capture_output=True, text=True)
        if r.returncode:
            raise SourceError(f"{name}: {r.stderr.strip()}")
        return out.read_bytes(), r.stdout


def assemble(tid: int) -> bytes:
    """The template's bytes, as the assembler writes them."""
    return _run(tid)[0]


def listing(tid: int) -> list[tuple[int, str]]:
    """The template as (word, text) pairs, in the disassembler's spelling; a PDS data
    dword reads "data"."""
    raw, text = _run(tid)
    width = WORD[PROGS[tid][1]]
    words = [int.from_bytes(raw[i:i + width], "little") for i in range(0, len(raw), width)]
    texts = [line.split("\t", 1)[1].strip() for line in text.splitlines() if "\t" in line]
    if len(texts) != len(words):
        raise SourceError(f"{PROGS[tid][0]}: the listing does not match the binary")
    return list(zip(words, texts))


def templates() -> dict[int, bytes]:
    """Every template, by id."""
    return {tid: assemble(tid) for tid in PROGS}
