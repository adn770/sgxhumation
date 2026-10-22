#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Measure and check the ESIL of the radare2 fork's in-core `usse` plugin.

    esil_check.py survey <file.bin> [--random N] [-x]   op->esil set/empty per base mnemonic
    esil_check.py tokens <file.bin>                     every ESIL token is an operator,
                                                        a number or a profile register
    esil_check.py cross [--seed S] [--count N]          emulate random modelled instructions
                                                        and compare with a Python model

The radare2 tools come from $R2_PREFIX/bin (default: ../../radare2-prefix), which must be a
build of the fork: the out-of-tree plugin has no ESIL. `cross` assembles each instruction
with the fork's rasm2 and runs it with `aes`; the model here restates the semantics that
README.md ("Emulation") documents, independently of the plugin's C code.
"""
from __future__ import annotations

import collections
import json
import math
import os
import pathlib
import random
import re
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
PREFIX = pathlib.Path(os.environ.get("R2_PREFIX", HERE.parent.parent / "radare2-prefix"))
M32 = 0xFFFFFFFF
CR_BASE = 0x100000000          # the plugin's register-file window (USSE_CR_ESIL_BASE)
PRED = ("p0", "p1", "p2", "p3", "!p0", "!p1", "!p2", "!p3", "Pn")
OPS = set("= + - * & | ^ ! << >> ASR ~ ?{ } F2D D2F I2D F* F+ F/ SQRT [4] [2] =[4] =[2] "
          "+= -= DUP".split())
DATA = 0x20000                 # cross: a mapped data area for loads and stores


def tool(name: str) -> str:
    return str(PREFIX / "bin" / name)


def r2(cmds: str, path: str, *extra: str) -> str:
    return subprocess.run([tool("radare2"), "-q", "-a", "usse", "-b", "32", "-e", "scr.color=0",
                           *extra, "-c", cmds, path], capture_output=True, text=True).stdout


# ---- survey ------------------------------------------------------------------------------

def base_mnemonic(text: str) -> str:
    """'p1 and.testtaz.chan0 ...' -> 'and'."""
    t = text.split(" ")
    w = t[1] if t[0] in PRED and len(t) > 1 else t[0]
    return w.split(".")[0]


def tally(ops: list[dict]) -> dict[str, list[int]]:
    """{base: [with esil, without]} over aoj records, invalid words left out."""
    st: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
    for o in ops:
        m = o.get("opcode", "invalid")
        if m == "invalid":
            continue
        st[base_mnemonic(m)][0 if o.get("esil") else 1] += 1
    return dict(st)


def report(st: dict[str, list[int]]) -> str:
    y = sum(v[0] for v in st.values())
    n = y + sum(v[1] for v in st.values())
    out = [f"words {n}: esil {y} ({100 * y / max(1, n):.1f}%)"]
    for b, (s, e) in sorted(st.items(), key=lambda kv: (-sum(kv[1]), kv[0])):
        out.append(f"{b:12s} {s:6d} set {e:6d} empty")
    return "\n".join(out)


def aoj(path: str) -> list[dict]:
    n = os.path.getsize(path) // 8
    return json.loads(r2(f"aoj {n} @ 0", path) or "[]")


# ---- tokens ------------------------------------------------------------------------------

def profile_regs(arp: str) -> set[str]:
    """Register names of an `arp` register profile."""
    return {f[1] for f in (ln.split() for ln in arp.splitlines())
            if len(f) >= 3 and not f[0].startswith("=") and f[2].startswith(".")}


def bad_tokens(esil: str, regs: set[str]) -> list[str]:
    return [t for t in esil.split(",") if t not in OPS and t not in regs
            and not re.fullmatch(r"0x[0-9a-fA-F]+|\d+", t)]


# ---- cross-check model -------------------------------------------------------------------

def f2u(f: float) -> int:
    return struct.unpack("<I", struct.pack("<f", f))[0]


def u2f(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def sign_mod(u: int, m: str) -> int:
    return {"": u, ".neg": u ^ 0x80000000, ".abs": u & 0x7FFFFFFF,
            ".neg.abs": u | 0x80000000}[m]


def alu(op: str, x: int, y: int) -> int:
    """32-bit result of the bitwise group, the shifts and rotates (any 32-bit amount) and the
    adds. [HW]: shl/shr by 32 and over give 0, asr fills the sign, rol takes the amount mod
    32; iadd32/iaddu32 add src1's low half, sign- or zero-extended, to the 32-bit src2."""
    sx = x - (1 << 32) if x >> 31 else x
    return {"and": lambda: x & y, "or": lambda: x | y, "xor": lambda: x ^ y,
            "shl": lambda: (x << y) & M32 if y < 32 else 0,
            "shr": lambda: x >> y if y < 32 else 0,
            "rol": lambda: ((x << (y & 31)) | (x >> (32 - (y & 31)))) & M32,
            "asr": lambda: (sx >> min(y, 31)) & M32,
            "iadd32": lambda: (s16(x) + y) & M32,
            "iaddu32": lambda: ((x & 0xFFFF) + y) & M32}[op]()


