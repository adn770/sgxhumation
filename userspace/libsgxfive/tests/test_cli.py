# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""sgxfive-test against the in-process mock (--mock): every subcommand, its output and exit
status. Run through `make check`, which builds the binary and sets SGXFIVE_TEST."""
from __future__ import annotations

import os
import pathlib
import re
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
TOOL = os.environ.get("SGXFIVE_TEST", str(HERE.parent / "build" / "sgxfive-test"))


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([TOOL, *args], capture_output=True, text=True, timeout=30)


@pytest.fixture(scope="module", autouse=True)
def tool_exists():
    if not pathlib.Path(TOOL).exists():
        pytest.skip(f"{TOOL} not built (run make -C userspace/libsgxfive check)")


def test_usage_without_command():
    r = run()
    assert r.returncode == 2
    assert "usage" in r.stderr


def test_unknown_command():
    r = run("--mock", "frobnicate")
    assert r.returncode == 2


def test_query_rev1():
    r = run("--mock", "query")
    assert r.returncode == 0, r.stderr
    out = r.stdout
    assert re.search(r"^device\s+/dev/dri/renderD128$", out, re.M)
    assert re.search(r"^core_id\s+0x01150000$", out, re.M)
    assert re.search(r"^quirks\s+0x1 SAMPLE_CACHE$", out, re.M)
    assert re.search(r"^max_updates\s+0$", out, re.M)
    assert re.search(r"^heap GENERAL\s+base 0x3e000000 size 0xb1fff000$", out, re.M)
    assert re.search(r"^heap KERNEL_CODE\s+base 0x30000000 size 0x00400000 RESERVED$",
                     out, re.M)
    assert re.search(r"^heap USSE_PIXEL\s+base 0x37000000 size 0x00500000 FIXED_WINDOWS$",
                     out, re.M)


def test_query_harness_layout():
    r = run("--mock=harness", "query")
    assert r.returncode == 0, r.stderr
    assert re.search(r"^heap PDS_PIXEL\s+base 0xe4000000 ", r.stdout, re.M)
    assert re.search(r"^heap TEXTURE\s+absent$", r.stdout, re.M)


def test_no_device_found():
    r = run("--mock=none", "query")
    assert r.returncode == 1
    assert "no sgxfive render node" in r.stderr


def test_dummy_chain():
    r = run("--mock", "dummy", "-n", "16")
    assert r.returncode == 0, r.stderr
    assert "dummy: 16 jobs ok" in r.stdout


@pytest.mark.parametrize("args", [
    [],
    ["--width", "32", "--height", "16", "--x", "8", "--y", "4", "--stride", "320"],
    ["--width", "33", "--height", "16", "--x", "8", "--y", "4", "--stride", "320"],
    ["--width", "1", "--height", "1", "--x", "5", "--y", "7"],
])
@pytest.mark.parametrize("layout", ["rev1", "harness"])
def test_fill(layout, args):
    r = run(f"--mock={layout}", "fill", *args)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "fill: ok" in r.stdout


def test_fill_rejects_odd_stride():
    r = run("--mock", "fill", "--stride", "260")
    assert r.returncode == 1
    assert "Invalid argument" in r.stderr


@pytest.mark.parametrize("args", [
    [],
    ["--width", "32", "--height", "16", "--sx", "5", "--sy", "3", "--src-stride", "320",
     "--dx", "8", "--dy", "4", "--dst-stride", "288"],
    ["--width", "33", "--height", "16"],
])
def test_copy(args):
    r = run("--mock", "copy", *args)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "copy: ok" in r.stdout


def test_isolation():
    r = run("--mock", "isolation")
    assert r.returncode == 0, r.stdout + r.stderr
    checks = re.findall(r"^(PASS|FAIL) (\S+)$", r.stdout, re.M)
    assert [c for c, _ in checks] == ["PASS"] * 9
    assert {n for _, n in checks} == {
        "kernel-heap-unmappable", "same-address-separate-vms", "other-vm-address-faults",
        "lost-context-cancelled", "other-context-survives", "read-only-faults",
        "queued-innocent-job-reruns", "fault-recovered-quickly",
        "faulting-file-banned",
    }


@pytest.mark.parametrize("cmd", ["fill", "copy", "isolation"])
def test_fake_programs_still_run_on_the_mock(cmd):
    r = run("--mock", "--programs=fake", cmd)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "programs fake" in r.stdout


@pytest.mark.parametrize("cmd", ["fill", "copy"])
def test_assembled_programs_by_default(cmd):
    r = run("--mock", cmd)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "programs asm" in r.stdout


def test_fake_programs_refused_on_hardware():
    # Without --mock the tool would use the real backend; the fake program source must
    # never be sent to the GPU, so it is refused before opening anything.
    r = run("--device", "/nonexistent", "--programs=fake", "fill")
    assert r.returncode == 2
    assert "fake program source" in r.stderr


def test_hardware_fill_needs_no_mock():
    # The assembled programs are real: without --mock, fill goes to the device.
    r = run("--device", "/nonexistent", "fill")
    assert r.returncode == 1
    assert "/nonexistent" in r.stderr and "program" not in r.stderr


def test_unknown_program_source():
    assert run("--mock", "--programs=fw", "fill").returncode == 2


def test_q4_probe_words_are_their_source():
    """Each q4 probe word is usse-asm's encoding of the source in its comment."""
    import sys
    sys.path.insert(0, str(HERE.parent.parent.parent / "tools" / "fwcutter"))
    import ours  # builds usse-asm on demand
    src = (HERE.parent / "tools" / "sgxfive-test.c").read_text()
    lines = re.findall(r"0x([0-9A-F]{16})ull,\s*/\* ([^(*]+?)\s*(?:\([^)]*\))? \*/", src)
    assert len(lines) == 25
    asm = "\n".join(text for _, text in lines) + "\n"
    r = subprocess.run([str(ours._tool("usse")), "-o", "/dev/stdout", "-"], input=asm.encode(),
                       capture_output=True, check=True)
    words = [int.from_bytes(r.stdout[i:i + 8], "little") for i in range(0, len(r.stdout), 8)]
    assert words == [int(w, 16) for w, _ in lines]


