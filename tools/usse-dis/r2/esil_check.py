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
OPS = set("= + - * / & | ^ ! << >> ASR ~ ?{ } F2D D2F I2D D2I F* F+ F- F/ F< SQRT FLOOR CEIL "
          "[4] [2] =[4] =[2] += -= DUP SWAP".split())
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


def shift_amount(t: str, regs: dict | None = None) -> int:
    """A shift amount operand: #imm, ~#imm, r or ~r ([HW] shift-immediates: ~ complements the
    32-bit amount; immediates of 32 and over act as register amounts)."""
    inv, t = t.startswith("~"), t.lstrip("~")
    v = int(t[1:], 0) if t.startswith("#") else regs[t]
    return v ^ M32 if inv else v


def idiv(a: int, b: int) -> tuple[int, int]:
    """[HW] idiv: unsigned quotient and remainder; all ones for both when b = 0."""
    return (a // b, a % b) if b else (M32, M32)


def imae(mode: str, a16: int, b16: int, c: int, rs: int = 0, coe: bool = False,
         cin: int = 0):
    """[HW] imae-modes: a16 * b16 + c (+ cin), the halves already extended; usat/ssat
    saturate (ssat signed), rs shifts the sum right, coe also returns the carry out."""
    t = a16 * b16 + c + cin
    if mode == "usat":
        return min(t, M32)
    if mode == "ssat":
        return max(-(1 << 31), min(t, (1 << 31) - 1)) & M32
    if coe:
        return t & M32, t >> 32 & 1
    return (t >> rs) & M32


def ima16x(a: int, b: int, c: int, bsel, csel, fb: str, fc: str, rs: int = 0) -> int:
    """[HW] ima16-8bit: in each lane a16 * b + c, b and c u16 or the .low/.high byte of the
    lane's half as u8, s8 or o8 (byte - 128); rs shifts the lane's full result right."""
    def lane(v, sel, fmt, h):
        x = v >> 16 * h & 0xFFFF
        if fmt == "u16":
            return x
        x = x & 0xFF if sel == "low" else x >> 8
        return x - 256 if fmt == "s8" and x & 0x80 else x - 128 if fmt == "o8" else x
    out = 0
    for h in (0, 1):
        t = (a >> 16 * h & 0xFFFF) * lane(b, bsel, fb, h) + lane(c, csel, fc, h)
        out |= ((t >> rs) & 0xFFFF) << 16 * h
    return out


def ima8(a: int, b: int, c: int, csel: list, asel: list, sat: bool) -> int:
    """[HW] ima8: per byte x + y * z from the selectors (s0-s2: a, b, c; .comp: 255 - byte),
    the alpha selectors for byte 3, wrapping or clamped to 255 with .sat."""
    out = 0
    for k in range(4):
        v = []
        for t in (asel if k == 3 else csel):
            x = (a, b, c)[int(t[1])] >> 8 * k & 0xFF
            v.append(255 - x if t.endswith(".comp") else x)
        r = v[0] + v[1] * v[2]
        out |= (min(r, 255) if sat else r & 0xFF) << 8 * k
    return out


def lanes16(a: int, b: int, c: int) -> int:
    """[HW] dual-issue-ima32: two 16-bit lanes a * b + c, wrapping."""
    return ima16(a, b, c, False)


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


# [HW] round 2 of the ISA probes (tools/usse-asm/progs/probes/{colour,intdot,fpma,efo,fmisc,
# leftovers}.toml). Colour channels are bytes, byte 3 the alpha, x standing for x / 255.

def _b(v: int, i: int) -> int:
    return v >> (8 * i) & 0xFF


def _rdiv(p: int, q: int) -> int:
    """p / q rounded to nearest, halves away from zero."""
    return (2 * p + q) // (2 * q) if p >= 0 else -((-2 * p + q) // (2 * q))


def _sat(p: int) -> int:
    return max(0, min(255, _rdiv(p, 255)))


def _sel(name: str, s0: int, s1: int, s2: int, b: int) -> int:
    """A sop3/lrp/sop2wm selector in byte b (b = 3: the alpha reading)."""
    comp = name.endswith(".comp")
    n = name[:-5] if comp else name
    if n == "asat":
        if b == 3:
            return 255
        a1, a2 = _b(s1, 3), _b(s2, 3)
        return min(255 - a1, a2) if comp else min(a1, 255 - a2)
    v = {"zero": 0, "one": 255, "s0": _b(s0, b), "s1": _b(s1, b), "s2": _b(s2, b),
         "s0a": _b(s0, 3), "s1a": _b(s1, 3), "s2a": _b(s2, 3)}[n]
    return 255 - v if comp else v


def _blend(op: str, x1: int, f1: int, x2: int, f2: int) -> int:
    return x1 * f1 + x2 * f2 if op == "add" else x1 * f1 - x2 * f2


def _sop2_byte(c: list[str], s1: int, s2: int, b: int, neg: bool) -> int:
    comp = c[0] == "comp"
    f1, f2, op = c[comp:]
    x1, x2 = _b(s1, b), _b(s2, b)
    n1, n2 = f1.removesuffix(".comp"), f2.removesuffix(".comp")
    scale = 2 if n1 == "s2scale" else 1
    g1 = (255 - x2 if f1.endswith(".comp") else x2) if n1 == "s2scale" else _sel(f1, 0, s1, s2, b)
    g2 = (255 if f2.endswith(".comp") else 0) if n2 == "zeros" else _sel(f2, 0, s1, s2, b)
    if op in ("min", "max"):
        d = 255 * scale * (min(x1, x2) if op == "min" else max(x1, x2))
    else:
        if comp:
            x1 = 255 - x1
        if n2 == "zeros":
            x2 -= 128
        d = scale * _blend(op, x1, g1, x2, g2)
    return _sat(-d if neg else d)


def colour(text: str, s0: int, s1: int, s2: int, old: int = 0) -> int:
    """sop2/sop3/lrp1/lrp2/sop2wm ([HW] sop2-*, sop3-*, sop2wm)."""
    mn = text.split()[0].split(".")[0]
    head, _, tail = text.partition("+")
    akind, _, al = tail.partition(" ")
    c = head.split(", ")[3 if mn in ("sop2", "sop2wm") else 4:]
    a = al.split(", ") if al else []
    if mn == "sop2wm":
        mask = int(head.split(".bytemask")[1][:4], 2)
        v = []
        for b in range(4):
            op = c[2] if b < 3 else c[3]
            x1, x2 = _b(s1, b), _b(s2, b)
            if op in ("min", "max"):
                v.append(min(x1, x2) if op == "min" else max(x1, x2))
            else:
                v.append(_sat(_blend(op, x1, _sel(c[0], 0, s1, s2, b), x2, _sel(c[1], 0, s1, s2, b))))
        return sum((v[b] if mask >> b & 1 else _b(old, b)) << (8 * b) for b in range(4))
    if mn == "sop2":
        neg = a[-1] == "neg"
        v = [_sop2_byte(c, s1, s2, b, False) for b in range(3)]
        v.append(_sop2_byte(a[:-1] if neg else a, s1, s2, 3, neg))
        return sum(x << (8 * b) for b, x in enumerate(v))
    v = []
    for b in range(3):
        if mn == "sop3":
            neg = c[-1] == "neg"
            f1, f2 = (c[1], c[0]) if neg else (c[0], c[1])
            d = _blend(c[2], _b(s1, b), _sel(f1, s0, s1, s2, b), _b(s2, b), _sel(f2, s0, s1, s2, b))
            v.append(_sat(-d if neg else d))
        elif mn == "lrp1":
            t = _b(s0, b if c[2] == "s0" else 3)
            v.append(_sat(_sel(c[0], s0, s1, s2, b) * (255 - t) + _sel(c[1], s0, s1, s2, b) * t))
        else:
            v.append(_sat(_sel(c[0], s0, s1, s2, b) * _sel(c[2], s0, s1, s2, b)
                          + _sel(c[1], s0, s1, s2, b) * _sel(c[3], s0, s1, s2, b)))
    a1, a2 = _b(s1, 3), _b(s2, 3)
    if akind == "asop":
        v.append(_sat(_blend(a[2], a1, _sel(a[0], s0, s1, s2, 3), a2, _sel(a[1], s0, s1, s2, 3))))
    elif akind == "arsop":
        v.append(_sat(-_blend(a[1], a1, _sel(c[1], s0, s1, s2, 3), a2, _sel(a[0], s0, s1, s2, 3))))
    else:
        t = _b(s0, 3)
        v.append(_sat(_sel(a[0], s0, s1, s2, 3) * (255 - t) + _sel(a[1], s0, s1, s2, 3) * t))
    return sum(x << (8 * b) for b, x in enumerate(v))


def idot(text: str, s1: int, s2: int, s0: int = 0) -> int:
    """u8dot3/4, u16dot3/4 and off ([HW] intdot-lanes, intdot-alpha)."""
    head, _, tail = text.partition("+")
    mn = head.split()[0].split(".")[0]
    ops = head.split(" ", 1)[1].split(", ")
    cx = int(ops[1][2:])
    ax = int(ops[2][2:]) if ops[2].startswith("Ax") else cx
    x, y = ops[-2], ops[-1]
    if x.endswith(".comp"):
        s1 ^= M32
    if y.endswith(".comp"):
        s2 ^= M32
    n, off = (4 if "dot4" in mn else 3), mn.endswith("off")
    k = 128 if off else 0
    S = sum((_b(s1, i) - k) * (_b(s2, i) - k) for i in range(n))
    if mn.startswith("u16"):
        lim = 0x7FFF
        h = max(-lim if off else 0, min(lim, _rdiv(cx * S * 32767, 255 * 255))) & 0xFFFF
        return h | h << 16
    col, alpha = _sat(cx * S), _sat(ax * S)
    if tail:
        akind, _, al = tail.partition(" ")
        a = al.split(", ")
        x1, x2 = (s1 ^ M32 if x.endswith(".comp") else s1), (s2 ^ M32 if y.endswith(".comp") else s2)

        def sv(t):
            v = {"zero": 0, "s1a": _b(x1, 3), "s2a": _b(x2, 3)}[t.removesuffix(".comp")]
            return 255 - v if t.endswith(".comp") else v
        if akind in ("aadd", "asub"):
            p = sv(a[1]) * sv(a[2]) + (1 if akind == "aadd" else -1) * sv(a[3]) * sv(a[4])
            alpha = _sat(int(a[0][2:]) * p)
        else:
            t, X, Y = _b(s0, 3), sv(a[1]), sv(a[2])
            if akind == "aintrp2":
                X = 255 - X
            alpha = _sat(X * (255 - t) + Y * t)
    return col | col << 8 | col << 16 | alpha << 24


def fpma(text: str, s0: int, s1: int, s2: int) -> int:
    """c0 + c1 c2 per channel ([HW] fpma)."""
    ops = text.split(" ", 1)[1].split(", ")
    S = {"s0": s0, "s1": s1, "s2": s2}
    neg = ops[1].endswith(".neg")

    def sel(t, b):
        n = t.removesuffix(".comp")
        v = _b(S[n[:2]], 3 if n.endswith("a") else b)
        if n.startswith("s0") and neg:
            v = -v
        return 255 - v if t.endswith(".comp") else v
    out = 0
    for b in range(4):
        c = ops[4:7] if b < 3 else ops[7:10]
        out |= _sat(255 * sel(c[0], b) + sel(c[1], b) * sel(c[2], b)) << (8 * b)
    return out


def _fmul(x: int, y: int) -> int:
    return flush(f2u(u2f(flush(x)) * u2f(flush(y))))


def _fadd(x: int, y: int, sub: bool = False) -> int:
    return flush(f2u(u2f(x) - u2f(y) if sub else u2f(x) + u2f(y)))


def fmisc(op: str, a: int, b: int, c: int = 0) -> int:
    """fadm = a.c + b.c, fmsa = a.a + b.c, fsubflr = a - floor(b) ([HW] fadm-fmsa-fsubflr)."""
    if op == "fsubflr":
        x = u2f(flush(b))
        return flush(f2u(u2f(flush(a)) - (x if x == 0 or not math.isfinite(x) else math.floor(x))))
    return _fadd(_fmul(a, c) if op == "fadm" else _fmul(a, a), _fmul(b, c))


def efo(text: str, regs: dict) -> dict:
    """efo ([HW] efo-dataflow, efo-rounding): the registers it writes. .gpiext is not modelled:
    one pixel in four sees other values (efo-gpiext-instances)."""
    m = re.match(r"efo(\S*) (\w+)= (\w+), (!?)i0 = (\w+), (!?)i1 = (\w+), a0=(\w+)([+-])(\w+),? "
                 r"a1=(-?)(\w+)\+(\w+), m0=(\w+)\*(\w+), m1=(\w+)\*(\w+), (\S+), (\S+), (\S+)$", text)
    (fl, d, ds, n0, w0, n1, w1, x0, o0, y0, g1, x1, y1, p0, q0, p1, q1, *src) = m.groups()

    def rd(t):
        u = regs[t.removesuffix(".neg")]
        return u ^ 0x80000000 if t.endswith(".neg") else u
    v = {f"src{i}": flush(rd(t)) for i, t in enumerate(src)}
    v.update(i0=flush(regs["i0"]), i1=flush(regs["i1"]))
    v["m0"], v["m1"] = _fmul(v[p0], v[q0]), _fmul(v[p1], v[q1])
    v["a0"] = _fadd(v[x0], v[y0], o0 == "-")
    v["a1"] = _fadd(v[x1] ^ (0x80000000 if g1 else 0), v[y1])
    out = {d: regs[ds] if ds in ("i0", "i1") else v[ds]}   # d = i0/i1 moves the old bits
    if not n0:
        out["i0"] = v[w0]
    if not n1:
        out["i1"] = v[w1]
    return out


def statomic(x: int, y: int) -> int:
    """Two bytes read as s8, negatives as 0, in both halves ([HW] statomic)."""
    x, y = (0 if x & 0x80 else x & 0x7F), (0 if y & 0x80 else y & 0x7F)
    h = x | y << 8
    return h | h << 16


def smsk(fmt: str, s0: int, s2: int, c: int) -> bool:
    """movmsk.<fmt>.smsk: s2's test and bit 0 of s0, except for .mask ([HW] movmsk-smsk2)."""
    return movmsk(fmt, s2, c) and (fmt == "mask" or bool(s0 & 1))


def rlp(v: int, p: int) -> tuple[int, int]:
    """rlp d, s, pN ([HW] rlp): d = s << 1 | pN, then pN = bit 31 of s."""
    return ((v << 1) | p) & M32, v >> 31


def test_chan(test: str, chan: str, op: str, v: int) -> int:
    """A test on a channel ([HW] test-channels): v is the 32-bit result; the 16-bit ops have
    their halves as channels 0 and 2; a channel without a lane tests as a positive non-zero
    value; andall/orall combine all four, and02/or02 channels 0 and 2."""
    lanes16 = op.startswith("isub")
    ch = [test_pred(test, v & 0xFFFF, bits=16) if lanes16 else test_pred(test, v)]
    absent = test_pred(test, 1)
    ch += [absent, test_pred(test, v >> 16, bits=16) if lanes16 else absent, absent]
    if chan.startswith("chan"):
        return ch[int(chan[4])]
    return {"andall": int(all(ch)), "orall": int(any(ch)), "and02": ch[0] & ch[2],
            "or02": ch[0] | ch[2]}[chan]


def adif(a: int, b: int) -> int:
    """adif ([HW] video-adif): |a - b| per byte."""
    return sum(abs(_b(a, i) - _b(b, i)) << (8 * i) for i in range(4))


def adifsum(a: int, b: int, s0: int, high: bool) -> int:
    """adifsum ([HW] video-adif): the four byte differences plus a 16-bit half of s0."""
    return (sum(abs(_b(a, i) - _b(b, i)) for i in range(4)) + ((s0 >> 16) if high else s0 & 0xFFFF)) & M32


def c10u16(v: int) -> int:
    """unpcku16c10/unpcks16c10 ([HW] unpack-c10-16): lane 0's bits 4, 6, 8 into bits 15,
    1-14, 0."""
    return (0x8000 if v >> 4 & 1 else 0) | (0x7FFE if v >> 6 & 1 else 0) | (v >> 8 & 1)


DUAL_PAIRS = {"mov": "fmad fmsa fmaxmin fsubflr fadd fsub fmul frcp frsq mov",
              "fmad": "fmad fsubflr fadd fsub fmul frcp frsq mov",
              "ima32": "fmad fmsa fsubflr fadd fsub fmul frcp frsq mov",
              "fadm": "fsubflr fadd fsub frcp frsq mov", "fmsa": "fsubflr fadd fsub frcp frsq mov",
              "fssq": "fsubflr fadd fsub frcp frsq mov", "fminmax": "fmul frcp frsq mov"}


def _dual_op(op: str, v: list[int]) -> int:
    if op == "mov":
        return v[0]
    if op == "fmad":
        return fmad(*v)
    if op in ("fadm", "fmsa", "fsubflr"):
        return fmisc(op, *v)
    if op == "fminmax":
        return fminmax(*v)
    if op == "fmaxmin":
        return fmaxmin(*v)
    if op == "ima32":
        return (v[0] * v[1] + v[2]) & M32
    if op == "fssq":
        return _fadd(_fmul(v[0], v[0]), _fmul(v[1], v[1]))
    if op in ("fadd", "fsub"):
        return _fadd(flush(v[0]), flush(v[1]), op == "fsub")
    if op == "fmul":
        return _fmul(v[0], v[1])
    x = u2f(flush(v[0]))
    return f2u(1 / x if op == "frcp" else 1 / math.sqrt(x))


def _dual_shared(pop: str, pv_in: list[int], pv: int, sop: str, sv_in: list[int]):
    """[HW] dual-issue-ima32, dual-issue-shared: what a secondary sharing the primary's unit
    computes, or None for a pair that co-issues cleanly."""
    two = pop in ("fadm", "fmsa", "fssq")
    if sop == "ima32" and (pop in ("mov", "fmad") or two):
        b = sv_in[1] if pop == "mov" else pv_in[0] if pop == "fssq" else pv_in[1]
        return lanes16(sv_in[0], b, sv_in[2])
    if two and sop in ("fmul", "fmad"):
        x, y = (pv_in[1], pv_in[1]) if pop == "fssq" else (pv_in[1], pv_in[2])
        p2 = _fmul(x, y)
        return _fadd(p2, flush(sv_in[2])) if sop == "fmad" else p2
    if (sop == "fmsa" and pop in ("fmad", "fadm", "fmsa")) or (sop == pop == "ima32"):
        return pv
    if pop == "fminmax" and sop in ("fadd", "fsub", "fmad"):
        return _fadd(minmax(pv_in[0], pv_in[1], True), pv_in[2], True)
    return None


def dual(text: str, regs: dict) -> dict:
    """A dual-issue word ([HW] dual-issue): both operations read before either writes, the
    secondary wins a shared destination, .repeat2 runs both twice (the primary's operands
    advance). Returns the registers written, with their final values."""
    prim, sec = text.split("+")
    pw, pa = prim.split(" ", 1)
    sw, sa = sec.split(" ", 1)
    pa, sa = pa.split(", "), sa.split(", ")
    R, out = dict(regs), {}
    for k in range(2 if ".repeat2" in pw else 1):
        def step(t):
            m = re.fullmatch(r"r(\d+)(\.neg)?", t)
            return f"r{int(m.group(1)) + k}{m.group(2) or ''}" if m else t

        def rd(t):
            return R[t[:-4]] ^ 0x80000000 if t.endswith(".neg") else R[t]
        ops = [step(t) for t in pa]
        pb = pw.split(".")[0]
        pv = _dual_op(pb, [rd(t) for t in ops[1:]])
        sv = _dual_shared(pb, [rd(t) for t in ops[1:]], pv, sw, [rd(t) for t in sa[1:]])
        if sv is None:
            sv = _dual_op(sw, [rd(t) for t in sa[1:]])
        R[ops[0]] = pv
        R[sa[0]] = sv
        out[ops[0]] = pv
        out[sa[0]] = sv
    return out


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
                       "wmask", "frep", "fdp", "movc", "movmsk", "pack", "unpack",
                       "colour", "dot", "fpma", "efo", "fmisc", "statomic", "smsk",
                       "rlp", "chan", "adif", "c10u16", "crep", "dual",
                       "shimm", "pn", "idiv", "imae4", "ima16x", "ima8", "dual4"])
    if kind in ("rlp", "chan", "adif", "c10u16", "crep", "dual"):
        return gen_round3(rng, kind)
    if kind in ("shimm", "pn", "idiv", "imae4", "ima16x", "ima8", "dual4"):
        return gen_round4(rng, kind)
    if kind in ("wmask", "frep", "fdp", "movc", "movmsk", "pack", "unpack"):
        return gen_round1(rng, kind)
    if kind in ("colour", "dot", "fpma", "efo", "fmisc", "statomic", "smsk"):
        return gen_round2(rng, kind)
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