def test_pred(test: str, v: int, unsigned: bool = False, bits: int = 32) -> int:
    """Predicate of test<S><a|o><Z> on result v. [HW]: n is the sign bit, p its complement
    (>= 0), t is true; unsigned (iaddu32) n is never and p always true; bits=16 tests the
    low lane (chan0 of the 16-bit ops)."""
    v &= (1 << bits) - 1
    sg, join, zr = test[0], test[1], test[2:]
    neg = v >> (bits - 1)
    sv = {"t": True, "n": bool(neg) and not unsigned, "p": not neg or unsigned}[sg]
    zv = {"t": True, "z": v == 0, "nz": v != 0}[zr]
    return int(sv and zv if join == "a" else sv or zv)


def sub16(x: int, y: int) -> int:
    """isub16/isubu16: both 16-bit lanes x - y, wrapping ([HW])."""
    lo, hi = (x - y) & 0xFFFF, ((x >> 16) - (y >> 16)) & 0xFFFF
    return hi << 16 | lo


def ima16(a: int, b: int, c: int, neg: bool) -> int:
    """ima16 ..., u16, u16: both lanes a * b + c (or - c), wrapping ([HW])."""
    out = 0
    for sh in (0, 16):
        x, y, z = (a >> sh) & 0xFFFF, (b >> sh) & 0xFFFF, (c >> sh) & 0xFFFF
        out |= ((x * y + (-z if neg else z)) & 0xFFFF) << sh
    return out


def flush(u: int) -> int:
    """A denormal becomes a zero of its sign ([HW] fmad-denormal, fmad-zero-sign)."""
    return u & 0x80000000 if not u & 0x7F800000 else u


def fmad(a: int, b: int, c: int) -> int:
    """fmad: inputs flushed, the product rounded to binary32 (to nearest even) and flushed,
    then the sum rounded and flushed ([HW] fmad-*, sweep-fmad). The sum goes through a double,
    as the ESIL does."""
    p = flush(f2u(u2f(flush(a)) * u2f(flush(b))))
    return flush(f2u(u2f(p) + u2f(flush(c))))


def regoff(o: int) -> int:
    """Byte offset of a register-offset load or store: (o >> 16) * (o & 0xffff), unsigned,
    added modulo 2^32 ([HW] ldad-reg-off*, stad-reg-off)."""
    return ((o >> 16) * (o & 0xFFFF)) & M32


def index_reg(bank: str, il: int, n: int) -> str | None:
    """The register bank[il + #n] names: from the bank's first, i.l bits 10 and up ignored
    ([HW] index-*); None outside the profile."""
    i = (il & 0x3FF) + n
    return f"{bank}{i}" if i < {"r": 64, "sa": 32}[bank] else None


