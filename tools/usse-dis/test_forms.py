# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Pin the table-driven USSE decoder (usse_form.c) to refdisasm545.

The expected texts are refdisasm545 output for the same words, so these tests
pin oracle compatibility without needing the oracle. The words are the
instruction forms of the transfer programs (design/sgx545_transfer.md §4.2.4
and §4.4.3, where they are published one by one) plus variants of their
fields. The last tests run random_oracle_check.py, which compares every form
of the table against refdisasm545 on random words, when the oracle is built.
"""
import pathlib
import struct
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
REFDIS = HERE / "refdisasm" / "refdisasm545"

FORMS = {
    # phas: immediate and register forms
    0xFA44070000000000: "phas #0, #0, pixel, end, parallel",
    0xFA44270100000010: "phas #16, #4, pixel, end, perinstance",
    0xFA40070000000081: "phas r1, r1",
    # limm: flags, predicates and every destination bank
    0xFCA4000100000000: "mov.skipinv.end o0, #0x00000000",
    0xFCA7F19100136699: "mov.skipinv.end o0, #0xFF336699",
    0xFCA0000000000000: "mov.skipinv r0, #0x00000000",
    0xFCA0000000600000: "mov.skipinv r3, #0x00000000",
    0xFC20000000000000: "mov r0, #0x00000000",
    0xFC24080100000000: "p3 mov.end o0, #0x00000000",
    0xFCAC000108000000: "mov.skipinv.end g0, #0x00000000",
    0xFCAC000200000000: "mov.skipinv.end i., #0x00000000",
    0xFCAC000200600000: "mov.skipinv.end i.lh, #0x00000000",
    0xFCA4000304000000: "mov.skipinv.end o[il + #0], #0x00000000",
    0xFCA0000000000000 | 124 << 21: "mov.skipinv i0, #0x00000000",
    # class-5 mov: flags, write mask and source banks
    0x28851001A0000000: "mov.skipinv.end o0, pa0",
    0x2825100020000000: "mov.end.repeat2 r0, r0",
    0x28851001A0000000 ^ 1 << 49: "mov.skipinv.end o0, #0x00000000",
    0x28851001A0000000 ^ 1 << 45: "mov.skipinv.end o0.xy, pa0",
    # emitpix
    0xFB20010000200080: "emitpix1.freep #0 /* incp */, r0, r1",
    0xFB24000000208180: "emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000000",
    0xFB26004080200000: "emitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200",
    # nop flags
    0xF804014000000000: "nop.end",
    0xF884094000000003: "nop.syncs.nosched.end.tog.trigger",
}


def _disasm(tool, tmp_path, words):
    blob = tmp_path / "words.bin"
    blob.write_bytes(struct.pack(f"<{len(words)}Q", *words))
    out = subprocess.run([str(tool), str(blob)], capture_output=True, text=True,
                         check=True).stdout
    return [line.split("\t", 1)[1].strip() for line in out.splitlines() if "\t" in line]


@pytest.fixture(scope="module", autouse=True)
def build():
    subprocess.run(["make", "-s", "-C", str(HERE), "usse-dis"], check=True)


def test_transfer_forms_decode_as_refdisasm(tmp_path):
    words = list(FORMS)
    got = _disasm(HERE / "usse-dis", tmp_path, words)
    assert dict(zip(words, got)) == FORMS


def test_forms_dump_lists_every_form():
    out = subprocess.run([str(HERE / "usse-dis"), "--forms"], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    assert len(out) > 50
    for line in out:
        idx, mask, val, bits, pat = line.split("\t")
        assert int(val, 16) & ~int(mask, 16) == 0
        assert int(mask, 16) & ~int(bits, 16) == 0
    assert any("emitpix2" in line for line in out)


@pytest.mark.skipif(not REFDIS.exists(), reason="refdisasm545 not built")
def test_pinned_texts_match_refdisasm(tmp_path):
    words = list(FORMS)
    assert _disasm(REFDIS, tmp_path, words) == list(FORMS.values())


@pytest.mark.skipif(not REFDIS.exists(), reason="refdisasm545 not built")
def test_random_oracle_check():
    r = subprocess.run([sys.executable, str(HERE / "random_oracle_check.py"), "--per-form", "300"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
