# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""vectors.py: probe readbacks (sgxfive-test probe output) into instruction-vector files
(design/sgx545_vectors.md)."""
import json
import pathlib
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import probe  # noqa: E402
import vectors  # noqa: E402

TEMPLATE = """phas #0, #0, pixel, end, parallel
mov.skipinv r1, #{A}
mov.skipinv r10, #0xA0A1A2A3
{OP}
mov.skipinv.end o0, r1
"""

RAW = """programs asm
probe t-add__0: scratch at 0x3e003000 (64 KiB)
probe t-add__0: o0 0x00000002 (2048 of 2048)
programs asm
probe t-add__1: scratch at 0x3e003000 (64 KiB)
probe t-add__1: o0 mixed 0x00000001 x2000 0x00000000 x48
probe t-add__1: scratch +0x004 0x12345678
probe gone__0: scratch at 0x3e003000 (64 KiB)
probe gone__0: o0 0x00000000 (2048 of 2048)
probe t-add__2: scratch at 0x3e003000 (64 KiB)
probe t-add__2: failed: job failed: -5
"""


def table():
    return [probe.Probe(name="t-add", question="add one?", source="t.s",
                        params={"OP": "iadd32 r1, r1, #1"}, inputs=["A"],
                        cases=[[1], [0xffffffff], [7]], hypotheses={}, temps=8)]


def fake_words(p, i, text):
    assert "iadd32 r1, r1, #1" in text            # the rendered case
    return [0x1000 + i, 0x2000]


def convert(raw=RAW, **kw):
    return vectors.convert(raw, table(), templates={"t.s": TEMPLATE}, words_for=fake_words,
                           raw_name="runs/x/raw.txt", **kw)


def test_one_vector_per_case_in_order():
    batch = convert()
    assert batch["format"] == "sgx545-vectors" and batch["version"] == 1
    assert [v["id"] for v in batch["vectors"]] == ["t-add__0", "t-add__1", "t-add__2"]
    assert batch["orphans"] == ["gone__0"]
    assert batch["run"]["raw"] == "runs/x/raw.txt"
    assert len(batch["run"]["raw_sha256"]) == 64


def test_program_and_init():
    v = convert()["vectors"][0]
    assert v["program"] == {"isa": "usse", "entry": 0, "source": "rebuilt",
                            "words": ["0x0000000000001000", "0x0000000000002000"]}
    assert v["init"]["temps"] == 8
    assert v["init"]["pa0"] == "fill"
    assert v["init"]["instances"] == 2048
    assert v["init"]["scratch"] == {"base": "0x3e003000", "bytes": 65536,
                                    "fill": "0xd0000000|i"}
    # the registers the program presets, before the instruction under test
    assert v["init"]["regs"] == {"r1": "0x00000001", "r10": "0xa0a1a2a3"}


def test_readbacks():
    vs = convert()["vectors"]
    assert vs[0]["readback"] == {"failed": None, "o0": "0x00000002", "mixed": [],
                                 "scratch": {}, "pixels": None}
    assert vs[1]["readback"]["o0"] is None
    assert vs[1]["readback"]["mixed"] == [["0x00000001", 2000], ["0x00000000", 48]]
    assert vs[1]["readback"]["scratch"] == {"0x004": "0x12345678"}
    assert vs[2]["readback"]["failed"] == "job failed: -5"


def test_provenance():
    p = convert()["vectors"][1]["provenance"]
    assert p["probe"] == "t-add" and p["case"] == 1
    assert p["inputs"] == {"A": "0xffffffff"}
    assert p["params"] == {"OP": "iadd32 r1, r1, #1"}
    assert p["template"] == "t.s" and len(p["template_sha256"]) == 64
    assert p["options"] == ["--temps", "8"]
    assert p["question"] == "add one?"


def test_recorded_words_win(tmp_path):
    (tmp_path / "t-add__0.bin").write_bytes((0xdead).to_bytes(8, "little"))
    batch = convert(bins=tmp_path)
    assert batch["vectors"][0]["program"]["source"] == "recorded"
    assert batch["vectors"][0]["program"]["words"] == ["0x000000000000dead"]
    assert batch["vectors"][1]["program"]["source"] == "rebuilt"


def test_coords_and_pixels_options():
    t = table()
    t[0].coords, t[0].pixels = True, True
    raw = RAW + "probe t-add__0: row 0: " + " ".join(["00000003"] * 64) + "\n"
    batch = vectors.convert(raw, t, templates={"t.s": TEMPLATE}, words_for=fake_words,
                            raw_name="r")
    v = batch["vectors"][0]
    assert v["init"]["pa0"] == "coords"
    assert v["readback"]["pixels"][:2] == ["0x00000003", "0x00000003"]


def test_validate_round_trip(tmp_path):
    path = tmp_path / "x.vec.json"
    vectors.dump(path, convert())
    again = vectors.load(path)
    assert again == json.loads(path.read_text())
    assert again["vectors"][0]["id"] == "t-add__0"


@pytest.mark.parametrize("mutate", [
    lambda b: b.update(version=2),
    lambda b: b.update(format="other"),
    lambda b: b["vectors"][0]["program"].update(isa="arm"),
    lambda b: b["vectors"][0]["program"].update(words=[3]),          # not a hex string
    lambda b: b["vectors"][0]["readback"].update(o0=5),
    lambda b: b["vectors"][0].pop("provenance"),
])
def test_validate_rejects(mutate):
    b = convert()
    mutate(b)
    with pytest.raises(ValueError):
        vectors.validate(b)


def test_converts_a_real_case_with_the_assembler(tmp_path):
    """The default words_for assembles with usse-asm (built by conftest)."""
    t = [p for p in probe.load() if p.name == "sign-p-shl"]
    raw = "probe sign-p-shl__1: scratch at 0x3e003000 (64 KiB)\n" \
          "probe sign-p-shl__1: o0 0x00000001 (2048 of 2048)\n"
    batch = vectors.convert(raw, t, raw_name="r")
    v = batch["vectors"][0]
    assert v["program"]["source"] == "rebuilt"
    assert v["provenance"]["table"] == "probes.toml"
    assert len(v["program"]["words"]) > 2
    assert all(len(w) == 18 for w in v["program"]["words"])
    vectors.validate(batch)


def test_converts_a_pds_probe(tmp_path):
    """A PDS probe's vector: the PDS program (32-bit words, its data segment) and the USE
    program its douta results go through, recorded from the run's bins when there."""
    t = [p for p in probe.load() if p.name == "pds-readback"]
    raw = "probe pds-readback__0: scratch at 0x3e003000 (64 KiB)\n" \
          "probe pds-readback__0: o0 0x0da7a000 (2048 of 2048)\n" \
          "probe pds-readback__0: scratch +0x000 0x11111111\n"
    v = vectors.convert(raw, t, raw_name="r")["vectors"][0]
    assert v["program"]["isa"] == "pds" and v["program"]["data"] == 36
    assert all(len(w) == 10 for w in v["program"]["words"])
    assert v["init"]["task"] == "pds-primary" and v["init"]["attrs"] == 16
    use = v["init"]["use"]
    assert use["source"] == "pa_dump.s" and all(len(w) == 18 for w in use["words"])
    assert v["provenance"]["options"][-4:] == ["--pds", "36", "--use", "pa_dump.bin"]
    vectors.validate({"format": vectors.FORMAT, "version": vectors.VERSION, "vectors": [v]})
    # recorded: the bytes sent, PDS and USE
    probe.build(t[0], tmp_path)
    probe.build_use(tmp_path)
    w = vectors.convert(raw, t, raw_name="r", bins=tmp_path)["vectors"][0]
    assert w["program"]["source"] == "recorded" and w["program"]["words"] == v["program"]["words"]
    assert w["init"]["use"]["words"] == use["words"]