def repeat(regs: dict, db: str, d: int, sb: str, s: int, n: int) -> dict:
    """mov.repeatN: one iteration after the other ([HW] repeat-order)."""
    r = dict(regs)
    for i in range(n):
        r[f"{db}{d + i}"] = r[f"{sb}{s + i}"]
    return {f"{db}{d + i}": r[f"{db}{d + i}"] for i in range(n)}


def cr_addr(idx: int) -> int:
    return CR_BASE + 4 * idx


class Case:
    """One instruction: register and window inputs, and what to read back afterwards."""

    def __init__(self, text: str, regs: dict[str, int], mem: dict[int, int] | None = None):
        self.text, self.regs, self.mem = text, dict(regs), dict(mem or {})
        self.expect: dict[str, int] = {}          # 'r1' or '@0x100000004' -> value

    def setup(self) -> list[str]:
        return ([f"wv4 0x{v:x} @ 0x{a:x}" for a, v in self.mem.items()]
                + ["ar pc=0"] + [f"ar {r}=0x{v:x}" for r, v in self.regs.items()])

    def reads(self) -> list[str]:
        return [f"pv4 {k}" if k.startswith("@") else f"ar {k}" for k in self.expect]


GEN_REGS = [f"r{i}" for i in range(1, 8)]


def rand_val(rng: random.Random, flt: bool = False) -> int:
    if flt:
        return f2u(rng.uniform(-100, 100))
    return rng.choice([0, 1, M32, 0x80000000, rng.getrandbits(32), rng.getrandbits(8)])


