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
          "+= -=".split())


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
    """32-bit result of the bitwise group, shifts/rotates by y < 32 and the adds."""
    return {"and": lambda: x & y, "or": lambda: x | y, "xor": lambda: x ^ y,
            "shl": lambda: (x << y) & M32, "shr": lambda: x >> y,
            "rol": lambda: ((x << y) | (x >> (32 - y))) & M32 if y else x,
            "asr": lambda: ((x - (1 << 32) if x >> 31 else x) >> y) & M32,
            "iadd32": lambda: (x + y) & M32, "iaddu32": lambda: (x + y) & M32}[op]()


def test_pred(test: str, v: int) -> int:
    neg, z = v >> 31, v == 0
    return int({"taz": z, "tanz": not z, "nat": neg, "naz": neg and z, "nanz": neg and not z,
                "noz": neg or z, "nonz": neg or not z}[test])


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
                       "movrep"])
    d, a, b, c = rng.sample(GEN_REGS, 4)
    flt = kind in ("fmad", "f1")
    R = {r: rand_val(rng, flt) for r in GEN_REGS}
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
        tests = ["taz", "tanz"] + ([] if op.startswith("iadd") else
                                   ["nat", "naz", "nanz", "noz", "nonz"])
        t, p, w = rng.choice(tests), f"p{rng.randrange(4)}", rng.random() < .5
        imm = op in ("shl", "shr", "rol", "asr")
        y = rng.randrange(32) if imm else R[b]
        cs = Case(f"{op}.test{t}.chan0 {d if w else '!c0'}, {p}, {a}, {f'#{y}' if imm else b}",
                  R)
        v = alu(op, R[a], y)
        cs.expect[p] = test_pred(t, v)
        if w:
            cs.expect[d] = v
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
        f = [u2f(sign_mod(R[r], m)) for r, m in ((a, ma), (b, mb), (c, mc))]
        cs.expect[d] = f2u(f[0] * f[1] + f[2])
    elif kind == "f1":
        op, m = rng.choice(["frcp", "frsq", "fsqrt"]), rng.choice(["", ".neg", ".abs",
                                                                     ".neg.abs"])
        x = u2f(sign_mod(R[a], m))
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
            R[a] = idx
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
    else:  # movrep: disjoint ranges of the pa and o banks or of r
        n = rng.randrange(2, 17)
        s = rng.randrange(0, 33 - n)
        if rng.random() < .5:
            dst, src = "o", "pa"
            dn = rng.randrange(0, 33 - n)
        else:
            dst, src = "r", "r"
            s = rng.randrange(0, 64 - n)
            dn = rng.choice([x for x in range(0, 65 - n) if x + n <= s or s + n <= x])
        regs = {f"{src}{s + i}": rng.getrandbits(32) for i in range(n)}
        regs.update({f"{dst}{dn + i}": 0 for i in range(n)})
        cs = Case(f"mov.repeat{n} {dst}{dn}, {src}{s}", regs)
        for i in range(n):
            cs.expect[f"{dst}{dn + i}"] = regs[f"{src}{s + i}"]
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
    cmds = ["o malloc://0x10000 0x100000000", "aei", "aeim"]
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
