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


def test_expect_dump_list():
    # a list: the first scratch dwords in order (dump.s stores its registers there); a value
    # equal to the dword's fill shows as unchanged, the dwords after the list are not checked
    obs = probe.Obs(o0=0, scratch=((0x0, 5), (0x8, 7), (0x40, 9)))
    assert probe.matches([5, 0xD0000001, 7], obs)
    assert probe.matches([5], obs)
    assert not probe.matches([5, 6, 7], obs)
    assert not probe.matches([5, 0xD0000001, 8], obs)
    assert not probe.matches([5, 0xD0000001, 7, 1], obs)
    assert not probe.matches([5], probe.Obs(failed="x"))
    assert probe.matches([0xffffffff], probe.Obs(o0=0, scratch=((0x0, 0xffffffff),)))
    assert probe.matches([-1], probe.Obs(o0=0, scratch=((0x0, 0xffffffff),)))


def test_expect_dump_list_near():
    # a list element may be a tolerance: {near = x, ulp = n, abs = e} matches a dword whose
    # binary32 value is within the larger of n units in the last place of x and e of x
    one, two = 0x3F800000, 0x40000000
    obs = probe.Obs(o0=0, scratch=((0x0, one + 1), (0x4, two)))
    assert probe.matches([{"near": 1.0, "ulp": 1}, two], obs)
    assert not probe.matches([{"near": 1.0, "ulp": 0}, two], obs)
    assert probe.matches([{"near": 1.0, "abs": 2.0 ** -20}], obs)
    assert not probe.matches([{"near": 1.0, "abs": 2.0 ** -30}], obs)
    assert probe.matches([one + 1, {"near": 2.0, "ulp": 0}], obs)
    # either bound suffices; an infinite or NaN dword is never near
    assert probe.matches([{"near": 1.0, "ulp": 0, "abs": 1e-6}], obs)
    assert not probe.matches([{"near": 1.0, "ulp": 4}],
                             probe.Obs(o0=0, scratch=((0x0, 0x7FFFFFFF),)))
    # the unit in the last place of 0 is that of the smallest normal
    assert probe.matches([{"near": 0.0, "ulp": 1}],
                         probe.Obs(o0=0, scratch=((0x0, 0x00000001),)))
    assert not probe.matches([{"near": 0.0, "ulp": 1}],
                             probe.Obs(o0=0, scratch=((0x0, 0x00000002),)))
    assert not probe.matches([{"near": 1.0, "ulp": 1}], probe.Obs(failed="x"))


def test_dump_template_stores_its_registers(tmp_path):
    params = {"OP": "nop", "OUT": "r10", **{k: 0 for k in "ABCDEFGH"}}
    p = probe.Probe(name="d", question="q", source="dump.s", params=params,
                    inputs=[], cases=[[]], hypotheses={})
    text = probe.render(p, [], (HERE / "progs" / "probes" / "dump.s").read_text())
    for i, r in enumerate([*range(10, 18), *range(1, 9)]):
        assert f"stad [r29,+#{i}], r{r}" in text


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


ROWS = "".join(f"probe p__0: row {y}: " + " ".join(f"{(y << 16) | x:08x}" for x in range(64))
               + "\n" for y in range(32))


def test_parse_pixels():
    # --pixels: every pixel, row by row, kept as a tuple of 2048 values in raster order
    obs = probe.parse("probe p__0: o0 mixed 0x00000000 x1 other x2047\n" + ROWS)
    assert obs["p__0"].pixels == tuple((i // 64) << 16 | i % 64 for i in range(2048))
    assert probe.parse(OUTPUT)["a__0"].pixels is None


def test_expect_pixels():
    px = tuple(range(2048))
    obs = probe.Obs(mixed=((0, 1),), pixels=px)
    assert probe.matches({"pixels": list(px)}, obs)
    assert not probe.matches({"pixels": [0] * 2048}, obs)
    # no pixels printed: never matches
    assert not probe.matches({"pixels": list(px)}, probe.Obs(o0=0))


def test_flags_select_the_sgxfive_test_options():
    p = probe.Probe(name="x", question="q", source="pred.s", params={}, inputs=[], cases=[[]],
                    hypotheses={}, coords=True, pixels=True)
    assert probe.tool_args(p) == ["--temps", "32", "--coords", "--pixels"]
    q = probe.Probe(name="y", question="q", source="pred.s", params={}, inputs=[], cases=[[]],
                    hypotheses={}, temps=8)
    assert probe.tool_args(q) == ["--temps", "8"]


def test_table_flags_load(tmp_path):
    t = tmp_path / "t.toml"
    t.write_text('[[probe]]\nname = "c"\nquestion = "q"\nsource = "pred.s"\n'
                 'coords = true\npixels = true\ncases = [[]]\n')
    (p,) = probe.load(t)
    assert p.coords and p.pixels


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


@pytest.mark.skipif(not TOOL.exists(), reason="sgxfive-test not built")
def test_end_to_end_coords_on_the_mock(tmp_path):
    # --coords --pixels: the mock runs the copy shader itself, so pa0 = (y << 16) | x
    (tmp_path / "cp.s").write_text(f"; {SPDX}\n"
                                   "phas #0, #0, pixel, end, parallel\n"
                                   "mov.skipinv.end o0, pa0\n")
    want = [(i // 64) << 16 | i % 64 for i in range(2048)]
    p = probe.Probe(name="mock-cp", question="q", source="cp.s", params={}, inputs=[],
                    cases=[[]], hypotheses={"coords": [{"pixels": want}]}, coords=True,
                    pixels=True, temps=8)
    out = probe.run_local([p], [str(TOOL), "--mock"], tmp_path / "work", src_dir=tmp_path)
    v = probe.verdict(p, [probe.parse(out)["mock-cp__0"]])
    assert v.settled == "coords", v.text
