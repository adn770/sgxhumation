"""Tests for fwcutter.py — identifying the closed library and cutting the firmware file."""
import hashlib
import pathlib
import subprocess
import sys

import pytest

import facts
import fwcutter
import fwformat as F

HERE = pathlib.Path(__file__).resolve().parent
EXTRACTED = HERE.parent.parent / "harness" / "ukernel-extract" / "extracted"
BLOB = fwcutter.default_input()
needs_blob = pytest.mark.skipif(BLOB is None, reason="libsrv_init.so not available")


def test_unknown_input_is_refused():
    with pytest.raises(fwcutter.UnknownInput, match="not a known libsrv_init.so"):
        fwcutter.cut(b"\x7fELF" + bytes(100))


def test_known_input_table_is_consistent():
    for k in facts.KNOWN_INPUTS:
        assert len(k.sha256) == 64
        ids = [s.buffer for s in k.sections]
        assert sorted(ids) == [0, 1, 2, 3, 4, 5]
        by_id = {b.id: b for b in facts.BUFFERS}
        for s in k.sections:
            assert s.size == by_id[s.buffer].size
            assert s.offset + s.size <= k.size


@needs_blob
def test_static_carve_equals_harness_template():
    fw = fwcutter.cut(BLOB.read_bytes())
    tpl = EXTRACTED / "sgx545_ukernel_template.bin"
    if not tpl.exists():
        pytest.skip("harness template not present")
    assert fw.images[0] == tpl.read_bytes()
    assert set(fw.images) == {0, 1, 2, 3, 4, 5}


@needs_blob
def test_section_hashes_are_checked():
    blob = bytearray(BLOB.read_bytes())
    k = fwcutter.identify(bytes(blob))
    for s in k.sections:
        assert hashlib.sha256(blob[s.offset:s.offset + s.size]).hexdigest() == s.sha256


@needs_blob
def test_cli_cut_and_info(tmp_path):
    out = tmp_path / "sgx545_ukernel.fw"
    subprocess.run([sys.executable, str(HERE / "fwcutter.py"), "cut", str(BLOB), "-o", str(out)],
                   check=True)
    fw = F.unpack(out.read_bytes())
    assert fw.ddk == (1, 7, 862890) and fw.build_options == 0x60218
    info = subprocess.run([sys.executable, str(HERE / "fwcutter.py"), "info", str(out)],
                          capture_output=True, text=True, check=True).stdout
    assert "DDK 1.7.862890" in info and "IMAGE" in info and "CUT" in info
