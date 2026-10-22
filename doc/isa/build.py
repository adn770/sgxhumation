#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Build the ISA documents from the snippet library (doc/isa/README.md).

A book (books/*.book) names its output, its options and, in order, the snippets
(content/**.md) and generated sections it is made of. Snippets are handwritten Markdown; a
line `<!-- @name args -->` in a snippet, or `@name args` in a manifest, is replaced by
generated text read from the form tables of the disassemblers and assemblers, which
formdump.c dumps. The outputs are committed; test_build.py rebuilds them and compares.

    usage: build.py [--check] [BOOK ...]

Directives:
    @group ISA [class=A-B,C] [pat=REGEX] [not=REGEX]   the forms of an opcode group: list,
                                                       mnemonics, a bit diagram and the
                                                       fields of every word layout
    @appendix ISA                                      every form of the table
    @classmap ISA                                      opcode classes and their mnemonics
    @example ISA TEXT                                  assemble TEXT and show its fields
    @prog FILE [match=REGEX]                           lines of tools/usse-asm/progs/FILE
    @banks ISA pat=REGEX field=N                       a register operand's banks
    @values ISA pat=REGEX field=N                      every value of a field
    @probes                                            the book's probe snippets, each tagged
                                                       [HW] (measured) or [open] (not probed)
"""
from __future__ import annotations

import argparse
import dataclasses
import itertools
import json
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import textwrap
from typing import Callable

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CONTENT = HERE / "content"
BOOKS = HERE / "books"
PROGS = ROOT / "tools" / "usse-asm" / "progs"
WIDTH = 100
# A probe snippet (content/*/probe/*.md) carries exactly one of these marks.
PROBE_MARKS = {
    "measured": "Measured on the hardware (2026-10-22)",
    "unprobed": "Not probed yet",
}
PROBE_TAGS = {"measured": "**[HW]**", "unprobed": "**[open]**"}
PROBES_SLOT = "\x00probes\x00"

DIRECTIVE = re.compile(r"^<!--\s*@([a-z][a-z-]*)\s*(.*?)\s*-->\s*$")
SPDX = re.compile(r"^<!--\s*SPDX-[A-Za-z-]+:.*-->\s*$")


class BuildError(Exception):
    pass


# ---- the form tables ------------------------------------------------------------------

_tables: dict | None = None
_formdump: pathlib.Path | None = None


def _tool() -> pathlib.Path:
    """Compile formdump.c against the tools' sources (once per process)."""
    global _formdump
    if _formdump is None:
        out = pathlib.Path(tempfile.mkdtemp(prefix="isa-formdump-")) / "formdump"
        dis, asm = ROOT / "tools" / "usse-dis", ROOT / "tools" / "usse-asm"
        cmd = ["cc", "-O1", "-std=c11", "-D_POSIX_C_SOURCE=200809L", f"-I{dis}", f"-I{asm}",
               "-o", str(out), str(HERE / "formdump.c"), str(dis / "usse.c"),
               str(dis / "usse_form.c"), str(dis / "pds.c")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise BuildError(f"cannot build formdump:\n{r.stderr}")
        _formdump = out
    return _formdump


def formdump(*args: str) -> str:
    r = subprocess.run([str(_tool()), *args], capture_output=True, text=True)
    if r.returncode:
        raise BuildError(f"formdump {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def load_tables() -> dict:
    global _tables
    if _tables is None:
        _tables = json.loads(formdump("tables"))
    return _tables


# ---- fields: names, values, bits ------------------------------------------------------

def values(f: dict) -> dict[int, str | None]:
    return {v: t for v, t in f["values"]}


def text0(f: dict) -> str | None:
    return values(f).get(0)


def is_pred(f: dict) -> bool:
    texts = [t for t in values(f).values() if t is not None]
    if not any(texts) or "" not in texts:
        return False
    return all(t == "" or re.fullmatch(r"\(?!?[A-Za-z]+\d*\)? ", t) for t in texts) and \
        len(texts) > 2


REG = re.compile(r"^(r|o|pa|sa|i|c|g|ds|ir|mr|tim|pc)\d*(\[|\b|\.|$)")
POSITIONS = {
    "usse": {21: "dst", 7: "src1", 0: "src2", 14: "src0"},
    "pds": {0: "dst", 5: "srcs", 17: "src", 16: "src", 8: "src", 9: "src", 10: "src"},
}
PAIRS = {("", "~"): "inv", ("!", ""): "w", ("+", "-"): "sign", ("++", "--"): "step",
         ("low", "high"): "half"}


def _name(form: dict, i: int, isa: str) -> str:
    f = form["fields"][i]
    vals = values(f)
    texts = [t for t in vals.values() if t is not None]
    distinct = list(dict.fromkeys(texts))
    before = form["pat"].split("{%d}" % i)[0]
    if is_pred(f):
        return "pred"
    m = re.search(r"([A-Za-z]+)$", before)
    if m and not before.endswith(" "):
        letters = m.group(1).lower()
        if f["kind"] == "num" or letters in ("drc", "ds", "p", "cp", "i", "cx", "ax"):
            return letters
    t0 = text0(f)
    if before.endswith("["):
        return "base" if t0 and REG.match(t0) and f["kind"] == "other" else "idx"
    if before.endswith("#") or f["kind"] == "num" or \
            texts and all(re.match(r"-?#", t) for t in texts):
        return "imm"
    if distinct == [""]:
        return "mode"
    if texts and all(t.startswith(", ") or t == "" for t in texts):
        return "extra"
    if texts and all(re.fullmatch(r"p\d|", t) for t in texts):
        return "pdst"
    if texts and all(re.fullmatch(r"\d+", t) for t in texts):
        return "n"
    if len(distinct) == 2:
        if distinct[0] == "" and re.fullmatch(r"\.[a-z0-9_]+", distinct[1]):
            return distinct[1][1:]
        if tuple(distinct) in PAIRS:
            return PAIRS[tuple(distinct)]
    if ".xy" in texts:
        return "wmask"
    family = {re.sub(r"\d+$", "", t) for t in texts if t}
    if len(family) == 1 and len(distinct) > 2 and re.fullmatch(r"\.[a-z]+", next(iter(family))):
        return next(iter(family))[1:]
    if before.endswith("."):
        return "test" if all(t.startswith("test") for t in texts) else "type"
    if t0 and REG.match(t0):
        first = f["chunks"][0][0]
        return POSITIONS.get(isa, {}).get(first, "opnd") if f["kind"] == "other" else "reg"
    if "doutu" in texts:
        return "port"
    if len(distinct) > 2 and all(t.startswith(".") for t in distinct if t):
        return "flags"
    if texts and all(re.match(r"[a-z0-9]", t) for t in texts if t):
        return "op"
    return f"f{i}"


def field_names(form: dict, isa: str) -> list[str]:
    names = [_name(form, i, isa) for i in range(len(form["fields"]))]
    plus = form["pat"].find("+")
    if plus >= 0:
        for i in range(len(names)):
            if form["pat"].find("{%d}" % i) > plus:
                names[i] = "s" + names[i]
    for n in set(names):
        if names.count(n) > 1:
            k = 0
            for j, m in enumerate(names):
                if m == n:
                    names[j] = f"{n}{'abcdefghij'[k]}" if n[-1].isdigit() else f"{n}{k + 1}"
                    k += 1
    return names


def named_pattern(form: dict, isa: str) -> str:
    names = field_names(form, isa)
    return re.sub(r"\{(\d)\}", lambda m: f"<{names[int(m.group(1))]}>", form["pat"])


def bits_text(f: dict) -> str:
    chunks = f["chunks"]

    def rng(hi: int, lo: int) -> str:
        return f"{hi}" if hi == lo else f"{hi}:{lo}"
    if len(chunks) == 1:
        lo, w = chunks[0]
        return rng(lo + w - 1, lo)
    out, at = [], 0
    for lo, w in chunks:
        out.append(f"{rng(lo + w - 1, lo)} → v[{rng(at + w - 1, at)}]")
        at += w
    return ", ".join(out)


def _code(t: str) -> str:
    t = t.strip()
    if not t:
        return "(none)"
    return f"`` {t} ``" if "`" in t else f"`{t}`"


NUMBERED = re.compile(r"^(.*?)(\d+)(\D*)$")


def _runs(items: list[tuple[int, str]]) -> list[str]:
    """`v text` items; runs of consecutive values whose texts count up in step (`.repeat2`,
    `.repeat3`, ...) are written once as `a-b first…last`."""
    out, i = [], 0
    while i < len(items):
        j = i
        m = NUMBERED.match(items[i][1].strip())
        while m and j + 1 < len(items):
            nxt = NUMBERED.match(items[j + 1][1].strip())
            cur = NUMBERED.match(items[j][1].strip())
            if not (nxt and items[j + 1][0] == items[j][0] + 1 and nxt.group(1) == m.group(1)
                    and nxt.group(3) == m.group(3)
                    and int(nxt.group(2)) == int(cur.group(2)) + 1):
                break
            j += 1
        if j - i >= 2:
            out.append(f"{items[i][0]}-{items[j][0]} {_code(items[i][1])}…{_code(items[j][1])}")
        else:
            out += [f"{v} {_code(t)}" for v, t in items[i:j + 1]]
        i = j + 1
    return out


def _flag_bits(f: dict) -> list[str]:
    """The flags of a flag-run field, relative to what value 0 prints: what each bit adds
    (or removes), with runs of bits that count up a numbered modifier written as a count."""
    vals = values(f)

    def atoms(t: str) -> list[str]:
        return re.findall(r"\.[^.\s,/]+|,\s[^,]+", t)
    base = atoms(vals.get(0) or "")

    def added(v: int) -> list[str]:
        return [a for a in atoms(vals.get(v) or "") if a not in base]

    w, out, i = f["width"], [], 0
    while i < w:
        t = vals.get(1 << i)
        if t is None:
            i += 1
            continue
        add = added(1 << i)
        gone = [a for a in base if a not in atoms(t)]
        m = NUMBERED.match(add[0]) if len(add) == 1 else None
        if m and m.group(1) not in (".", "") and not gone:
            j = i
            while j + 1 < w and [a for a in added(1 << (j + 1))
                                 if a.startswith(m.group(1))]:
                j += 1
            if j > i:
                top = added(((1 << (j - i + 1)) - 1) << i)
                out.append(f"v[{j}:{i}] a count, {_code(add[0])}…"
                           + (_code(top[0]) if len(top) == 1 else ""))
                i = j + 1
                continue
        if add:
            out.append(f"v[{i}] {_code(''.join(add))}")
        elif gone:
            out.append(f"v[{i}] drops {_code(''.join(gone))}")
        i += 1
    return out


def _banks(f: dict) -> str | None:
    """A register operand's banks, `b first…last` by bank code, or None if f is not one."""
    vals, w = values(f), f["width"]
    t0 = vals.get(0)
    if not (f["kind"] == "other" and t0 and REG.match(t0) and 7 <= w <= 12 and ", " not in t0):
        return None
    banks: list[tuple[int, int, str]] = []        # first bank, last bank, what they print
    for b in range(1 << (w - 7)):
        t = vals.get(b << 7)
        if t is None:
            continue
        last = vals.get(b << 7 | 127)
        desc = _code(t) + (f"…{_code(last)}" if last and last != t else "")
        if banks and banks[-1][2] == desc and banks[-1][1] == b - 1:
            banks[-1] = (banks[-1][0], b, desc)
        else:
            banks.append((b, b, desc))
    return ", ".join(f"{a}{'' if a == z else f'-{z}'} {d}" for a, z, d in banks)


# The register operands of the operand section, by a form and field that use the printer.
CANON_OPERANDS = {
    "usse": [
        (r"^\{0\}mov\{1\}\{2\}\{3\}\{4\} \{5\}\{6\}, \{7\}$", 5, "the standard destination"),
        (r"^\{0\}mov\{1\}\{2\}\{3\}\{4\} \{5\}\{6\}, \{7\}$", 7,
         "the standard source 1 or 2, with a seven-bit immediate printed in hex"),
        (r"^\{0\}\{1\}\{2\} \{3\}\{4\}, \{5\}, \{6\}\{7\}$", 5, "the standard source 1 or 2"),
        (r"^emitpix1", 4, "the standard source 0"),
    ],
}


def canon_operands(tables: dict | None, isa: str) -> dict[str, str]:
    """Bank descriptions of the standard operands → the words that name them."""
    out: dict[str, str] = {}
    if not tables:
        return out
    for pat, field, label in CANON_OPERANDS.get(isa, []):
        forms = select(tables, isa, {"pat": pat})
        if forms:
            banks = _banks(forms[0]["fields"][field])
            if banks:
                out.setdefault(banks, label)
    return out


def _a(n: int) -> str:
    """The article before a spoken number: "an 8-bit", "an 18-bit", "a 7-bit"."""
    return "An" if str(n).startswith("8") or n in (11, 18) else "A"


def describe_field(name: str, f: dict, isa: str, canon: dict[str, str] | None = None) -> str:
    """One line (to be wrapped) describing a field: its bits and what its values print."""
    vals = values(f)
    w = f["width"]
    one = len(f["chunks"]) == 1 and f["chunks"][0][1] == 1
    head = f"`{name}` — {'bit' if one else 'bits'} {bits_text(f)}."
    if f["kind"] == "num":
        base = "hex" if "X" in f["fmt"] else "decimal"
        scale = "" if f["mul"] == 1 else f", times {f['mul']}"
        off = "" if not f["add"] else f", plus {f['add']}"
        return f"{head} {_a(w)} {w}-bit number, printed in {base}{scale}{off}."
    valid = [(v, t) for v, t in sorted(vals.items()) if t is not None]
    distinct = list(dict.fromkeys(t for _v, t in valid))
    banks = _banks(f)
    if banks is not None:
        if canon and banks in canon:
            return f"{head} A register operand: {canon[banks]} (*Register operands*)."
        return (f"{head} A register operand, v = bank·128 + number (the bank bits may carry "
                f"modifiers). By bank: " + banks + ".")
    t0 = vals.get(0)
    nonempty = [t for _v, t in valid if t]
    if w > 8 and nonempty and all(t.startswith(", ") for t in nonempty):
        sample = [f"{v} {_code(t)}" for v, t in valid if t][:3]
        return (f"{head} An optional trailing operand: some values print nothing, the others "
                f"a comma and the operand, for example " + ", ".join(sample) + ".")
    enum = f["kind"] == "enum" or not _flaglike(f)
    if w <= 8 and enum and len(_runs(valid)) <= 40:
        tail = " Other values do not decode." if len(valid) < (1 << w) else ""
        return f"{head} Values: " + ", ".join(_runs(valid)) + "." + tail
    if _flaglike(f) and _flag_bits(f):
        zero = f" Value 0 prints {_code(t0)}." if t0 else ""
        return (f"{head} Flags, printed in this order.{zero} Each bit adds: "
                + ", ".join(_flag_bits(f)) + ".")
    if t0 and ", " in t0:
        sample = [f"{v} {_code(t)}" for v, t in valid[:3]]
        return (f"{head} An operand list (see *Operand encodings*), for example "
                + ", ".join(sample) + ".")
    if valid and all(re.fullmatch(r"-?#?-?(0x[0-9A-Fa-f]+|\d+)", t) for _v, t in valid):
        picks = [0, 1, 1 << (w - 1), (1 << w) - 1]
        sample = [f"{v} {_code(vals[v])}" for v in dict.fromkeys(picks) if vals.get(v)]
        return (f"{head} {_a(w)} {w}-bit value printed as a number by the field's own printer (the "
                f"group's text says how), for example " + ", ".join(sample) + ".")
    sample = [f"{v} {_code(t)}" for v, t in valid[:6]]
    more = " or more" if w > 8 else ""
    return f"{head} {len(distinct)}{more} distinct texts, for example " + ", ".join(sample) + "."


def _flaglike(f: dict) -> bool:
    vals = values(f)
    singles = [vals.get(1 << i) for i in range(f["width"])]
    return any(singles) and \
        all(t is None or t == "" or t.startswith((".", ",")) for t in singles)


# ---- mnemonics ----------------------------------------------------------------------------

MOD_FAMILIES = [(r"\.repeat\d+", ".repeatN"), (r"\.fetch\d+", ".fetchN"),
                (r"\.incs[01]{3}", ".incsXYZ"), (r"\.bytemask[01]{4}", ".bytemaskXXXX"),
                (r"\.rs\d", ".rsN"), (r"\.fcs\d", ".fcsN")]


def _norm_mod(m: str) -> str:
    for rx, rep in MOD_FAMILIES:
        if re.fullmatch(rx, m):
            return rep
    return m


def _atoms(t: str) -> list[str]:
    return re.findall(r"\.[^.\s,/]+", t)


def mnemonics(form: dict, isa: str) -> tuple[list[str], list[str]]:
    """The mnemonics a form prints and the modifiers on them (normalised)."""
    pat = form["pat"]
    heads = [pat.split(" ")[0]] + re.findall(r"\+(\S+?)[ ,]", pat)
    mns, mods = set(), set()
    for h, head in enumerate(heads):
        parts: list[list[str]] = []
        after_dot = False
        for tok in re.split(r"(\{\d\})", head):
            if not tok:
                continue
            m = re.fullmatch(r"\{(\d)\}", tok)
            if not m:
                parts.append([tok])
                after_dot = tok.endswith(".")
                continue
            f = form["fields"][int(m.group(1))]
            if is_pred(f):
                continue
            texts = [t for t in dict.fromkeys(values(f).values()) if t is not None]
            for t in texts:
                mods.update(_atoms(t))
            alts = set()
            for t in texts:
                if after_dot:
                    alts.add(t.split(".")[0])
                else:
                    alts.add("" if t.startswith(".") else t.split(".")[0])
            parts.append(sorted(alts) or [""])
            after_dot = False
        for combo in itertools.islice(itertools.product(*parts), 4096):
            s = "".join(combo)
            base = s.split(".")[0]
            if base:
                mns.add(("+" if h else "") + base)
            mods.update(_atoms(s[len(base):]))
    return sorted(mns), sorted({_norm_mod(m) for m in mods})


# ---- selection and layouts ----------------------------------------------------------------

def form_classes(form: dict, isa: str) -> list[int]:
    shift = (64 if isa == "usse" else 32) - 5
    mask, val = int(form["mask"], 16) >> shift & 31, int(form["val"], 16) >> shift & 31
    return [c for c in range(32) if c & mask == val]


def _int_set(spec: str) -> set[int]:
    out = set()
    for part in spec.split(","):
        a, _, b = part.partition("-")
        lo = int(a, 0)
        out.update(range(lo, int(b, 0) + 1) if b else [lo])
    return out


def _kv(args: list[str]) -> dict[str, str]:
    kv = {}
    for a in args:
        k, eq, v = a.partition("=")
        if not eq:
            raise BuildError(f"expected key=value, got {a!r}")
        kv[k] = v
    return kv


def select(tables: dict, isa: str, kv: dict[str, str]) -> list[dict]:
    if isa not in tables:
        raise BuildError(f"unknown instruction set {isa!r}")
    classes = _int_set(kv["class"]) if "class" in kv else None
    out = []
    for f in tables[isa]["forms"]:
        if f["pat"] is None:
            continue
        if classes is not None and not set(form_classes(f, isa)) <= classes:
            continue
        if "pat" in kv and not re.search(kv["pat"], f["pat"]):
            continue
        if "not" in kv and re.search(kv["not"], f["pat"]):
            continue
        out.append(f)
    return out


def _layout_key(f: dict) -> tuple:
    return f["mask"], tuple(tuple(tuple(c) for c in x["chunks"]) for x in f["fields"])


def layouts(forms: list[dict], isa: str) -> list[list[dict]]:
    groups: dict[tuple, list[dict]] = {}
    for f in forms:
        groups.setdefault(_layout_key(f), []).append(f)
    return list(groups.values())


# ---- diagrams -------------------------------------------------------------------------------

def _segments(form: dict, isa: str, lo: int, hi: int, others: list[dict]):
    names = field_names(form, isa)
    mask, val = int(form["mask"], 16), int(form["val"], 16)
    owner: dict[int, tuple] = {}
    for i, f in enumerate(form["fields"]):
        for clo, w in f["chunks"]:
            for b in range(clo, clo + w):
                owner[b] = owner.get(b, ()) + (names[i],)
    segs: list[list] = []          # [key, label, hi, lo]
    for b in range(hi, lo - 1, -1):
        if b in owner:
            key, label = ("f",) + owner[b], "/".join(owner[b])
        elif mask >> b & 1:
            bit = str(val >> b & 1)
            if any((int(o["val"], 16) >> b & 1) != (val >> b & 1) for o in others):
                bit = "?"
            key, label = ("x",), bit
        else:
            key, label = ("-",), "-"
        if segs and segs[-1][0] == key and key != ("x",) or \
                segs and key == ("x",) and segs[-1][0] == key:
            segs[-1][3] = b
            if key == ("x",):
                segs[-1][1] += label
        else:
            segs.append([key, label, b, b])
    return [(label, h, l) for _k, label, h, l in segs]


def _row(segs: list[tuple[str, int, int]], keys: dict[str, str] | None = None):
    cells = []
    for label, h, l in segs:
        shown = keys.get(label, label) if keys is not None else label
        hdr = f"{h}" if h == l else f"{h} {l}"
        cw = max(len(shown) + 2, len(hdr))
        top = f"{h}".ljust(cw - len(str(l))) + f"{l}" if h != l else f"{h}".ljust(cw)
        cells.append((cw, top, shown.center(cw)))
    head = "".join(" " + t for _cw, t, _s in cells)
    border = "+" + "+".join("-" * cw for cw, _t, _s in cells) + "+"
    body = "|" + "|".join(s for _cw, _t, s in cells) + "|"
    return [head.rstrip(), border, body, border]


def diagram(form: dict, isa: str, width: int | None = None,
            others: list[dict] | None = None) -> str:
    """The word as boxes, one row per 32-bit half (16-bit pieces if a half is too wide):
    field names, fixed bits as their value ('?' where the layout's forms differ) and
    ignored bits as '-'."""
    width = width or (64 if isa == "usse" else 32)
    others = others or []
    rows: list[str] = []
    legend: list[str] = []
    step = min(32, width)
    for top in range(width - 1, -1, -step):
        piece = [(top, top - step + 1)]
        segs = _segments(form, isa, top - step + 1, top, others)
        if len(_row(segs)[1]) > WIDTH and step > 16:
            piece = [(top, top - 15), (top - 16, top - 31)]
        for h, l in piece:
            segs = _segments(form, isa, l, h, others)
            lines = _row(segs)
            if len(lines[1]) > WIDTH:
                keys = {}
                for label, sh, sl in segs:
                    hdr = len(f"{sh}" if sh == sl else f"{sh} {sl}")
                    if len(label) + 2 > max(hdr, 3) and label not in keys:
                        keys[label] = chr(ord("a") + len(legend) + len(keys))
                legend += [f"{k} = {label}" for label, k in keys.items()]
                lines = _row(segs, keys)
            rows += lines
    if legend:
        rows += textwrap.wrap("where " + ", ".join(legend), WIDTH)
    return "\n".join(rows)


# ---- Markdown helpers -------------------------------------------------------------------------

def wrap_item(prefix: str, text: str, indent: str | None = None) -> str:
    indent = " " * len(prefix) if indent is None else indent
    return "\n".join(textwrap.wrap(text, WIDTH, initial_indent=prefix, subsequent_indent=indent,
                                   break_long_words=False, break_on_hyphens=False))


def fence(lines: list[str]) -> str:
    return "\n".join(["```", *lines, "```"])


# ---- directives -------------------------------------------------------------------------------

@dataclasses.dataclass
class Context:
    tables: dict | None
    extra: dict[str, Callable] = dataclasses.field(default_factory=dict)
    probes: list[tuple[str, str]] = dataclasses.field(default_factory=list)


def _form_line(f: dict, isa: str) -> list[str]:
    mn, _mods = mnemonics(f, isa)
    cls = form_classes(f, isa)
    word = "opcode" if isa == "pds" else "class"
    c = f"{word} {cls[0]}" if len(cls) == 1 else \
        f"{word}s {cls[0]}-{cls[-1]}" if len(cls) < 32 else f"any {word}"
    nd = 16 if isa == "usse" else 8
    first = (f"#{f['index']:<4} {c:<14} mask {f['mask'][-nd:]}  value {f['val'][-nd:]}  "
             + " ".join(mn))
    return textwrap.wrap(first, WIDTH, subsequent_indent=" " * 6) + \
        ["      " + named_pattern(f, isa)]


def d_group(ctx: Context, args: str) -> str:
    a = shlex.split(args)
    isa, kv = a[0], _kv(a[1:])
    forms = select(ctx.tables, isa, kv)
    if not forms:
        raise BuildError(f"@group {args}: selects no form")
    out = ["Forms (generated from the decoder's table; `<name>` is a field):", ""]
    lines: list[str] = []
    for f in forms:
        lines += _form_line(f, isa)
    out += [fence(lines), ""]
    mns, mods = set(), set()
    for f in forms:
        m, d = mnemonics(f, isa)
        mns.update(m)
        mods.update(d)
    out.append(wrap_item("", "Mnemonics: " + ", ".join(f"`{m}`" for m in sorted(mns)) + "."))
    if mods:
        out += ["", wrap_item("", "Modifiers: " + ", ".join(_code(m) for m in sorted(mods))
                              + ".")]
    lays = layouts(forms, isa)
    canon = canon_operands(ctx.tables, isa)
    shown: dict[str, int] = {}          # description → the layout that gave it first
    for n, group in enumerate(lays, 1):
        first = group[0]
        idx = ", ".join(f"#{f['index']}" for f in group)
        title = f"Layout {n} of {len(lays)}" if len(lays) > 1 else "Layout"
        out += ["", wrap_item("", f"{title} (form{'s' if len(group) > 1 else ''} {idx}):"),
                "", fence(diagram(first, isa, others=group[1:]).split("\n")), ""]
        names = field_names(first, isa)
        for i, f in enumerate(first["fields"]):
            descs: dict[str, list[int]] = {}
            for g in group:
                d = describe_field(names[i], g["fields"][i], isa, canon)
                descs.setdefault(d, []).append(g["index"])
            for d, ids in descs.items():
                who = "" if len(descs) == 1 else \
                    f"(form{'s' if len(ids) > 1 else ''} {', '.join(f'#{x}' for x in ids)}) "
                if d in shown and shown[d] != n:
                    out.append(wrap_item("- ", f"{who}`{names[i]}` — as in layout {shown[d]}."))
                else:
                    shown.setdefault(d, n)
                    out.append(wrap_item("- ", who + d))
    return "\n".join(out)


def d_appendix(ctx: Context, args: str) -> str:
    isa = args.strip()
    out = []
    by_class: dict[str, list[dict]] = {}
    for f in ctx.tables[isa]["forms"]:
        if f["pat"] is None:
            continue
        cls = form_classes(f, isa)
        key = f"{cls[0]}" if len(cls) == 1 else f"{cls[0]}-{cls[-1]}" if len(cls) < 32 \
            else "any"
        by_class.setdefault(key, []).append(f)
    word = "opcode" if isa == "pds" else "class"
    for key, forms in by_class.items():
        title = f"{word.capitalize()} {key}" if key != "any" else f"Any {word} (in a field)"
        lines: list[str] = []
        for f in forms:
            lines += _form_line(f, isa)
        out += [f"**{title}**", "", fence(lines), ""]
    return "\n".join(out).rstrip()


def d_classmap(ctx: Context, args: str) -> str:
    isa = args.strip()
    rows: dict[int, set[str]] = {c: set() for c in range(32)}
    infield: set[str] = set()
    for f in ctx.tables[isa]["forms"]:
        if f["pat"] is None:
            continue
        cls = form_classes(f, isa)
        mn, _ = mnemonics(f, isa)
        if len(cls) == 32:
            infield.update(mn)
            continue
        for c in cls:
            rows[c].update(m for m in mn if not m.startswith("+"))
    word = "Opcode" if isa == "pds" else "Class"
    out = []
    for c in range(32):
        names = ", ".join(f"`{m}`" for m in sorted(rows[c])) or "(none: no form decodes it)"
        out.append(wrap_item("- ", f"{word} {c} (`0x{c:02x}`): {names}"))
    if infield:
        out.append(wrap_item("- ", "With the opcode in a field: "
                             + ", ".join(f"`{m}`" for m in sorted(infield))))
    return "\n".join(out)


def d_example(ctx: Context, args: str) -> str:
    isa, _, text = args.strip().partition(" ")
    d = json.loads(formdump("encode", isa, text.strip()))
    f = ctx.tables[isa]["forms"][d["form"]]
    names = field_names(f, isa)
    w = int(d["word"], 16)
    if isa == "usse":
        mem = " ".join(f"{w >> (8 * i) & 0xff:02X}" for i in range(8))
        enc = (f"enc[63:32] = `0x{w >> 32:08X}`, enc[31:0] = `0x{w & 0xffffffff:08X}`; "
               f"in memory (little endian) `{mem}`")
    else:
        mem = " ".join(f"{w >> (8 * i) & 0xff:02X}" for i in range(4))
        enc = f"the word is `0x{w:08X}`; in memory (little endian) `{mem}`"
    out = [wrap_item("", f"`{d['text']}` assembles with form #{d['form']}: {enc}."), "",
           "| field | value | prints |", "|---|---|---|"]
    for name, (v, t) in zip(names, d["fields"]):
        out.append(f"| `{name}` | {v:#x} | {_code(t)} |")
    return "\n".join(out)


def d_prog(ctx: Context, args: str) -> str:
    a = shlex.split(args)
    path = PROGS / a[0]
    kv = _kv(a[1:])
    lines = [ln for ln in path.read_text().split("\n")
             if not re.match(r"\s*;\s*SPDX-", ln)]
    while lines and not lines[-1].strip():
        lines.pop()
    if "match" in kv:
        lines = [ln for ln in lines if re.search(kv["match"], ln.split(";")[0])]
        if not lines:
            raise BuildError(f"@prog {args}: no line matches")
    return f"From `tools/usse-asm/progs/{a[0]}`:\n\n" + fence(lines)


def _one_field(ctx: Context, args: str) -> tuple[str, dict, dict]:
    a = shlex.split(args)
    isa, kv = a[0], _kv(a[1:])
    forms = select(ctx.tables, isa, {"pat": kv["pat"]})
    if not forms:
        raise BuildError(f"{args}: selects no form")
    return isa, forms[0], forms[0]["fields"][int(kv["field"])]


def d_banks(ctx: Context, args: str) -> str:
    _isa, _form, f = _one_field(ctx, args)
    vals = values(f)
    out = ["| bank | number 0 | number 1 | number 127 |", "|---|---|---|---|"]
    for b in range(1 << (f["width"] - 7)):
        cells = [_code(vals[b << 7 | n]) if vals.get(b << 7 | n) else "—" for n in (0, 1, 127)]
        out.append(f"| {b} | " + " | ".join(cells) + " |")
    return "\n".join(out)


def d_values(ctx: Context, args: str) -> str:
    _isa, _form, f = _one_field(ctx, args)
    items = [f"{v} {_code(t)}" for v, t in sorted(values(f).items()) if t is not None]
    return wrap_item("", ", ".join(items) + ".")


def d_probes(ctx: Context, args: str) -> str:
    return PROBES_SLOT


DIRECTIVES: dict[str, Callable[[Context, str], str]] = {
    "group": d_group, "appendix": d_appendix, "classmap": d_classmap, "example": d_example,
    "prog": d_prog, "banks": d_banks, "values": d_values, "probes": d_probes,
}


def directive(ctx: Context, name: str, args: str) -> str:
    fn = ctx.extra.get(name) or DIRECTIVES.get(name)
    if fn is None:
        raise BuildError(f"unknown directive @{name}")
    return fn(ctx, args)


# ---- books --------------------------------------------------------------------------------------

@dataclasses.dataclass
class Book:
    name: str
    output: str
    options: dict[str, str]
    items: list[str]


def parse_manifest(text: str, name: str) -> Book:
    output, options, items = None, {}, []
    for line in text.split("\n"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@") or " " not in line:
            items.append(line)
            continue
        key, _, value = line.partition(" ")
        if key == "output":
            output = value.strip()
        else:
            options[key] = value.strip()
    if output is None:
        raise BuildError(f"{name}: no output")
    return Book(name, output, options, items)


def load_books(names: list[str] | None = None) -> list[Book]:
    paths = sorted(BOOKS.glob("*.book"))
    if names:
        paths = [p for p in paths if p.stem in names]
    return [parse_manifest(p.read_text(), p.stem) for p in paths]


def snippet_title(path: pathlib.Path) -> str:
    for line in path.read_text().split("\n"):
        if line.startswith("#"):
            return line.lstrip("#").strip()
    raise BuildError(f"{path}: no heading")


def probe_status(path: pathlib.Path) -> str:
    """Which of PROBE_MARKS a probe snippet carries ("measured" or "unprobed")."""
    text = path.read_text()
    found = [status for status, mark in PROBE_MARKS.items() if mark in text]
    if len(found) != 1:
        raise BuildError(f"{path}: needs exactly one probe mark, found {found}")
    return found[0]


def _snippet(path: pathlib.Path, ctx: Context) -> str:
    if not path.is_file():
        raise BuildError(f"missing snippet {path}")
    out = []
    for line in path.read_text().split("\n"):
        if SPDX.match(line):
            continue
        m = DIRECTIVE.match(line)
        out.append(directive(ctx, m.group(1), m.group(2)) if m else line)
    return "\n".join(out).strip("\n")


def _number(text: str) -> str:
    out, fenced, n2, n3 = [], False, 0, 0
    for line in text.split("\n"):
        if line.startswith("```"):
            fenced = not fenced
        elif not fenced and line.startswith("## "):
            n2, n3 = n2 + 1, 0
            line = f"## {n2}. {line[3:]}"
        elif not fenced and line.startswith("### "):
            n3 += 1
            line = f"### {n2}.{n3} {line[4:]}"
        out.append(line)
    return "\n".join(out)


def assemble(book: Book, content: pathlib.Path, ctx: Context) -> str:
    parts = []
    for item in book.items:
        if item.startswith("@"):
            name, _, args = item[1:].partition(" ")
            parts.append(directive(ctx, name, args))
        else:
            if "/probe/" in item:
                ctx.probes.append((snippet_title(content / item),
                                   probe_status(content / item)))
            parts.append(_snippet(content / item, ctx))
    text = "\n\n".join(parts) + "\n"
    if book.options.get("number") == "yes":
        text = _number(text)
    if PROBES_SLOT in text:
        index = []
        for title, status in ctx.probes:
            m = re.search(r"^#+ ([\d.]+)? ?" + re.escape(title) + "$", text, re.M)
            where = f"§{m.group(1).rstrip('.')} " if m and m.group(1) else ""
            index.append(f"- {where}{title} {PROBE_TAGS[status]}")
        text = text.replace(PROBES_SLOT, "\n".join(index) or "(none)")
    return text


def build_book(book: Book, tables: dict | None = None) -> str:
    return assemble(book, CONTENT, Context(tables or load_tables()))


def group_claims(book: Book, tables: dict) -> dict[int, int]:
    """How many @group sections of the book list each form."""
    isa = book.options.get("isa")
    claims: dict[int, int] = {}

    def claim(args: str) -> None:
        a = shlex.split(args)
        if a[0] != isa:
            return
        for f in select(tables, a[0], _kv(a[1:])):
            claims[f["index"]] = claims.get(f["index"], 0) + 1
    for item in book.items:
        if item.startswith("@group "):
            claim(item[len("@group "):])
        elif item.endswith(".md"):
            for line in (CONTENT / item).read_text().split("\n"):
                m = DIRECTIVE.match(line)
                if m and m.group(1) == "group":
                    claim(m.group(2))
    return claims


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="fail if an output is stale")
    ap.add_argument("books", nargs="*", help="book names (books/NAME.book); default: all")
    a = ap.parse_args(argv)
    stale = 0
    for book in load_books(a.books):
        text = build_book(book)
        path = ROOT / book.output
        if a.check:
            if not path.is_file() or path.read_text() != text:
                print(f"stale: {book.output}", file=sys.stderr)
                stale += 1
        else:
            path.write_text(text)
            print(f"wrote {book.output} ({text.count(chr(10))} lines)")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