def _c(rng: random.Random, names: list[str]) -> str:
    return rng.choice(names) + rng.choice(["", ".comp"])


S3 = ["zero", "asat", "s1", "s1a", "s0", "s0a", "s2", "s2a"]


def gen_round2(rng: random.Random, kind: str) -> Case | None:
    """The forms round 2 of the probes pinned: colour operations, integer dot products,
    fpma, efo, fadm/fmsa/fsubflr, statomic, movmsk .smsk. Inputs r1-r3 (and r9), out r20."""
    R = {f"r{i}": rng.choice([rng.getrandbits(32), rng.getrandbits(32), M32, 0, 0x80808080])
         for i in (1, 2, 3, 9, 20)}
    s0, s1, s2 = R["r1"], R["r2"], R["r3"]
    if kind == "colour":
        mn = rng.choice(["sop2", "sop3", "lrp1", "lrp2", "sop2wm"])
        op4, op2 = ["add", "sub", "min", "max"], ["add", "sub"]
        if mn == "sop2":
            t = (f"sop2 r20, r2, r3, {rng.choice(['', 'comp, '])}"
                 f"{_c(rng, ['zero', 's1', 's2', 's1a', 's2a', 'asat', 's2scale'])}, "
                 f"{_c(rng, ['zero', 's1', 's2', 's1a', 's2a', 'asat', 'zeros'])}, {rng.choice(op4)}"
                 f"+asop2 {rng.choice(['', 'comp, '])}{_c(rng, ['zero', 's1a', 's2a', 's2scale'])}, "
                 f"{_c(rng, ['zero', 's1a', 's2a', 'zeros'])}, {rng.choice(op4)}"
                 f"{rng.choice(['', ', neg'])}")
            s0, s1, s2 = 0, R["r2"], R["r3"]
        elif mn == "sop2wm":
            W = ["zero", "asat", "s1", "s1a", "s2", "s2a"]
            t = (f"sop2wm r20.bytemask{rng.randrange(1, 16):04b}, r2, r3, {_c(rng, W)}, {_c(rng, W)}, "
                 f"{rng.choice(op4)}, {rng.choice(op4)}")
            s0, s1, s2 = 0, R["r2"], R["r3"]
        elif mn == "sop3":
            y = _c(rng, S3)
            t = rng.choice([
                f"sop3 r20, r1, r2, r3, {_c(rng, S3)}, {y}, {rng.choice(op2)}+asop "
                f"{_c(rng, ['zero', 's0a', 's1a', 's2a'])}, {y}, {rng.choice(op2)}",
                f"sop3 r20, r1, r2, r3, {_c(rng, S3)}, {y}, {rng.choice(op2)}+arsop "
                f"{_c(rng, ['zero', 's0a', 's1a', 's2a'])}, {rng.choice(op2)}",
                f"sop3 r20, r1, r2, r3, {_c(rng, S3)}, {y}, {rng.choice(op2)}{rng.choice(['', ', neg'])}"
                f"+alrp {_c(rng, ['zero', 's1a'])}, {_c(rng, ['zero', 's2a'])}"])
        elif mn == "lrp1":
            t = (f"lrp1 r20, r1, r2, r3, {_c(rng, ['zero', 's1'])}, {_c(rng, ['zero', 's2'])}, "
                 f"{rng.choice(['s0', 's0a'])}+asop {_c(rng, S3)}, {_c(rng, S3)}, {rng.choice(op2)}")
        else:
            t = (f"lrp2 r20, r1, r2, r3, {_c(rng, S3)}, {_c(rng, S3)}, "
                 f"{rng.choice(['s1, s2', 's1, s2.comp', 'one, s2', 's1.comp, s2'])}+alrp "
                 f"{_c(rng, ['zero', 's1a'])}, {_c(rng, ['zero', 's2a'])}")
        cs = Case(t, R)
        cs.expect["r20"] = colour(t, s0, s1, s2, R["r20"])
        return cs
    if kind == "dot":
        mn = rng.choice(["u8dot3", "u8dot4", "u16dot3", "u16dot4"]) + rng.choice(["", "off"])
        x, y = "r2" + rng.choice(["", ".comp"]), "r3" + rng.choice(["", ".comp"])
        cx, ax = rng.choice([1, 2, 4, 8]), rng.choice([1, 2, 4, 8])
        if mn.startswith("u8dot3") and rng.random() < .6:
            al = rng.choice([f"aadd Ax{ax}, {_c(rng, ['zero', 's1a', 's2a'])}, s1a{rng.choice(['', '.comp'])}, "
                             f"{_c(rng, ['zero', 's1a', 's2a'])}, s2a",
                             f"asub Ax{ax}, {_c(rng, ['zero', 's1a', 's2a'])}, s1a{rng.choice(['', '.comp'])}, "
                             f"{_c(rng, ['zero', 's1a', 's2a'])}, s2a",
                             f"aintrp{rng.choice('12')} r9, {rng.choice(['zero', 's1a', 's2a'])}, "
                             f"{_c(rng, ['zero', 's1a', 's2a'])}"])
            t = f"{mn} r20, Cx{cx}, {x}, {y}+{al}"
        elif mn.startswith("u16dot3"):
            t = f"{mn} r20, Cx{cx}, {x}, {y}"
        else:
            t = f"{mn} r20, Cx{cx}, Ax{ax}, {x}, {y}"
        cs = Case(t, R)
        cs.expect["r20"] = idot(t, R["r2"], R["r3"], R["r9"])
        return cs
    if kind == "fpma":
        t = (f"fpma{rng.choice(['', '.sat'])} r20, r1{rng.choice(['', '.neg'])}, r2, r3, "
             f"{_c(rng, ['s0', 's1', 's0a', 's1a'])}, {_c(rng, ['s1', 's1a'])}, {_c(rng, ['s2', 's2a'])}, "
             f"{_c(rng, ['s0a', 's1a'])}, s1a{rng.choice(['', '.comp'])}, s2a{rng.choice(['', '.comp'])}")
        cs = Case(t, R)
        cs.expect["r20"] = fpma(t, s0, s1, s2)
        return cs
    F = {r: rand_flt(rng) for r in ("r1", "r2", "r3", "i0", "i1", "i2")}
    R.update(F)
    if kind == "fmisc":
        op = rng.choice(["fadm", "fmsa", "fsubflr"])
        t = f"{op} r20, r1, r2{'' if op == 'fsubflr' else ', r3'}"
        cs = Case(t, R)
        cs.expect["r20"] = fmisc(op, R["r1"], R["r2"], R["r3"])
        return cs
    if kind == "efo":
        iw = rng.choice(["i0 = a0, i1 = a1", "i0 = a1, i1 = a0", "i0 = m0, i1 = m1", "i0 = a0, i1 = m1"])
        iw = ", ".join(rng.choice(["", "!"]) + p for p in iw.split(", "))
        sub, neg = rng.choice("+-"), rng.choice(["", "-"])
        ad = rng.choice([f"a0=m0{sub}m1, a1={neg}i1+i0", f"a0=m0{sub}src0, a1={neg}i1+i0",
                         f"a0=m0{sub}i0, a1={neg}i1+m1", f"a0=src0{sub}src2 a1={neg}src1+src2"])
        ms = rng.choice(["m0=src0*src2, m1=src1*src2", "m0=src2*src1, m1=src2*src2",
                         "m0=src1*src0, m1=src2*src2", "m0=src1*i0, m1=src2*i1"])
        srcs = ", ".join(f"r{i}{rng.choice(['', '.neg'])}" for i in (1, 2, 3))
        t = (f"efo r20= {rng.choice(['a0', 'a1', 'i0', 'i1'])}, {iw}, "
             f"{ad}, {ms}, {srcs}")
        cs = Case(t, R)
        cs.expect.update(efo(t, R))
        return cs
    if kind == "statomic":
        c1, c2 = rng.randrange(4), rng.randrange(4)
        t = f"statomic r20, r2{f'.{c1}' if c1 else ''}, r3{f'.{c2}' if c2 else ''}, {rng.choice(['zero', 'up'])}"
        cs = Case(t, R)
        cs.expect["r20"] = statomic(_b(R["r2"], c1), _b(R["r3"], c2))
        return cs
    fmt, c = rng.choice([("mask", 1), ("u8", 1), ("flt", 0), ("flt16", 1)])
    R["r1"] = rng.choice([0, 1, 2, 3, M32])
    R["r3"] = rng.choice([0x00000100, 0x00008000, 0x0000FF00, f2u(1.0), f2u(0.25), 0x003C0000])
    mn = {"mask": "movmsk.mask.smsk", "u8": "movmsk.smsk.u8", "flt": "movmsk.flt.smsk",
          "flt16": "movmsk.flt16.smsk"}[fmt]
    t = f"{mn} r20, r1, r2, r3{f'.{c}' if c else ''}"
    cs = Case(t, R)
    cs.expect["r20"] = R["r2"] if smsk(fmt, R["r1"], R["r3"], c) else R["r20"]
    return cs


