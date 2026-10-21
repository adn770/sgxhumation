#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Decode the TA/3D kick the closed Cedarview GLES2 stack submits (harness "3D capture").

fakedrm.c dumps each SGX_DOKICK's IN buffer (kickNN_in.bin) and a snapshot of every
device object at that moment (kickNN/objects.txt + objNNN_<dva>.bin). This module names
the fields of the structures involved and walks the pointers from the kick to give every
object it reaches a role.

Layouts, all 32-bit:
- PVRSRV_BRIDGE_IN_DOKICK / SGX_CCB_KICK and SGXMKIF_CMDTA_SHARED: the GPL DDK 1.7
  (sgx_bridge.h, sgxinfo.h, sgx_mkif_km.h) for the Cedarview kernel configuration
  (SUPPORT_SGX_NEW_STATUS_VALS on, generalised sync objects off).
- The client structures the kernel never defines (SGXMKIF_CMDTA, the TA and 3D register
  blocks, the HW render context, render details, RT data and PB descriptor): DDK 1.8
  client headers with the SGX545 feature set, every conditional member pinned by the
  structure sizes the microkernel reports (SGX_MISCINFO_STRUCT_SIZES, initinfo.py).
- The VDM control stream words: block type in bits 31:30 (link, state, index list,
  terminate). A state block is two words: the PDS program address >> 4 in bits 27:0
  with last-task (29) and complex (28) flags, then (when not complex; the complex form
  is not decoded yet) the PDS data size in 16-byte units
  (31:26), SD (25), fence (18), MTE emit (17), USE pipe (15:13), partial (12),
  partitions (11:9), secondary (8) and the USE attribute size in 16-byte units (7:0).
  An index list header holds the primitive type (29:26), presence bits for its third
  and fourth words (22, 23) and the index count (21:0); its second word is the index
  base address.

    usage: kick3d.py <kickNN_in.bin> <kickNN/>      # decode one captured kick