def gen_case(rng: random.Random) -> Case | None:
    """A random modelled instruction with its inputs and the modelled result."""
    kind = rng.choice(["bw", "bwt", "rolasr", "imae", "ima32", "fmad", "f1", "ldr", "str",
                       "movrep", "shreg", "isub", "ima16", "idx", "memreg"])
    d, a, b, c = rng.sample(GEN_REGS, 4)
    flt = kind in ("fmad", "f1")
    R = {r: rand_val(rng, flt) for r in GEN_REGS}
    if flt and rng.random() < .2:                   # a denormal input now and then
        R[rng.choice(GEN_REGS)] = rng.getrandbits(1) << 31 | rng.randrange(1, 1 << 23)
    R.update({f"p{i}": 0 for i in range(4)})
    if kind == "bw":
        op, inv = rng.choice(["and", "or", "xor"]), rng.random() < .5
        cs = Case(f"{op} {d}, {a}, {'~' if inv else ''}{b}", R)
        cs.expect[d] = alu(op, R[a], R[b] ^ M32 if inv else R[b])
    elif kind == "rolasr":
        op, n = rng.choice(["rol", "asr"]), rng.randrange(32)
        cs = Case(f"{op} {d}, {a}, #0x{n:08X}", R)
        cs.expect[d] = alu(op, R[a], n)
    elif kind == "bwt":
        op = rng.choice(["and", "or", "xor", "shl", "shr", "rol", "asr", "iadd32", "iaddu32"])
        t = rng.choice("tnp") + rng.choice("ao") + rng.choice(["t", "z", "nz"])
        p, w = f"p{rng.randrange(4)}", rng.random() < .5
        imm = op in ("shl", "shr", "rol", "asr") and rng.random() < .5
        if op in ("shl", "shr", "rol", "asr") and not imm:
            R[b] = rng.choice([rng.randrange(32), 32, 33, 64, rng.getrandbits(32)])
        y = rng.randrange(32) if imm else R[b]
        cs = Case(f"{op}.test{t}.chan0 {d if w else '!c0'}, {p}, {a}, {f'#{y}' if imm else b}",
                  R)
        v = alu(op, R[a], y)
        cs.expect[p] = test_pred(t, v, unsigned=op == "iaddu32")
        if w:
            cs.expect[d] = v
    elif kind == "shreg":
        op = rng.choice(["shl", "shr", "rol", "asr"])
        R[b] = rng.choice([rng.randrange(32), 32, 33, 64, 0x100, rng.getrandbits(32)])
        cs = Case(f"{op} {d}, {a}, {b}", R)
        cs.expect[d] = alu(op, R[a], R[b])
    elif kind == "isub":
        op = rng.choice(["isub16", "isubu16"])
        t = (rng.choice("tnp") if op == "isub16" else "t") + rng.choice("ao") + \
            rng.choice(["t", "z", "nz"])
        p, w, imm = f"p{rng.randrange(4)}", rng.random() < .5, rng.random() < .5
        R[a] = rng.choice([R[a], rng.getrandbits(16), 0x8000, 0x10001])
        y = rng.randrange(128) if imm else R[b]
        cs = Case(f"{op}.test{t}.chan0 {d if w else '!c0'}, {p}, {a}, {f'#{y}' if imm else b}",
                  R)
        v = sub16(R[a], y | y << 16 if imm else y)
        cs.expect[p] = test_pred(t, v, bits=16)
        if w:
            cs.expect[d] = v
    elif kind == "ima16":
        ib, ic, neg = rng.random() < .5, rng.random() < .5, rng.random() < .5
        yb, yc = rng.randrange(128), rng.randrange(128)
        sb = f"#{yb}" if ib else b
        sc = (f"#{yc}" if ic else c) + (".neg" if neg else "")
        cs = Case(f"ima16.{rng.choice('su')} {d}, {a}, {sb}, {sc}, u16, u16", R)
        vb = yb | yb << 16 if ib else R[b]
        vc = yc | yc << 16 if ic else R[c]
        cs.expect[d] = ima16(R[a], vb, vc, neg)
    elif kind == "idx":
        bank = rng.choice(["r", "sa"])
        n, il = rng.randrange(16), rng.choice([rng.randrange(32), rng.randrange(32) | 0x400,
                                               rng.randrange(32) | 0x40000])
        if rng.random() < .2:
            cs = Case(f"mov i.l, {a}", R)
            cs.expect["il"] = R[a]
            return cs
        reg = index_reg(bank, il, n)
        if reg is None or reg in GEN_REGS:
            return None
        R["il"] = il
        R[reg] = rng.getrandbits(32)
        if rng.random() < .5:
            cs = Case(f"mov {d}, {bank}[il + #{n}]", R)
            cs.expect[d] = R[reg]
        else:
            cs = Case(f"mov {bank}[il + #{n}], {a}", R)
            cs.expect[reg] = R[a]
    elif kind == "memreg":
        # dwords at DATA hold DATA + 4i; the base points into the middle of them
        base = DATA + 0x8000
        stride, idx = rng.choice([1, 2, 4, 8]), rng.randrange(0x400)
        o, neg = stride << 16 | idx, rng.random() < .3
        addr = (base - regoff(o)) if neg else (base + regoff(o))
        R.update({b: base, c: o})
        mem = {a4: a4 for a4 in range((addr & ~3) - 64, (addr & ~3) + 68, 4)}
        form = rng.choice(["ld", "st", "fetch"]) if not neg else rng.choice(["ld", "st"])
        sg = "-" if neg else "+"
        if form == "st":
            cs = Case(f"stad [{b},{sg}{c}], {a}", R, mem)
            cs.expect[f"@0x{addr & ~3:x}"] = R[a] if addr % 4 == 0 else addr & ~3
            return cs
        def rd(x: int) -> int:                  # a dword of the area, maybe unaligned
            lo = x & ~3
            raw = struct.pack("<II", lo, lo + 4)
            return struct.unpack("<I", raw[x - lo:x - lo + 4])[0]
        if form == "ld":
            cs = Case(f"ldad {d}, [{b},{sg}{c}], drc0", R, mem)
            cs.expect[d] = rd(addr)
        else:
            k = rng.randrange(2, 4)
            dr = [f"r{i}" for i in range(10, 10 + k)]
            cs = Case(f"ldad.fetch{k} {dr[0]}, [{b},+{c}], drc0", R, mem)
            for i, r in enumerate(dr):
                cs.expect[r] = rd(addr + 4 * (i + 1))
    elif kind == "imae":
        sg, ha, hb = rng.choice("us"), rng.choice(["low", "high"]), rng.choice(["low", "high"])
        cs = Case(f"imae.{sg} {d}, {a}.{ha}, {b}.{hb}, {c}, u32", R)
        x = (R[a] >> 16 if ha == "high" else R[a]) & 0xFFFF
        y = (R[b] >> 16 if hb == "high" else R[b]) & 0xFFFF
        if sg == "s":
            x, y = s16(x), s16(y)
        cs.expect[d] = (x * y + R[c]) & M32
    elif kind == "ima32":
        cs = Case(f"ima32.{rng.choice('us')} {d}, {a}, {b}, {c}", R)
        cs.expect[d] = (R[a] * R[b] + R[c]) & M32
    elif kind == "fmad":
        mods = ["", ".neg", ".abs", ".neg.abs"]
        ma, mb, mc = rng.choice(["", ".abs"]), rng.choice(mods), rng.choice(mods)
        cs = Case(f"fmad {d}, {a}{ma}, {b}{mb}, {c}{mc}", R)
        cs.expect[d] = fmad(*(sign_mod(R[r], m) for r, m in ((a, ma), (b, mb), (c, mc))))
    elif kind == "f1":
        op, m = rng.choice(["frcp", "frsq", "fsqrt"]), rng.choice(["", ".neg", ".abs",
                                                                     ".neg.abs"])
        x = u2f(flush(sign_mod(R[a], m)))       # [HW] frcp: a denormal input is flushed
        if x == 0 or (op != "frcp" and x < 0):
            return None                         # special values are not modelled
        cs = Case(f"{op} {d}, {a}{m}", R)
        cs.expect[d] = f2u({"frcp": lambda: 1 / x, "frsq": lambda: 1 / math.sqrt(x),
                            "fsqrt": lambda: math.sqrt(x)}[op]())
    elif kind == "ldr":
        idx = rng.randrange(0x4000)
        val = rng.getrandbits(32)
        if rng.random() < .5:
            cs = Case(f"ldr {d}, #{idx}, drc{rng.randrange(2)}", R, {cr_addr(idx): val})
        else:
            R[a] = idx | rng.choice([0, 0x4000, 0x10000, 0xFFFC0000])   # masked to 14 bits
            cs = Case(f"ldr {d}, {a}, drc0", R, {cr_addr(idx): val})
        cs.expect[d] = val
    elif kind == "str":
        idx = rng.randrange(0x4000)
        imm = rng.random() < .3
        src = f"#{rng.randrange(128)}" if imm else b
        val = int(src[1:]) if imm else R[b]
        if rng.random() < .5:
            cs = Case(f"str #{idx}, {src}", R, {cr_addr(idx): 0})
        else:
            R[a] = idx
            if not imm:
                val = R[b]
            cs = Case(f"str {a}, {src}", R, {cr_addr(idx): 0})
        cs.expect[f"@0x{cr_addr(idx):x}"] = val
    else:  # movrep: pa to o, or within r (ranges may overlap), maybe predicated
        n = rng.randrange(2, 17)
        s = rng.randrange(0, 33 - n)
        if rng.random() < .5:
            dst, src = "o", "pa"
            dn = rng.randrange(0, 33 - n)
        else:
            dst, src = "r", "r"
            s = rng.randrange(0, 65 - n)
            dn = rng.choice([s + rng.randrange(-3, 4), rng.randrange(0, 65 - n)])
            dn = min(max(dn, 0), 64 - n)
        regs = {f"{src}{s + i}": rng.getrandbits(32) for i in range(n)}
        regs.update({f"{dst}{dn + i}": rng.getrandbits(32) for i in range(n)})
        pred, run = "", True
        if rng.random() < .3:
            pn, pv = rng.randrange(4), rng.randrange(2)   # (rasm2 takes no "!pN" text)
            regs[f"p{pn}"] = pv
            pred, run = f"p{pn} ", bool(pv)
        cs = Case(f"{pred}mov.repeat{n} {dst}{dn}, {src}{s}", regs)
        out = repeat(regs, dst, dn, src, s, n)
        for r, v in out.items():
            cs.expect[r] = v if run else regs[r]
    return cs


