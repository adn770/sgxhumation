# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for regtouch.py — firmware MMIO register-access scanner."""
import regtouch

LINES = [
    "     0: 0xFC22B08002002600\tmov r16, #0xAD002600",
    "     5: 0xFE00000000000000\tldr r16, #75, drc0",
    "     6: 0xFE00000000000000\tp2 str #77, #64",
    "     7: 0xFE00000000000000\t!p0 str #421, r3",
    "     8: 0xFE28000000000914\tstr r20, r18",     # register-supplied address: not static
    "garbage line",
]


def test_scan_finds_immediate_register_accesses():
    acc = regtouch.scan(LINES)
    assert [(a.insn, a.kind, a.reg) for a in acc] == [
        (5, "R", 0x12C), (6, "W", 0x134), (7, "W", 0x694),
    ]


def test_summarize_counts_reads_and_writes():
    s = regtouch.summarize(regtouch.scan(LINES))
    assert s == {0x12C: (1, 0), 0x134: (0, 1), 0x694: (0, 1)}


def test_load_names_skips_field_macros(tmp_path):
    p = tmp_path / "defs.h"
    p.write_text("#define EUR_CR_EVENT_STATUS 0x012C\n"
                 "#define EUR_CR_EVENT_STATUS_MASK 0x00000001\n"
                 "#define EUR_CR_EVENT_HOST_CLEAR 0x0134U\n")
    assert regtouch.load_names([p]) == {0x12C: "EUR_CR_EVENT_STATUS",
                                        0x134: "EUR_CR_EVENT_HOST_CLEAR"}


def test_r2_comments_name_each_access_site():
    s = regtouch.r2_comments(regtouch.scan(LINES), {0x12C: "EUR_CR_EVENT_STATUS"})
    assert "CCu read EUR_CR_EVENT_STATUS @ 0x28" in s     # insn 5 * 8 bytes
    assert "CCu write 0x0134 @ 0x30" in s                 # unnamed -> hex offset
