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
import fractions
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
OPS = set("= + - * & | ^ ! << >> ASR ~ ?{ } F2D D2F I2D D2I F* F+ F- F/ F< SQRT FLOOR CEIL "
          "[4] [2] =[4] =[2] += -= DUP".split())
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


# [HW] round 1 of the ISA probes (tools/usse-asm/progs/probes/{fdp,minmax,movc,pack,masks}.toml)

def isnan(u: int) -> bool:
    return (u & 0x7FFFFFFF) > 0x7F800000


def fkey(u: int) -> int:
    """An integer order of binary32 values (-0 below +0)."""
    return u ^ 0xFFFFFFFF if u >> 31 else u | 0x80000000


def minmax(a: int, b: int, mx: bool) -> int:
    """IEEE maxNum/minNum on flushed inputs: a NaN loses, -0 is below +0 ([HW]
    fminmax-specials)."""
    a, b = flush(a), flush(b)
    if isnan(a):
        return b
    if isnan(b):
        return a
    lt = fkey(a) < fkey(b) if mx else fkey(b) < fkey(a)
    return b if lt else a


def fminmax(a: int, b: int, c: int) -> int:
    """min(max(a, b), c) ([HW] fminmax-roles)."""
    return minmax(minmax(a, b, True), c, False)


def fmaxmin(a: int, b: int, c: int) -> int:
    """max(min(a, c), b) ([HW] fmaxmin-roles)."""
    return minmax(minmax(a, c, False), b, True)


def fdot(a: list[int], b: list[int]) -> int:
    """fdp: each product rounded to binary32 and flushed, then added in order, each sum rounded
    and flushed ([HW] fdp-rounding, fdp-specials); the sums go through doubles, as the ESIL's
    do."""
    acc = None
    for x, y in zip(a, b):
        p = flush(f2u(u2f(flush(x)) * u2f(flush(y))))
        acc = p if acc is None else flush(f2u(u2f(acc) + u2f(p)))
    return acc