"""
from __future__ import annotations

import pathlib
import re
import struct
import sys

# A field is (name, dwords) for scalars, (name, dwords, count) for arrays of
# multi-dword elements, or (name, [fields], count) for nested structures.
DEVSYNC = 4         # PVRSRV_DEVICE_SYNC_OBJECT: ReadOpsPendingVal, ReadOpsCompleteDevVAddr,
                    #   WriteOpsPendingVal, WriteOpsCompleteDevVAddr
CTL_STATUS = 2      # sStatusDevAddr, ui32StatusValue
STATUS_UPDATE = 3   # SGX_INTERNEL_STATUS_UPDATE: CTL_STATUS + hKernelMemInfo

COMMAND = [("ui32ServiceAddress", 1), ("ui32CacheControl", 1), ("ui32Data", 1, 6)]

CCB_KICK = [
    ("sCommand", COMMAND, 1), ("hCCBKernelMemInfo", 1), ("ui32NumDstSyncObjects", 1),
    ("hKernelHWSyncListMemInfo", 1), ("pahDstSyncHandles", 1), ("ui32NumTAStatusVals", 1),
    ("ui32Num3DStatusVals", 1), ("asTAStatusUpdate", STATUS_UPDATE, 32),
    ("as3DStatusUpdate", STATUS_UPDATE, 4), ("bFirstKickOrResume", 1), ("bLastInScene", 1),
    ("ui32CCBOffset", 1), ("ui32NumSrcSyncs", 1), ("ahSrcKernelSyncInfo", 1, 8),
    ("bTADependency", 1), ("hTA3DSyncInfo", 1), ("hTASyncInfo", 1), ("h3DSyncInfo", 1),
    ("hDevMemContext", 1),
]

CMDTA_SHARED = [
    ("ui32CtrlFlags", 1), ("ui32NumTAStatusVals", 1), ("ui32Num3DStatusVals", 1),
    ("ui32TATQSyncWriteOpsPendingVal", 1), ("sTATQSyncWriteOpsCompleteDevVAddr", 1),
    ("ui32TATQSyncReadOpsPendingVal", 1), ("sTATQSyncReadOpsCompleteDevVAddr", 1),
    ("ui323DTQSyncWriteOpsPendingVal", 1), ("s3DTQSyncWriteOpsCompleteDevVAddr", 1),
    ("ui323DTQSyncReadOpsPendingVal", 1), ("s3DTQSyncReadOpsCompleteDevVAddr", 1),
    ("ui32NumSrcSyncs", 1), ("asSrcSyncs", DEVSYNC, 8), ("sTA3DDependency", DEVSYNC, 1),
    ("sCtlTAStatusInfo", CTL_STATUS, 32), ("sCtl3DStatusInfo", CTL_STATUS, 4),
]

TAREGISTERS = [
    ("teaa", 1), ("te_mtile_1", 1), ("te_mtile_2", 1), ("te_screen", 1),
    ("te_mtile_stride", 1), ("te_psg", 1), ("vdm_ctrl_stream_base", 1),
    ("mte_ctrl", 1), ("te_psg_rgn_base", 1), ("te_tpc_base", 1),
    ("mtew_compare", 1), ("mtew_clamp", 1), ("mte_screen", 1),
    ("useld_redirect", 1), ("usest_range", 1), ("bif_ta_req_base", 1),
    ("gsg_stride", 1), ("gsg_base", 1), ("gsg_wrap", 1), ("gsg_state_store", 1),
    ("mte_1st_phase_comp_geom_base", 1), ("mte_1st_phase_comp_geom_size", 1),
    ("vtx_buf_write_pointer_pds_prog", 1), ("sc_word_copy_program", 1),
    ("itp_program", 1), ("tart_max", 1), ("te_tpc", 1), ("mtems_ctrl", 1),
]

PBDESC_UPDATE = [("ta_threshold", 1), ("zls_threshold", 1), ("global_threshold", 1),
                 ("pds_threshold", 1)]

CMDTA = [
    ("ui32Size", 1), ("ui32Flags", 1), ("spec_object", 1, 3),
    ("num_vertex_partitions", 1), ("spm_num_vertex_partitions", 1), ("render_flags", 1),
    ("ui32FrameNum", 1), ("sHWPBDescDevVAddr", 1), ("hw_pb_desc_update", PBDESC_UPDATE, 1),
    ("hw_render_details_dev_addr", 1), ("hw_dst_sync_list_dev_addr", 1), ("sHWRTDataSetDevAddr", 1),
    ("sHWRTDataDevAddr", 1), ("vis_test_results_dev_addr", 1), ("z_buffer_stride", 1),
    ("s_buffer_stride", 1), ("num_3d_regs", 1), ("shared_state", CMDTA_SHARED, 1),
    ("ta_regs", TAREGISTERS, 1), ("reg_updates_3d", 2, 1),     # PVRSRV_HWREG {addr, value}
]

REGISTERS_3D = [
    ("bif_3d_req_base", 1), ("bif_zls_req_base", 1), ("pixel_be", 1),
    ("isp_ipf_misc", 1), ("aa_mode_3d", 1), ("isp_dbias", 1, 3), ("isp_rgn_base", 1),
    ("double_pixel_partitions", 1), ("zls_extz_rgn_base", 1), ("isp_zls_ctl", 1),
    ("isp_zload_base", 1), ("isp_zstore_base", 1), ("isp_stencil_load_base", 1),
    ("isp_stencil_store_base", 1), ("isp_bg_obj_depth", 1), ("isp_bg_obj", 1),
    ("isp_bg_obj_tag", 1), ("isp_multisample_ctl_2", 1), ("isp_multisample_ctl", 1),
    ("isp_tag_ctrl", 1), ("tsp_config", 1), ("edm_pixel_pds_exec", 1),
    ("edm_pixel_pds_data", 1), ("edm_pixel_pds_info", 1), ("isp_fpu_ctrl", 1),
    ("use_filter_0_left", 1), ("use_filter_0_right", 1), ("use_filter_0_extra", 1),
    ("use_filter_1_left", 1), ("use_filter_1_right", 1), ("use_filter_1_extra", 1),
    ("use_filter_2_left", 1), ("use_filter_2_right", 1), ("use_filter_2_extra", 1),
    ("use_filter_table", 1), ("csc_0_coeff_01", 1), ("csc_0_coeff_23", 1),
    ("csc_0_coeff_45", 1), ("csc_0_coeff_67", 1), ("csc_1_coeff_01", 1),
    ("csc_1_coeff_23", 1), ("csc_1_coeff_45", 1), ("csc_1_coeff_67", 1),
    ("csc_0_coeff_8", 1), ("csc_1_coeff_8", 1), ("csc_scale", 1), ("tpu_luma_0", 1),
    ("tpu_luma_1", 1), ("tpu_luma_2", 1), ("isp_depthsort", 1),
]

RENDERDETAILS = [
    ("regs_3d", REGISTERS_3D, 1), ("sAccessDevAddr", 1), ("render_flags", 1),
    ("sHWRTDataSetDevAddr", 1), ("sHWRTDataDevAddr", 1), ("hw_dst_sync_list_dev_addr", 1),
    ("tq_sync_write_ops_pending_val", 1), ("tq_sync_write_ops_complete_dev_vaddr", 1),
    ("tq_sync_read_ops_pending_val", 1), ("tq_sync_read_ops_complete_dev_vaddr", 1),
    ("sTA3DDependency", DEVSYNC, 1), ("num_rts", 1), ("next_rt_idx", 1),
    ("pix_event_update_dev_addr", 1), ("hw_pix_event_update_list_dev_addr", 1),
    ("hw_bg_obj_prim_pds_update_list_dev_addr", 1), ("z_buffer_stride", 1),
    ("s_buffer_stride", 1), ("vis_test_results_dev_addr", 1), ("te_state", 1),
    ("prev_dev_addr", 1), ("next_dev_addr", 1), ("spec_object", 1, 3),
    ("num_pixel_partitions", 1), ("ui32FrameNum", 1), ("ui32Num3DStatusVals", 1),
    ("sCtl3DStatusInfo", CTL_STATUS, 4), ("ui32NumSrcSyncs", 1), ("asSrcSyncs", DEVSYNC, 8),
]

RTDATA = [
    ("hw_render_context_dev_addr", 1), ("hw_render_details_dev_addr", 1),
    ("num_tile_blocks_per_mt", 1), ("mt_region_array_size", 1), ("bg_obj_base_dev_addr", 1),
    ("context_state_dev_addr", 1), ("context_control_dev_addr", 1), ("context_otpm_dev_addr", 1),
    ("tail_ptr_dev_addr", 1), ("tail_size", 1), ("num_rts_in_array", 1),
    ("rgn_stride", 1), ("rt_status_dev_addr", 1), ("common_status", 1),
]

RTDATASET = [
    ("pending_count_dev_addr", 1), ("complete_count", 1), ("mtile_width", 1),
    ("ui32NumOutOfMemSignals", 1), ("ui32NumSPMRenders", 1), ("num_rt_data", 1),
    ("hw_rt_data", RTDATA, 1),
]

HWCONTEXT = [("ui32Flags", 1), ("ui32Priority", 1), ("sPDDevPAddr", 1)]

RENDERCONTEXT = [
    ("common", HWCONTEXT, 1), ("ta_ccb_base_dev_addr", 1), ("ta_ccb_ctl_dev_addr", 1),
    ("ta_count", 1), ("sHWPBDescDevVAddr", 1), ("scratch_prim_block", 1),
    ("hw_partial_renders_head", 1), ("hw_partial_renders_tail", 1),
    ("hw_complete_renders_head", 1), ("hw_complete_renders_tail", 1),
    ("prev_partial_dev_addr", 1), ("next_partial_dev_addr", 1), ("prev_complete_dev_addr", 1),
    ("next_complete_dev_addr", 1), ("ui32PID", 1),
]

PBDESC = [
    ("pb_flags", 1), ("ui32NumPages", 1), ("free_list_initial_ht", 1),
    ("free_list_initial_prev", 1), ("evm_page_table_dev_vaddr", 1), ("alloc_pages", 1),
    ("free_list_ht", 1), ("free_list_prev", 1), ("local_pages", 1), ("global_pages", 1),
    ("ta_threshold", 1), ("zls_threshold", 1), ("global_threshold", 1),
    ("pds_threshold", 1), ("list_pb_block_dev_vaddr", 1),
]

PBBLOCK = [("ui32PageCount", 1), ("ui32HeadTail", 1), ("parent_hw_pb_desc_dev_vaddr", 1),
           ("next_hw_pb_block_dev_vaddr", 1)]

DEVICE_SYNC_LIST = [("sAccessDevAddr", 1), ("ui32NumSyncObjects", 1), ("asSyncData", DEVSYNC, 1)]

STRUCTS = {
    "PVRSRV_BRIDGE_IN_DOKICK": [("ui32BridgeFlags", 1), ("hDevCookie", 1),
                                ("sCCBKick", CCB_KICK, 1)],
    "SGX_CCB_KICK": CCB_KICK,
    "SGXMKIF_CMDTA": CMDTA,
    "SGXMKIF_CMDTA_SHARED": CMDTA_SHARED,
    "hw_ta_registers": TAREGISTERS,
    "hw_3d_registers": REGISTERS_3D,
    "hw_render_details": RENDERDETAILS,
    "hw_rt_data": RTDATA,
    "hw_rt_data_set": RTDATASET,
    "SGXMKIF_HWRENDERCONTEXT": RENDERCONTEXT,
    "hw_pb_desc": PBDESC,
    "hw_pb_block": PBBLOCK,
    "SGXMKIF_HWDEVICE_SYNC_LIST": DEVICE_SYNC_LIST,
}

# Flag words of the client structures (DDK 1.8 client headers), in plain words.
FLAGS = {
    "TAFLAGS": {0x1: "first kick", 0x2: "last kick", 0x4: "resume mid-scene",
                0x8: "resumed after free", 0x10: "invalidate USE cache",
                0x20: "invalidate PDS cache", 0x40: "invalidate data cache",
                0x80: "complex geometry present", 0x400: "dependent TA",
                0x2000: "PB threshold update"},
    "RENDERFLAGS": {0x1: "no render", 0x2: "abort", 0x4: "mid-scene render",
                    0x8: "no dealloc", 0x10: "Z-only render", 0x20: "get vis results",
                    0x40: "accumulate vis results", 0x80: "depth buffer",
                    0x100: "stencil buffer", 0x200: "TA dependency", 0x400: "render scene",
                    0x1000: "bounding-box render", 0x2000: "no macrotile split"},
    "HWCFLAGS": {0x1: "new context", 0x2: "dummy transfer", 0x4: "dummy 2D", 0x10: "suspend",
                 0x20: "per-context PB", 0x40: "per-context sync info"},
    "PBFLAGS": {0x1: "init page table", 0x2: "threshold update"},
}


def flag_names(kind: str, value: int) -> list[str]:
    names, rest = [], value
    for bit, name in FLAGS[kind].items():
        if value & bit:
            names.append(name)
            rest &= ~bit
    if rest:
        names.append(f"{rest:#x}")
    return names


def _layout(fields, prefix=""):
    """Yield (path, dword offset, element dwords, count, is_array) for every leaf."""
    off = 0
    for f in fields:
        name, shape = f[0], f[1]
        count = f[2] if len(f) > 2 else 1
        if isinstance(shape, list):
            for path, o, w, c, arr in _layout(shape):
                yield f"{prefix}{name}.{path}", off + o, w, c, arr
            off += _dwords(shape) * count
        else:
            yield f"{prefix}{name}", off, shape, count, len(f) > 2
            off += shape * count


def _dwords(fields) -> int:
    n = 0
    for f in fields:
        shape, count = f[1], f[2] if len(f) > 2 else 1
        n += (_dwords(shape) if isinstance(shape, list) else shape) * count
    return n


def size(name: str) -> int:
    return 4 * _dwords(STRUCTS[name])


def offset(name: str, field: str) -> int:
    for path, off, _w, _c, _a in _layout(STRUCTS[name]):
        if path == field or path.startswith(field + "."):
            return 4 * off
    raise KeyError(field)


def decode(name: str, raw: bytes) -> dict:
    """Name every field: scalars as ints, arrays as lists (of lists for multi-dword
    elements). Only the declared size is read: the variable-length reg_updates_3d tail
    of a command is decoded as its first entry."""
    need = size(name)
    if len(raw) < need:
        raise ValueError(f"{name}: need {need} bytes, got {len(raw)}")
    w = struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4])
    out = {}
    for path, off, width, count, array in _layout(STRUCTS[name]):
        if not array:
            out[path] = w[off]
        elif width == 1:
            out[path] = list(w[off:off + count])
        else:
            out[path] = [list(w[off + i * width:off + (i + 1) * width]) for i in range(count)]
    return out


# --- VDM control stream ----------------------------------------------------------------

VDM_TYPES = ("LINK", "STATE", "INDEXLIST", "TERMINATE")
VDM_PRIMS = {0: "triangles", 1: "lines", 2: "points", 3: "strip", 4: "fan",
             5: "edge triangles", 6: "line strip", 8: "complex triangles", 9: "complex lines",
             10: "complex points", 11: "complex strip", 12: "complex fan",
             14: "complex line strip"}
STATE_W1_FLAGS = ((29, "last task"), (28, "complex"))
STATE_W2_FLAGS = ((25, "SD"), (24, "debug"), (18, "fence"), (17, "MTE emit"), (12, "partial"),
                  (8, "secondary"))


def decode_vdm(words: list[int], limit: int = 4096) -> list[dict]:
    """Walk a VDM control stream from its first word to the first terminate block."""
    out, i = [], 0
    while i < len(words) and len(out) < limit:
        w = words[i]
        kind = VDM_TYPES[w >> 30]
        b = {"offset": i, "type": kind, "word": w}
        if kind in ("STATE", "TERMINATE") and (w & 0x0FFFFFFF) and i + 1 < len(words):
            w2 = words[i + 1]
            flags = [n for bit, n in STATE_W1_FLAGS if w >> bit & 1]
            b.update(pds_base=(w & 0x0FFFFFFF) << 4, word2=w2, words=2, flags=flags)
            if "complex" in flags:        # complex geometry: word 2 has another layout
                b.update(data_bytes=None, use_attr_bytes=None, use_pipe=None, partitions=None)
            else:
                b.update(data_bytes=(w2 >> 26) * 16, use_attr_bytes=(w2 & 0xFF) * 16,
                         use_pipe=(w2 >> 13) & 7, partitions=(w2 >> 9) & 7)
                flags += [n for bit, n in STATE_W2_FLAGS if w2 >> bit & 1]
        elif kind == "INDEXLIST":
            n = 2 + (w >> 22 & 1) + (w >> 23 & 1)
            b.update(primitive=VDM_PRIMS.get(w >> 26 & 0xF, f"type{w >> 26 & 0xF}"),
                     count=w & 0x3FFFFF, words=n,
                     index_base=words[i + 1] if i + 1 < len(words) else None,
                     extra=words[i + 2:i + n])
        elif kind == "LINK":
            b.update(words=1, target=(w & 0x3FFFFFFF) << 2)
        else:
            b.update(words=1)
        out.append(b)
        if kind == "TERMINATE":
            break
        if kind == "LINK":       # a link continues elsewhere; the walker does not follow it
            break
        i += b["words"]
    return out


# --- walking the object graph -------------------------------------------------------------

class Snapshot:
    """A capture snapshot directory: objects.txt + objNNN_<dva>.bin."""

    LINE = re.compile(r"\s*(\d+) heap=(\d+)\s+(\S+)\s+dva=([0-9a-f]+) size=(0x[0-9a-f]+) "
                      r"mapped=(0x[0-9a-f]+) attribs=([0-9a-f]+) kind=(\d+)")

    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.objs = []
        for line in (self.path / "objects.txt").read_text().splitlines():
            m = self.LINE.match(line)
            if m:
                k, heap, hname, dva, sz, mapped, attribs, kind = m.groups()
                self.objs.append({"index": int(k), "heap": int(heap), "heap_name": hname,
                                  "dva": int(dva, 16), "size": int(sz, 16),
                                  "mapped": int(mapped, 16), "attribs": int(attribs, 16),
                                  "kind": int(kind),
                                  "file": self.path / f"obj{int(k):03d}_{dva}.bin"})
        self._cache = {}

    def find(self, dva: int):
        for o in self.objs:
            if o["dva"] <= dva < o["dva"] + o["mapped"]:
                return o
        return None

    def by_handle(self, handle: int) -> int:
        k = handle - 0x4D410000
        for o in self.objs:
            if o["index"] == k:
                return o["dva"]
        raise KeyError(hex(handle))

    def data(self, o) -> bytes:
        if o["index"] not in self._cache:
            self._cache[o["index"]] = o["file"].read_bytes()
        return self._cache[o["index"]]

    def read(self, dva: int, n: int) -> bytes:
        o = self.find(dva)
        if o is None or dva + n > o["dva"] + o["mapped"]:
            raise KeyError(hex(dva))
        off = dva - o["dva"]
        return self.data(o)[off:off + n]


# Pointer fields of each structure that the walker follows or records: field -> (role,
# structure to decode there or None).
POINTERS = {
    "SGXMKIF_CMDTA": {
        "sHWPBDescDevVAddr": ("parameter buffer descriptor", "hw_pb_desc"),
        "hw_render_details_dev_addr": ("render details", "hw_render_details"),
        "hw_dst_sync_list_dev_addr": ("destination sync list", "SGXMKIF_HWDEVICE_SYNC_LIST"),
        "sHWRTDataSetDevAddr": ("render target data set", "hw_rt_data_set"),
        "sHWRTDataDevAddr": ("render target data (this kick)", "hw_rt_data"),
        "vis_test_results_dev_addr": ("visibility test results", None),
        "ta_regs.vdm_ctrl_stream_base": ("VDM control stream", "VDM"),
        "ta_regs.te_psg_rgn_base": ("TE region headers (PSG)", None),
        "ta_regs.te_tpc_base": ("TE tail pointer cache", None),
        "ta_regs.bif_ta_req_base": ("TA requestor base (parameter heap)", None),
        "ta_regs.gsg_base": ("GSG base", None),
        "ta_regs.mte_1st_phase_comp_geom_base": ("complex geometry buffer", None),
    },
    "SGXMKIF_HWRENDERCONTEXT": {
        "ta_ccb_base_dev_addr": ("TA client CCB", None),
        "ta_ccb_ctl_dev_addr": ("TA client CCB control", None),
        "sHWPBDescDevVAddr": ("parameter buffer descriptor", "hw_pb_desc"),
        "scratch_prim_block": ("scratch primitive block", None),
    },
    "hw_pb_desc": {
        "evm_page_table_dev_vaddr": ("DPM page table (parameter buffer pages)", None),
        "list_pb_block_dev_vaddr": ("parameter buffer block", "hw_pb_block"),
    },
    "hw_render_details": {
        "regs_3d.bif_3d_req_base": ("3D requestor base (parameter heap)", None),
        "regs_3d.isp_rgn_base": ("ISP region headers", None),
        "regs_3d.zls_extz_rgn_base": ("ZLS external-Z region headers", None),
        "regs_3d.edm_pixel_pds_exec": ("end-of-render PDS program (EDM pixel)", "PDS"),
        "sAccessDevAddr": ("render details access word", None),
        "hw_pix_event_update_list_dev_addr": ("pixel event update list", None),
        "hw_bg_obj_prim_pds_update_list_dev_addr": ("background object PDS update list", None),
        "vis_test_results_dev_addr": ("visibility test results", None),
    },
    "hw_rt_data": {
        "hw_render_context_dev_addr": ("HW render context", "SGXMKIF_HWRENDERCONTEXT"),
        "bg_obj_base_dev_addr": ("background object", None),
        "context_state_dev_addr": ("DPM context state", None),
        "context_control_dev_addr": ("DPM context control", None),
        "context_otpm_dev_addr": ("OTPM", None),
        "tail_ptr_dev_addr": ("tail pointers", None),
        "rt_status_dev_addr": ("render target status word", None),
    },
    "hw_rt_data_set": {"pending_count_dev_addr": ("render target pending count", None)},
}


def _pds_data_dwords_3d(d: dict) -> int:
    # EDM pixel PDS data size counts rows of four dwords (the data segment comes first).
    return 4 * d["regs_3d.edm_pixel_pds_data"]


def walk(kick_in: bytes, ccb_dva: int, mem) -> list[dict]:
    """Roles of everything reachable from one kick. mem.read(dva, n) -> bytes."""
    roles: dict[int, dict] = {}

    def add(dva, role, struct_name=None, **extra):
        if not dva:
            return None
        if dva in roles:
            roles[dva].setdefault("aliases", []).append(role)
            return None
        roles[dva] = {"dva": dva, "role": role, "struct": struct_name, **extra}
        return roles[dva]

    def visit(dva, role, struct_name):
        r = add(dva, role, struct_name)
        if r is None or struct_name is None or struct_name in ("VDM", "PDS"):
            return r
        try:
            d = decode(struct_name, mem.read(dva, size(struct_name)))
        except (KeyError, ValueError):
            return r
        r["fields"] = d
        follow(struct_name, d)
        if struct_name == "hw_rt_data_set":
            for i in range(min(d["num_rt_data"], 8)):
                rt = dva + offset(struct_name, "hw_rt_data") + i * size("hw_rt_data")
                visit(rt, f"render target data [{i}]", "hw_rt_data")
        return r

    def follow(struct_name, d):
        for field, (role, target) in POINTERS.get(struct_name, {}).items():
            dva = d.get(field)
            if target == "VDM":
                vdm_stream(dva)
            elif target == "PDS":
                add(dva & ~0xF, role, "PDS", pds_data_dwords=_pds_data_dwords_3d(d))
            else:
                visit(dva, role, target)

    def vdm_stream(dva):
        r = add(dva, "VDM control stream", "VDM")
        if r is None:
            return
        words = []
        try:
            raw = mem.read(dva, 4)
            n = 4
            while n <= 0x1000:              # read as far as the object allows
                try:
                    raw = mem.read(dva, n * 2)
                    n *= 2
                except KeyError:
                    break
            words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
        except KeyError:
            return
        r["blocks"] = blocks = decode_vdm(words)
        for b in blocks:
            if "pds_base" in b:
                n = b["data_bytes"] // 4 if b["data_bytes"] is not None else None
                add(b["pds_base"], f"PDS state program (VDM word {b['offset']})", "PDS",
                    pds_data_dwords=n)
            if b["type"] == "INDEXLIST" and b.get("index_base"):
                add(b["index_base"], f"index buffer (VDM word {b['offset']})", None)

    k = decode("PVRSRV_BRIDGE_IN_DOKICK", kick_in)
    cmd_dva = ccb_dva + k["sCCBKick.ui32CCBOffset"] - offset("SGXMKIF_CMDTA", "shared_state")
    cmd = add(cmd_dva, "TA command (SGXMKIF_CMDTA)", "SGXMKIF_CMDTA")
    try:
        d = decode("SGXMKIF_CMDTA", mem.read(cmd_dva, size("SGXMKIF_CMDTA")))
        cmd["fields"] = d
    except (KeyError, ValueError):
        d = None
    visit(k["sCCBKick.sCommand.ui32Data"][1], "HW render context", "SGXMKIF_HWRENDERCONTEXT")
    if d:
        follow("SGXMKIF_CMDTA", d)
    for kind in ("TA", "3D"):
        n = k[f"sCCBKick.ui32Num{kind}StatusVals"]
        for i, (addr, value, _h) in enumerate(k[f"sCCBKick.as{kind}StatusUpdate"][:n]):
            add(addr, f"{kind} status word [{i}] (EDM writes {value:#x})", None)
    return list(roles.values())


# --- report ------------------------------------------------------------------------------

def report(kick_in: bytes, snap: Snapshot) -> str:
    k = decode("PVRSRV_BRIDGE_IN_DOKICK", kick_in)
    out = ["SGX_CCB_KICK:"]
    for f, v in k.items():
        if f.startswith("sCCBKick.as") and f.endswith("StatusUpdate"):
            n = k[f.replace("as", "ui32Num", 1).replace("Update", "Vals")]
            v = [f"*{a:#010x}={b:#x}" for a, b, _ in v[:n]]
        elif f == "sCCBKick.ahSrcKernelSyncInfo":
            v = v[:k["sCCBKick.ui32NumSrcSyncs"]]
        out.append(f"  {f:44s} {_fmt(v)}")
    ccb = snap.by_handle(k["sCCBKick.hCCBKernelMemInfo"])
    for r in sorted(walk(kick_in, ccb, snap), key=lambda r: r["dva"]):
        o = snap.find(r["dva"])
        where = (f"obj {o['index']} {o['heap_name']}+{r['dva'] - o['dva']:#x}" if o
                 else "not in any object")
        also = f"  (also: {'; '.join(r['aliases'])})" if r.get("aliases") else ""
        out.append(f"\n{r['dva']:#010x} {r['role']} [{where}]{also}")
        for f, v in r.get("fields", {}).items():
            if f.endswith(("Flags", "RenderFlags")) and isinstance(v, int):
                kind = {"ui32Flags": "TAFLAGS", "render_flags": "RENDERFLAGS",
                        "common.ui32Flags": "HWCFLAGS", "pb_flags": "PBFLAGS"}.get(f)
                if kind:
                    v = f"{v:#x} {flag_names(kind, v)}"
            if isinstance(v, list):
                while v and not (v[-1] if isinstance(v[-1], int) else any(v[-1])):
                    v = v[:-1]                  # drop unused trailing entries
            if _interesting(f, v):
                out.append(f"    {f:44s} {_fmt(v)}")
        for b in r.get("blocks", []):
            out.append("    " + _fmt_block(b))
    return "\n".join(out)


def _interesting(field, v) -> bool:
    if isinstance(v, list):
        return any(x if isinstance(x, int) else any(x) for x in v)
    return bool(v) if not isinstance(v, str) else True


def _fmt(v) -> str:
    if isinstance(v, int):
        return f"{v:#x}"
    if isinstance(v, list):
        return "[" + ", ".join(_fmt(x) for x in v) + "]"
    return str(v)


def _fmt_block(b: dict) -> str:
    s = f"[{b['offset']:3d}] {b['type']:9s} {b['word']:#010x}"
    if "pds_base" in b and b["data_bytes"] is None:
        s += f" pds={b['pds_base']:#010x} word2={b['word2']:#010x} {' '.join(b['flags'])}"
    elif "pds_base" in b:
        s += (f" pds={b['pds_base']:#010x} data={b['data_bytes']}B "
              f"attr={b['use_attr_bytes']}B pipe={b['use_pipe']} {' '.join(b['flags'])}")
    if b["type"] == "INDEXLIST":
        s += (f" {b['primitive']} count={b['count']} base={b['index_base']:#010x} "
              f"extra={_fmt(b['extra'])}")
    return s


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    print(report(pathlib.Path(argv[0]).read_bytes(), Snapshot(argv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
