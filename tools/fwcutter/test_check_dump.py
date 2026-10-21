# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for check_dump.py — compare a device dump (debugfs dri/N/sgx) with fwload.py."""
import json
import struct

import check_dump
import fwformat as F
from test_fwload import TINY_LAYOUT, tiny


def write_dump(tmp_path, images, layout=TINY_LAYOUT):
    (tmp_path / "fw").write_bytes(F.pack(tiny()))
    doc = {"addr": {str(i): hex(a) for i, a in layout.addr.items()},
           "heap_base": {str(h): hex(b) for h, b in layout.heap_base.items()},
           "pd_phys": hex(layout.pd_phys)}
    (tmp_path / "layout.json").write_text(json.dumps(doc))
    for i, img in images.items():
        (tmp_path / f"buf{i:02d}.bin").write_bytes(img)


def test_matching_dump(tmp_path):
    import fwload
    write_dump(tmp_path, fwload.load(tiny(), TINY_LAYOUT).images)
    report = check_dump.check(tmp_path / "fw", tmp_path)
    assert report.ok and [r.status for r in report.rows] == ["match"] * 3


def test_reports_first_differing_word(tmp_path):
    import fwload
    images = dict(fwload.load(tiny(), TINY_LAYOUT).images)
    bad = bytearray(images[9])
    struct.pack_into("<I", bad, 4, 0xDEAD)
    images[9] = bytes(bad)
    write_dump(tmp_path, images)
    report = check_dump.check(tmp_path / "fw", tmp_path)
    assert not report.ok
    row = [r for r in report.rows if r.buffer == 9][0]
    assert row.status == "differ" and row.first_diff == 4


def test_missing_buffer_file(tmp_path):
    import fwload
    images = dict(fwload.load(tiny(), TINY_LAYOUT).images)
    del images[1]
    write_dump(tmp_path, images)
    report = check_dump.check(tmp_path / "fw", tmp_path)
    assert not report.ok and [r for r in report.rows if r.buffer == 1][0].status == "missing"


def test_cli_exit_status(tmp_path):
    import subprocess
    import sys
    import fwload
    write_dump(tmp_path, fwload.load(tiny(), TINY_LAYOUT).images)
    r = subprocess.run([sys.executable, str(check_dump.__file__), str(tmp_path / "fw"),
                        str(tmp_path)], capture_output=True, text=True)
    assert r.returncode == 0 and "all 3 buffers match" in r.stdout
