#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Trace what an EDM code range reads and writes: SGX registers and structure fields.

A linear value tracer over usse-dis output ("<idx>: 0x<ENC16>\\t<text>"). It walks the
instructions of a range in order (no branches are followed) and keeps a value for each
register: a constant, a field of a named structure, a pointer into a named structure or
unknown. Seeds say which registers point at which structures when the range starts
(e.g. r1 = the SGXMKIF_CMDTA the TA handler picked). Structure layouts and field names come
from kick3d.py; a field whose name says it points at another structure (sHWRenderDetails...,
sHWRTData..., sHWPBDesc...) yields a typed pointer when it is loaded.

Events, in instruction order:
    W REG <- value         an SGX register write (str #idx), the value as traced
    R REG -> rN            an SGX register read (ldr rN, #idx)
    ld S.field, ...        a load through a pointer into a named structure
    st S.field <- value    a store through such a pointer (or through *S.field, a pointer
                           loaded from a field, or [rN+#k] when the base is unknown)
    call 0xN label         a ba.savelink

Memory operands follow tools/usse-dis/README.md: [b,+#k] is dword b+k, [b,#k++] is b then
b += k, .fetchN [b,+#k] is b+k+1 .. b+k+N, .fetchN [b,#k++] is b+k .. b+k+N-1 then b += N.
The trace is an aid for reading the disassembly, not an emulator: a predicated write or any
instruction it does not model makes the destination unknown, and branch targets do not merge
values.

    usage: edmtrace.py <disasm.txt> <start> <end> [--seed rN=STRUCT[+0xOFF]]...
                       [--labels <file.r2>]... [--defs <hdr>...]
"""
from __future__ import annotations

import pathlib
import re
import sys
from dataclasses import dataclass, field

import kick3d

LINE = re.compile(r"^\s*(\d+):\s+0x[0-9A-Fa-f]{16}\s+(.*)$")
PRED = re.compile(r"^(!?p\d+)\s+(.*)$")
LABEL = re.compile(r"^f\s+(\S+)\s+@\s+(0x[0-9A-Fa-f]+)\s*$")
MEM = re.compile(r"^\[(\w+),(?:([+-])#(\d+)|#(\d+)\+\+)\]$")
REG = re.compile(r"^(r|sa)(\d+)$")
IMM = re.compile(r"^(~?)#(0x[0-9A-Fa-f]+|\d+)$")
HALF = re.compile(r"^(\w+|#0x[0-9A-Fa-f]+|#\d+)(?:\.(low|high))?$")

NO_WRITE = {"str", "stad", "ba", "br", "lapc", "wdf", "idf", "nop", "phas", "smlsi", "ci",
            "cfi", "lock", "emitpds"}

# pointer fields (by name) -> the structure they point at
POINTS_AT = (
    ("hw_render_details_dev_addr", "hw_render_details"),
    ("sHWRTDataSetDevAddr", "hw_rt_data_set"),
    ("sHWRTDataDevAddr", "hw_rt_data"),
    ("sHWPBDescDevVAddr", "hw_pb_desc"),
    ("parent_hw_pb_desc_dev_vaddr", "hw_pb_desc"),
    ("list_pb_block_dev_vaddr", "hw_pb_block"),
    ("next_hw_pb_block_dev_vaddr", "hw_pb_block"),
    ("hw_dst_sync_list_dev_addr", "SGXMKIF_HWDEVICE_SYNC_LIST"),
    ("hw_render_context_dev_addr", "SGXMKIF_HWRENDERCONTEXT"),
)


@dataclass(frozen=True)
class Insn:
    index: int
    pred: str | None
    op: str
    mods: list[str]
    args: list[str]
    text: str


@dataclass(frozen=True)
class Event:
    index: int
    text: str


@dataclass(frozen=True)
class Val:
    kind: str            # "const", "field", "ptr", "unknown"
    value: int = 0       # constant, or byte offset of a pointer, or dword of a field
    struct: str = ""
    name: str = ""       # unknown: what it was (a register or an SGX register name)


def _split_args(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse(lines) -> list[Insn]:
    out = []
    for ln in lines:
        m = LINE.match(ln)
        if not m:
            continue
        text = m.group(2).strip()
        pred, body = None, text
        if (p := PRED.match(text)):
            pred, body = p.group(1), p.group(2)
        word, _, rest = body.partition(" ")
        op, *mods = word.split(".")
        out.append(Insn(int(m.group(1)), pred, op, mods, _split_args(rest), text))
    return out


def load_labels(lines) -> dict[int, str]:
    out = {}
    for ln in lines:
        if (m := LABEL.match(ln.strip())):
            out[int(m.group(2), 16) // 8] = m.group(1)
    return out


def mem_dwords(mods: list[str], operand: str):
    """(base register, dword offsets accessed, post-increment in dwords), or None."""
    m = MEM.match(operand)
    if not m:
        return None
    base, sign, k, post = m.group(1), m.group(2), m.group(3), m.group(4)
    n = next((int(x[5:]) for x in mods if x.startswith("fetch") and x[5:].isdigit()), None)
    if post is not None:
        k = int(post)
        return (base, list(range(k, k + n)), n) if n else (base, [0], k)
    k = int(k) * (-1 if sign == "-" else 1)
    return (base, list(range(k + 1, k + 1 + n)), 0) if n else (base, [k], 0)


def _field_path(struct: str, dword: int) -> str:
    if struct not in kick3d.STRUCTS:
        return f"{struct}[{dword}]"
    for path, off, width, count, array in kick3d._layout(kick3d.STRUCTS[struct]):
        if off <= dword < off + width * count:
            rel = dword - off
            name = path
            if array or count > 1:
                name += f"[{rel // width}]"
            if width > 1:
                name += f"+{rel % width}"
            return f"{struct}.{name}"
    return f"{struct}[{dword}]"


def _points_at(path: str) -> str | None:
    leaf = path.rsplit(".", 1)[-1]
    for suffix, target in POINTS_AT:
        if leaf == suffix:
            return target
    return None


def _reg_name(reg: int, names: dict[int, str]) -> str:
    return names.get(reg, f"0x{reg:04x}").removeprefix("EUR_CR_")


def _next_reg(r: str, i: int) -> str:
    m = REG.match(r)
    return f"{m.group(1)}{int(m.group(2)) + i}" if m else r


@dataclass
class _State:
    regs: dict[str, Val] = field(default_factory=dict)

    def get(self, r: str) -> Val:
        if (m := IMM.match(r)) and not m.group(1):
            return Val("const", int(m.group(2), 0))
        return self.regs.get(r, Val("unknown", name=r))

    def set(self, r: str, v: Val) -> None:
        if REG.match(r):
            self.regs[r] = v

    def forget(self, r: str) -> None:
        self.set(r, Val("unknown", name=r))


def describe(v: Val) -> str:
    if v.kind == "const":
        return f"{v.value:#x}"
    if v.kind == "field":
        return _field_path(v.struct, v.value)
    if v.kind == "ptr":
        return f"&{v.struct}+{v.value:#x}"
    return v.name


def _load_val(base: Val, dword: int) -> Val:
    d = base.value // 4 + dword
    path = _field_path(base.struct, d)
    target = _points_at(path)
    return Val("ptr", 0, target) if target else Val("field", d, base.struct)


def _operand(st: _State, a: str) -> tuple[Val, bool]:
    """Value of an imae source (reg or #imm, optional .low/.high) and whether it is 16-bit."""
    m = HALF.match(a)
    if not m:
        return Val("unknown", name=a), False
    v = st.get(m.group(1))
    if m.group(2) and v.kind == "const":
        v = Val("const", (v.value >> 16 if m.group(2) == "high" else v.value) & 0xFFFF)
    return v, bool(m.group(2))


def _imae(st: _State, args: list[str]) -> Val:
    a, _ = _operand(st, args[1])
    b, _ = _operand(st, args[2])
    c, _ = _operand(st, args[3])
    if a.kind == b.kind == "const":
        prod = Val("const", a.value * b.value)
    elif b.kind == "const" and b.value == 1:
        prod = a
    else:
        return Val("unknown", name=args[0])
    if prod.kind == c.kind == "const":
        return Val("const", (prod.value + c.value) & 0xFFFFFFFF)
    if prod.kind == "const" and c.kind == "ptr":
        return Val("ptr", c.value + prod.value, c.struct)
    if prod.kind == "ptr" and c.kind == "const":
        return Val("ptr", prod.value + c.value, prod.struct)
    return Val("unknown", name=args[0])


def _logic(st: _State, op: str, args: list[str]) -> Val:
    a = st.get(args[1])
    m = IMM.match(args[2])
    b = Val("const", int(m.group(2), 0)) if m else st.get(args[2])
    if a.kind != "const" or b.kind != "const":
        return Val("unknown", name=args[0])
    bv = ~b.value & 0xFFFFFFFF if m and m.group(1) else b.value
    res = {"or": a.value | bv, "and": a.value & bv, "xor": a.value ^ bv}[op]
    return Val("const", res)


def trace(insns: list[Insn], start: int, end: int, names: dict[int, str],
          seeds: dict[str, tuple[str, int]], labels: dict[int, str]) -> list[Event]:
    st = _State()
    for r, (struct, off) in seeds.items():
        st.set(r, Val("ptr", off, struct))
    out: list[Event] = []
    for i in insns:
        if not start <= i.index <= end:
            continue
        ev = _step(st, i, names, labels)
        out.extend(Event(i.index, t) for t in ev)
    return out


def _step(st: _State, i: Insn, names, labels) -> list[str]:
    a = i.args
    if i.op == "str" and len(a) >= 2 and (m := IMM.match(a[0])):
        reg = int(m.group(2), 0) * 4
        return [f"W {_reg_name(reg, names)} <- {describe(st.get(a[1]))}"]
    if i.op == "ldr" and len(a) >= 2 and (m := IMM.match(a[1])):
        name = _reg_name(int(m.group(2), 0) * 4, names)
        st.set(a[0], Val("unknown", name=name) if not i.pred else Val("unknown", name=a[0]))
        return [f"R {name} -> {a[0]}"]
    if i.op == "ba" and "savelink" in i.mods and a and (m := IMM.match(a[0])):
        tgt = int(m.group(2), 0)
        return [f"call {tgt:#x}" + (f" {labels[tgt]}" if tgt in labels else "")]
    if i.op in ("ldad", "stad") and len(a) >= 2:
        return _memop(st, i)
    if i.op in NO_WRITE or not a:
        return []
    dst = a[0]
    if i.pred:
        st.forget(dst)
    elif i.op == "mov" and len(a) == 2:
        st.set(dst, st.get(a[1]))
    elif i.op == "imae" and len(a) >= 4:
        st.set(dst, _imae(st, a))
    elif i.op in ("or", "and", "xor") and not i.mods and len(a) == 3:
        st.set(dst, _logic(st, i.op, a))
    else:
        st.forget(dst)
    return []


def _memop(st: _State, i: Insn) -> list[str]:
    a = i.args
    mem_arg, other = (a[1], a[0]) if i.op == "ldad" else (a[0], a[1])
    acc = mem_dwords(i.mods, mem_arg)
    if acc is None:
        if i.op == "ldad":
            st.forget(other)
        return []
    base_reg, dwords, step = acc
    base = st.get(base_reg)
    events = []
    if i.op == "ldad":
        for n, d in enumerate(dwords):
            r = _next_reg(other, n)
            if base.kind == "ptr" and not i.pred:
                st.set(r, _load_val(base, d))
            else:
                st.forget(r)
        if base.kind == "ptr":
            events.append("ld " + ", ".join(_field_path(base.struct, base.value // 4 + d)
                                            for d in dwords))
    else:
        srcs = [st.get(_next_reg(other, n)) if REG.match(other) else st.get(other)
                for n in range(len(dwords))]
        for d, v in zip(dwords, srcs):
            if base.kind == "ptr":
                where = _field_path(base.struct, base.value // 4 + d)
            elif base.kind == "field":
                where = "*" + _field_path(base.struct, base.value) + (f"+{d}" if d else "")
            else:
                where = f"[{base_reg}+#{d}]"
            events.append(f"st {where} <- {describe(v)}")
    if step:
        if base.kind == "ptr":
            st.set(base_reg, Val("ptr", base.value + 4 * step, base.struct))
        else:
            st.forget(base_reg)
    return events


def format_events(events: list[Event]) -> str:
    return "\n".join(f"{e.index:#06x}  {e.text}" for e in events)


def _parse_seed(s: str) -> tuple[str, tuple[str, int]]:
    reg, _, rhs = s.partition("=")
    struct, _, off = rhs.partition("+")
    return reg, (struct, int(off, 0) if off else 0)


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("\n".join(ln.strip() for ln in __doc__.strip().splitlines()[-2:]),
              file=sys.stderr)
        return 2
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "usse-dis"))
    import regtouch
    listing, start, end, rest = argv[0], int(argv[1], 16), int(argv[2], 16), argv[3:]
    seeds, label_files, defs = {}, [], []
    while rest:
        flag = rest.pop(0)
        if flag == "--seed" and rest:
            reg, seed = _parse_seed(rest.pop(0))
            seeds[reg] = seed
        elif flag == "--labels" and rest:
            label_files.append(rest.pop(0))
        elif flag == "--defs":
            while rest and not rest[0].startswith("--"):
                defs.append(rest.pop(0))
    root = pathlib.Path(__file__).resolve().parents[2]
    defs = defs or [str(root / d) for d in regtouch.DEFAULT_DEFS if (root / d).exists()]
    names = regtouch.load_names(defs)
    labels = {}
    for f in label_files:
        labels.update(load_labels(pathlib.Path(f).read_text().splitlines()))
    insns = parse(pathlib.Path(listing).read_text().splitlines())
    print(format_events(trace(insns, start, end, names, seeds, labels)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
