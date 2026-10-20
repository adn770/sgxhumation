"""Tests for fwcutter.py — identifying the closed library and cutting the firmware file."""
import dataclasses
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
        if k.role != "init":
            continue
        ids = [s.buffer for s in k.sections]
        assert sorted(ids) == [0, 1, 2, 3, 4, 5]
        by_id = {b.id: b for b in facts.BUFFERS}
        for s in k.sections:
            assert s.size == by_id[s.buffer].size
            assert s.offset + s.size <= k.size


def test_known_um_table_is_consistent():
    (um,) = [k for k in facts.KNOWN_INPUTS if k.role == "um"]
    assert um.ddk == (1, 7, 862890) and um.size == 196152 and not um.sections
    meta = {m.id: m for m in facts.TQ_META}
    assert sorted(t.template for t in um.templates) == sorted(meta) == list(range(9))
    assert facts.TQ_NAMES[7:] == ("USE_COPY", "PDS_PRIM_1SRC")
    for t in um.templates:
        if t.template in COPY_PIECES:
            assert [(p.src, p.size, p.dst) for p in t.pieces] == COPY_PIECES[t.template]
    for t in um.templates:
        assert t.size == meta[t.template].size and len(t.sha256) == 64
        for p in t.pieces:
            assert p.src + p.size <= um.size and p.dst + p.size <= t.size


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


# --- libsrv_um.so: the transfer-queue templates (format §9) ------------------------------

UM = fwcutter.default_um_input()
needs_um = pytest.mark.skipif(BLOB is None or UM is None, reason="libsrv_um.so not available")

# Format §9.2 and §10.2, in full.
TQ_SHA256 = {
    0: "7727cde6c86f89289c02a2c24a807c2112d33438e01c6bc19bda1b49a7a400b0",
    1: "74c3e748098b2bb8929d17726a235565ab9d36ace0bd4897604ecc6e1847cece",
    2: "d03fa3dcad8f058155bee1532d2432158920ef65249130fd9a5e90c35a6667be",
    3: "dfbb3b5d76d2776de4aad6a5760a52b7253d9f9a0dc3eb7090dbe1b1a4ccddaa",
    4: "0963b59aa21ddb00386bb5fa8fad3bab0cd50806da4b3513ce5b362c55005092",
    5: "0963b59aa21ddb00386bb5fa8fad3bab0cd50806da4b3513ce5b362c55005092",
    6: "65e78fa8304ae7ef41064086db9ca16a3b2da3ec701b5f61fa076c7c19881671",
    7: "9d0dfd83a18e929ed4da9bd5363bedd84c26e5d652515c8d646a545d5a78b297",
    8: "e3f3a906453236f89efa7cdd4132db26a2f2cb45010dd532da0e4acf13da3758",
}
# The copy templates come from .rodata in one piece each (transfer §4.4.6).
COPY_PIECES = {7: [(0x21D60, 16, 0)], 8: [(0x22960, 64, 0)]}


def test_um_alone_is_refused_as_init():
    with pytest.raises(fwcutter.UnknownInput, match="not a known libsrv_init.so"):
        fwcutter.cut(b"\x7fELF" + bytes(100), b"")


@needs_blob
def test_unknown_um_is_refused():
    with pytest.raises(fwcutter.UnknownInput, match="not a known libsrv_um.so"):
        fwcutter.cut(BLOB.read_bytes(), b"\x7fELF" + bytes(100))


@needs_blob
def test_init_given_as_um_is_refused():
    with pytest.raises(fwcutter.UnknownInput, match="not a known libsrv_um.so"):
        fwcutter.cut(BLOB.read_bytes(), BLOB.read_bytes())


@needs_blob
def test_single_input_still_writes_v1():
    raw = F.pack(fwcutter.cut(BLOB.read_bytes()))
    assert int.from_bytes(raw[8:10], "little") == 1
    fw = F.unpack(raw)
    assert not fw.tq_images and not fw.tq_meta and F.V_TQ_CMD not in fw.values


