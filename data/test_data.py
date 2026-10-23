# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""data/: the published part of the offline corpus (data/README.md). Every vector file is a
valid version-1 file whose raw readback is here with the recorded hash, and, where the local
corpus is present, no program word of ours also occurs in the closed binaries except the
reviewed idioms of closed-idioms.txt."""
import glob
import hashlib
import json
import pathlib
import struct
import sys

import pytest

DATA = pathlib.Path(__file__).resolve().parent
ROOT = DATA.parent
sys.path.insert(0, str(ROOT / "tools/usse-asm"))
import vectors  # noqa: E402

FILES = sorted(DATA.glob("vectors/**/*.vec.json"))


def test_the_vectors_are_here():
    assert len(FILES) == 83
    assert (DATA / "vectors/vec1.vec.json") in FILES


@pytest.mark.parametrize("path", FILES, ids=lambda p: p.name)
def test_each_file_is_valid_and_its_raw_readback_matches(path):
    batch = json.loads(path.read_text())
    vectors.validate(batch)
    raw = DATA / batch["run"]["raw"]
    assert raw.is_file(), raw
    assert hashlib.sha256(raw.read_bytes()).hexdigest() == batch["run"]["raw_sha256"]


def test_every_table_is_tracked():
    """Each vector's probe table is in the tree: progs/probes, progs/vectors, or the run's round
    under progs/probes/explore (README there)."""
    progs = ROOT / "tools/usse-asm/progs"
    for path in FILES:
        batch = json.loads(path.read_text())
        rnd = pathlib.Path(batch["run"]["raw"]).parts[2] if batch["run"]["raw"].startswith("runs/") else ""
        for v in batch["vectors"]:
            t = v["provenance"]["table"]
            assert any(p.is_file() for p in (progs / "probes" / t, progs / "vectors" / t,
                                              progs / "probes/explore" / rnd / t)), (path.name, t)


def test_nothing_else_is_published():
    """Only the vector files, their raw readbacks and this directory's own files."""
    raws = {json.loads(p.read_text())["run"]["raw"] for p in FILES}
    own = {"README.md", "test_data.py", "closed-idioms.txt"}
    for p in DATA.rglob("*"):
        if p.is_dir() or "__pycache__" in p.parts:
            continue
        rel = str(p.relative_to(DATA))
        assert rel in own or p in FILES or rel in raws, rel


def idioms() -> dict[int, str]:
    """closed-idioms.txt: `0x<word>  <disassembly>` per line, # comments."""
    out = {}
    for line in (DATA / "closed-idioms.txt").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            word, text = line.split(None, 1)
            out[int(word, 16)] = text
    return out


def closed_windows() -> set[int]:
    """Every 4-aligned 8-byte window of the closed binaries in the local corpus."""
    corpus = pathlib.Path(__import__("os").environ.get("SGX_CORPUS", ROOT / "corpus"))
    srcs = glob.glob(f"{corpus}/fw/**/*.bin", recursive=True) + glob.glob(f"{corpus}/fw/*.fw") \
        + glob.glob(f"{corpus}/golden/captures/programs/*.bin") \
        + glob.glob(f"{corpus}/fetch/intel-pkg/lib/libsrv_*.so*") \
        + glob.glob(f"{corpus}/fetch/ubuntu-pkg/extracted/usr/lib/libsrv_*.so*")
    out = set()
    for p in srcs:
        b = pathlib.Path(p).read_bytes()
        out.update(struct.unpack_from("<Q", b, o)[0] for o in range(0, len(b) - 7, 4))
    return out


def test_no_program_word_is_closed_except_the_idioms():
    closed = closed_windows()
    if not closed:
        pytest.skip("no closed binaries (the local corpus) to compare with")
    allowed = idioms()
    found = set()
    for path in FILES:
        for v in json.loads(path.read_text())["vectors"]:
            assert v["program"]["isa"] == "usse"
            found.update(int(w, 16) for w in v["program"]["words"] if int(w, 16) in closed)
    assert not found - allowed.keys(), [hex(w) for w in sorted(found - allowed.keys())]
    # the list stays exact: an idiom no vector uses any more is dropped
    assert not allowed.keys() - found, [hex(w) for w in sorted(allowed.keys() - found)]