def test_q4_runs_on_the_mock():
    r = run("--mock", "q4")
    assert r.returncode == 0, r.stdout + r.stderr
    assert re.search(r"^ldr-core-id: ", r.stdout, re.M)
    assert re.search(r"^str-use-g0: ", r.stdout, re.M)


def test_query_render():
    r = run("--mock", "query")
    assert r.returncode == 0, r.stderr
    assert re.search(r"^job_types\s+0x3$", r.stdout, re.M)
    assert re.search(r"^render_cmd\s+0x68 pb 0x422 pages max 256x128$", r.stdout, re.M)
    assert re.search(r"^heap PARAMETERS\s+base 0x10000000 size 0x10000000 RESERVED$",
                     r.stdout, re.M)
    assert re.search(r"^heap TA_DATA\s+base 0x20000000 size 0x0d000000 RESERVED$",
                     r.stdout, re.M)


@pytest.mark.parametrize("size", [None, ("64", "64"), ("256", "128"), ("128", "32")])
def test_tri(size):
    args = ["--mock", "tri", "-n", "2"]
    if size:
        args += ["--width", size[0], "--height", size[1]]
    r = run(*args)
    assert r.returncode == 0, r.stdout + r.stderr
    w, h = size or ("64", "64")
    assert re.search(rf"^tri {w}x{h} #1: \d+ triangle, \d+ background, 0 other; "
                     r"wrong 0 inside, 0 outside, 0 at edges; \d+ if flipped; guard 0$",
                     r.stdout, re.M), r.stdout
    assert "tri: 2 renders ok" in r.stdout


def test_tri_default_is_the_first_triangle():
    r = run("--mock", "tri", "-n", "1")
    assert r.returncode == 0, r.stdout + r.stderr
    m = re.search(r"^tri 64x64 #1: (\d+) triangle, (\d+) background", r.stdout, re.M)
    assert m and int(m.group(1)) + int(m.group(2)) == 4096
    assert 1000 < int(m.group(1)) < 1100       # 1062 on the hardware


def test_tri_rejects_sizes_the_kernel_refuses():
    r = run("--mock", "tri", "--width", "512")
    assert r.returncode == 1
    assert "Invalid argument" in r.stderr


def test_tri_needs_render_heaps():
    r = run("--mock=harness", "tri")
    assert r.returncode == 1
