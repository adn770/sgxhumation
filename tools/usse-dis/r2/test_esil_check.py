# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for esil_check.py: the survey tally, the token check and the cross-check model.

The last test runs the cross-check against the radare2 fork; it needs R2_PREFIX to point at
an install of the fork (the out-of-tree plugin has no ESIL) and is skipped otherwise."""
import random
import subprocess

import pytest

import esil_check as ec


def test_base_mnemonic_drops_predicate_and_flags():
    assert ec.base_mnemonic("p1 and.testtaz.chan0 !c0, p0, r1, r2") == "and"
    assert ec.base_mnemonic("!p0 ba #0x00000102") == "ba"
    assert ec.base_mnemonic("ldr r1, #7, drc0") == "ldr"
    assert ec.base_mnemonic("Pn and r1, r2, r3") == "and"


def test_tally_counts_esil_per_base_and_skips_invalid():
    ops = [{"opcode": "mov r1, r2", "esil": "r2,r1,="},
           {"opcode": "mov i.l, r4", "esil": ""},
           {"opcode": "invalid"},
           {"opcode": "str #7, r1", "esil": "r1,0x10000001c,=[4]"}]
    st = ec.tally(ops)
    assert st == {"mov": [1, 1], "str": [1, 0]}
    assert ec.report(st).splitlines()[0] == "words 3: esil 2 (66.7%)"


def test_profile_regs_and_bad_tokens():
    arp = "=PC\tpc\ngpr\tr0\t.32\t0\t0\ngpr\tpa3\t.32\t4\t0\n"
    regs = ec.profile_regs(arp)
    assert regs == {"r0", "pa3"}
    assert ec.bad_tokens("pa3,0x100000014,=[4]", regs) == []
    assert ec.bad_tokens("r0,i.l,=", regs) == ["i.l"]


def test_alu_and_test_predicates():
    assert ec.alu("rol", 0x80000001, 4) == 0x18
    assert ec.alu("rol", 5, 0) == 5
    assert ec.alu("asr", 0x80000000, 4) == 0xF8000000
    assert ec.alu("iadd32", ec.M32, 1) == 0
    assert ec.test_pred("nat", 0x80000000) == 1
    assert ec.test_pred("nonz", 0) == 0
    assert ec.test_pred("noz", 0) == 1


def test_register_file_window():
    assert ec.cr_addr(7) == 0x10000001C
    assert ec.cr_addr(0x3FFF) == 0x10000FFFC


def test_ldr_case_reads_the_window():
    rng = random.Random(0)
    for _ in range(200):
        cs = ec.gen_case(rng)
        if cs and cs.text.startswith("ldr"):
            break
    (addr, val), = cs.mem.items()
    dst = cs.text.split()[1].rstrip(",")
    assert cs.expect == {dst: val}
    assert addr >= ec.CR_BASE and (addr - ec.CR_BASE) % 4 == 0
    assert f"wv4 0x{val:x} @ 0x{addr:x}" in cs.setup()


def test_str_case_expects_the_window_word():
    rng = random.Random(1)
    for _ in range(400):
        cs = ec.gen_case(rng)
        if cs and cs.text.startswith("str"):
            break
    (key, val), = cs.expect.items()
    assert key.startswith("@0x1") and cs.reads() == [f"pv4 {key}"]


def test_mov_repeat_cases_use_disjoint_profile_ranges():
    rng = random.Random(2)
    seen = 0
    for _ in range(2000):
        cs = ec.gen_case(rng)
        if not cs or not cs.text.startswith("mov.repeat"):
            continue
        seen += 1
        dst = set(cs.expect)
        src = set(cs.regs) - dst
        assert len(dst) == len(src) == int(cs.text.split()[0][10:])
        assert all(int(r.lstrip("opar")) < (64 if r[0] == "r" else 32) for r in dst | src)
    assert seen > 50


def _fork_r2():
    try:
        out = subprocess.run([ec.tool("radare2"), "-q", "-a", "usse", "-b", "32", "-c",
                              "wx 0002202000102128; aoj 1", "malloc://8"],
                             capture_output=True, text=True).stdout
    except OSError:
        return False
    return '"esil":"r4,r1,=,r5,r2,="' in out


@pytest.mark.skipif(not _fork_r2(), reason="R2_PREFIX is not an install of the radare2 fork")
def test_cross_check_against_the_fork():
    n, bad = ec.cross(seed=7, count=300)
    assert n == 300
    assert bad == []
