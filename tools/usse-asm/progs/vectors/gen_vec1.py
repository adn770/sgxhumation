#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Generate vec1.toml: the first instruction-vector batch (design/sgx545_vectors.md).

Recorded, not probed: no hypotheses, the hardware's readback is the vector. Our own
instructions only, on the probe templates (progs/probes/dump.s, memoff.s): the integer ALU,
integer tests and the predicates they write, predicated branches, ldad/stad with immediate,
register and post-increment offsets. Inputs are edge values plus a fixed-seed random set, so
the file is reproducible: `gen_vec1.py > vec1.toml`; test_vec_tables.py checks it is fresh.

Run: probe.py --table progs/vectors/vec1.toml run --out <corpus>/hw/vectors/vec1, then
vectors.py convert; the result and its raw.txt are published in data/ (data/README.md).
"""
from __future__ import annotations

import random

EDGE = [0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0x8000, 0xffff, 0x10000, 0x7fff]
SHIFTS = [0, 1, 15, 16, 31, 32, 33, 63, 64, 0xffffffff]


def rnd(r: random.Random) -> int:
    return r.getrandbits(32)


def pairs(r: random.Random, n_random: int) -> list[tuple[int, int, int]]:
    """(a, b, c): edges against each other, then random triples."""
    out = [(a, b, 0x5a5a5a5a) for a, b in
           [(0, 0), (1, 1), (0xffffffff, 1), (0x7fffffff, 1), (0x80000000, 0xffffffff),
            (0x8000, 0x8000), (0xffff, 1), (0x10000, 0xffffffff), (2, 0x7fff),
            (0x80000000, 0x80000000)]]
    out += [(rnd(r), rnd(r), rnd(r)) for _ in range(n_random)]
    return out


def q(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def probe(name: str, question: str, source: str, params: dict, inputs: list,
          cases: list, labels: list | None = None) -> str:
    ps = ", ".join(f"{k} = {q(v) if isinstance(v, str) else f'0x{v:08X}'}"
                   for k, v in params.items())
    cs = ", ".join("[" + ", ".join(q(v) if isinstance(v, str) else f"0x{v:08X}" for v in c)
                   + "]" for c in cases)
    out = [f'[[probe]]\nname = "{name}"\nquestion = {q(question)}\nsource = "{source}"',
           f"params = {{ {ps} }}", f"inputs = [{', '.join(q(i) for i in inputs)}]"]
    if labels:
        out.append(f"labels = [{', '.join(q(x) for x in labels)}]")
    out.append(f"cases = [{cs}]")
    return "\n".join(out) + "\n"


# dump.s: A-H in r1-r8, r10-r17 preset, OP, then r10-r17 and r1-r8 stored, o0 = OUT.
ZERO_REST = {"D": 0, "E": 0, "F": 0, "G": 0, "H": 0}
PRED = "\nmov r11, #0\np0 mov r11, #1"          # the predicate as a value, in r11

ALU = [
    ("and", "and r10, r1, r2"), ("or", "or r10, r1, r2"), ("xor", "xor r10, r1, r2"),
    ("andn", "and r10, r1, ~r2"),
    ("ima32u", "ima32.u r10, r1, r2, r3"), ("ima32s", "ima32.s r10, r1, r2, r3"),
    ("ima16u", "ima16.u r10, r1, r2, r3, u16, u16"),
    ("ima16s", "ima16.s r10, r1, r2, r3, u16, u16"),
    ("idiv", "idiv r10, r1, r2, drc0\nwdf drc0"),
]
SHIFT = [("shl", "shl r10, r1, r2"), ("shr", "shr r10, r1, r2"), ("asr", "asr r10, r1, r2"),
         ("rol", "rol r10, r1, r2")]
TESTS = [
    ("iadd32-taz", "iadd32.testtaz.chan0 r10, p0, r1, r2"),
    ("iaddu32-taz", "iaddu32.testtaz.chan0 r10, p0, r1, r2"),
    ("iadd32-nat", "iadd32.testnat.chan0 r10, p0, r1, r2"),
    ("iadd32-pat", "iadd32.testpat.chan0 r10, p0, r1, r2"),
    ("iadd32-tanz", "iadd32.testtanz.chan0 r10, p0, r1, r2"),
    ("iadd16-taz", "iadd16.testtaz.chan0 r10, p0, r1, r2"),
    ("iaddu16-taz", "iaddu16.testtaz.chan0 r10, p0, r1, r2"),
    ("isub16-taz", "isub16.testtaz.chan0 r10, p0, r1, r2"),
    ("isubu16-taz", "isubu16.testtaz.chan0 r10, p0, r1, r2"),
    ("iadd16-taz-chan1", "iadd16.testtaz.chan1 r10, p0, r1, r2"),
    ("iadd16-taz-chan2", "iadd16.testtaz.chan2 r10, p0, r1, r2"),
    ("iadd16-taz-chan3", "iadd16.testtaz.chan3 r10, p0, r1, r2"),
    ("and-taz", "and.testtaz.chan0 r10, p0, r1, r2"),
    ("xor-nat", "xor.testnat.chan0 r10, p0, r1, r2"),
    ("shl-pat", "shl.testpat.chan0 r10, p0, r1, r2"),
]
# memoff.s: r1 = scratch + 0x8000 (dword 0x2000), r2 = offset O, r3 = 0x11111111, OP, o0 = OUT
MEM = [
    ("ldad-reg", "ldad r30, [r1,+r2], drc0", "r30",
     [0, 1, 2, 0x10, 0x1fff, 0x10000, 0x10001, 0x20002, 0x40001, 0x80004, 0xffff0001]),
    ("ldad-regneg", "ldad r30, [r1,-r2], drc0", "r30", [0, 1, 0x10001, 0x40001, 0x40002]),
    ("ldad-fetch4", "ldad.fetch4 r30, [r1,+r2], drc0\nwdf drc0\nmov r29, r31", "r29",
     [0, 1, 0x10001, 0x40001]),
    ("stad-reg", "stad [r1,+r2], r3\nidf drc0, bif\nwdf drc0\nldad r30, [r1,+r2], drc0", "r30",
     [0, 1, 0x10001, 0x40001, 0x20002]),
    ("stad-postinc", "mov r28, r1\nstad [r28,#1++], r3\nstad [r28,#1++], r3\n"
     "idf drc0, bif\nwdf drc0\nldad r30, [r1,+r2], drc0", "r30", [0, 1, 2]),
    ("ldad-postinc", "mov r28, r1\nldad r30, [r28,#1++], drc0\nwdf drc0\n"
     "ldad r30, [r28,#1++], drc0", "r30", [0]),
]
# immediate offsets: one case each (the offset is part of the encoding)
MEM_IMM = [0, 1, 2, 3, 7, 0x10, 0x3f]


def generate() -> str:
    r = random.Random(20261022)
    out = ["# SPDX-License-Identifier: MIT\n# SPDX-FileCopyrightText: 2026 Josep Torra\n#\n"
           "# Generated by gen_vec1.py: do not edit. The first instruction-vector batch\n"
           "# (design/sgx545_vectors.md); recorded, so no hypotheses.\n"]
    for name, op in ALU:
        cases = [[a, b, c] for a, b, c in pairs(r, 6)]
        if name == "idiv":
            cases = [[a, b or 7, c] for a, b, c in cases] + [[100, 0, 0]]
        out.append(probe(f"vec-{name}", f"{op.splitlines()[0]}: values", "dump.s",
                         {"OP": op, "OUT": "r10", **ZERO_REST}, ["A", "B", "C"], cases))
    for name, op in SHIFT:
        vals = [0x80000001, 0xcafef00d, 0x12345678]   # never the tool's sentinel
        cases = [[v, s] for v in vals for s in SHIFTS]
        out.append(probe(f"vec-{name}", f"{op}: every amount class", "dump.s",
                         {"OP": op, "OUT": "r10", "C": 0, **ZERO_REST}, ["A", "B"], cases))
    for name, op in TESTS:
        cases = [[a, b] for a, b, _ in pairs(r, 4)]
        out.append(probe(f"vec-{name}", f"{op}: result and predicate (r11)", "dump.s",
                         {"OP": op + PRED, "OUT": "r10", "C": 0, **ZERO_REST},
                         ["A", "B"], cases))
    for neg in ("", "!"):
        for n in range(1, 6):
            op = ("and.testtaz.chan0 !c0, p0, r1, r1\nmov r10, #0\n"
                  f"{neg}p0 br #{n}\nor r10, r10, #1\nor r10, r10, #2\nor r10, r10, #4\n"
                  "or r10, r10, #8\nor r10, r10, #16")
            out.append(probe(f"vec-br{'n' if neg else ''}{n}",
                             f"{neg}p0 br #{n} past an or chain (p0: A == 0)", "dump.s",
                             {"OP": op, "OUT": "r10", "B": 0, "C": 0, **ZERO_REST}, ["A"],
                             [[0], [1]]))
    for name, op, outreg, offs in MEM:
        out.append(probe(f"vec-{name}", f"{op.splitlines()[0]}: by offset", "memoff.s",
                         {"OP": op, "OUT": outreg}, ["O"], [[o] for o in offs]))
    for k in MEM_IMM:
        out.append(probe(f"vec-ldad-imm{k}", f"ldad [r1,+#{k}]", "memoff.s",
                         {"OP": f"ldad r30, [r1,+#{k}], drc0", "OUT": "r30"}, ["O"], [[0]]))
        out.append(probe(f"vec-stad-imm{k}", f"stad [r1,+#{k}] then load back", "memoff.s",
                         {"OP": f"stad [r1,+#{k}], r3\nidf drc0, bif\nwdf drc0\n"
                                f"ldad r30, [r1,+#{k}], drc0", "OUT": "r30"}, ["O"], [[0]]))
    return "\n".join(out)


if __name__ == "__main__":
    print(generate(), end="")
