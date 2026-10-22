# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""probe.py: the hardware probe table (progs/probes/probes.toml), its programs, the parser of
sgxfive-test's probe output and the verdicts. The hardware runs are not tested here; an
end-to-end run goes through sgxfive-test --mock."""
from __future__ import annotations

import os
import pathlib
import subprocess

import pytest

import probe

HERE = pathlib.Path(__file__).resolve().parent
TOOL = HERE.parent.parent / "userspace" / "libsgxfive" / "build" / "sgxfive-test"


@pytest.fixture(scope="module")
def table():
    return probe.load()


def test_table_loads(table):
    assert len(table) >= 10
    names = [p.name for p in table]
    assert len(names) == len(set(names))
    for p in table:
        assert probe.NAME_RE.fullmatch(p.name), p.name
        assert p.cases, p.name
        assert p.question, p.name


def test_every_table_is_loaded(table):
    tables = sorted((HERE / "progs" / "probes").glob("*.toml"))
    assert len(table) == sum(len(probe.load(t)) for t in tables)


def test_hypotheses_cover_every_case_and_differ(table):
    for p in table:
        vecs = list(p.hypotheses.values())
        for name, v in p.hypotheses.items():
            assert len(v) == len(p.cases), (p.name, name)
        # Two hypotheses with the same predictions could never be told apart.
        assert len({repr(v) for v in vecs}) == len(vecs), p.name


SPDX = "SPDX-" + "License-Identifier: MIT"   # split, so that reuse does not parse it


def test_sources_carry_spdx():
    for s in (HERE / "progs" / "probes").glob("*.s"):
        assert SPDX in s.read_text(), s


def test_every_case_assembles(table, tmp_path):
    for p in table:
        files = probe.build(p, tmp_path)
        assert len(files) == len(p.cases)
        for f in files:
            data = f.read_bytes()
            assert data and len(data) % 8 == 0 and len(data) <= 256 * 8, f
            assert probe.FILE_RE.fullmatch(f.stem), f


def test_render_substitutes_params_and_inputs():
    p = probe.Probe(name="x", question="q", source="pred.s", params={"OP": "nop"},
                    inputs=["A", "B"], cases=[[1, 0x80000000]], hypotheses={})
    text = probe.render(p, p.cases[0], "mov.skipinv r1, #{A}\n{OP}\nmov r2, #{B}\n")
    assert text == "mov.skipinv r1, #0x00000001\nnop\nmov r2, #0x80000000\n"


def test_render_rejects_an_unfilled_placeholder():
    p = probe.Probe(name="x", question="q", source="pred.s", params={}, inputs=["A"],
                    cases=[[1]], hypotheses={})
    with pytest.raises(probe.ProbeError):
        probe.render(p, p.cases[0], "mov r1, #{A}\n{OP}\n")


OUTPUT = """programs asm
probe a__0: scratch at 0x3e003000
probe a__0: o0 0x00000001 (2048 of 2048)
probe a__1: scratch at 0x3e003000
probe a__1: o0 mixed 0x00000001 x1024 0x00000000 x1024
probe a__2: scratch at 0x3e003000
probe a__2: failed: Connection timed out
probe a__3: scratch at 0x3e003000
probe a__3: o0 0x00000000 (2048 of 2048)
probe a__3: scratch +0x104 0x11111111
"""


def test_parse():
    obs = probe.parse(OUTPUT)
    assert obs["a__0"] == probe.Obs(o0=1)
    assert obs["a__1"] == probe.Obs(o0=None, mixed=((1, 1024), (0, 1024)))
    assert obs["a__2"] == probe.Obs(failed="Connection timed out")
    assert obs["a__3"] == probe.Obs(o0=0, scratch=((0x104, 0x11111111),))


def test_expect_forms():
    assert probe.matches(1, probe.Obs(o0=1))
    assert not probe.matches(1, probe.Obs(o0=0))
    assert not probe.matches(1, probe.Obs(failed="x"))
    assert probe.matches("fail", probe.Obs(failed="x"))
    assert probe.matches({"scratch": {"0x104": 0x11111111}},
                         probe.Obs(o0=0, scratch=((0x104, 0x11111111),)))
    # the scratch set is exact: no changed dword may be missing or extra
    assert not probe.matches({"scratch": {}}, probe.Obs(o0=0, scratch=((0x104, 1),)))
    assert probe.matches({"o0": 0, "scratch": {}}, probe.Obs(o0=0))
    assert not probe.matches(1, probe.Obs(o0=None, mixed=((1, 1024), (0, 1024))))
    # most: o0, or the value of at least 90% of the pixels when they differ
    assert probe.matches({"most": 1}, probe.Obs(o0=1))
    assert probe.matches({"most": 1}, probe.Obs(mixed=((1, 2020), (0, 28))))
    assert not probe.matches({"most": 1}, probe.Obs(mixed=((1, 1024), (0, 1024))))
    assert not probe.matches({"most": 0}, probe.Obs(mixed=((1, 2020), (0, 28))))
    # o0_not: anything but that value, a failed job excluded
    assert probe.matches({"o0_not": 1}, probe.Obs(o0=2))
    assert not probe.matches({"o0_not": 1}, probe.Obs(o0=1))
    assert not probe.matches({"o0_not": 1}, probe.Obs(failed="x"))


def test_verdicts():
    p = probe.Probe(name="a", question="q", source="pred.s", params={}, inputs=["A"],
                    cases=[[0], [1]], hypotheses={"h1": [1, 1], "h2": [0, 1]})
    v = probe.verdict(p, [probe.Obs(o0=0), probe.Obs(o0=1)])
    assert v.settled == "h2" and v.consistent == ["h2"]
    v = probe.verdict(p, [probe.Obs(o0=2), probe.Obs(o0=1)])
    assert v.settled is None and v.consistent == []
    q = probe.Probe(name="b", question="q", source="pred.s", params={}, inputs=["A"],
                    cases=[[0]], hypotheses={"h1": [0], "h2": [0]})
    v = probe.verdict(q, [probe.Obs(o0=0)])
    assert v.settled is None and v.consistent == ["h1", "h2"]
    r = probe.Probe(name="c", question="q", source="pred.s", params={}, inputs=["A"],
                    cases=[[0]], hypotheses={})
    v = probe.verdict(r, [probe.Obs(o0=5)])
    assert v.settled is None and "record" in v.text


def test_target_from_env(monkeypatch):
    monkeypatch.setenv("SGX_TARGET_HOST", "u@h")
    monkeypatch.setenv("SGX_TARGET_SSH_OPTS", "-o HostKeyAlias=x")
    assert probe.target() == ("u@h", ["-o", "HostKeyAlias=x"])


@pytest.mark.skipif(not TOOL.exists(), reason="sgxfive-test not built")
def test_end_to_end_on_the_mock(tmp_path):
    # The mock colours a fill with its program's second instruction's immediate.
    (tmp_path / "imm.s").write_text(f"; {SPDX}\n"
                                    "phas #0, #0, pixel, end, parallel\n"
                                    "mov.skipinv.end o0, #{A}\n")
    p = probe.Probe(name="mock-imm", question="q", source="imm.s", params={}, inputs=["A"],
                    cases=[[3], [0x5CAF0010]], hypotheses={"same": [3, 0x5CAF0010],
                                                         "other": [3, 4]})
    out = probe.run_local([p], [str(TOOL), "--mock"], tmp_path / "work", src_dir=tmp_path)
    obs = probe.parse(out)
    assert obs["mock-imm__0"].o0 == 3
    # the scratch immediate was patched to the scratch page's address + 0x10
    assert obs["mock-imm__1"].o0 != 0x5CAF0010 and obs["mock-imm__1"].o0 & 0xfff == 0x10
    v = probe.verdict(p, [obs["mock-imm__0"], obs["mock-imm__1"]])
    assert v.consistent == []
