#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Instruction vectors from probe readbacks (format: design/sgx545_vectors.md).

Each case `sgxfive-test probe` ran becomes one vector: the program words, the task's initial
state, the readback and where it all came from. Programs are the bytes saved with the run
(`--bins DIR` of `<probe>__<case>.bin`, "recorded") or assembled again from the current probe
tables ("rebuilt").

    vectors.py convert RAW... -o OUTDIR [--root DIR] [--bins DIR]
    vectors.py check FILE...

convert writes OUTDIR/<run>.vec.json per raw file; <run> is the raw file's directory relative
to --root (default: the current directory), with "/" turned into "_".
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import probe  # noqa: E402

FORMAT = "sgx545-vectors"
VERSION = 1
INSTANCES = 2048                # the 64 x 32 fill
SCRATCH_BYTES = 0x10000
_SCRATCH_AT = re.compile(r"^probe (\S+): scratch at (0x[0-9a-f]+)")
_PRESET = re.compile(r"^mov(?:\.\w+)*\s+(r\d+),\s+#(0x[0-9A-Fa-f]+|\d+)\s*$")
_HEX = re.compile(r"^0x[0-9a-f]+$")
_ISA_DIGITS = {"usse": 16, "pds": 8}


def _h32(v: int) -> str:
    return f"0x{v & 0xffffffff:08x}"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def assemble_words(p: probe.Probe, i: int, text: str) -> list[int]:
    """The case's program, assembled with usse-asm (64-bit words)."""
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "case.bin"
        probe._assemble(text, out)
        data = out.read_bytes()
    return list(struct.unpack(f"<{len(data) // 8}Q", data))


def presets(text: str) -> dict[str, str]:
    """Registers the program sets with immediates before its first other instruction."""
    regs: dict[str, str] = {}
    for line in text.splitlines():
        line = line.split(";")[0].strip()
        if not line or line.startswith("phas"):
            continue
        m = _PRESET.match(line)
        if not m:
            break
        regs[m.group(1)] = _h32(int(m.group(2), 0))
    return regs


def tables_for(raw: pathlib.Path, base: list) -> tuple[list, list]:
    """The probes a run used: its own table next to its directory (<dir>.toml) first, then
    the repo's; and the directories its templates are looked up in."""
    own = raw.parent.with_suffix(".toml")
    if not own.is_file():
        return list(base), [probe.PROBES_DIR]
    mine = probe.load(own)
    names = {p.name for p in mine}
    return mine + [p for p in base if p.name not in names], [own.parent, probe.PROBES_DIR]


def template_text(p: probe.Probe, dirs: list) -> str:
    for d in dirs:
        f = pathlib.Path(d) / p.source
        if f.is_file():
            return f.read_text()
    raise probe.ProbeError(f"{p.name}: no template {p.source}")


def _readback(o: probe.Obs) -> dict:
    return {"failed": o.failed,
            "o0": None if o.o0 is None else _h32(o.o0),
            "mixed": [[_h32(v), c] for v, c in o.mixed],
            "scratch": {f"0x{off:03x}": _h32(v) for off, v in o.scratch},
            "pixels": None if o.pixels is None else [_h32(v) for v in o.pixels]}


