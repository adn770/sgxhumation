#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""USSE field solver — derive instruction field layouts from the SGX540 oracle.

This is the methodology behind every field layout in usse.c: for a chosen
instruction *form* (a regex over the oracle mnemonic with named groups), it
finds, per value-bit of each field, the encoding bit(s) that match it across
all samples — i.e. it reverse-engineers the bit layout from ground truth rather
than guessing.

Caveats the output makes visible (so we never encode an under-determined field):
  * a value-bit that never varies in the corpus maps to a big ambiguous set
    (e.g. branch targets are all <=7 -> target bits >2 unresolved);
  * correlated operands (e.g. `or r16, r16, r17`, dest==src1) can tie bits
    together. Prefer de-correlated, wide-range samples before trusting a map.

Usage:
  ./solve.py limm      # mov r<n>, #imm32
  ./solve.py alu       # and/or/xor r,r,r  (3-register form)
  ./solve.py '<regex with (?P<name>...) groups>'  [num:field ...]
"""
import re, sys, pathlib
REF = pathlib.Path(__file__).resolve().parent.parent.parent / "reference/sgx540-reversing/ukernel"
LINE = re.compile(r'0x([0-9A-Fa-f]{16})\s+(.*)')

def load():
    out = []
    for f in ("ukernel-disasm", "TA-disasm", "3D-disasm", "spm-disasm"):
        for ln in (REF / f).read_text(errors="replace").splitlines():
            m = LINE.search(ln)
            if m:
                out.append((int(m.group(1), 16), m.group(2).strip()))
    return out

def per_bit(samples, valfn, nbits, label):
    res = []
    for vb in range(nbits):
        cand = [b for b in range(64)
                if all(((e >> b) & 1) == ((valfn(g) >> vb) & 1) for e, g in samples)]
        res.append(cand[0] if len(cand) == 1 else cand)
    print(f"  {label:10s}: {res}")

def fixed(samples, lo=0, hi=63):
    on = (1 << 64) - 1; off = 0
    for e, _ in samples: on &= e; off |= e
    f1 = [b for b in range(lo, hi + 1) if (on >> b) & 1]
    f0 = [b for b in range(lo, hi + 1) if not (off >> b) & 1]
    print(f"  fixed1={f1}\n  fixed0={f0}")

FORMS = {
    "limm": (r'^mov (?P<bank>r)(?P<num>\d+), #0x(?P<imm>[0-9A-Fa-f]+)$',
             [("num", 6), ("imm", 32)]),
    "alu":  (r'^(?P<op>and|or|xor) (?P<db>r|pa|sa|o|c|i|g)(?P<dn>\d+), '
             r'(?P<s1b>r|pa|sa|o|c|i|g)(?P<s1n>\d+), '
             r'(?P<s2b>r|pa|sa|o|c|i|g)(?P<s2n>\d+)$',
             [("dn", 6), ("s1n", 6), ("s2n", 6)]),
}

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    key = sys.argv[1]
    if key in FORMS:
        rx, fields = FORMS[key]
    else:
        rx, fields = key, [a.split(":")[::-1] for a in sys.argv[2:]]
        fields = [(n, int(w)) for n, w in fields]
    cr = re.compile(rx)
    data = load()
    S = [(e, m.groupdict()) for e, mn in data for m in [cr.match(mn)] if m]
    print(f"form {key!r}: {len(S)} samples")
    if not S: return 1
    fixed(S)
    for name, width in fields:
        vals = [g[name] for _, g in S]
        numeric = all(re.fullmatch(r'\d+|[0-9A-Fa-f]+', v) for v in vals)
        base = 16 if any(c in "abcdefABCDEF" for v in vals for c in v) else 10
        fn = (lambda g, n=name, b=base: int(g[n], b)) if numeric else None
        if fn: per_bit(S, fn, width, name)
    return 0

if __name__ == "__main__":
    sys.exit(main())