def gen_round3(rng: random.Random, kind: str) -> Case | None:
    """The forms round 3 of the probes pinned: rlp, test channels, adif/adifsum, the c10 to
    16-bit unpacks, colour/fpma/dot repeats and dual issue. Inputs r1-r6, i0-i2, out r20."""
    R = {f"r{i}": rng.choice([rng.getrandbits(32), rng.getrandbits(32), M32, 0, 0x80808080])
         for i in range(1, 7)}
    R.update({f"p{i}": rng.randrange(2) for i in range(4)})
    if kind == "rlp":
        n = rng.randrange(8)
        cs = Case(f"rlp r20, r1, p{n}", R)
        cs.expect["r20"], cs.expect[f"p{n % 4}"] = rlp(R["r1"], R[f"p{n % 4}"])
        return cs
    if kind == "chan":
        op = rng.choice(["and", "or", "xor", "iadd32", "isub16"])
        t = rng.choice("tnp") + rng.choice("ao") + rng.choice(["t", "z", "nz"])
        ch = rng.choice(["chan0", "chan1", "chan2", "chan3", "andall", "orall", "and02", "or02"])
        if rng.random() < .5:
            R["r2"] = rng.choice([0, R["r1"], R["r1"] & 0xFFFF0000, R["r1"] & 0xFFFF])
        v = sub16(R["r1"], R["r2"]) if op == "isub16" else alu(op, R["r1"], R["r2"])
        cs = Case(f"{op}.test{t}.{ch} !c0, p3, r1, r2", R)
        cs.expect["p3"] = test_chan(t, ch, op, v)
        return cs
    if kind == "adif":
        if rng.random() < .5:
            cs = Case("adif r20, r1, r2", R)
            cs.expect["r20"] = adif(R["r1"], R["r2"])
        else:
            h = rng.random() < .5
            cs = Case(f"adifsum r20, r3.{'high' if h else 'low'}, r1, r2", R)
            cs.expect["r20"] = adifsum(R["r1"], R["r2"], R["r3"], h)
        return cs
    if kind == "c10u16":
        a, b = rng.sample(range(3), 2)
        cs = Case(f"{rng.choice(['unpcku16c10', 'unpcks16c10'])}{rng.choice(['', '.scale'])} "
                  f"r20, r1.{a}, r1.{b}, {rng.choice(['nearest', 'zero'])}", R)
        cs.expect["r20"] = c10u16(R["r1"] & 0x3FF)
        return cs
    if kind == "crep":
        n = rng.randrange(2, 4)
        mn = rng.choice(["sop2", "fpma", "u8dot3", "u16dot4"])
        if mn == "sop2":
            t = "sop2 r20, r1, r2, s1, s2, add+asop2 s1a, s2a, add"
        elif mn == "fpma":
            t = "fpma r20, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a"
        else:
            t = f"{mn} r20, Cx1, Ax1, r1, r2"
        w = t.split(" ", 1)
        cs = Case(f"{w[0]}.repeat{n} {w[1]}", R)
        for k in range(n):
            s0, s1, s2 = R[f"r{1 + k}"], R[f"r{2 + k}"], R[f"r{3 + k}"]
            cs.expect[f"r{20 + k}"] = (colour(t, 0, s0, s1) if mn == "sop2" else
                                       fpma(t, s0, s1, s2) if mn == "fpma" else idot(t, s0, s1))
        return cs
    F = {r: rand_flt(rng) for r in ("r1", "r2", "r3", "r4", "i0", "i1", "i2")}
    R.update(F)
    pop = rng.choice(list(DUAL_PAIRS))
    sop = rng.choice(DUAL_PAIRS[pop].split())
    pd = rng.choice(["r20", "r20", "i0", "i1", "i2"])
    if pop in ("fminmax", "fmsa", "fadm") and pd.startswith("i"):
        pd = "r20"
    nsrc = {"mov": 1, "fssq": 2}.get(pop, 3)
    ps = ["r1", "r2", "r3"][:nsrc]
    if pop == "fmad" and rng.random() < .3:
        ps[rng.randrange(3)] = rng.choice(["i0", "i1", "i2"])
    sn = {"fmad": 3, "fmsa": 3, "fmaxmin": 3, "frcp": 1, "frsq": 1, "mov": 1}.get(sop, 2)
    sd = rng.choice(["i0", "i1", "i2"])
    ss = [rng.choice(["i0", "i1", "i2"]) for _ in range(sn)]
    if sn == 3:
        ss[0] = sd                  # a three-source secondary's destination is its source 0
    if sop in ("frcp", "frsq"):
        R[ss[0]] = f2u(abs(u2f(R[ss[0]])) + 0.5)
    rep = rng.random() < .25 and pd == "r20" and not any(s.startswith("i") for s in ps)
    t = f"{pop}{'.repeat2' if rep else ''} {', '.join([pd] + ps)}+{sop} {', '.join([sd] + ss)}"
    # the orders the ESIL takes: no write that the other operation reads
    if pd in ss or (sd in ps) or (rep and pd == sd) or (sop == "fmaxmin" and ss[1] == sd):
        return None
    cs = Case(t, R)
    cs.expect.update(dual(t, R))
    return cs


