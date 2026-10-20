"""Tests for ours.py: the nine transfer-queue templates assembled from our own source.

The assembled programs need no closed input, so most of this always runs: each template
must hash to the SHA-256 the cutter checks the closed library's template against (format
§9.2, §10.2), so the bytes are equal without holding any of them here. With the user's
libsrv_um.so, they are also compared with the cut directly.
"""
import hashlib

import pytest

import facts
import fwcutter
import ours
from test_fwcutter import TQ_SHA256

UM = fwcutter.default_um_input()


def test_every_template_has_a_source():
    assert sorted(ours.PROGS) == list(range(len(facts.TQ_NAMES)))
    for name, isa in ours.PROGS.values():
        assert (ours.PROGS_DIR / name).is_file() and isa in ("usse", "pds")


@pytest.fixture(scope="module")
def templates():
    return ours.templates()


def test_templates_hash_like_the_cut(templates):
    assert {i: hashlib.sha256(b).hexdigest() for i, b in templates.items()} == TQ_SHA256


def test_templates_fill_their_records(templates):
    for m in facts.TQ_META:
        assert len(templates[m.id]) == m.size


def test_listing_is_the_bytes(templates):
    for tid in ours.PROGS:
        words = ours.listing(tid)
        width = 8 if ours.PROGS[tid][1] == "usse" else 4
        raw = b"".join(w.to_bytes(width, "little") for w, _ in words)
        assert raw == templates[tid]
        assert all(text for _, text in words)


@pytest.mark.skipif(UM is None, reason="libsrv_um.so not available")
def test_templates_equal_the_cut(templates):
    um = UM.read_bytes()
    k = fwcutter.identify(um, "um")
    assert templates == {t.template: fwcutter.cut_template(k, t, um) for t in k.templates}


def test_assembler_errors_are_reported(tmp_path, monkeypatch):
    bad = tmp_path / "bad.s"
    bad.write_text("frobnicate r0\n")
    monkeypatch.setattr(ours, "PROGS_DIR", tmp_path)
    monkeypatch.setitem(ours.PROGS, facts.TQ_USE_FILL, ("bad.s", "usse"))
    with pytest.raises(ours.SourceError, match="bad.s"):
        ours.assemble(facts.TQ_USE_FILL)
