#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Hardware probes of the USSE instruction set, run as client fills on the sgxfive render node.

The table, progs/probes/probes.toml, lists the probes: a program template (progs/probes/*.s,
our own assembly), its parameters, the input cases and, per hypothesis, the readback each case
should give. Every case is assembled with usse-asm into its own program, which
`sgxfive-test probe` runs as the shader of its own fill; the host reads o0 (and a scratch page)
back. A hypothesis that predicts every case is consistent; one consistent hypothesis settles
the probe.

    probe.py list                          the probes and their questions
    probe.py build DIR [NAME...]           assemble every case into DIR
    probe.py run [--out DIR] [NAME...]     build, run on the laptop, report
    probe.py local [--mock] [NAME...]      the same with a local sgxfive-test
    probe.py eval RAW [NAME...]            report on a saved run (RAW: its output)

The laptop comes from SGX_TARGET_HOST and SGX_TARGET_SSH_OPTS, or kernel/scripts/local.env;
`sgxfive-test` must be in its home directory (SGX_PROBE_TOOL overrides the path).
"""
from __future__ import annotations

import argparse
import dataclasses
import os
import pathlib
import re
import math
import shlex
import struct
import subprocess
import sys
import tempfile
import tomllib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PROBES_DIR = HERE / "progs" / "probes"
TABLE = PROBES_DIR / "probes.toml"
ASM = HERE / "usse-asm"
LOCAL_ENV = ROOT / "kernel" / "scripts" / "local.env"

NAME_RE = re.compile(r"[a-z0-9][a-z0-9-]*")
FILE_RE = re.compile(r"[a-z0-9][a-z0-9-]*__\d+")
PLACEHOLDER_RE = re.compile(r"\{([A-Z][A-Z0-9_]*)\}")
SCRATCH_FILL = 0xD0000000      # scratch dword i starts as SCRATCH_FILL | i (sgxfive-test)


class ProbeError(Exception):
    pass


@dataclasses.dataclass
class Probe:
    name: str
    question: str
    source: str
    params: dict
    inputs: list
    cases: list
    hypotheses: dict
    temps: int = 32
    labels: list = dataclasses.field(default_factory=list)
    coords: bool = False        # run as the copy shader: pa0 = (y << 16) | x
    pixels: bool = False        # read every pixel back


@dataclasses.dataclass(frozen=True)
class Obs:
    """What one fill gave: o0 (None unless uniform), the mixed values, the failure, the
    changed scratch dwords (offset, value), every pixel in raster order (with --pixels)."""
    o0: int | None = None
    mixed: tuple = ()
    failed: str | None = None
    scratch: tuple = ()
    pixels: tuple | None = None


@dataclasses.dataclass
class Verdict:
    consistent: list
    settled: str | None
    text: str


def load(path: pathlib.Path | None = None) -> list[Probe]:
    """The probes of one table, or of every table in progs/probes (probes.toml first)."""
    paths = [path] if path else [TABLE] + sorted(p for p in PROBES_DIR.glob("*.toml")
                                                 if p != TABLE)
    out = []
    for d in (d for p in paths for d in tomllib.loads(p.read_text()).get("probe", [])):
        out.append(Probe(name=d["name"], question=d["question"], source=d["source"],
                         params=d.get("params", {}), inputs=d.get("inputs", []),
                         cases=d["cases"], hypotheses=d.get("hypotheses", {}),
                         temps=d.get("temps", 32), labels=d.get("labels", []),
                         coords=d.get("coords", False), pixels=d.get("pixels", False)))
    return out


def _fmt(v) -> str:
    if isinstance(v, int):
        return f"0x{v & 0xffffffff:08X}"
    return str(v)


def render(p: Probe, case: list, template: str) -> str:
    """The program text of one case: params, then the case's inputs, substituted."""
    if len(case) != len(p.inputs):
        raise ProbeError(f"{p.name}: case {case} does not give {p.inputs}")
    values = {k: str(v) for k, v in p.params.items()}
    values.update({k: _fmt(v) for k, v in zip(p.inputs, case)})

    def sub(m):
        if m.group(1) not in values:
            raise ProbeError(f"{p.name}: no value for {{{m.group(1)}}}")
        return values[m.group(1)]

    # params may themselves name inputs ({OP} = "... #{A}"): substitute twice
    return PLACEHOLDER_RE.sub(sub, PLACEHOLDER_RE.sub(sub, template))


def _assemble(text: str, out: pathlib.Path) -> None:
    if not ASM.exists():
        subprocess.run(["make", "-s", "-C", str(HERE)], check=True)
    r = subprocess.run([str(ASM), "-o", str(out), "-"], input=text, capture_output=True,
                       text=True)
    if r.returncode:
        raise ProbeError(f"{out.name}: {r.stderr.strip()}\n{text}")


def build(p: Probe, outdir: pathlib.Path, src_dir: pathlib.Path = PROBES_DIR) -> list:
    """Assemble every case of p into outdir/<name>__<i>.bin."""
    outdir.mkdir(parents=True, exist_ok=True)
    template = (src_dir / p.source).read_text()
    files = []
    for i, case in enumerate(p.cases):
        f = outdir / f"{p.name}__{i}.bin"
        _assemble(render(p, case, template), f)
        files.append(f)
    return files


_LINE = re.compile(r"^probe (\S+): (.*)$")
_ROW = re.compile(r"^row (\d+): ([0-9a-f ]+)$")


def parse(text: str) -> dict:
    """sgxfive-test probe output -> {file label: Obs}."""
    acc: dict = {}
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        label, rest = m.groups()
        o = acc.setdefault(label, {"o0": None, "mixed": (), "failed": None, "scratch": [],
                                   "rows": {}})
        row = _ROW.match(rest)
        if row:
            o["rows"][int(row.group(1))] = tuple(int(v, 16) for v in row.group(2).split())
        elif rest.startswith("failed: "):
            o["failed"] = rest[len("failed: "):]
        elif rest.startswith("o0 mixed"):
            o["mixed"] = tuple((int(v, 16), int(c)) for v, c in
                               re.findall(r"(0x[0-9a-f]+) x(\d+)", rest))
        elif rest.startswith("o0 "):
            o["o0"] = int(rest.split()[1], 16)
        elif rest.startswith("scratch +"):
            off, val = rest[len("scratch +"):].split()
            o["scratch"].append((int(off, 16), int(val, 16)))
    def pixels(rows):
        if not rows:
            return None
        return tuple(x for y in sorted(rows) for x in rows[y])

    return {k: Obs(o0=v["o0"], mixed=v["mixed"], failed=v["failed"],
                   scratch=tuple(v["scratch"]), pixels=pixels(v["rows"]))
            for k, v in acc.items()}


def _near(want: dict, got: int) -> bool:
    """Is the binary32 value of got within the larger of want["ulp"] units in the last place
    of want["near"] (that of the smallest normal for a denormal or zero) and want["abs"]?"""
    x = struct.unpack("<f", struct.pack("<I", got & 0xffffffff))[0]
    if not math.isfinite(x):
        return False
    w = float(want["near"])
    ulp = 2.0 ** (max(math.frexp(w)[1] - 1 if w else -126, -126) - 23)
    return abs(x - w) <= max(want.get("ulp", 0) * ulp, want.get("abs", 0.0))


def _dword(want, got: int) -> bool:
    return _near(want, got) if isinstance(want, dict) else got == want & 0xffffffff


def quad_pixels(quad: list) -> list:
    """Every pixel of a "quad" expectation: coords.s gives pixel q the marks
    (0x45000000 + k * 0x06000000) | q << 12; the pixel at quad position j (TL, TR, BL, BR:
    even/odd x, even/odd y) is expected to hold mark quad[j][0] of the pixel at position
    quad[j][1] of its own 2x2 quad."""
    out = []
    for p in range(2048):
        x, y = p % 64, p // 64
        k, n = quad[(x & 1) + 2 * (y & 1)]
        q = ((y & ~1) + n // 2) * 64 + (x & ~1) + n % 2
        out.append((0x45000000 + k * 0x06000000) | q << 12)
    return out


def matches(expect, obs: Obs) -> bool:
    """Does obs agree with one expected readback: an int (o0), "fail" (the job failed), a
    list (the first scratch dwords, in order: what dump.s stores; an element may instead be a
    tolerance, {near = x, ulp = n, abs = e}) or a table with an "o0", "o0_not" (anything
    else), "near" (o0 within the tolerance its "ulp"/"abs" give), "most" (the value of at
    least 90% of the pixels), "pixels" (every pixel, in raster order; needs the probe's pixels
    flag), "quad" (every pixel, as quad_pixels gives it) and/or a "scratch" ({offset: value},
    the exact set of changes)."""
    if expect == "fail":
        return obs.failed is not None
    if obs.failed is not None:
        return False
    if isinstance(expect, int):
        return obs.o0 == expect & 0xffffffff
    if isinstance(expect, list):
        got = dict(obs.scratch)
        return all(_dword(v, got.get(4 * i, SCRATCH_FILL | i)) for i, v in enumerate(expect))
    if "o0" in expect and obs.o0 != expect["o0"] & 0xffffffff:
        return False
    if "near" in expect and (obs.o0 is None or not _near(expect, obs.o0)):
        return False
    if "most" in expect:
        top = obs.o0 if obs.o0 is not None else \
            (obs.mixed[0][0] if obs.mixed and obs.mixed[0][1] * 10 >= 2048 * 9 else None)
        if top != expect["most"] & 0xffffffff:
            return False
    if "pixels" in expect and (obs.pixels is None or
                               list(obs.pixels) != [v & 0xffffffff for v in expect["pixels"]]):
        return False
    if "quad" in expect and (obs.pixels is None or
                             list(obs.pixels) != quad_pixels(expect["quad"])):
        return False
    if "o0_not" in expect and (obs.o0 is None or obs.o0 == expect["o0_not"] & 0xffffffff):
        return False
    if "scratch" in expect:
        want = {int(k, 0): v & 0xffffffff for k, v in expect["scratch"].items()}
        if dict(obs.scratch) != want:
            return False
    return True


def describe(obs: Obs) -> str:
    if obs.failed is not None:
        return f"failed ({obs.failed})"
    s = f"0x{obs.o0:08x}" if obs.o0 is not None else \
        "mixed " + " ".join(f"0x{v:08x}x{c}" for v, c in obs.mixed)
    if obs.scratch:
        s += " scratch " + " ".join(f"+0x{o:03x}=0x{v:08x}" for o, v in obs.scratch)
    return s


def verdict(p: Probe, obs: list) -> Verdict:
    lines = []
    for i, (case, o) in enumerate(zip(p.cases, obs)):
        label = p.labels[i] if i < len(p.labels) else \
            ", ".join(f"{k}={_fmt(v)}" for k, v in zip(p.inputs, case))
        lines.append(f"  [{i}] {label}: {describe(o)}")
    consistent = [h for h, exp in p.hypotheses.items()
                  if all(matches(e, o) for e, o in zip(exp, obs))]
    if not p.hypotheses:
        head = "recorded (no hypotheses)"
    elif len(consistent) == 1:
        head = f"SETTLED: {consistent[0]}"
    elif not consistent:
        head = "NONE of the hypotheses fits"
    else:
        head = "UNDECIDED between " + ", ".join(consistent)
    text = f"{p.name}: {head}\n  question: {p.question}\n" + "\n".join(lines)
    return Verdict(consistent=consistent, settled=consistent[0] if len(consistent) == 1
                   else None, text=text)


def report(table: list, out: str) -> str:
    obs = parse(out)
    texts = []
    for p in table:
        got = [obs.get(f"{p.name}__{i}") for i in range(len(p.cases))]
        if any(g is None for g in got):
            texts.append(f"{p.name}: not run")
            continue
        texts.append(verdict(p, got).text)
    return "\n\n".join(texts) + "\n"


def tool_args(p: Probe) -> list:
    """The options of `sgxfive-test probe` a probe runs with."""
    return ["--temps", str(p.temps)] + (["--coords"] if p.coords else []) + \
        (["--pixels"] if p.pixels else [])


def _groups(table: list) -> dict:
    by: dict = {}
    for p in table:
        by.setdefault(tuple(tool_args(p)), []).append(p)
    return by


def run_local(table: list, tool: list, work: pathlib.Path,
              src_dir: pathlib.Path = PROBES_DIR) -> str:
    """Build and run with a local sgxfive-test (tool: its argv prefix, e.g. with --mock)."""
    out = []
    for args, ps in _groups(table).items():
        # one process per case: the kernel bans a file after its faults (recovery), and a
        # probe may fault on purpose
        for f in (f for p in ps for f in build(p, work, src_dir)):
            r = subprocess.run([*tool, "probe", *args, str(f)],
                               capture_output=True, text=True, timeout=600)
            if r.returncode:
                raise ProbeError(f"sgxfive-test failed ({r.returncode}): {r.stderr}")
            out.append(r.stdout)
    return "".join(out)


def _local_env() -> dict:
    env = {}
    if LOCAL_ENV.exists():
        for line in LOCAL_ENV.read_text().splitlines():
            m = re.match(r'^(\w+)=\$\{\w+:-"?(.*?)"?\}$', line.strip())
            if m:
                env[m.group(1)] = m.group(2)
    return env


def target() -> tuple:
    env = _local_env()
    host = os.environ.get("SGX_TARGET_HOST", env.get("SGX_TARGET_HOST"))
    opts = os.environ.get("SGX_TARGET_SSH_OPTS", env.get("SGX_TARGET_SSH_OPTS", ""))
    if not host:
        raise ProbeError("set SGX_TARGET_HOST (or kernel/scripts/local.env)")
    return host, shlex.split(opts)


def run_remote(table: list, work: pathlib.Path) -> str:
    host, opts = target()
    tool = os.environ.get("SGX_PROBE_TOOL", "./sgxfive-test")
    rdir = "sgxprobes"
    out = []
    subprocess.run(["ssh", *opts, host, f"rm -rf {rdir} && mkdir -p {rdir}"], check=True)
    for args, ps in _groups(table).items():
        files = [f for p in ps for f in build(p, work)]
        subprocess.run(["scp", "-q", *opts, *map(str, files), f"{host}:{rdir}/"], check=True)
        names = " ".join(f"{rdir}/{f.name}" for f in files)
        # one process per case (see run_local)
        r = subprocess.run(["ssh", *opts, host, f"for f in {names}; do "
                            f"{tool} probe {' '.join(args)} $f || exit 1; done"],
                           capture_output=True, text=True, timeout=3600)
        if r.returncode:
            raise ProbeError(f"sgxfive-test failed ({r.returncode}): {r.stderr}")
        out.append(r.stdout)
    return "".join(out)


def _select(table: list, names: list) -> list:
    if not names:
        return table
    sel = [p for p in table if p.name in names or any(
        n.endswith("*") and p.name.startswith(n[:-1]) for n in names)]
    if not sel:
        raise ProbeError(f"no probe named {names}")
    return sel


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    b = sub.add_parser("build")
    b.add_argument("dir")
    b.add_argument("names", nargs="*")
    r = sub.add_parser("run")
    r.add_argument("--out", default=None, help="directory for raw.txt and report.txt")
    r.add_argument("names", nargs="*")
    lo = sub.add_parser("local")
    lo.add_argument("--tool", default=str(ROOT / "userspace/libsgxfive/build/sgxfive-test"))
    lo.add_argument("--mock", action="store_true")
    lo.add_argument("names", nargs="*")
    e = sub.add_parser("eval")
    e.add_argument("raw")
    e.add_argument("names", nargs="*")
    a = ap.parse_args(argv)
    try:
        table = load()
        if a.cmd == "list":
            for p in table:
                print(f"{p.name:24} {len(p.cases):3} cases  {p.question}")
            return 0
        sel = _select(table, a.names)
        if a.cmd == "build":
            for p in sel:
                build(p, pathlib.Path(a.dir))
            return 0
        if a.cmd == "eval":
            print(report(sel, pathlib.Path(a.raw).read_text()), end="")
            return 0
        with tempfile.TemporaryDirectory() as d:
            if a.cmd == "local":
                out = run_local(sel, [a.tool] + (["--mock"] if a.mock else []),
                                pathlib.Path(d))
            else:
                out = run_remote(sel, pathlib.Path(d))
        rep = report(sel, out)
        if a.cmd == "run" and a.out:
            o = pathlib.Path(a.out)
            o.mkdir(parents=True, exist_ok=True)
            (o / "raw.txt").write_text(out)
            (o / "report.txt").write_text(rep)
        print(rep, end="")
        return 0
    except ProbeError as err:
        print(f"probe.py: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
