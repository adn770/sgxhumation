# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for bridgecmd.h: the PVRSRV bridge command names fakedrm.c logs.

The closed Cedarview userspace is built with PDUMP and SUPPORT_OVERLAY_ROTATE_BLIT off,
so the absolute command indices follow pvr_bridge.h / sgx_bridge.h (DDK 1.7) with those
blocks removed: the SGX commands start at 83. The header is plain C, checked here
through a small host program like ukdir.h.
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
#include "bridgecmd.h"
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) puts(bridge_cmd_name((unsigned)strtoul(argv[i], 0, 0)));
    return 0;
}
"""


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("bridgecmd")
    (d / "probe.c").write_text(PROBE)
    exe = d / "probe"
    subprocess.run([CC, "-std=c11", "-Wall", "-Werror", "-I", str(HERE),
                    str(d / "probe.c"), "-o", str(exe)], check=True)
    return exe


def names(probe, *nrs):
    out = subprocess.run([str(probe), *map(str, nrs)], capture_output=True, text=True,
                         check=True).stdout
    return out.split()


def test_core_commands(probe):
    assert names(probe, 0, 3, 6, 7, 11, 12, 20, 26, 27) == [
        "ENUM_DEVICES", "CREATE_DEVMEMCONTEXT", "ALLOC_DEVICEMEM", "FREE_DEVICEMEM",
        "MHANDLE_TO_MMAP_DATA", "CONNECT_SERVICES", "MAP_DEV_MEMORY", "EXPORT_DEVICEMEM",
        "RELEASE_MMAP_DATA"]


def test_blocks_after_core_without_pdump_or_overlay(probe):
    assert names(probe, 31, 34, 37, 38, 39, 40, 41, 42, 56, 57, 61, 63) == [
        "PROCESS_SIMISR_EVENT", "MAPPHYSTOUSERSPACE", "GET_FB_STATS", "GET_MISC_INFO",
        "RELEASE_MISC_INFO", "GET_OEMJTABLE", "ENUM_CLASS", "OPEN_DISPCLASS_DEVICE",
        "SWAP_DISPCLASS_TO_SYSTEM", "OPEN_BUFFERCLASS_DEVICE", "WRAP_EXT_MEMORY",
        "ALLOC_SHARED_SYS_MEM"]


def test_initsrv_event_and_sync_blocks(probe):
    assert names(probe, 67, 68, 69, 70, 71, 72, 74, 76, 80, 81) == [
        "INITSRV_CONNECT", "INITSRV_DISCONNECT", "EVENT_OBJECT_WAIT", "EVENT_OBJECT_OPEN",
        "EVENT_OBJECT_CLOSE", "CREATE_SYNC_INFO_MOD_OBJ", "MODIFY_PENDING_SYNC_OPS",
        "SYNC_OPS_TAKE_TOKEN", "ALLOC_SYNC_INFO", "FREE_SYNC_INFO"]


def test_sgx_commands_start_at_83(probe):
    assert names(probe, 83, 84, 85, 86, 96, 97, 98, 99) == [
        "SGX_GETCLIENTINFO", "SGX_RELEASECLIENTINFO", "SGX_GETINTERNALDEVINFO", "SGX_DOKICK",
        "SGX_SUBMITTRANSFER", "SGX_GETMISCINFO", "SGXINFO_FOR_SRVINIT", "SGX_DEVINITPART2"]
    assert names(probe, 100, 101, 102, 103, 104, 105, 109, 110, 111, 112, 113, 114) == [
        "SGX_FINDSHAREDPBDESC", "SGX_UNREFSHAREDPBDESC", "SGX_ADDSHAREDPBDESC",
        "SGX_REGISTER_HW_RENDER_CONTEXT", "SGX_FLUSH_HW_RENDER_TARGET",
        "SGX_UNREGISTER_HW_RENDER_CONTEXT", "SGX_REGISTER_HW_TRANSFER_CONTEXT",
        "SGX_UNREGISTER_HW_TRANSFER_CONTEXT", "SGX_SCHEDULE_PROCESS_QUEUES",
        "SGX_READ_HWPERF_CB", "SGX_SET_RENDER_CONTEXT_PRIORITY",
        "SGX_SET_TRANSFER_CONTEXT_PRIORITY"]


def test_unknown_and_unused_indices(probe):
    # 2D hardware (SGX_CMD_BASE + 23..25) is absent on SGX545; past the last command.
    assert names(probe, 106, 115, 255, 4096) == ["?", "?", "?", "?"]