def movc(typ: str, test: str, s0: int, s1: int, s2: int) -> int:
    """movc: each lane of s1 where the test of s0's lane passes, else s2's; .i10 has three
    10-bit lanes and clears bits 30-31; .flt tests the float ([HW] movc-*)."""
    if typ == "flt":
        nan, z, neg = isnan(s0), flush(s0) & 0x7FFFFFFF == 0, bool(s0 >> 31)
        ok = {"tz": z, "tnz": not z, "tn": neg and not z and not nan,
              "tp|z": (not neg or z) and not nan}[test]
        return s1 if ok else s2
    w = {"i8": 8, "i16": 16, "i10": 10, "i32": 32}[typ]
    r = 0
    for k in range(3 if w == 10 else 32 // w):
        m = ((1 << w) - 1) << (w * k)
        lv = (s0 & m) >> (w * k)
        ok = {"tz": lv == 0, "tnz": lv != 0, "tn": bool(lv >> (w - 1)),
              "tp|z": not lv >> (w - 1)}[test]
        r |= (s1 if ok else s2) & m
    return r


def movmsk(fmt: str, s2: int, c: int) -> bool:
    """movmsk: does s2's component c let s1 through ([HW] movmsk-*)."""
    if fmt == "mask":
        return bool(s2 >> (8 * c) & 1)
    if fmt == "u8":
        return bool(s2 >> (8 * c + 7) & 1)
    if fmt == "flt":
        return not isnan(s2) and fkey(s2) >= fkey(0x3F000000)
    h = (s2 >> (0, 8, 16, 16)[c]) & 0xFFFF
    return not h >> 15 and 0x3800 <= (h & 0x7FFF) <= 0x7C00


PACK = {  # lo, hi, offset unscaled; lo, hi, factor scaled
    "u8": (0, 255, 0, 0, 255, 255), "s8": (-128, 127, 0, -127, 127, 127),
    "o8": (-128, 127, 128, -127, 127, 127), "u16": (0, 65535, 0, 0, 65535, 65535),
    "s16": (-32768, 32767, 0, -32767, 32767, 32767), "c10": (-510, 510, 0, -510, 510, 255)}


def pack_int(fmt: str, u: int, scale: bool, mode: str) -> int:
    """One f32 converted for a pack ([HW] pack-*-int, pack-*-scale, pack-round-*): clamped,
    scaled values (and c10) always to nearest even, others in the mode."""
    lo, hi, off, slo, shi, k = PACK[fmt]
    sc = scale or fmt == "c10"
    x = fractions.Fraction(u2f(flush(u))) * (k if sc else 1)
    x = max(slo if sc else lo, min(shi if sc else hi, x))
    if sc or mode == "nearest":
        n = round(x)                       # Python rounds ties to even
    else:
        n = {"down": math.floor, "up": math.ceil, "zero": math.trunc}[mode](x)
    return n + off


def place(fmt_w: int, v1: int, v2: int, mask: int, old: int) -> int:
    """Lay two lane values out under a byte mask ([HW] pack-u8-bytemask, pack-u16-bytemask)."""
    b = [mask >> i & 1 for i in range(4)]
    r = old
    for i in range(4):
        if not b[i]:
            continue
        if fmt_w == 8:
            sel = [0, b[0], b[0] ^ b[1], b[0] | b[1] | b[2]][i]
            byte = (v2 if sel else v1) & 0xFF
        else:
            sel = 0 if i < 2 else b[0]
            byte = ((v2 if sel else v1) >> (8 * (i & 1))) & 0xFF
        r = (r & ~(0xFF << 8 * i)) | byte << 8 * i
    return r & M32


def pack(fmt: str, a: int, b: int, scale: bool, mode: str, mask: int, old: int) -> int:
    v1, v2 = (pack_int(fmt, x, scale, mode) for x in (a, b))
    if fmt == "c10":
        v1, v2 = v1 & 0x3FF, v2 & 0x3FF
        return v1 | v2 << 10 | v1 << 20
    return place(8 if fmt[-1] == "8" else 16, v1, v2, mask, old)


def unpack_f32(fmt: str, v: int, scale: bool) -> int:
    """An 8- or 16-bit lane to f32, correctly rounded, -1 clamped ([HW] unpack-*-scale)."""
    w = 8 if fmt[-1] == "8" else 16
    n = v - (1 << w) if fmt[0] == "s" and v >> (w - 1) else v
    n -= 128 if fmt == "o8" else 0
    if not scale:
        return f2u(float(n))
    q = max(fractions.Fraction(n, {8: 255 if fmt == "u8" else 127,
                                   16: 65535 if fmt == "u16" else 32767}[w]), -1)
    return f2u_exact(q)


def f2u_exact(q: fractions.Fraction) -> int:
    """A rational correctly rounded to binary32 (nearest even), for normal results."""
    if q == 0:
        return 0
    s, a = (0x80000000, -q) if q < 0 else (0, q)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    e += 1 if 2 ** e <= a / 2 else 0
    e -= 1 if a < fractions.Fraction(2) ** e else 0
    m = round(a / fractions.Fraction(2) ** (e - 23))
    if m >> 24:
        m, e = m >> 1, e + 1
    return s | (e + 127) << 23 | (m & 0x7FFFFF)


def unpack_int(dfmt: str, sfmt: str, b: int) -> int:
    """Byte to integer ([HW] unpack-*): u8 <- s8 makes a negative 0x7f, u16 <- s8 0."""
    neg = b >> 7
    if sfmt == "u8":
        return b
    return {"u8": 0x7F if neg else b, "u16": 0 if neg else b,
            "s16": (b | 0xFF00) if neg else b}[dfmt]


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
                       "movrep", "shreg", "isub", "ima16", "idx", "memreg",
                       "wmask", "frep", "fdp", "movc", "movmsk", "pack", "unpack"])
    if kind in ("wmask", "frep", "fdp", "movc", "movmsk", "pack", "unpack"):
        return gen_round1(rng, kind)
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


def rand_flt(rng: random.Random) -> int:
    """A float input: mostly ordinary values, now and then a zero, a denormal or a tie."""
    return rng.choice([f2u(rng.uniform(-100, 100)), f2u(rng.uniform(-2, 2)),
                       f2u(rng.randrange(-8, 9) + 0.5), rng.choice([0, 0x80000000]),
                       rng.getrandbits(1) << 31 | rng.randrange(1, 1 << 23)])


