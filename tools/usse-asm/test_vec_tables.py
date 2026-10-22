# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The instruction-vector tables (progs/vectors): generated, fresh, and every case assembles."""
import pathlib
import subprocess
import sys

import probe

HERE = pathlib.Path(__file__).resolve().parent
VEC = HERE / "progs" / "vectors"


def test_vec1_is_fresh():
    out = subprocess.run([sys.executable, str(VEC / "gen_vec1.py")], capture_output=True,
                         text=True, check=True).stdout
    assert (VEC / "vec1.toml").read_text() == out, "run gen_vec1.py > vec1.toml"


def test_vec1_cases_assemble(tmp_path):
    table = probe.load(VEC / "vec1.toml")
    names = [p.name for p in table]
    assert len(names) == len(set(names))
    assert not any(p.hypotheses for p in table)          # recorded, not probed
    assert not set(names) & {p.name for p in probe.load()}
    n = sum(len(probe.build(p, tmp_path)) for p in table)
    assert n == sum(len(p.cases) for p in table) >= 300
