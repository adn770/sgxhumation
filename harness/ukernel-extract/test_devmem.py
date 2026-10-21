# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for devmem.h: the fake device-memory model behind fakedrm.c.

fakedrm.c hands every allocation a device virtual address inside its heap (a per-heap
bump allocator, page-granular, honouring the requested alignment) and later translates
device addresses written by the driver back into its own buffers (to apply status
updates at a kick and to say which allocation a command field points into).
"""
import pathlib
import shutil
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
CC = shutil.which("cc") or shutil.which("gcc")

PROBE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devmem.h"
static struct devmem m;
int main(int argc, char **argv) {
    devmem_init(&m, 0x1000);
    devmem_set_heap(&m, 0, 0x08000000u, 0x10000000u);
    devmem_set_heap(&m, 8, 0xE0000000u, 0x01000000u);
    for (int i = 1; i < argc; i++) {
        char op = argv[i][0];
        if (op == 'a') {                        /* a<heap>,<size>,<align> */
            unsigned h, sz, al;
            sscanf(argv[i] + 1, "%u,%x,%x", &h, &sz, &al);
            int k = devmem_alloc(&m, h, sz, al, 0);
            printf("%d %08x\n", k, k >= 0 ? m.obj[k].dva : 0);
        } else if (op == 'f') {                 /* f<dva> */
            unsigned dva = (unsigned)strtoul(argv[i] + 1, 0, 16), off = 0;
            int k = devmem_find(&m, dva, &off);
            printf("%d %x\n", k, off);
        } else if (op == 'h') {                 /* h<handle> */
            unsigned hd = (unsigned)strtoul(argv[i] + 1, 0, 16);
            printf("%d\n", devmem_by_handle(&m, hd));
        }
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("devmem")
    (d / "probe.c").write_text(PROBE)
    exe = d / "probe"
    subprocess.run([CC, "-std=c11", "-Wall", "-Werror", "-I", str(HERE),
                    str(d / "probe.c"), "-o", str(exe)], check=True)
    return exe


def run(probe, *ops):
    return subprocess.run([str(probe), *ops], capture_output=True, text=True,
                          check=True).stdout.splitlines()


def test_allocations_are_page_granular_from_the_heap_start(probe):
    assert run(probe, "a0,10,0", "a0,1001,0", "a0,20,0") == [
        "0 08001000", "1 08002000", "2 08004000"]


def test_alignment_is_honoured(probe):
    assert run(probe, "a0,10,0", "a0,10,10000") == ["0 08001000", "1 08010000"]


def test_heaps_are_independent(probe):
    assert run(probe, "a0,10,0", "a8,18,0", "a8,18,0") == [
        "0 08001000", "1 e0001000", "2 e0002000"]


def test_unknown_heap_or_full_heap_fails(probe):
    assert run(probe, "a3,10,0") == ["-1 00000000"]
    assert run(probe, "a8,1000000,0") == ["-1 00000000"]


def test_find_returns_object_and_offset(probe):
    assert run(probe, "a0,1800,0", "a8,18,0", "f08001010", "f08002fff", "fe0001014",
               "f08003000", "f00000000") == [
        "0 08001000", "1 e0001000", "0 10", "0 1fff", "1 14", "-1 0", "-1 0"]


def test_handles_identify_objects(probe):
    # Each object gets a distinct mapping handle, 0x4D410000 + index ('MA').
    assert run(probe, "a0,10,0", "a0,10,0", "h4d410001", "h4d410007") == [
        "0 08001000", "1 08002000", "1", "-1"]