def gen_round4(rng: random.Random, kind: str) -> Case | None:
    """The forms round 4 of the probes pinned: shift immediates and ~ amounts, Pn, idiv,
    imae's modes, ima16's 8-bit formats, ima8 and the dual-issue pairs sharing a unit.
    Inputs r1-r6, i0-i2, out r20 (r21)."""
    R = {f"r{i}": rand_val(rng) for i in range(1, 7)}
    R.update({f"p{i}": rng.randrange(2) for i in range(4)})
    if kind == "shimm":
        op = rng.choice(["shl", "shr", "asr", "rol"])
        amt = rng.choice([f"#{rng.randrange(32, 128)}", f"~#{rng.randrange(0, 64)}", "~r2"])
        cs = Case(f"{op} r20, r1, {amt}", R)
        cs.expect["r20"] = alu(op, R["r1"], shift_amount(amt, R))
        return cs
    if kind == "pn":
        R["r20"] = rng.getrandbits(32)
        cs = Case("Pn mov r20, r1", R)
        cs.expect["r20"] = R["r20"]
        return cs
    if kind == "idiv":
        if rng.random() < .2:
            R["r2"] = 0
        cs = Case("idiv r20, r1, r2, drc0", R)
        cs.expect["r20"], cs.expect["r21"] = idiv(R["r1"], R["r2"])
        return cs
    if kind == "imae4":
        mode = rng.choice(["u", "s", "usat", "ssat"])
        ha, hb = rng.choice(["low", "high"]), rng.choice(["low", "high"])
        sign = mode in ("s", "ssat")

        def half(v, h):
            x = v >> 16 if h == "high" else v & 0xFFFF
            return x - 0x10000 if sign and x & 0x8000 else x
        a16, b16 = half(R["r1"], ha), half(R["r2"], hb)
        ctype = rng.choice(["u32", "z16"] if mode == "usat" else ["u32", "u16", "z16"])
        hc = rng.choice(["low", "high"])
        if ctype == "u32":
            c, ct = R["r3"], "r3"
            if mode == "ssat":
                c = c - (1 << 32) if c >> 31 else c
        else:
            x = R["r3"] >> 16 if hc == "high" else R["r3"] & 0xFFFF
            c, ct = (x - 0x10000 if ctype == "u16" and x & 0x8000 else x), f"r3.{hc}"
        extra, rs, cin, coe = "", 0, 0, False
        if mode == "u":
            r = rng.random()
            if r < .25:
                rs = rng.randrange(1, 8)
            elif r < .5:
                coe, extra = True, ", coe, i1"
            elif r < .75:
                R["i0"] = rng.randrange(2)
                cin, extra = R["i0"], ", cie, i0"
        cs = Case(f"imae.{mode}{f'.rs{rs}' if rs else ''} r20, r1.{ha}, r2.{hb}, {ct}, "
                  f"{ctype}{extra}", R)
        v = imae(mode, a16, b16, c, rs=rs, coe=coe, cin=cin)
        if coe:
            cs.expect["r20"], cs.expect["i1"] = v
        else:
            cs.expect["r20"] = v & M32
        return cs
    if kind == "ima16x":
        fb, fc = rng.choice(["u16", "u8", "s8", "o8"]), rng.choice(["u16", "u8", "s8", "o8"])
        bs = None if fb == "u16" else rng.choice(["low", "high"])
        cs_ = None if fc == "u16" else rng.choice(["low", "high"])
        b = "r2" + (f".{bs}" if bs else "")
        c = "r3" + (f".{cs_}" if cs_ else "")
        cs = Case(f"ima16.u r20, r1, {b}, {c}, {fb}, {fc}", R)
        cs.expect["r20"] = ima16x(R["r1"], R["r2"], R["r3"], bs, cs_, fb, fc)
        return cs
    if kind == "ima8":
        # the encodable selectors: x is s0 or s1, y s1, z s2, each maybe complemented
        csel = [n + rng.choice(["", ".comp"]) for n in (rng.choice(["s0", "s1"]), "s1", "s2")]
        asel = [n + rng.choice(["", ".comp"]) for n in (rng.choice(["s0a", "s1a"]), "s1a", "s2a")]
        sat = rng.random() < .5
        cs = Case(f"ima8{'.sat' if sat else ''} r20, r1, r2, r3, {', '.join(csel + asel)}", R)
        cs.expect["r20"] = ima8(R["r1"], R["r2"], R["r3"], csel, [a.replace("a", "", 1)
                                                                 for a in asel], sat)
        return cs
    F = {r: rand_flt(rng) for r in ("r1", "r2", "r3", "i0", "i1", "i2")}
    R.update(F)
    pair = rng.choice([("mov r20, r1", "ima32 i0, i0, i1, i2"), ("fmad r20, r1, r2, r3", "ima32 i0, i0, i1, i2"),
                       ("fadm r20, r1, r2, r3", "fmul i0, i1, i2"), ("fmsa r20, r1, r2, r3", "fmad i0, i0, i1, i2"),
                       ("fssq r20, r2, r3", "fmad i0, i0, i1, i2"), ("fmad r20, r1, r2, r3", "fmsa i0, i0, i1, i2"),
                       ("fminmax r20, r1, r2, r3", "fadd i0, i1, i2"), ("ima32 r20, r1, r2, r3", "ima32 i0, i0, i1, i2")])
    t = "+".join(pair)
    if pair[0].startswith(("mov", "ima32")) and rng.random() < .5:
        R.update({r: rng.getrandbits(32) for r in ("r1", "r2", "r3", "i0", "i1", "i2")})
    cs = Case(t, R)
    cs.expect.update(dual(t, R))
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
