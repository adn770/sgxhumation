# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Instruction text assembles whatever its case: radare2 lowercases what it hands an
assembler (r_asm_assemble_single), so `rasm2 -a usse` and `pa` only work if the lowercase
spelling of every decoded instruction assembles to a word that decodes to the same canonical
text. The test also shows the instruction sets have no two encodings whose texts differ only
in case."""
import random

from conftest import DIS_DIR, disasm, run_asm
from test_roundtrip import SEED, decoded, usse_corpus


def lower_trip(tool, dis, words, tmp_path, *dis_args):
    texts = disasm(dis, words, tmp_path, *dis_args)
    keep = [t for t in texts if decoded(t)]
    again = run_asm(tool, "\n".join(t.lower() for t in keep) + "\n", tmp_path)
    assert len(again) == len(keep)
    back = disasm(dis, again, tmp_path, *dis_args)
    bad = [(t, b) for t, b in zip(keep, back) if t != b]
    assert not bad, bad[:5]
    return len(keep)


def test_usse_lowercase(tmp_path):
    assert lower_trip("usse-asm", DIS_DIR / "usse-dis", usse_corpus(random.Random(SEED + 1)),
                      tmp_path) > 50000


def test_pds_lowercase(tmp_path):
    rng = random.Random(SEED + 1)
    words = [(op << 27) | rng.getrandbits(27) for op in range(32) for _ in range(500)]
    assert lower_trip("pds-asm", DIS_DIR / "pds-dis", words, tmp_path, "0") > 5000
