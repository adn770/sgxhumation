# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for asmmap.py: sgx-asm's images and map read as firmware-format records.

The nine transfer-queue templates written as one sgx-asm source
(tools/usse-asm/sgx/transfer.s) give ours.py's templates byte for byte and, from their
.patch lines, facts.TQ_PATCHES record for record.
"""
import shutil

import pytest

import asmmap
import facts
import ours

pytestmark = pytest.mark.skipif(shutil.which("cargo") is None, reason="needs cargo")

TRANSFER = asmmap.REPO / "tools" / "usse-asm" / "sgx" / "transfer.s"


@pytest.fixture(scope="module")
def transfer(tmp_path_factory):
    return asmmap.assemble(TRANSFER, tmp_path_factory.mktemp("tq"))


def test_sections_are_the_templates(transfer):
    assert [s.id for s in transfer.sections] == list(ours.PROGS)
    templates = ours.templates()
    for s in transfer.sections:
        name, isa = ours.PROGS[s.id]
        assert s.isa == isa, name
        assert s.image == templates[s.id], name


def test_patches_are_the_facts(transfer):
    assert transfer.patches == facts.TQ_PATCHES


def test_pds_data_segments(transfer):
    # the data words lead each PDS image; labels count from the first instruction
    assert transfer.section("pds_prim").data == 4


def test_symbols_and_errors(tmp_path):
    src = tmp_path / "s.s"
    src.write_text(".section a, usse, 3\nnop\nhere: nop\n.section b, data, 4\n"
                   ".word addr(a, 8), code_word(here, 0xf)\n")
    p = asmmap.assemble(src, tmp_path / "out")
    assert p.symbols == [asmmap.Symbol("here", "a", 1, 8)]
    assert [(e.buffer, e.offset, e.kind, e.target, e.addend) for e in p.patches] == [
        (4, 0, facts.K_ADDR, 3, 8), (4, 4, facts.K_CODE_WORD, 1, 0xf)]
    src.write_text("nop\n")
    with pytest.raises(asmmap.AsmError, match="statement outside a section"):
        asmmap.assemble(src, tmp_path / "bad")