def gen_round1(rng: random.Random, kind: str) -> Case | None:
    """The forms round 1 of the probes pinned: write masks, float repeats with .incsXYZ, dot
    products, min/max, movc, movmsk, packs and unpacks. Operands r1-r12 in, r20-r23 out."""
    R = {f"r{i}": rng.getrandbits(32) for i in range(1, 13)}
    R.update({f"r{i}": rng.getrandbits(32) for i in range(20, 24)})
    if kind == "wmask":
        op, m = rng.choice(["mov", "and", "or", "xor", "frcp"]), rng.randrange(2, 16)
        mask = "".join(c for i, c in enumerate("xyzw") if m >> i & 1)
        if op == "frcp":
            R.update({f"r{i}": f2u(rng.uniform(0.5, 100)) for i in range(1, 5)})
        cs = Case(f"{op} r20.{mask}, r1" + ("" if op in ("mov", "frcp") else ", r5"), R)
        for k in range(4):
            if m >> k & 1:
                a = R[f"r{1 + k}"]
                cs.expect[f"r{20 + k}"] = a if op == "mov" else \
                    f2u(1 / u2f(a)) if op == "frcp" else alu(op, a, R[f"r{5 + k}"])
        return cs
    R.update({f"r{i}": rand_flt(rng) for i in range(1, 13)})
    n = rng.randrange(1, 5)
    rep = f".repeat{n}" if n > 1 else ""
    inc = rng.choice([None, rng.randrange(8)])
    incs = "" if inc is None else f".incs{inc:03b}"
    adv = 7 if inc is None else inc
    if kind == "frep":
        op = rng.choice(["fmad", "fminmax", "fmaxmin"])
        n = max(n, 1)
        cs = Case(f"{op}{rep}{incs} r20, r1, r5, r9", R)
        f = {"fmad": fmad, "fminmax": fminmax, "fmaxmin": fmaxmin}[op]
        for k in range(n):
            a, b, c = (R[f"r{base + (k if adv >> sh & 1 else 0)}"]
                       for base, sh in ((1, 2), (5, 1), (9, 0)))
            cs.expect[f"r{20 + k}"] = f(a, b, c)
        return cs
    if kind == "fdp":
        n = rng.randrange(1, 5)
        rep = f".repeat{n}" if n > 1 else ""
        def vec(base, on):
            return [R[f"r{base + (k if on else 0)}"] for k in range(n)]
        if rng.random() < .5:
            cs = Case(f"fdp{rep}{incs} r20, r1, r5", R)
            cs.expect[f"r{19 + n}"] = fdot(vec(1, adv >> 1 & 1), vec(5, adv & 1))
        else:
            i = rng.choice(["i0", "i1"])
            cs = Case(f"fddp{rep}{incs} r20, {i}, r1, r5, r9", R)
            s0 = vec(1, adv >> 2 & 1)
            cs.expect[f"r{19 + n}"] = fdot(s0, vec(5, adv >> 1 & 1))
            cs.expect[i] = fdot(s0, vec(9, adv & 1))
        return cs
    R.update({f"r{i}": rng.getrandbits(32) for i in range(1, 13)})
    if kind == "movc":
        typ, test = rng.choice(["i8", "i16", "i32", "i10", "flt"]), rng.choice(["tz", "tnz", "tn",
                                                                               "tp|z"])
        R["r1"] = rng.choice([R["r1"], 0, rng.getrandbits(8) << rng.choice([0, 8, 10, 16, 20]),
                              rand_flt(rng), 0x7FC00000, 0xFF800000])
        cs = Case(f"movc.{typ}.{test} r20, r1, r2, r3", R)
        cs.expect["r20"] = movc(typ, test, R["r1"], R["r2"], R["r3"])
        return cs
    if kind == "movmsk":
        fmt, c = rng.choice(["mask", "u8", "flt", "flt16"]), rng.randrange(4)
        if fmt == "flt":
            c = 0
            R["r3"] = rng.choice([rand_flt(rng), f2u(0.5), 0x3EFFFFFF, 0x7FC00000])
        cs = Case(f"movmsk.{fmt} r20, r2, r3{f'.{c}' if c else ''}", R)
        cs.expect["r20"] = R["r2"] if movmsk(fmt, R["r3"], c) else R["r20"]
        return cs
    mode = rng.choice(["nearest", "down", "up", "zero"])
    if kind == "pack":
        fmt, sc = rng.choice(list(PACK)), rng.random() < .5
        for r in ("r1", "r2"):
            R[r] = rng.choice([f2u(rng.uniform(-1.5, 1.5)), f2u(rng.uniform(-400, 400)),
                               f2u(rng.randrange(-300, 300) + 0.5), f2u(rng.uniform(-70000, 70000))])
        m = 15 if fmt == "c10" else rng.choice([15, rng.randrange(16)])
        bm = "" if m == 15 else f".bytemask{m:04b}"
        cs = Case(f"pck{fmt}f32{'.scale' if sc else ''} r20{bm}, r1.0, r2.0, {mode}", R)
        cs.expect["r20"] = pack(fmt, R["r1"], R["r2"], sc, mode, m, R["r20"])
        return cs
    c1, c2 = rng.randrange(4), rng.randrange(4)
    if rng.random() < .5:
        fmt, sc = rng.choice(["u8", "s8", "o8", "u16", "s16"]), rng.random() < .5
        w = 8 if fmt[-1] == "8" else 16
        lane = 0 if w == 16 and c1 == 3 else (R["r1"] >> (8 * c1)) & ((1 << w) - 1)
        cs = Case(f"unpckf32{fmt}{'.scale' if sc else ''} r20, r1.{c1}, r1.0, nearest", R)
        cs.expect["r20"] = unpack_f32(fmt, lane, sc)
        return cs
    d, sf = rng.choice([("u8", "s8"), ("s16", "s8"), ("s16", "u8"), ("u16", "u8"), ("u16", "s8")])
    m = rng.choice([15, rng.randrange(16)])
    bm = "" if m == 15 else f".bytemask{m:04b}"
    cs = Case(f"unpck{d}{sf} r20{bm}, r1.{c1}, r2.{c2}, {mode}", R)
    v1, v2 = (unpack_int(d, sf, (R[r] >> (8 * c)) & 0xFF) for r, c in (("r1", c1), ("r2", c2)))
    cs.expect["r20"] = place(8 if d == "u8" else 16, v1, v2, m, R["r20"])
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
