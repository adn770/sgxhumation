#!/usr/bin/env python3
"""Check every form of the usse-dis table against refdisasm545 on random words.

oracle_check.py compares the decoder with the oracle on the captured corpora,
which exercise only the field values the firmware happens to use. This script
draws random words from each form of the table (`usse-dis --forms`): the
form's fixed bits, random everywhere else, and also words with one or two of
the fixed bits flipped, to probe the edges between forms. It adds uniformly
random words. Invariant, as for oracle_check.py: a word we decode must print
exactly what refdisasm545 prints, so WRONG must be 0.

    random_oracle_check.py [--per-form N] [--uniform N] [--seed S]
"""
import argparse
import collections
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REF = HERE / "refdisasm" / "refdisasm545"
OURS = HERE / "usse-dis"


def forms():
    out = subprocess.run([str(OURS), "--forms"], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        idx, mask, val, _, pat = line.split("\t")
        yield int(idx), int(mask, 16), int(val, 16), pat


def disasm(tool, words, tmp, which=False):
    blob = pathlib.Path(tmp) / "words.bin"
    blob.write_bytes(struct.pack(f"<{len(words)}Q", *words))
    cmd = [str(tool)] + (["--which"] if which else []) + [str(blob)]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    lines = [line.split("\t", 1)[1] for line in out.splitlines()
             if "\t" in line and ": 0x" in line]
    if not which:
        return [line.strip() for line in lines]
    return [(t.strip(), int(n[1:])) for t, n in (line.rsplit("\t", 1) for line in lines)]


def decoded(text):
    return not (text.startswith(".long") or "TODO" in text)


def corpus(per_form, uniform, seed):
    """(word, origin) pairs: origin is the generating form, or -1 for uniform."""
    rng = random.Random(seed)
    words = []
    for idx, mask, val, pat in forms():
        if pat == "(reject)":
            continue
        free = ~mask & (2**64 - 1)
        fixed = [b for b in range(64) if mask >> b & 1]
        for i in range(per_form):
            w = val | (rng.getrandbits(64) & free)
            if i % 3 == 2 and fixed:                     # probe the form's edges
                for b in rng.sample(fixed, min(len(fixed), rng.choice((1, 2)))):
                    w ^= 1 << b
            words.append((w, idx))
    words += [(rng.getrandbits(64), -1) for _ in range(uniform)]
    return words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-form", type=int, default=2000)
    ap.add_argument("--uniform", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    for tool in (REF, OURS):
        if not tool.exists():
            sys.exit(f"missing {tool} (run refdisasm/build.sh and make)")
    pats = {idx: pat for idx, _, _, pat in forms()}
    words = corpus(args.per_form, args.uniform, args.seed)
    with tempfile.TemporaryDirectory() as tmp:
        ours = disasm(OURS, [w for w, _ in words], tmp, which=True)
        ref = disasm(REF, [w for w, _ in words], tmp)
    assert len(ours) == len(ref) == len(words)
    dec = wrong = 0
    bad = collections.defaultdict(list)
    for (w, _), (o, form), r in zip(words, ours, ref):
        if not decoded(o):
            continue
        dec += 1
        if o != r:
            wrong += 1
            bad[form].append(f"    0x{w:016X}  ref={r!r}  ours={o!r}")
    print(f"random: {len(words)} words, {dec} decoded, WRONG {wrong}")
    for origin, lines in sorted(bad.items()):
        print(f"  form {origin} {pats[origin]!r}: {len(lines)} wrong")
        print("\n".join(lines[:4]))
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