def cross(seed: int, count: int) -> tuple[int, list[str]]:
    """Run `count` random cases through r2; return (cases run, mismatch descriptions)."""
    rng = random.Random(seed)
    cases: list[tuple[Case, str]] = []
    while len(cases) < count:
        cs = gen_case(rng)
        if cs is None:
            continue
        w = subprocess.run([tool("rasm2"), "-a", "usse", "-b", "32", cs.text],
                           capture_output=True, text=True).stdout.strip()
        if len(w) != 16:
            raise RuntimeError(f"rasm2 cannot assemble {cs.text!r}")
        cases.append((cs, w))
    cmds = ["o malloc://0x10000 0x100000000", f"o malloc://0x10000 0x{DATA:x}", "aei", "aeim"]
    for cs, w in cases:
        cmds += [f"wx {w} @ 0"] + cs.setup() + ["aes"] + cs.reads()
    with tempfile.NamedTemporaryFile("w", suffix=".r2", delete=False) as f:
        f.write("\n".join(cmds) + "\n")
    try:
        out = subprocess.run([tool("radare2"), "-q", "-a", "usse", "-b", "32", "-e",
                              "scr.color=0", "-i", f.name, "malloc://64"],
                             capture_output=True, text=True).stdout.split()
    finally:
        os.unlink(f.name)
    vals = [int(x, 16) for x in out if x.startswith("0x")]
    bad, at = [], 0
    for cs, _ in cases:
        got = vals[at:at + len(cs.expect)]
        at += len(cs.expect)
        exp = list(cs.expect.values())
        if got != exp:
            bad.append(f"{cs.text}: " + ", ".join(
                f"{k} {e:#x} != {g:#x}" for k, e, g in zip(cs.expect, exp, got) if e != g))
    return len(cases), bad


