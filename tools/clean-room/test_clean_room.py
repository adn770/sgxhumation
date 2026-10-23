# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""No identifier found only in PVR_PSP2 is in the tracked tree (HACKING.md, "Confidential
sources stay out"). A token counts as PVR_PSP2-only when it occurs there and in none of the
public trees under reference/ nor the closed libraries' strings; allowed.txt lists
the reviewed exceptions. It needs the local reference trees and skips without them."""
import os
import pathlib
import re
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
REF = ROOT / "reference"
PUBLIC = ["sgx545-ce", "linux_openpvrsgx", "emgd-linux", "preserved", "sgx540-reversing",
          "SGX535-reMESA"]
LIBS = [ROOT / "corpus/fetch/intel-pkg/lib", ROOT / "corpus/fetch/ubuntu-pkg/extracted/usr/lib"]
TOKEN = re.compile(rb"(?<![A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]{3,}")


def tokens(root: pathlib.Path) -> set[bytes]:
    out = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for f in filenames:
            p = os.path.join(dirpath, f)
            try:
                if os.path.getsize(p) <= 20_000_000:
                    out.update(TOKEN.findall(open(p, "rb").read()))
            except OSError:
                pass
    return out


def allowed() -> set[str]:
    text = (pathlib.Path(__file__).parent / "allowed.txt").read_text()
    return {w for line in text.splitlines() if not line.startswith("#") for w in line.split()}


def test_no_psp2_only_identifier_is_tracked():
    if not (REF / "PVR_PSP2").is_dir():
        pytest.skip("no reference/PVR_PSP2")
    only = tokens(REF / "PVR_PSP2")
    for r in PUBLIC:
        only -= tokens(REF / r)
    for lib in LIBS:
        only -= tokens(lib)
    files = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True,
                           check=True).stdout.split("\n")
    found: dict[str, str] = {}
    for f in files:
        p = ROOT / f
        if f and p.is_file():
            for t in set(TOKEN.findall(p.read_bytes())) & only:
                found.setdefault(t.decode(), f)
    extra = {t: f for t, f in found.items() if t not in allowed()}
    assert not extra, sorted(extra.items())[:40]
