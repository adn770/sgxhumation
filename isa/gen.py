#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Generate the instruction-form tables of the C and Rust decoders/encoders from
the ISA description (isa/README.md).

    usage: gen.py [--check]

Writes, for USSE, tools/usse-dis/usse_table.inc (C: the data kinds and
usse_forms[]) and crates/usse-isa/src/table.rs (Rust: the same). Both are
committed; --check exits 1 when one is stale, without writing.

A description file holds, all optional: consts (expressions), layouts (bit
ranges), fields (a layout with a kind, or a kind with bit ranges), code (the
names of kinds written as code in both languages), data kinds (enum, flags,
num, seq, sel) and forms. Names are global across the files of one ISA.
Expressions use b(n), m(hi, lo), cls(c), CLSM, the consts, | & ~ << >> and
numbers. Bit ranges are "hi:lo" or a single bit, LSB-first chunks.
"""
from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import sys
import tomllib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
M64 = (1 << 64) - 1

ISAS = {
    "usse": {
        "parts": ["main", "float", "memctl", "intpack", "misc"],
        "c": ROOT / "tools/usse-dis/usse_table.inc",
        "c_table": ("usse_forms", "usse_nforms", "usse_table_main"),
        "rust": ROOT / "crates/usse-isa/src/table.rs",
        "rust_use": ["use crate::form::{Chunk, Enum, Field, Flags, Form, Num, Radix, Sel, SelItem, Seq};",
                     "#[allow(unused_imports)]",
                     "use crate::forms::{float::*, intpack::*, memctl::*, misc::*};",
                     "use crate::kinds::*;"],
    },
    "pds": {
        "parts": ["pds"],
        "c": ROOT / "tools/usse-dis/pds_table.inc",
        "c_table": ("pds_forms", "pds_nforms", "pds_table"),
        "rust": ROOT / "crates/pds-isa/src/table.rs",
        "rust_use": ["#[allow(unused_imports)]",
                     "use usse_isa::form::{Chunk, Enum, Field, Flags, Form, Num, Radix, Sel, SelItem, Seq};",
                     "use crate::kinds::*;"],
    },
}
# A test ISA (isa/test_gen.py) gets the USSE names.
DEFAULT = ISAS["usse"]


class DescError(Exception):
    pass


@dataclasses.dataclass
class Field:
    kind: str
    chunks: list[tuple[int, int]]  # (lo, w), LSB first
    role: str = ""                 # what the field is to the simulator ("dst", ...)


@dataclasses.dataclass
class Form:
    pat: str | None
    mask: int
    val: int
    fields: list[Field]
    dflt: int
    where: str
    op: str = ""                   # what the form does, for the simulator


@dataclasses.dataclass
class Isa:
    name: str
    kinds: dict[str, dict]  # data kinds, in definition order
    code: list[str]
    forms: list[Form]


def b(n: int) -> int:
    return 1 << n


def m(hi: int, lo: int) -> int:
    return (M64 >> (63 - hi)) & (M64 << lo) & M64


def cls(c: int) -> int:
    return c << 59


def chunks(s: str, where: str) -> list[tuple[int, int]]:
    out = []
    for t in s.split():
        mt = re.fullmatch(r"(\d+)(?::(\d+))?", t)
        if not mt:
            raise DescError(f"{where}: bad bit range {t!r}")
        hi = int(mt.group(1))
        lo = int(mt.group(2)) if mt.group(2) else hi
        if lo > hi or hi > 63:
            raise DescError(f"{where}: bad bit range {t!r}")
        out.append((lo, hi - lo + 1))
    return out


def load(name: str, base: pathlib.Path = HERE, parts: list[str] | None = None) -> Isa:
    """Read the description of one ISA: base/<name>/<part>.toml, in order."""
    parts = parts if parts is not None else ISAS[name]["parts"]
    consts: dict[str, int] = {}
    layouts: dict[str, list] = {}
    fields: dict[str, Field] = {}
    kinds: dict[str, dict] = {}
    code: list[str] = []
    raw_forms = []
    env = {"b": b, "m": m, "cls": cls, "CLSM": m(63, 59), "__builtins__": {}}

    def ev(e: str, where: str) -> int:
        try:
            return eval(e, env, consts) & M64  # our own description files
        except Exception as ex:
            raise DescError(f"{where}: {e!r}: {ex}") from None

    def field(s: str, where: str) -> Field:
        mt = re.fullmatch(r"(\w+)=(.*)", s)
        if mt:
            return dataclasses.replace(field(mt.group(2), where), role=mt.group(1))
        if s in fields:
            return fields[s]
        mt = re.fullmatch(r"(\w+)\((\w+)\)", s)
        if mt:
            if mt.group(1) not in layouts:
                raise DescError(f"{where}: unknown layout {mt.group(1)}")
            return Field(mt.group(2), layouts[mt.group(1)])
        k, _, rest = s.partition(" ")
        if not rest:
            raise DescError(f"{where}: unknown field {s!r}")
        return Field(k, chunks(rest, where))

    def unique(n: str, where: str):
        if n in consts or n in layouts or n in fields or n in kinds or n in code:
            raise DescError(f"{where}: {n} defined twice")

    for part in parts:
        path = base / name / f"{part}.toml"
        d = tomllib.loads(path.read_text())
        rel = path.relative_to(base.parent) if path.is_relative_to(base.parent) else path
        for n, e in d.get("const", {}).items():
            unique(n, f"{rel}: const")
            consts[n] = ev(e, f"{rel}: const {n}")
        for n, c in d.get("layout", {}).items():
            unique(n, f"{rel}: layout")
            layouts[n] = chunks(c, f"{rel}: layout {n}")
        for n, s in d.get("field", {}).items():
            unique(n, f"{rel}: field")
            fields[n] = field(s, f"{rel}: field {n}")
        for n in d.get("code", []):
            unique(n, f"{rel}: code")
            code.append(n)
        for n, k in d.get("kind", {}).items():
            unique(n, f"{rel}: kind")
            kinds[n] = k
        for i, f in enumerate(d.get("form", [])):
            raw_forms.append((f, f"{rel}: form {i}"))

    forms = []
    for f, where in raw_forms:
        if f.get("reject"):
            pat = None
        elif "pat" in f:
            pat = f["pat"]
        else:
            raise DescError(f"{where}: no pat")
        fl = [field(s, where) for s in f.get("fields", [])]
        for x in fl:
            if x.kind not in kinds and x.kind not in code:
                raise DescError(f"{where}: unknown kind {x.kind}")
        if len(fl) > 10:
            raise DescError(f"{where}: more than 10 fields")
        roles = [x.role for x in fl if x.role]
        for r in roles:
            if roles.count(r) > 1:
                raise DescError(f"{where}: role {r} twice")
        op = f.get("op", "")
        if op and not re.fullmatch(r"[a-z0-9_.]+", op):
            raise DescError(f"{where}: bad op {op!r}")
        if pat is not None:
            for p in re.findall(r"\{(\d)\}", pat):
                if int(p) >= len(fl):
                    raise DescError(f"{where}: {{{p}}} has no field")
        forms.append(Form(pat, ev(f["mask"], where), ev(f["val"], where), fl,
                          ev(f.get("dflt", "0"), where), where, op))
        if forms[-1].val & ~forms[-1].mask & M64:
            raise DescError(f"{where}: val has bits outside mask")
    return Isa(name, kinds, code, forms)


def names_of(k: dict, key: str) -> list[str | None]:
    """A names list: strings, false (None) for an invalid value; or a sparse table."""
    t = k[key]
    if isinstance(t, dict):
        n = max(int(i) for i in t) + 1
        out: list[str | None] = [None] * n
        for i, s in t.items():
            out[int(i)] = s
        return out
    return [s if isinstance(s, str) else None for s in t]


def log2(n: int, what: str) -> int:
    w = n.bit_length() - 1
    if n != 1 << w:
        raise DescError(f"{what}: {n} names, not a power of two")
    return w


def kind_class(n: str, k: dict) -> str:
    for c in ("enum", "flags", "num", "seq", "sel"):
        if c in k:
            return c
    raise DescError(f"kind {n}: no enum, flags, num, seq or sel")


def max_widths(isa: Isa) -> dict[str, int]:
    w: dict[str, int] = {}
    for f in isa.forms:
        for x in f.fields:
            w[x.kind] = max(w.get(x.kind, 0), sum(c[1] for c in x.chunks))
    return w


# ---- C ----------------------------------------------------------------------

def c_str(s: str | None) -> str:
    if s is None:
        return "NULL"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_names(name: str, t: list, size: int) -> str:
    t = t + [None] * (size - len(t))
    return f"static const char *const {name}[{len(t)}] = {{ {', '.join(map(c_str, t))} }};"


def gen_c(isa: Isa) -> str:
    mw = max_widths(isa)
    forms_sym, n_sym, table_sym = ISAS.get(isa.name, DEFAULT)["c_table"]
    # REUSE-IgnoreStart (the generated files' own header)
    L = ["/* SPDX-License-Identifier: MIT */",
         "/* SPDX-FileCopyrightText: 2026 Josep Torra */",
    # REUSE-IgnoreEnd
         f"/* {isa.name}_table.inc: GENERATED by isa/gen.py from the files in isa/{isa.name}/,",
         f" * do not edit. The data kinds and the form table; {isa.name}.c includes it after",
         " * its code kinds. */", ""]
    for n, k in isa.kinds.items():
        c = kind_class(n, k)
        if c == "enum":
            t = names_of(k, "enum")
            L.append(c_names(f"{n}_T", t, max(len(t), 1 << mw.get(n, 0))))
            L.append(f"static const usse_kind {n} = {{ usse_k_enum_print, NULL, {n}_T }};")
        elif c == "flags":
            L.append(c_names(f"{n}_T", names_of(k, "flags"), 0))
            L.append(f"static const usse_kind {n} = {{ usse_k_flags_print, NULL, {n}_T }};")
        elif c == "num":
            fmt = {"dec": "%u", "hex": "0x%08X"}[k["num"]]
            L.append(f"static const usse_num {n}_N = {{ {c_str(fmt)}, {k.get('mul', 1)}, "
                     f"{k.get('add', 0)} }};")
            L.append(f"static const usse_kind {n} = {{ usse_k_num_print, usse_k_num_parse, "
                     f"&{n}_N }};")
        elif c == "seq":
            subs = []
            for i, s in enumerate(k["seq"]):
                t = [x if isinstance(x, str) else None for x in s]
                L.append(c_names(f"{n}_S{i}", t, 0))
                subs.append(f"{{ {log2(len(t), n)}, {n}_S{i} }}")
            L.append(f"static const usse_seq_sub {n}_SUB[] = {{ {', '.join(subs)} }};")
            L.append(f"static const usse_seq {n}_Q = {{ {n}_SUB, {len(subs)} }};")
            L.append(f"static const usse_kind {n} = {{ usse_k_seq_print, NULL, &{n}_Q }};")
        else:
            items = []
            for i, it in enumerate(k["sel"]):
                t = [x if isinstance(x, str) else None for x in it["t"]]
                L.append(c_names(f"{n}_I{i}", t, 0))
                items.append(f"{{ {n}_I{i}, {log2(len(t), n)}, {int(it['comp'])} }}")
            L.append(f"static const usse_sel {n}_S = {{ {int(k.get('pre', False))}, "
                     f"{int(k.get('neg', False))}, {len(items)}, {{ {', '.join(items)} }} }};")
            L.append(f"static const usse_kind {n} = {{ usse_k_sel_print, NULL, &{n}_S }};")
    L += ["", "#pragma GCC diagnostic push",
          '#pragma GCC diagnostic ignored "-Wmissing-field-initializers"',
          f"const usse_form {forms_sym}[] = {{"]
    for i, f in enumerate(isa.forms):
        fl = ", ".join(
            f"{{ &{x.kind}, {{ {', '.join(f'{{ {lo}, {w} }}' for lo, w in x.chunks)} }} }}"
            for x in f.fields) or "{ 0 }"
        L.append(f"    /* {i} */ {{ {c_str(f.pat)}, 0x{f.mask:016X}ull, 0x{f.val:016X}ull, "
                 f"{{ {fl} }}, 0x{f.dflt:016X}ull }},")
    L += ["};", "#pragma GCC diagnostic pop",
          f"const size_t {n_sym} = sizeof {forms_sym} / sizeof {forms_sym}[0];",
          f"const usse_table {table_sym} = {{ {forms_sym}, sizeof {forms_sym} / sizeof {forms_sym}[0], "
          "NULL };", ""]
    return "\n".join(L)


# ---- Rust -------------------------------------------------------------------

def rs_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def rs_names(t: list) -> str:
    return "&[" + ", ".join("None" if x is None else f"Some({rs_str(x)})" for x in t) + "]"


def gen_rust(isa: Isa) -> str:
    # REUSE-IgnoreStart (the generated files' own header)
    L = ["// SPDX-License-Identifier: MIT",
         "// SPDX-FileCopyrightText: 2026 Josep Torra",
    # REUSE-IgnoreEnd
         f"//! GENERATED by isa/gen.py from isa/{isa.name}/*.toml, do not edit: the data",
         "//! kinds and the form table. The code kinds live in `kinds` and `forms`.",
         "", *ISAS.get(isa.name, DEFAULT)["rust_use"], ""]
    for n, k in isa.kinds.items():
        c = kind_class(n, k)
        if c == "enum":
            L.append(f"pub(crate) const {n}: Enum = Enum({rs_names(names_of(k, 'enum'))});")
        elif c == "flags":
            L.append(f"pub(crate) const {n}: Flags = Flags(&["
                     f"{', '.join(rs_str(s) for s in names_of(k, 'flags'))}]);")
        elif c == "num":
            radix = {"dec": "Dec", "hex": "Hex"}[k["num"]]
            L.append(f"pub(crate) const {n}: Num = Num {{ radix: Radix::{radix}, "
                     f"mul: {k.get('mul', 1)}, add: {k.get('add', 0)} }};")
        elif c == "seq":
            subs = []
            for s in k["seq"]:
                t = [x if isinstance(x, str) else None for x in s]
                subs.append(f"({log2(len(t), n)}, {rs_names(t)})")
            L.append(f"pub(crate) const {n}: Seq = Seq(&[{', '.join(subs)}]);")
        else:
            items = []
            for it in k["sel"]:
                t = [x if isinstance(x, str) else None for x in it["t"]]
                items.append(f"SelItem {{ t: {rs_names(t)}, w: {log2(len(t), n)}, "
                             f"comp: {str(it['comp']).lower()} }}")
            L.append(f"pub(crate) const {n}: Sel = Sel {{ pre: {str(k.get('pre', False)).lower()}, "
                     f"neg: {str(k.get('neg', False)).lower()}, it: &[{', '.join(items)}] }};")
    L += ["", "pub(crate) static FORMS: &[Form] = &["]
    for i, f in enumerate(isa.forms):
        fl = ", ".join(
            f"Field {{ k: &{x.kind}, c: &[{', '.join(f'Chunk {{ lo: {lo}, w: {w} }}' for lo, w in x.chunks)}], r: {rs_str(x.role)} }}"
            for x in f.fields)
        pat = "None" if f.pat is None else f"Some({rs_str(f.pat)})"
        L.append(f"    /* {i} */ Form {{ pat: {pat}, mask: 0x{f.mask:016X}, val: 0x{f.val:016X}, "
                 f"f: &[{fl}], dflt: 0x{f.dflt:016X}, op: {rs_str(f.op)} }},")
    L += ["];", ""]
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="fail if an output is stale")
    args = ap.parse_args()
    stale = []
    for name, spec in ISAS.items():
        try:
            isa = load(name)
        except DescError as e:
            print(f"gen.py: {e}", file=sys.stderr)
            return 2
        for path, text in ((spec["c"], gen_c(isa)), (spec["rust"], gen_rust(isa))):
            if path.exists() and path.read_text() == text:
                continue
            stale.append(path.relative_to(ROOT))
            if not args.check:
                path.write_text(text)
    for p in stale:
        print(f"{'stale' if args.check else 'wrote'}: {p}")
    return 1 if args.check and stale else 0


if __name__ == "__main__":
    sys.exit(main())
