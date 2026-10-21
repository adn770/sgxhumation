# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Checks on sgx545_labels.r2, the hand-written radare2 labels and comments for the ukernel.

radare2 parses `;`, `|`, `>` and `~` in a script line as command separators, pipes,
redirections and filters, so an unquoted comment such as `CCu a -> b @ 0x10` writes a file
named `b @ 0x10`. Comments are therefore written as `"CCu text" @ 0xADDR`: the quotes keep
the text literal and the temporary seek stays outside them."""
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
LABELS = HERE / "sgx545_labels.r2"

FLAG = re.compile(r'^f ([A-Za-z_][\w.]*) @ 0x([0-9a-f]+)$')
COMMENT = re.compile(r'^"CCu ([^"]+)" @ 0x([0-9a-f]+)$')


def lines():
    return [ln for ln in LABELS.read_text().splitlines() if ln.strip()]


def test_every_line_is_a_flag_or_a_quoted_comment():
    bad = [ln for ln in lines() if not (FLAG.match(ln) or COMMENT.match(ln))]
    assert bad == []


def test_addresses_are_instruction_aligned():
    for ln in lines():
        m = FLAG.match(ln) or COMMENT.match(ln)
        assert int(m.group(2), 16) % 8 == 0, ln


def test_flag_names_and_addresses_are_unique():
    flags = [FLAG.match(ln).groups() for ln in lines() if FLAG.match(ln)]
    names = [n for n, _ in flags]
    addrs = [a for _, a in flags]
    assert len(set(names)) == len(names)
    assert len(set(addrs)) == len(addrs)
