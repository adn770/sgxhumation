# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""regsnap.py: parse debugfs dri/N/sgx/regs, write snapshots, diff them, and take the
before/after pair around a command. The laptop is replaced by a fake runner."""
from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import regsnap  # noqa: E402


def regs_text(*, live=1, sw=3, r408=0x10000000, hc0=1, ccb=(0x40, 0x40), loaded=1):
    lines = ["sgxfive regs 1",
             f"state loaded {loaded} live {live} sw_events {sw} bif_faults 0 ccb_sent 2 "
             "deferred 0",
             "reg 0x0000 0x00000000",
             "reg 0x0200 skip",
             f"reg 0x0408 0x{r408:08x}",
             "reg 0x0c08 0x00000000"]
    if loaded:
        lines += [f"hostctl 0x00 0x{hc0:08x}", "hostctl 0x04 0x00000000",
                  "ta3dctl 0xb8 0x00000000",
                  f"ccbctl 0x00 0x{ccb[0]:08x}", f"ccbctl 0x04 0x{ccb[1]:08x}"]
    return "\n".join(lines + ["end"]) + "\n"


def test_parse_keys_and_values():
    s = regsnap.parse(regs_text())
    assert s.version == 1
    assert s.state["sw_events"] == 3 and s.state["live"] == 1
    assert s.values["reg 0x0408"] == 0x10000000
    assert s.values["reg 0x0200"] is None            # skipped: never read
    assert s.values["hostctl 0x00"] == 1
    assert s.values["ccbctl 0x04"] == 0x40
    assert list(s.values)[:2] == ["reg 0x0000", "reg 0x0200"]   # file order kept


@pytest.mark.parametrize("bad", [
    "",
    "sgxfive regs 2\nend\n",                           # unknown version
    "sgxfive regs 1\nreg 0x0000 0x00000000\n",         # truncated: no end
    "sgxfive regs 1\nreg 0x0000 zz\nend\n",            # not a value
    "sgxfive regs 1\nreg 0x0000 0x0\nreg 0x0000 0x1\nend\n",   # duplicate
])
def test_parse_rejects(bad):
    with pytest.raises(ValueError):
        regsnap.parse(bad)


def test_snapshot_file_round_trips(tmp_path):
    p = tmp_path / "a.regs"
    regsnap.write_snapshot(p, regs_text(), label="boot before",
                           meta={"command": "echo 1 > boot"})
    text = p.read_text()
    assert text.startswith("# regsnap 1\n# label: boot before\n")
    assert "# command: echo 1 > boot\n" in text
    assert "\nreg 0x0408 0x10000000\n" in text      # the raw lines, diff-able as is
    s = regsnap.read_snapshot(p)
    assert s.meta["label"] == "boot before"
    assert s.values["reg 0x0408"] == 0x10000000


def test_diff_lists_changes_in_order():
    a = regsnap.parse(regs_text())
    b = regsnap.parse(regs_text(sw=5, r408=0x10001000, ccb=(0x40, 0x80)))
    d = regsnap.diff(a, b)
    assert [c.key for c in d] == ["state sw_events", "reg 0x0408", "ccbctl 0x04"]
    assert d[1].before == 0x10000000 and d[1].after == 0x10001000


def test_diff_of_loaded_and_unloaded():
    a = regsnap.parse(regs_text(loaded=0))
    b = regsnap.parse(regs_text())
    d = {c.key: c for c in regsnap.diff(a, b)}
    assert d["hostctl 0x00"].before is regsnap.ABSENT
    assert d["hostctl 0x00"].after == 1
    assert "hostctl 0x04" in d                         # new keys show even when zero


def test_format_diff_with_names():
    a = regsnap.parse(regs_text())
    b = regsnap.parse(regs_text(r408=0x10001000))
    out = regsnap.format_diff(regsnap.diff(a, b), names={0x408: "RGN_BASE"})
    assert out == "reg 0x0408 RGN_BASE  0x10000000 -> 0x10001000  (xor 0x00001000)\n"
    assert regsnap.format_diff([]) == "no change\n"


def test_load_names(tmp_path):
    h = tmp_path / "defs.h"
    h.write_text("#define FOO_CTRL 0x0408U\n#define FOO_CTRL_MASK 0xffU\n"
                 "#define BAR 0x0C08\n")
    assert regsnap.load_names([h]) == {0x408: "FOO_CTRL", 0xc08: "BAR"}


class FakeTarget:
    def __init__(self, texts):
        self.texts = list(texts)
        self.calls = []

    def __call__(self, command: str) -> str:
        self.calls.append(command)
        if command.startswith("sudo cat "):
            return self.texts.pop(0)
        return "ran\n"


def test_read_regs_uses_the_dri_minor():
    t = FakeTarget([regs_text()])
    s = regsnap.read_regs(t, dri=1)
    assert t.calls == ["sudo cat /sys/kernel/debug/dri/1/sgx/regs"]
    assert s.state["sw_events"] == 3


def test_around_writes_before_after_and_diff(tmp_path):
    t = FakeTarget([regs_text(), regs_text(sw=4, r408=0x10002000)])
    changes = regsnap.around(t, "sudo sh -c 'echo 1 > x'", tmp_path, "xfer_fill")
    assert t.calls[1] == "sudo sh -c 'echo 1 > x'"
    assert (tmp_path / "xfer_fill.before.regs").exists()
    assert (tmp_path / "xfer_fill.after.regs").exists()
    log = (tmp_path / "xfer_fill.cmd.log").read_text()
    assert log == "ran\n"
    diff = (tmp_path / "xfer_fill.diff").read_text()
    assert "reg 0x0408" in diff and "state sw_events" in diff
    assert [c.key for c in changes] == ["state sw_events", "reg 0x0408"]
    after = regsnap.read_snapshot(tmp_path / "xfer_fill.after.regs")
    assert after.meta["command"] == "sudo sh -c 'echo 1 > x'"


def test_ssh_runner_command_line():
    argv = regsnap.ssh_argv("user@host", "-o HostKeyAlias=x", "echo hi")
    assert argv == ["ssh", "-o", "HostKeyAlias=x", "user@host", "echo hi"]
