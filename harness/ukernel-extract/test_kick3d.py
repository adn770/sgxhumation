# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Tests for kick3d.py — decoding the TA/3D kick the closed GLES2 stack submits.

The synthetic tests use word-index buffers (word i holds i) to pin every field position,
and small hand-built streams. The capture tests run only when the gitignored
extracted/gles2_tri/ capture is present.
"""
import pathlib
import struct

import pytest

import kick3d

HERE = pathlib.Path(__file__).resolve().parent
CAPTURE = HERE / "extracted" / "gles2_tri" / "dump3d"


def index_words(n: int) -> bytes:
    return struct.pack(f"<{n}I", *range(n))


# --- structure sizes ---------------------------------------------------------------

def test_sizes_match_what_the_microkernel_was_built_against():
    # SGX_MISCINFO_STRUCT_SIZES from the captured init info (initinfo.py).
    assert kick3d.size("SGXMKIF_CMDTA") == 0x2B0
    assert kick3d.size("SGXMKIF_CMDTA_SHARED") == 0x1E0
    assert kick3d.size("hw_3d_registers") == 0xD0
    assert kick3d.size("hw_pb_desc") == 0x3C
    assert kick3d.size("SGXMKIF_HWRENDERCONTEXT") == 0x44
    assert kick3d.size("hw_render_details") == 0x1EC
    assert kick3d.size("hw_rt_data") == 0x38
    assert kick3d.size("hw_rt_data_set") == 0x50


def test_kick_bridge_in_is_564_bytes():
    # ui32BridgeFlags + hDevCookie + SGX_CCB_KICK, as captured from SGX_DOKICK.
    assert kick3d.size("PVRSRV_BRIDGE_IN_DOKICK") == 564


# --- field positions -----------------------------------------------------------------

def test_ccb_kick_fields():
    k = kick3d.decode("PVRSRV_BRIDGE_IN_DOKICK", index_words(141))
    assert k["sCCBKick.sCommand.ui32ServiceAddress"] == 2
    assert k["sCCBKick.sCommand.ui32Data"] == [4, 5, 6, 7, 8, 9]
    assert k["sCCBKick.hCCBKernelMemInfo"] == 10
    assert k["sCCBKick.ui32NumTAStatusVals"] == 14
    assert k["sCCBKick.asTAStatusUpdate"][0] == [16, 17, 18]
    assert k["sCCBKick.as3DStatusUpdate"][3] == [121, 122, 123]
    assert k["sCCBKick.bFirstKickOrResume"] == 124
    assert k["sCCBKick.ui32CCBOffset"] == 126
    assert k["sCCBKick.ahSrcKernelSyncInfo"] == list(range(128, 136))
    assert k["sCCBKick.h3DSyncInfo"] == 139
    assert k["sCCBKick.hDevMemContext"] == 140


def test_cmdta_fields():
    c = kick3d.decode("SGXMKIF_CMDTA", index_words(172))
    assert c["ui32Size"] == 0 and c["ui32Flags"] == 1
    assert c["spec_object"] == [2, 3, 4]
    assert c["sHWPBDescDevVAddr"] == 9
    assert c["hw_pb_desc_update.pds_threshold"] == 13
    assert c["sHWRTDataDevAddr"] == 17
    assert c["num_3d_regs"] == 21
    assert kick3d.offset("SGXMKIF_CMDTA", "shared_state.ui32CtrlFlags") == 0x58
    assert c["shared_state.sCtlTAStatusInfo"][0] == [22 + 48, 22 + 49]
    assert kick3d.offset("SGXMKIF_CMDTA", "ta_regs.teaa") == 0x238
    assert c["ta_regs.vdm_ctrl_stream_base"] == 148
    assert c["ta_regs.mte_screen"] == 154
    assert c["ta_regs.bif_ta_req_base"] == 157
    assert c["ta_regs.mtems_ctrl"] == 169
    assert c["reg_updates_3d"] == [[170, 171]]


def test_render_details_fields():
    d = kick3d.decode("hw_render_details", index_words(123))
    assert d["regs_3d.bif_3d_req_base"] == 0
    assert d["regs_3d.isp_rgn_base"] == 8
    assert d["regs_3d.isp_bg_obj_depth"] == 16
    assert d["regs_3d.edm_pixel_pds_exec"] == 23
    assert d["regs_3d.isp_depthsort"] == 51
    assert d["sHWRTDataSetDevAddr"] == 54
    assert d["hw_bg_obj_prim_pds_update_list_dev_addr"] == 69
    assert d["num_pixel_partitions"] == 79
    assert d["asSrcSyncs"][7] == [119, 120, 121, 122]


def test_rtdataset_holds_rtdata_array():
    s = kick3d.decode("hw_rt_data_set", index_words(20 + 14))
    assert s["num_rt_data"] == 5
    assert kick3d.offset("hw_rt_data_set", "hw_rt_data.hw_render_context_dev_addr") == 0x18
    r = kick3d.decode("hw_rt_data", index_words(14))
    assert r["bg_obj_base_dev_addr"] == 4 and r["rt_status_dev_addr"] == 12


def test_render_context_and_pb_desc_fields():
    c = kick3d.decode("SGXMKIF_HWRENDERCONTEXT", index_words(17))
    assert c["common.sPDDevPAddr"] == 2 and c["ta_ccb_base_dev_addr"] == 3
    assert c["scratch_prim_block"] == 7 and c["ui32PID"] == 16
    p = kick3d.decode("hw_pb_desc", index_words(15))
    assert p["ui32NumPages"] == 1 and p["evm_page_table_dev_vaddr"] == 4
    assert p["list_pb_block_dev_vaddr"] == 14


def test_decode_rejects_short_buffers():
    with pytest.raises(ValueError):
        kick3d.decode("hw_pb_desc", index_words(14))


def test_flag_names():
    assert kick3d.flag_names("TAFLAGS", 0x71) == [
        "first kick", "invalidate USE cache", "invalidate PDS cache", "invalidate data cache"]
    assert kick3d.flag_names("HWCFLAGS", 0x21) == ["new context", "per-context PB"]
    assert kick3d.flag_names("RENDERFLAGS", 0x400) == ["render scene"]
    assert kick3d.flag_names("TAFLAGS", 0x80000000) == ["0x80000000"]


# --- VDM control stream ------------------------------------------------------------

def test_vdm_stream_blocks():
    words = [
        0x40000000 | (0xEC011000 >> 4), (3 << 26) | (1 << 25) | (1 << 17) | 3,   # state
        0x80C00003, 0x080C3000, 0xF0000000, 0x003FFFFF,                          # index list
        0xF0000000 | (0xEC01E000 >> 4), (1 << 24) | (3 << 22) | (1 << 7) | 2,    # its vertex PDS
        0x60000000 | (0xEC001000 >> 4), 1 << 26,                                 # last state
        0xC0000000,                                                              # terminate
        0x12345678,                                                              # not read
    ]
    blocks = kick3d.decode_vdm(words)
    assert [b["type"] for b in blocks] == ["STATE", "INDEXLIST", "STATE", "TERMINATE"]
    s = blocks[0]
    assert s["pds_base"] == 0xEC011000 and s["data_bytes"] == 48
    assert s["flags"] == ["SD", "MTE emit"] and s["use_attr_bytes"] == 48
    i = blocks[1]
    assert i["primitive"] == "triangles" and i["count"] == 3 and i["words"] == 6
    assert i["index_base"] == 0x080C3000 and i["extra"] == [0xF0000000, 0x003FFFFF]
    assert blocks[2]["flags"][0] == "last task" and blocks[2]["pds_base"] == 0xEC001000
    assert blocks[3]["offset"] == 10


def test_vdm_index_list_carries_the_vertex_pds_program():
    # The two words after an index list (and its optional words) start the vertex task:
    # partition size (31:28) and PDS address >> 4, then the output vertex size in
    # 4-dword units (31:24), bits 23:22, the USE attribute size in 16-byte rows (14:7)
    # and the PDS data size in 16-byte rows (6:0).
    i = kick3d.decode_vdm([0x80C00003, 0x080F5000, 0xF0000000, 0x003FFFFF,
                           0xFEC01E00, 0x02C00103, 0xC0000000])[0]
    v = i["vertex"]
    assert v["pds_base"] == 0xEC01E000 and v["partitions"] == 0xF
    assert v["vertex_dwords"] == 8 and v["advance"] == 3
    assert v["use_attr_bytes"] == 32 and v["data_bytes"] == 48
    assert v["word2"] == 0x02C00103


def test_vdm_index_list_word_count_follows_present_bits():
    blocks = kick3d.decode_vdm([0x80000006, 0x1000, 0x70000000, 0x01000082, 0xC0000000])
    assert blocks[0]["words"] == 4 and blocks[0]["count"] == 6
    assert blocks[0]["vertex"]["partitions"] == 7
    assert blocks[1]["type"] == "TERMINATE"


def test_vdm_stream_follows_no_link_and_stops_at_end():
    assert kick3d.decode_vdm([0x40000000 | 0x0EC0110, 0x0])[-1]["type"] == "STATE"
    assert kick3d.decode_vdm([]) == []


# --- object graph ------------------------------------------------------------------

class Mem:
    """A device memory image: {dva: bytes} objects, as the capture snapshots them."""

    def __init__(self, objs):
        self.objs = objs

    def read(self, dva, n):
        for base, data in self.objs.items():
            if base <= dva and dva + n <= base + len(data):
                return data[dva - base:dva - base + n]
        raise KeyError(hex(dva))


def put(buf: bytearray, struct_name: str, field: str, value: int):
    off = kick3d.offset(struct_name, field)
    buf[off:off + 4] = struct.pack("<I", value)


def test_walk_assigns_roles_from_pointer_fields():
    ccb, ctx, pbdesc, details = 0xF0003000, 0xF0016000, 0xF0001000, 0xF0033000
    rtset, vdm = 0xF0038000, 0x0808C000
    cmd = bytearray(0x400)
    put(cmd, "SGXMKIF_CMDTA", "sHWPBDescDevVAddr", pbdesc)
    put(cmd, "SGXMKIF_CMDTA", "hw_render_details_dev_addr", details)
    put(cmd, "SGXMKIF_CMDTA", "sHWRTDataSetDevAddr", rtset)
    put(cmd, "SGXMKIF_CMDTA", "sHWRTDataDevAddr", rtset + 0x18)
    put(cmd, "SGXMKIF_CMDTA", "ta_regs.vdm_ctrl_stream_base", vdm)
    put(cmd, "SGXMKIF_CMDTA", "ta_regs.bif_ta_req_base", 0xC0000000)
    kick = bytearray(564)
    data1 = kick3d.offset("PVRSRV_BRIDGE_IN_DOKICK", "sCCBKick.sCommand.ui32Data") + 4
    kick[data1:data1 + 4] = struct.pack("<I", ctx)        # ui32Data[1]: HW render context
    put(kick, "PVRSRV_BRIDGE_IN_DOKICK", "sCCBKick.ui32CCBOffset", 0x58)
    details_buf = bytearray(0x1EC)
    put(details_buf, "hw_render_details", "regs_3d.edm_pixel_pds_exec", 0xE4004000)
    put(details_buf, "hw_render_details", "regs_3d.edm_pixel_pds_data", 5)
    rt = bytearray(0x88)
    put(rt, "hw_rt_data_set", "num_rt_data", 2)
    mem = Mem({ccb: bytes(cmd), ctx: bytes(0x44), pbdesc: bytes(0x3C), details: bytes(details_buf),
               rtset: bytes(rt), vdm: struct.pack("<I", 0xC0000000),
               0xE4004000: bytes(0x100)})
    roles = kick3d.walk(bytes(kick), ccb, mem)
    by_dva = {r["dva"]: r for r in roles}
    assert by_dva[ccb]["role"] == "TA command (SGXMKIF_CMDTA)"
    assert by_dva[ctx]["struct"] == "SGXMKIF_HWRENDERCONTEXT"
    assert by_dva[pbdesc]["struct"] == "hw_pb_desc"
    assert by_dva[details]["struct"] == "hw_render_details"
    assert by_dva[rtset + 0x18]["struct"] == "hw_rt_data"
    assert by_dva[rtset + 0x18 + 0x38]["role"].startswith("render target data [1]")
    assert by_dva[vdm]["role"] == "VDM control stream"
    assert by_dva[0xE4004000]["role"] == "end-of-render PDS program (EDM pixel)"
    assert by_dva[0xE4004000]["pds_data_dwords"] == 20
    assert by_dva[0xC0000000]["role"] == "TA requestor base (parameter heap)"


def test_walk_reads_pds_programs_from_the_vdm_stream():
    ccb, vdm, pds = 0xF0003000, 0x0808C000, 0xEC011000
    cmd = bytearray(0x2C0)
    put(cmd, "SGXMKIF_CMDTA", "ta_regs.vdm_ctrl_stream_base", vdm)
    kick = bytearray(564)
    put(kick, "PVRSRV_BRIDGE_IN_DOKICK", "sCCBKick.ui32CCBOffset", 0x58)
    stream = struct.pack("<3I", 0x40000000 | (pds >> 4), 3 << 26, 0xC0000000)
    mem = Mem({ccb: bytes(cmd), vdm: stream, pds: bytes(0x80)})
    by_dva = {r["dva"]: r for r in kick3d.walk(bytes(kick), ccb, mem)}
    assert by_dva[pds]["role"] == "PDS state program (VDM word 0)"
    assert by_dva[pds]["pds_data_dwords"] == 12


def test_walk_reads_the_vertex_program_of_an_index_list():
    ccb, vdm, vtx = 0xF0003000, 0x0808C000, 0xEC01E000
    cmd = bytearray(0x2C0)
    put(cmd, "SGXMKIF_CMDTA", "ta_regs.vdm_ctrl_stream_base", vdm)
    kick = bytearray(564)
    put(kick, "PVRSRV_BRIDGE_IN_DOKICK", "sCCBKick.ui32CCBOffset", 0x58)
    stream = struct.pack("<5I", 0x80000003, 0x080F5000, 0xF0000000 | (vtx >> 4), 0x01C00082,
                         0xC0000000)
    mem = Mem({ccb: bytes(cmd), vdm: stream, vtx: bytes(0x80)})
    by_dva = {r["dva"]: r for r in kick3d.walk(bytes(kick), ccb, mem)}
    assert by_dva[vtx]["role"] == "PDS vertex program (VDM word 0)"
    assert by_dva[vtx]["pds_data_dwords"] == 8


def test_snapshot_reads_objects_and_zero_fills_dropped_files(tmp_path):
    (tmp_path / "objects.txt").write_text(
        "  0 heap=0  GENERAL                  dva=08001000 size=0x000008 mapped=0x001000 "
        "attribs=00000001 kind=0\n"
        "  2 heap=11 PERCONTEXT_3DPARAMETERS  dva=c0001000 size=0x002000 mapped=0x002000 "
        "attribs=00000001 kind=0\n")
    (tmp_path / "obj000_08001000.bin").write_bytes(struct.pack("<2I", 7, 9) + bytes(0xFF8))
    snap = kick3d.Snapshot(tmp_path)
    assert snap.read(0x08001004, 4) == struct.pack("<I", 9)
    # capture copies may leave out the all-zero parameter buffer pages: they read as zeros
    assert snap.read(0xC0001ff0, 0x20) == bytes(0x20)


# --- the capture (gitignored; skipped without it) ------------------------------------

@pytest.mark.skipif(not CAPTURE.exists(), reason="gles2_tri capture not present")
def test_capture_kicks_are_consistent():
    for n in (0, 1):
        k = kick3d.decode("PVRSRV_BRIDGE_IN_DOKICK", (CAPTURE / f"kick0{n}_in.bin").read_bytes())
        assert k["sCCBKick.ui32NumTAStatusVals"] <= 32
        assert k["sCCBKick.bLastInScene"] == n
        snap = kick3d.Snapshot(CAPTURE / f"kick0{n}")
        ccb = snap.by_handle(k["sCCBKick.hCCBKernelMemInfo"])
        cmd_off = k["sCCBKick.ui32CCBOffset"] - kick3d.offset("SGXMKIF_CMDTA", "shared_state")
        cmd = kick3d.decode("SGXMKIF_CMDTA", snap.read(ccb + cmd_off, kick3d.size("SGXMKIF_CMDTA")))
        assert cmd["ui32Size"] >= kick3d.size("SGXMKIF_CMDTA")
        assert cmd["shared_state.ui32CtrlFlags"] & 1                   # READY, set by the kernel
        rt = kick3d.decode("hw_rt_data", snap.read(cmd["sHWRTDataDevAddr"], 0x38))
        assert rt["hw_render_context_dev_addr"] == k["sCCBKick.sCommand.ui32Data"][1]


def test_vdm_complex_state_has_no_plain_data_size():
    # With the complex flag (bit 28) the second word has another layout, not decoded yet.
    b = kick3d.decode_vdm([0x70000000 | (0xEC003040 >> 4), 0x02C00082, 0xC0000000])[0]
    assert b["pds_base"] == 0xEC003040 and "complex" in b["flags"]
    assert b["data_bytes"] is None and b["use_attr_bytes"] is None