def test_run_id():
    root = pathlib.Path("/c")
    assert vectors.run_id(pathlib.Path("/c/runs/probes/isa1/final/raw.txt"), root) == \
        "runs_probes_isa1_final"


def test_a_run_s_own_table_comes_first(tmp_path):
    """Exploratory runs keep their table next to the run directory (isa3/q3.toml for
    isa3/q3/raw.txt); its probes win over the repo tables, its templates are found next to
    it or in progs/probes."""
    (tmp_path / "q3").mkdir()
    (tmp_path / "own.s").write_text(TEMPLATE)
    (tmp_path / "q3.toml").write_text(
        '[[probe]]\nname = "x-own"\nquestion = "q"\nsource = "own.s"\n'
        'params = { OP = "iadd32 r1, r1, #1" }\ninputs = ["A"]\ncases = [[2]]\n'
        '[[probe]]\nname = "x-repo"\nquestion = "q"\nsource = "dump.s"\n'
        'params = {}\ninputs = []\ncases = [[]]\n')
    tbl, dirs = vectors.tables_for(tmp_path / "q3" / "raw.txt", table())
    assert [p.name for p in tbl][:2] == ["x-own", "x-repo"]
    assert tbl[0].table == "q3.toml"
    assert dirs == [tmp_path, probe.PROBES_DIR]
    assert vectors.template_text(tbl[0], dirs) == TEMPLATE
    assert vectors.template_text(tbl[1], dirs).startswith("; SPDX")
    # a run without its own table: the repo tables only
    t2, d2 = vectors.tables_for(tmp_path / "other" / "raw.txt", table())
    assert [p.name for p in t2] == ["t-add"] and d2 == [probe.PROBES_DIR]
