#!/usr/bin/env python3
"""How much of the USSE instruction set usse-dis decodes, per opcode class.

Uniformly random 64-bit words go through refdisasm545 and usse-dis. Words the oracle prints
as an invalid encoding are left out; of the rest, a word counts as covered when usse-dis
prints exactly the oracle's text. Per class it lists the most frequent mnemonics still
missing.

  python3 coverage.py [--words N] [--seed S] [--classes 0,1,27]
"""
import argparse
import collections
import random
import re
import sys
import tempfile

import random_oracle_check as r

PRED = re.compile(r"^(!?p[0-3]|Pn|!?P[0-3])\s+")


def mnemonic(text):
    return PRED.sub("", text).split()[0].split(".")[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", type=int, default=60000)
    ap.add_argument("--seed", type=int, default=545)
    ap.add_argument("--classes", default="")
    args = ap.parse_args()
    want = {int(c) for c in args.classes.split(",") if c != ""}
    rng = random.Random(args.seed)
    words = []
    while len(words) < args.words:
        w = rng.getrandbits(64)
        if not want or w >> 59 in want:
            words.append(w)
    with tempfile.TemporaryDirectory() as tmp:
        ours = r.disasm(r.OURS, words, tmp)
        ref = r.disasm(r.REF, words, tmp)
    valid, ok = collections.Counter(), collections.Counter()
    miss = collections.defaultdict(collections.Counter)
    for w, o, f in zip(words, ours, ref):
        if "invalid" in f:
            continue
        c = w >> 59
        valid[c] += 1
        if o == f:
            ok[c] += 1
        else:
            miss[c][mnemonic(f)] += 1
    tv, to = sum(valid.values()), sum(ok.values())
    print(f"valid encodings: {tv}; decoded as the oracle: {to} ({100 * to / max(tv, 1):.1f}%)")
    for c in sorted(valid):
        m = ", ".join(n for n, _ in miss[c].most_common(8))
        print(f"class {c:2d}: {ok[c]:6d}/{valid[c]:6d}  " + (f"missing: {m}" if m else "complete"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