@needs_um
def test_v3_cut_carries_every_template():
    fw = fwcutter.cut(BLOB.read_bytes(), UM.read_bytes())
    raw = F.pack(fw)
    assert int.from_bytes(raw[8:10], "little") == 3
    assert F.unpack(raw) == fw
    assert {i: hashlib.sha256(b).hexdigest() for i, b in fw.tq_images.items()} == TQ_SHA256
    assert fw.tq_meta == facts.TQ_META and fw.tq_patches == facts.TQ_PATCHES
    assert fw.values[F.V_TQ_CMD] == facts.TQ_CMD
    # the v1 part is unchanged
    v1 = fwcutter.cut(BLOB.read_bytes())
    assert (fw.images, fw.patches, fw.meta) == (v1.images, v1.patches, v1.meta)


@needs_um
def test_v2_cut_is_the_v3_cut_without_the_copy():
    v3 = fwcutter.cut(BLOB.read_bytes(), UM.read_bytes())
    v2 = fwcutter.cut(BLOB.read_bytes(), UM.read_bytes(), version=2)
    raw = F.pack(v2)
    assert int.from_bytes(raw[8:10], "little") == 2
    assert F.unpack(raw) == v2
    copy = {facts.TQ_USE_COPY, facts.TQ_PDS_PRIM_1SRC}
    assert sorted(v2.tq_images) == list(range(7))
    assert v2.tq_images == {i: b for i, b in v3.tq_images.items() if i not in copy}
    assert v2.tq_meta == [m for m in v3.tq_meta if m.id not in copy]
    assert v2.tq_patches == [e for e in v3.tq_patches if e.buffer not in copy]
    assert v2.values == v3.values and v2.images == v3.images


@needs_blob
def test_unknown_version_is_refused():
    for v in (1, 4):
        with pytest.raises(ValueError):
            fwcutter.cut(BLOB.read_bytes(), UM.read_bytes() if UM else b"", version=v)


@needs_um
def test_copy_shader_has_a_twin():
    # transfer §4.4.3: identical bytes at 0x223a8; the cut takes table entry 0
    um = UM.read_bytes()
    assert um[0x223A8:0x223B8] == um[0x21D60:0x21D70]


@needs_um
def test_template_pieces_are_hash_checked(monkeypatch):
    (um,) = [k for k in facts.KNOWN_INPUTS if k.role == "um"]
    t = um.templates[2]
    bad = dataclasses.replace(t, pieces=t.pieces[:-1])
    broken = dataclasses.replace(um, templates=(*um.templates[:2], bad, *um.templates[3:]))
    monkeypatch.setattr(facts, "KNOWN_INPUTS",
                        [broken if k is um else k for k in facts.KNOWN_INPUTS])
    with pytest.raises(fwcutter.UnknownInput, match="template 2"):
        fwcutter.cut(BLOB.read_bytes(), UM.read_bytes())


@needs_um
def test_cli_cut_two_inputs(tmp_path):
    out = tmp_path / "sgx545_ukernel.fw"
    subprocess.run([sys.executable, str(HERE / "fwcutter.py"), "cut", str(BLOB), str(UM),
                    "-o", str(out)], check=True)
    info = subprocess.run([sys.executable, str(HERE / "fwcutter.py"), "info", str(out)],
                          capture_output=True, text=True, check=True).stdout
    assert "v3" in info and "TQ_TEMPLATE" in info and "PDS_PIXEVENT" in info
    assert "PDS_PRIM_1SRC" in info and "USE_COPY" in info
    subprocess.run([sys.executable, str(HERE / "fwcutter.py"), "cut", str(BLOB), str(UM),
                    "--format-version", "2", "-o", str(out)], check=True)
    info = subprocess.run([sys.executable, str(HERE / "fwcutter.py"), "info", str(out)],
                          capture_output=True, text=True, check=True).stdout
    assert "v2" in info and "PDS_PIXEVENT" in info and "USE_COPY" not in info