def convert(raw: str, table: list, *, raw_name: str, templates: dict | None = None,
            words_for=assemble_words, bins: pathlib.Path | None = None,
            dirs: list | None = None) -> dict:
    """One batch from the text of a probe run: a vector per known case, in table order."""
    obs = probe.parse(raw)
    scratch_at = {m.group(1): m.group(2) for m in map(_SCRATCH_AT.match, raw.splitlines()) if m}
    by_label = {}
    for p in table:
        for i in range(len(p.cases)):
            by_label[f"{p.name}__{i}"] = (p, i)
    vecs = []
    for p in table:
        tmpl = (templates or {}).get(p.source)
        if tmpl is None:
            tmpl = template_text(p, dirs or [probe.PROBES_DIR])
        for i, case in enumerate(p.cases):
            label = f"{p.name}__{i}"
            if label not in obs:
                continue
            text = probe.render(p, case, tmpl)
            rec = bins / f"{label}.bin" if bins else None
            if rec and rec.exists():
                data = rec.read_bytes()
                words, source = list(struct.unpack(f"<{len(data) // 8}Q", data)), "recorded"
            else:
                words, source = words_for(p, i, text), "rebuilt"
            vecs.append({
                "id": label,
                "program": {"isa": "usse", "entry": 0, "source": source,
                            "words": [f"0x{w:016x}" for w in words]},
                "init": {"task": "pixel", "instances": INSTANCES, "temps": p.temps,
                         "pa0": "coords" if p.coords else "fill",
                         "scratch": {"base": scratch_at.get(label, "0x3e003000"),
                                     "bytes": SCRATCH_BYTES, "fill": "0xd0000000|i"},
                         "regs": presets(text)},
                "readback": _readback(obs[label]),
                "provenance": {"probe": p.name, "case": i, "table": p.table,
                               "template": p.source, "template_sha256": _sha(tmpl.encode()),
                               "inputs": {k: probe._fmt(v).lower() if isinstance(v, int)
                                          else v for k, v in zip(p.inputs, case)},
                               "params": dict(p.params), "options": probe.tool_args(p),
                               "question": p.question},
            })
    orphans = [k for k in obs if k not in by_label]
    return {"format": FORMAT, "version": VERSION,
            "run": {"raw": raw_name, "raw_sha256": _sha(raw.encode()),
                    "converted": datetime.datetime.now().astimezone().isoformat(
                        timespec="seconds"),
                    "tool": f"vectors.py {VERSION}"},
            "vectors": vecs, "orphans": orphans}


def _hexstr(v, what: str, digits: int | None = None) -> None:
    if not isinstance(v, str) or not _HEX.match(v) or (digits and len(v) != digits + 2):
        raise ValueError(f"{what}: {v!r} is not a hex string")


def validate(batch: dict) -> None:
    if batch.get("format") != FORMAT:
        raise ValueError("not an sgx545-vectors file")
    if batch.get("version") != VERSION:
        raise ValueError(f"unknown version {batch.get('version')}")
    for v in batch.get("vectors", []):
        for key in ("id", "program", "init", "readback", "provenance"):
            if key not in v:
                raise ValueError(f"{v.get('id')}: no {key}")
        prog = v["program"]
        if prog.get("isa") not in _ISA_DIGITS:
            raise ValueError(f"{v['id']}: unknown isa {prog.get('isa')!r}")
        for w in prog["words"]:
            _hexstr(w, v["id"], _ISA_DIGITS[prog["isa"]])
        rb = v["readback"]
        if rb["o0"] is not None:
            _hexstr(rb["o0"], f"{v['id']} o0", 8)
        for off, val in rb["scratch"].items():
            _hexstr(off, f"{v['id']} scratch offset")
            _hexstr(val, f"{v['id']} scratch", 8)


def dump(path: pathlib.Path, batch: dict) -> None:
    validate(batch)
    pathlib.Path(path).write_text(json.dumps(batch, indent=1) + "\n")


def load(path: pathlib.Path) -> dict:
    batch = json.loads(pathlib.Path(path).read_text())
    validate(batch)
    return batch


def run_id(raw: pathlib.Path, root: pathlib.Path) -> str:
    rel = raw.resolve().parent.relative_to(root.resolve()) if raw.is_absolute() or \
        root.is_absolute() else raw.parent
    return str(rel).replace("/", "_")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("raw", nargs="+")
    c.add_argument("-o", required=True)
    c.add_argument("--root", default=".")
    c.add_argument("--bins", default=None)
    k = sub.add_parser("check")
    k.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "check":
        for f in a.files:
            b = load(pathlib.Path(f))
            print(f"{f}: {len(b['vectors'])} vectors, {len(b['orphans'])} orphans")
        return 0
    table = probe.load()
    out = pathlib.Path(a.o)
    out.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(a.root)
    for r in map(pathlib.Path, a.raw):
        rid = run_id(r.absolute(), root.absolute())
        bins = pathlib.Path(a.bins) if a.bins else (r.parent / "bins")
        tbl, dirs = tables_for(r, table)
        batch = convert(r.read_text(), tbl, raw_name=str(r.absolute().relative_to(
            root.absolute())), bins=bins if bins.is_dir() else None, dirs=dirs)
        dump(out / f"{rid}.vec.json", batch)
        n = len(batch["vectors"])
        print(f"{rid}: {n} vectors, {len(batch['orphans'])} orphans")
    return 0


if __name__ == "__main__":
    sys.exit(main())