# ---- command line ------------------------------------------------------------------------

def random_blob(n: int, seed: int = 1) -> str:
    rng = random.Random(seed)
    fd, path = tempfile.mkstemp(suffix=".bin")
    with os.fdopen(fd, "wb") as f:
        f.write(bytes(rng.getrandbits(8) for _ in range(8 * n)))
    return path


def main(argv: list[str]) -> int:
    if not argv or argv[0] not in ("survey", "tokens", "cross"):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    cmd, rest = argv[0], argv[1:]

    def opt(name: str, default: int) -> int:
        return int(rest[rest.index(name) + 1]) if name in rest else default

    if cmd == "cross":
        n, bad = cross(opt("--seed", 7), opt("--count", 400))
        for b in bad[:20]:
            print("MISMATCH", b)
        print(f"{n} cases, {len(bad)} mismatches")
        return 1 if bad else 0
    tmp = None
    if "--random" in rest:
        path = tmp = random_blob(opt("--random", 0))
    else:
        path = rest[0]
    try:
        ops = aoj(path)
    finally:
        if tmp:
            os.unlink(tmp)
    if cmd == "survey":
        st = tally(ops)
        print(report(st))
        if "-x" in rest:
            ex: dict[str, list[str]] = collections.defaultdict(list)
            for o in ops:
                m = o.get("opcode", "invalid")
                if m != "invalid" and not o.get("esil") and len(ex[base_mnemonic(m)]) < 6:
                    ex[base_mnemonic(m)].append(m)
            for b in sorted(ex):
                print("\n".join(f"  {m}" for m in ex[b]))
        return 0
    regs = profile_regs(r2("arp", "malloc://8"))
    n = bad = 0
    for o in ops:
        if o.get("esil"):
            n += 1
            if (t := bad_tokens(o["esil"], regs)):
                bad += 1
                print("BAD", t[0], "|", o["opcode"], "|", o["esil"])
    print(f"{n} esil strings, {bad} bad")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
