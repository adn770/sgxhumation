# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for the ISA document builder (build.py).

Unit tests use small synthetic forms; the integration tests read the real form tables
through formdump and check the committed documents: fresh, complete (every form of both
tables in the appendix and in exactly one group section) and at most 100 columns wide.
"""
from __future__ import annotations

import pathlib
import re
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build as B     # noqa: E402


def fld(chunks, values=None, kind="enum", **kw):
    """A synthetic field in formdump's JSON shape."""
    width = sum(w for _lo, w in chunks)
    if values is None:
        values = [[v, f"#{v}"] for v in range(1 << min(width, 4))]
    return {"chunks": [list(c) for c in chunks], "width": width, "kind": kind,
            "parse": False, "values": values, **kw}


def form(pat, mask, val, fields, index=0):
    bits = mask
    for f in fields:
        for lo, w in f["chunks"]:
            bits |= ((1 << w) - 1) << lo
    return {"index": index, "pat": pat, "mask": f"{mask:016X}", "val": f"{val:016X}",
            "dflt": "0" * 16, "bits_read": f"{bits:016X}", "fields": fields}


PRED = fld([(5, 2)], [[0, ""], [1, "p0 "], [2, "p1 "], [3, "!p0 "]])
END = fld([(4, 1)], [[0, ""], [1, ".end"]])
DST = fld([(0, 4)], [[v, f"r{v}"] for v in range(16)], kind="other")
SMALL = form("{0}op{1} {2}", 0b11 << 14 | 1 << 7, 0b10 << 14, [PRED, END, DST])


# ---- field names, patterns and mnemonics --------------------------------------------

def test_field_names_from_values():
    assert B.field_names(SMALL, "pds") == ["pred", "end", "dst"]


def test_named_pattern():
    assert B.named_pattern(SMALL, "pds") == "<pred>op<end> <dst>"


def test_mnemonics_and_modifiers():
    mn, mods = B.mnemonics(SMALL, "pds")
    assert mn == ["op"]
    assert mods == [".end"]


def test_mnemonics_from_an_operation_field():
    op = fld([(8, 2)], [[0, "add"], [1, "sub"], [2, None], [3, "mul"]])
    f = form("{0}{1}{2} {3}", 0, 0, [PRED, op, END, DST])
    assert B.mnemonics(f, "pds")[0] == ["add", "mul", "sub"]


def test_modifiers_after_a_dot_literal():
    sign = fld([(8, 1)], [[0, "u"], [1, "s"]])
    f = form("imae{0}.{1} {2}", 0, 0, [END, sign, DST])
    mn, mods = B.mnemonics(f, "pds")
    assert mn == ["imae"]
    assert mods == [".end", ".s", ".u"]


# ---- diagrams -------------------------------------------------------------------------

def test_diagram_of_a_16_bit_word():
    """Fixed bits print their value, ignored bits '-', fields their name; bit numbers
    head each box, highest bit first."""
    got = B.diagram(SMALL, "pds", 16)
    assert got == "\n".join([
        " 15 14 13 8 7   6    5 4     3   0",
        "+-----+----+---+------+-----+-----+",
        "|  10 | -  | 0 | pred | end | dst |",
        "+-----+----+---+------+-----+-----+",
    ])


def test_diagram_splits_a_64_bit_word_into_halves():
    got = B.diagram(form("x", 1 << 63, 1 << 63, []), "usse", 64)
    rows = got.split("\n")
    assert rows[0].split()[:2] == ["63", "62"]
    assert rows[0].rstrip().endswith("32")
    assert any(r.startswith(" 31") and r.rstrip().endswith("0") for r in rows)
    assert all(len(r) <= 100 for r in rows)


def test_diagram_keys_long_labels_in_a_crowded_row():
    """Too many one-bit fields with long names for 100 columns: the row gets letter keys
    and a legend, and stays within the limit."""
    fields = [fld([(i, 1)], [[0, ""], [1, f".flagnumber{i}"]]) for i in range(32)]
    f = form("".join(f"{{{i}}}" for i in range(10)), 0, 0, fields[:10])
    f["fields"] = fields
    got = B.diagram(f, "pds", 32)
    assert all(len(r) <= 100 for r in got.split("\n"))
    assert "flagnumber31" in got        # the legend names it


def test_layouts_share_a_diagram():
    a = form("a{0}", 0xF << 12, 0x1 << 12, [END], index=1)
    b = form("b{0}", 0xF << 12, 0x2 << 12, [END], index=2)
    c = form("c{0}", 0xF << 12, 0x3 << 12, [END, DST], index=3)
    layouts = B.layouts([a, b, c], "pds")
    assert [[f["index"] for f in group] for group in layouts] == [[1, 2], [3]]
    # differing fixed bits are shown as '?'
    assert "?" in B.diagram(a, "pds", 16, others=[b])


# ---- field descriptions -----------------------------------------------------------------

def test_field_bits_text():
    f = fld([(21, 7), (32, 2), (51, 1)], kind="other")
    assert B.bits_text(f) == "27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]"


