#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Register snapshots of the SGX545 on the test laptop, from debugfs dri/N/sgx/regs.

The file (kernel patch 0055) holds one value a line: the registers the EDM firmware
reaches with ldr/str, the host's own registers, HostCtl, TA3DCtl and the kernel CCB control
words. A snapshot file is that text under a few "# key: value" metadata lines, so two
snapshots also diff with plain `diff -u`.

    regsnap.py snap -o FILE [--label L]
    regsnap.py around -d DIR --label L [--names HDR...] -- COMMAND...
    regsnap.py diff A B [--names HDR...]

`around` snapshots, runs COMMAND on the laptop, snapshots again, and writes LABEL.before.regs,
LABEL.after.regs, LABEL.cmd.log and LABEL.diff into DIR.

The laptop comes from SGX_TARGET_HOST and SGX_TARGET_SSH_OPTS (environment, or
kernel/scripts/local.env as the other scripts read it), or --host and --ssh-opts=... before
the subcommand. --names reads "#define NAME 0xOFFSET" lines of any header to label registers
in a diff.
"""
from __future__ import annotations

import argparse
import datetime
import pathlib
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Callable

VERSION = 1
SNAP_VERSION = 1
ABSENT = object()           # a key one snapshot has and the other has not
HERE = pathlib.Path(__file__).resolve().parent

_HEADER = re.compile(r"^sgxfive regs (\d+)$")
_VALUE = re.compile(r"^(reg|hostctl|ta3dctl|ccbctl) (0x[0-9a-f]+) (0x[0-9a-f]{8}|skip)$")
_DEFINE = re.compile(r"^\s*#define\s+(\w+)\s+0x([0-9A-Fa-f]+)U?\s*$")
_FIELD = re.compile(r"(MASK|SHIFT|SIGNED|CLRMSK)$")

Runner = Callable[[str], str]


@dataclass
class Snapshot:
    version: int
    state: dict[str, int]
    values: dict[str, int | None]           # "reg 0x0408" -> value, None when skipped
    meta: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class Change:
    key: str
    before: object
    after: object


def parse(text: str, meta: dict[str, str] | None = None) -> Snapshot:
    """Parse the debugfs text; raises ValueError unless it is whole and well formed."""
    lines = [ln for ln in text.splitlines() if ln and not ln.startswith("#")]
    if not lines or not (m := _HEADER.match(lines[0])):
        raise ValueError("not an sgxfive regs dump")
    version = int(m.group(1))
    if version != VERSION:
        raise ValueError(f"unknown regs version {version}")
    if lines[-1] != "end":
        raise ValueError("truncated regs dump (no end line)")
    state: dict[str, int] = {}
    values: dict[str, int | None] = {}
    for ln in lines[1:-1]:
        if ln.startswith("state "):
            words = ln.split()[1:]
            if len(words) % 2:
                raise ValueError(f"bad state line: {ln!r}")
            state.update((k, int(v, 0)) for k, v in zip(words[::2], words[1::2]))
            continue
        m = _VALUE.match(ln)
        if not m:
            raise ValueError(f"bad line: {ln!r}")
        key = f"{m.group(1)} {m.group(2)}"
        if key in values:
            raise ValueError(f"duplicate {key}")
        values[key] = None if m.group(3) == "skip" else int(m.group(3), 16)
    return Snapshot(version, state, values, dict(meta or {}))


def write_snapshot(path: pathlib.Path, text: str, *, label: str,
                   meta: dict[str, str] | None = None) -> Snapshot:
    """Write the raw text under metadata lines; the text is checked first."""
    snap = parse(text)
    head = {"label": label,
            "taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds")}
    head.update(meta or {})
    lines = [f"# regsnap {SNAP_VERSION}"] + [f"# {k}: {v}" for k, v in head.items()]
    pathlib.Path(path).write_text("\n".join(lines) + "\n" + text)
    snap.meta = head
    return snap


def read_snapshot(path: pathlib.Path) -> Snapshot:
    text = pathlib.Path(path).read_text()
    meta = {}
    for ln in text.splitlines():
        if ln.startswith("# ") and ": " in ln:
            k, _, v = ln[2:].partition(": ")
            meta[k] = v
    return parse(text, meta)


def _flat(s: Snapshot) -> dict[str, object]:
    out: dict[str, object] = {f"state {k}": v for k, v in s.state.items()}
    out.update(s.values)
    return out


def diff(a: Snapshot, b: Snapshot) -> list[Change]:
    """Changed, added and removed keys, in b's order then a's leftovers."""
    fa, fb = _flat(a), _flat(b)
    keys = list(fb) + [k for k in fa if k not in fb]
    return [Change(k, fa.get(k, ABSENT), fb.get(k, ABSENT)) for k in keys
            if fa.get(k, ABSENT) != fb.get(k, ABSENT)]


def _show(v: object) -> str:
    if v is ABSENT:
        return "absent"
    if v is None:
        return "skip"
    return f"0x{v:08x}"


def format_diff(changes: list[Change], names: dict[int, str] | None = None) -> str:
    if not changes:
        return "no change\n"
    out = []
    for c in changes:
        kind, _, off = c.key.partition(" ")
        label = c.key
        if kind == "reg" and names and int(off, 16) in names:
            label += f" {names[int(off, 16)]}"
        if kind == "state":
            out.append(f"{label}  {c.before} -> {c.after}")
            continue
        line = f"{label}  {_show(c.before)} -> {_show(c.after)}"
        if isinstance(c.before, int) and isinstance(c.after, int):
            line += f"  (xor 0x{c.before ^ c.after:08x})"
        out.append(line)
    return "\n".join(out) + "\n"


def load_names(paths) -> dict[int, str]:
    names: dict[int, str] = {}
    for p in paths:
        for ln in pathlib.Path(p).read_text(errors="replace").splitlines():
            m = _DEFINE.match(ln)
            if m and not _FIELD.search(m.group(1)):
                names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def regs_command(dri: int = 0) -> str:
    return f"sudo cat /sys/kernel/debug/dri/{dri}/sgx/regs"


def read_regs(run: Runner, dri: int = 0) -> Snapshot:
    return parse(run(regs_command(dri)))


def around(run: Runner, command: str, outdir: pathlib.Path, label: str, *,
           dri: int = 0, names: dict[int, str] | None = None) -> list[Change]:
    """Snapshot, run command on the target, snapshot; write both, the log and the diff."""
    outdir = pathlib.Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    before_text = run(regs_command(dri))
    log = run(command)
    after_text = run(regs_command(dri))
    meta = {"command": command}
    a = write_snapshot(outdir / f"{label}.before.regs", before_text, label=f"{label} before",
                       meta=meta)
    b = write_snapshot(outdir / f"{label}.after.regs", after_text, label=f"{label} after",
                       meta=meta)
    (outdir / f"{label}.cmd.log").write_text(log)
    changes = diff(a, b)
    (outdir / f"{label}.diff").write_text(format_diff(changes, names))
    return changes


def ssh_argv(host: str, opts: str, command: str) -> list[str]:
    return ["ssh", *shlex.split(opts or ""), host, command]


def _local_env() -> dict[str, str]:
    """SGX_* settings as kernel/scripts/local.env gives them (sourced by bash)."""
    env_file = HERE / "local.env"
    if not env_file.exists():
        return {}
    out = subprocess.run(["bash", "-c", f'. "{env_file}"; env'], capture_output=True,
                         text=True, check=True).stdout
    return dict(ln.split("=", 1) for ln in out.splitlines()
                if ln.startswith("SGX_") and "=" in ln)


def ssh_runner(host: str, opts: str) -> Runner:
    def run(command: str) -> str:
        p = subprocess.run(ssh_argv(host, opts, command), capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(f"{command!r} failed ({p.returncode}): {p.stderr.strip()}")
        return p.stdout
    return run


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host")
    ap.add_argument("--ssh-opts")
    ap.add_argument("--dri", type=int, default=0)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("snap")
    p.add_argument("-o", required=True)
    p.add_argument("--label", default="snapshot")
    p = sub.add_parser("around")
    p.add_argument("-d", required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--names", nargs="+", default=[])
    p.add_argument("command", nargs=argparse.REMAINDER)
    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--names", nargs="+", default=[])
    args = ap.parse_args(argv)
    names = load_names(getattr(args, "names", []))
    if args.cmd == "diff":
        sys.stdout.write(format_diff(diff(read_snapshot(args.a), read_snapshot(args.b)),
                                     names))
        return 0
    env = _local_env()
    host = args.host or env.get("SGX_TARGET_HOST") or "sgx-target"
    opts = args.ssh_opts if args.ssh_opts is not None else env.get("SGX_TARGET_SSH_OPTS", "")
    run = ssh_runner(host, opts)
    if args.cmd == "snap":
        write_snapshot(pathlib.Path(args.o), run(regs_command(args.dri)), label=args.label)
        return 0
    command = " ".join(args.command[1:] if args.command[:1] == ["--"] else args.command)
    if not command:
        ap.error("around needs a command")
    changes = around(run, command, pathlib.Path(args.d), args.label, dri=args.dri,
                     names=names)
    sys.stdout.write(format_diff(changes, names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
