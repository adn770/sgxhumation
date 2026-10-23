# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Build and import pysgx without maturin: cargo builds the extension, this puts it where
Python finds it (target/pysgx/pysgx.abi3.so) and imports it.

    sys.path.insert(0, str(REPO / "crates" / "pysgx"))
    import load; pysgx = load.pysgx()
"""
from __future__ import annotations

import importlib
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
OUT = REPO / "target" / "pysgx"


class BuildError(RuntimeError):
    pass


def build() -> pathlib.Path:
    """Build the extension (cargo; a no-op when current) and return its directory."""
    if shutil.which("cargo") is None:
        raise BuildError("pysgx needs cargo")
    r = subprocess.run(["cargo", "build", "-q", "-p", "pysgx"], cwd=REPO, capture_output=True,
                       text=True)
    if r.returncode:
        raise BuildError(f"cannot build pysgx:\n{r.stderr}")
    lib = next((REPO / "target" / "debug" / f"libpysgx.{x}" for x in ("dylib", "so")
                if (REPO / "target" / "debug" / f"libpysgx.{x}").is_file()), None)
    if lib is None:
        raise BuildError("cargo built no libpysgx")
    OUT.mkdir(parents=True, exist_ok=True)
    dest = OUT / "pysgx.abi3.so"
    if not dest.is_file() or dest.stat().st_mtime < lib.stat().st_mtime:
        shutil.copy2(lib, dest)
    return OUT


def pysgx():
    """The pysgx module, built if needed."""
    if "pysgx" in sys.modules:
        return sys.modules["pysgx"]
    path = str(build())
    if path not in sys.path:
        sys.path.insert(0, path)
    return importlib.import_module("pysgx")