def test_field_description_lists_enum_values():
    text = B.describe_field("pred", PRED, "pds")
    assert "`p0 `" not in text and "`p0`" in text
    assert "6:5" in text


def test_wrap_keeps_lines_short():
    text = "word " * 60
    out = B.wrap_item("- ", text)
    assert all(len(line) <= 100 for line in out.split("\n"))
    assert out.split("\n")[1].startswith("  ")


# ---- manifests and assembly -------------------------------------------------------------

def test_parse_manifest():
    book = B.parse_manifest("\n".join([
        "# comment",
        "output design/x.md",
        "isa usse",
        "number yes",
        "usse/a.md",
        "@appendix usse",
    ]), "x")
    assert book.output == "design/x.md"
    assert book.options == {"isa": "usse", "number": "yes"}
    assert book.items == ["usse/a.md", "@appendix usse"]


def test_assemble_snippets_and_directives(tmp_path):
    (tmp_path / "s").mkdir()
    tag = "SPDX" + "-License-Identifier"        # split, so that reuse does not parse it
    (tmp_path / "s" / "a.md").write_text(
        f"<!-- {tag}: MIT -->\n# Title\n\n## First\n\nText.\n\n<!-- @echo hello -->\n")
    (tmp_path / "s" / "b.md").write_text("## Second\n\n### Sub\n\n```\n## not a heading\n```\n")
    book = B.parse_manifest("output o.md\nnumber yes\ns/a.md\ns/b.md\n@echo bye\n", "t")
    out = B.assemble(book, tmp_path, B.Context(None, extra={"echo": lambda ctx, a: a}))
    assert "SPDX" not in out
    assert "## 1. First" in out and "## 2. Second" in out and "### 2.1 Sub" in out
    assert "## not a heading" in out
    assert "hello" in out and "bye" in out


def test_missing_snippet_is_an_error(tmp_path):
    book = B.parse_manifest("output o.md\nnope.md\n", "t")
    with pytest.raises(B.BuildError):
        B.assemble(book, tmp_path, B.Context(None))


def test_unknown_directive_is_an_error(tmp_path):
    book = B.parse_manifest("output o.md\n@frobnicate\n", "t")
    with pytest.raises(B.BuildError):
        B.assemble(book, tmp_path, B.Context(None))


# ---- the real tables and documents ------------------------------------------------------

@pytest.fixture(scope="module")
def tables():
    return B.load_tables()


@pytest.fixture(scope="module")
def books():
    return B.load_books()


def test_tables_have_both_isas(tables):
    assert len(tables["usse"]["forms"]) > 200
    assert len(tables["pds"]["forms"]) > 20


def test_example_encodes_our_instruction(tables):
    ctx = B.Context(tables)
    out = B.directive(ctx, "example", "usse mov.skipinv.end o0, #0x12345678")
    assert "mov.skipinv.end o0, #0x12345678" in out
    assert "0x12345678" in out and "form #" in out


def test_documents_are_fresh(tables, books):
    for book in books:
        path = B.ROOT / book.output
        assert path.read_text() == B.build_book(book, tables), \
            f"stale: run doc/isa/build.py ({book.output})"


def test_every_form_is_in_the_appendix(tables, books):
    for book in books:
        isa = book.options.get("isa")
        if "@appendix " + str(isa) not in book.items:
            continue
        text = (B.ROOT / book.output).read_text()
        for f in tables[isa]["forms"]:
            if f["pat"] is None:
                continue
            assert f"#{f['index']} " in text
            assert B.named_pattern(f, isa) in text, (isa, f["index"])


def test_every_form_is_in_exactly_one_group(tables, books):
    for book in books:
        isa = book.options.get("isa")
        if book.options.get("groups") != "complete":
            continue
        claims = B.group_claims(book, tables)
        for f in tables[isa]["forms"]:
            if f["pat"] is not None:
                assert claims.get(f["index"]) == 1, (isa, f["index"], f["pat"])


def test_lines_are_at_most_100_columns(books):
    for book in books:
        for n, line in enumerate((B.ROOT / book.output).read_text().split("\n"), 1):
            assert len(line) <= 100, f"{book.output}:{n}"
    for path in (HERE / "content").rglob("*.md"):
        for n, line in enumerate(path.read_text().split("\n"), 1):
            assert len(line) <= 100, f"{path}:{n}"


def test_every_snippet_is_used(books):
    used = {item for book in books for item in book.items if item.endswith(".md")}
    for path in (HERE / "content").rglob("*.md"):
        assert str(path.relative_to(HERE / "content")) in used, path


def test_probe_notes_are_marked_and_listed(books):
    """One snippet per fact pending a hardware probe, each marked and listed in its book."""
    probes = sorted((HERE / "content").glob("*/probe/*.md"))
    assert probes
    for path in probes:
        assert B.PROBE_MARK in path.read_text(), path
    for book in books:
        text = (B.ROOT / book.output).read_text()
        for item in book.items:
            if "/probe/" in item:
                title = B.snippet_title(HERE / "content" / item)
                assert text.count(title) >= 2, (book.output, title)   # section + index
