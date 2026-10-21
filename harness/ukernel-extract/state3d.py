#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The fixed-function state of a captured 3D frame, as named words (harness "3D capture").

extract() walks one captured kick (kick3d.Snapshot) and returns every state-carrying word
under a stable path, so that two captures that differ in one piece of GL state can be
compared word by word (diff3d.py):

- cmd.*        the TA command (SGXMKIF_CMDTA) with the TA registers
- upd.*        the command's 3D register updates, by the render details field they write
- rd.*         the render details with the 3D registers; rtds.*, rt.* the RT data
- bg.w<i>      the background object (the RT data's bg_obj_base_dev_addr)
- spec.*       the pixel programs of the special (background) object words
- eor.*        the end-of-render PDS program (EDMPixelPDSExec) and its USSE tasks;
  eot.*        the end-of-tile task's PBE words (pbe0..3, sideband)
- vdm.b<i>.*   VDM block i: its words (w<j>), the PDS program of a state block or of
               the vertex task of an index list (d<j> data, c<j> code), what the
               program's doutd copies (dma<n>.w<j>), the USSE programs its doutu starts
               (usse<n>.i<j>, 64-bit instructions), and for an MTE state copy the decoded
               state block (mte.<word>) with its pixel programs (mte.sec.*, mte.pri.*);
               an index list adds its indices (idx<j>) and vertex streams
               (vtx.stream<n>.w<j>)

The state block words get plain names (PRESENCE); DECODERS splits the ones the
differential captures pin into fields, each with the variation that shows it.

Encodings used here, all from the captures unless marked:

- PDS movs (port and operands as tools/usse-dis/pds.c decodes them): the data segment is
  rows of ds0[2r], ds0[2r+1], ds1[2r], ds1[2r+1].
- doutd word 1: burst size - 1 (3:0), lines - 1 (7:4), the destination attribute offset
  (19:8), stride - 1 (29:22) [CAP: the state copies' lengths equal the USSE copy loops;
  the vertex fetch puts the second attribute at offset 4].
- A pixel PDS pointer (MTE state, special object words): data size in 16-byte rows
  (31:26), program offset from PDS_EXEC_BASE >> 4 (23:0).
- An MTE state block starts with a presence word; each set bit adds its words in bit
  order (PRESENCE).
- The end-of-tile USSE task loads the PBE words with limm moves into r0..r3 and emits
  them with emitpix1/emitpix2, whose sideband is spread over the instruction
  (sgx545_fw_format.md, EMITPIX_SB).

    usage: state3d.py <capture dir>        # print every state word of both kicks
"""
from __future__ import annotations

import pathlib
import struct
import sys

import kick3d
import summary3d

PDS_EXEC_BASE = summary3d.PDS_EXEC_BASE
MOVS_DEST = {2: "douti", 3: "doutd", 4: "doutt", 5: "doutu", 6: "douta"}

# MTE state block presence bits: (bit, name, words), emitted in bit order [CAP]: the
# clear's 0xd4c3 block is 12 words and the triangle's 0xdf43 block 18 (the USSE copy
# loops), stencil adds bit 2 and one word (19), a scissor adds bit 7 and two, and the
# end-of-scene block 0x2000 is two. Bits 3-5 (the back-face ISP words of two-sided
# stencil [infer]) and 16+ never appear, so a block using them is left undecoded.
PRESENCE = [
    (0, "isp0", 1), (1, "isp1", 1), (2, "isp2", 1), (6, "pds", 3), (7, "clip", 2),
    (8, "vp", 6), (9, "wrap", 1), (10, "outsel", 1), (11, "wclamp", 1), (12, "mtectl", 1),
    (13, "term", 1), (14, "coordsize", 1), (15, "coordfmt", 1),
]
PDS_WORDS = ("pds_sec", "pds_size", "pds_pri")


def bits(v: int, hi: int, lo: int) -> int:
    return (v >> lo) & ((1 << (hi - lo + 1)) - 1)


# --- PDS -------------------------------------------------------------------------------

def pds_movs(w: int):
    """(port, [(bank, index)] x 4) of a movs/movsa, or None for other instructions."""
    if bits(w, 31, 27) not in (0x00, 0x05):
        return None
    dest = MOVS_DEST.get(bits(w, 3, 0))
    if dest is None or bits(w, 23, 23):
        return None
    b0, b1 = bits(w, 22, 18), bits(w, 17, 13) * 2
    ops = []
    for s in range(3, -1, -1):
        sel = bits(w, 6 + 2 * s, 5 + 2 * s)
        ops.append(("ds1", b1 + (sel & 1)) if sel & 2 else ("ds0", b0 * 2 + (sel & 1)))
    return dest, ops


def data_index(bank: str, i: int) -> int:
    return 4 * (i // 2) + (i % 2) + (2 if bank == "ds1" else 0)


def pds_outputs(words: list[int], ndata: int) -> list[tuple[str, list]]:
    """The dout* writes of a PDS program, operands resolved to data words (None for a
    register outside the data segment, e.g. a temporary the code computed)."""
    out = []
    for w in summary3d.pds_code(words, ndata):
        m = pds_movs(w)
        if m:
            dest, ops = m
            vals = []
            for bank, i in ops:
                j = data_index(bank, i)
                vals.append(words[j] if j < ndata else None)
            out.append((dest, vals))
    return out


def doutd(ctrl: int) -> dict:
    lines = bits(ctrl, 7, 4) + 1
    return {"dwords": (bits(ctrl, 3, 0) + 1) * lines, "lines": lines,
            "offset": bits(ctrl, 19, 8), "stride": bits(ctrl, 29, 22)}


def douta_block(writes: list[tuple[int, int]]) -> list[int]:
    """The attribute block a run of douta writes leaves: word 0 is the value, word 1
    holds the attribute offset in 19:8 [CAP: the end-of-scene program writes offsets 0
    and 1, and its USSE task copies two attributes]."""
    attrs: dict[int, int] = {}
    for value, w1 in writes:
        attrs[bits(w1, 19, 8)] = value
    return [attrs.get(i, 0) for i in range(max(attrs) + 1)] if attrs else []


def pixel_pds_ptr(w: int) -> tuple[int, int]:
    return PDS_EXEC_BASE + (bits(w, 23, 0) << 4), 16 * bits(w, 31, 26)


# --- USSE ------------------------------------------------------------------------------

def usse_limm(insn: int):
    """(register, value) of a `mov rN, #imm32` (limm), else None."""
    hi = insn >> 32
    if hi >> 24 != 0xFC or not insn >> 53 & 1 or insn >> 52 & 1 or bits(hi, 1, 0):
        return None
    v = bits(insn, 20, 0) | bits(insn, 40, 36) << 21 | bits(insn, 49, 44) << 26
    return bits(insn, 27, 21), v


def is_emitpix2(insn: int) -> bool:
    return insn >> 48 & 0xFFFC == 0xFB24


def emitpix2_sideband(insn: int) -> int:
    return bits(insn, 27, 22) | bits(insn, 40, 35) << 6 | bits(insn, 55, 54) << 12


def usse_insns(raw: bytes) -> list[int]:
    code = summary3d.usse_code(raw)
    return list(struct.unpack(f"<{len(code) // 8}Q", code))


# --- MTE state -------------------------------------------------------------------------

def decode_mte_state(words: list[int]) -> list[tuple[str, int]]:
    """Name the words of an MTE state block by its presence word."""
    if not words:
        return []
    pres = words[0]
    out, i = [("present", pres)], 1
    known = {b for b, _, _ in PRESENCE}
    for b in range(32):
        if not pres >> b & 1:
            continue
        if b not in known:
            out += [(f"rest{k}", v) for k, v in enumerate(words[i:])]
            return out
        _, name, n = next(p for p in PRESENCE if p[0] == b)
        for k in range(n):
            if i >= len(words):
                return out
            label = PDS_WORDS[k] if name == "pds" else (name if n == 1 else f"{name}{k}")
            out.append((label, words[i]))
            i += 1
    return out


# --- word decoders ---------------------------------------------------------------------

# Each decoder returns {field: value} for the bits the differential captures pin (the
# variation that shows each field is in the comment); the bits no variation moved are
# returned raw as "rest" so a change in them still shows in a diff.

# Compare functions in GL order (never .. always), as depth=* and stencil=* show.
COMPARE = ("never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always")
# Stencil operations, as stencilop=* shows.
STENCIL_OPS = ("keep", "zero", "replace", "incr", "decr", "invert", "incrwrap", "decrwrap")
OBJTYPES = {0: "triangle", 1: "line", 3: "point"}


def _rest(v: int, mask: int) -> dict:
    return {"rest": v & ~mask & 0xFFFFFFFF} if v & ~mask & 0xFFFFFFFF else {}


def decode_isp0(v: int) -> dict:
    """ISP state word 0 of an MTE state block."""
    return {"sref": bits(v, 7, 0),                    # stencil=always,85
            "isp2_present": bits(v, 8, 8),            # stencil=*: with presence bit 2
            "isp1_present": bits(v, 9, 9),            # [infer] set whenever isp1 is
            "objtype": OBJTYPES.get(bits(v, 17, 15), bits(v, 17, 15)),   # prim=lines/points
            "dwrite_off": bits(v, 20, 20),            # depthmask=0; set without depth test
            "colour_off": bits(v, 21, 21),            # mask=0000
            "dcmp": COMPARE[bits(v, 24, 22)],         # depth=*
            "pass": bits(v, 27, 25),                  # blend=*: 1; scissor mask draws: 3
            **_rest(v, 0x0FF383FF)}


def decode_isp1(v: int) -> dict:
    """ISP state word 1: the colour write mask, R G B A in bits 7..4 (mask=*)."""
    mask = "".join(c for c, b in zip("RGBA", (7, 6, 5, 4)) if v >> b & 1)
    return {"cmask": mask, "b12": bits(v, 12, 12), **_rest(v, 0x10F0)}


def decode_isp2(v: int) -> dict:
    """ISP state word 2: stencil (stencil=*, stencilmask=*, stencilop=*)."""
    return {"wmask": bits(v, 7, 0), "cmpmask": bits(v, 15, 8),
            "zpass": STENCIL_OPS[bits(v, 18, 16)], "zfail": STENCIL_OPS[bits(v, 21, 19)],
            "sfail": STENCIL_OPS[bits(v, 24, 22)], "scmp": COMPARE[bits(v, 27, 25)],
            **_rest(v, 0x0FFFFFFF)}


def decode_pds_size(v: int) -> dict:
    """The size word between the two pixel PDS pointers: primary attribute rows (6:0),
    a constant 3 in 17:16, secondary attribute rows (24:18: 3 for the triangle's 11
    constant dwords, 0 with a no-op secondary); 31:25 vary with the shader."""
    return {"pri_attr_rows": bits(v, 6, 0), "task": bits(v, 17, 16),
            "sec_attr_rows": bits(v, 24, 18), "hi": bits(v, 31, 25), **_rest(v, 0xFFFF007F)}


def decode_clip0(v: int) -> dict:
    """Region clip word 0: on (31), first and last 32-pixel tile column (scissor=*,
    viewport=*, size=*)."""
    return {"on": bits(v, 31, 31), "xmin": bits(v, 30, 16), "xmax": bits(v, 15, 0)}


def decode_clip1(v: int) -> dict:
    """Region clip word 1: first and last 16-pixel tile row."""
    return {"ymin": bits(v, 31, 16), "ymax": bits(v, 15, 0)}


def decode_term(v: int) -> dict:
    """The end-of-scene state word: last 32 x 32 block column (31:16) and row (15:0)
    (size=*)."""
    return {"xmax32": bits(v, 31, 16), "ymax32": bits(v, 15, 0)}


def decode_mtectl(v: int) -> dict:
    """MTE control state word: culling in 1:0 (cull=*, frontface=*): 1 removes
    counter-clockwise triangles, 2 clockwise ones, in the y-up window coordinates the
    viewport words produce."""
    return {"cull": ("none", "ccw", "cw", 3)[bits(v, 1, 0)], **_rest(v, 0x3)}


def decode_te_screen(v: int) -> dict:
    """TEScreen: 32 x 16 tiles across - 1 (low bits) and down - 1 (from bit 12)."""
    return {"xtiles": bits(v, 11, 0) + 1, "ytiles": bits(v, 23, 12) + 1, **_rest(v, 0xFFFFFF)}


def decode_mte_screen(v: int) -> dict:
    """MTEScreen: width - 1 (15:0), height - 1 (31:16)."""
    return {"width": bits(v, 15, 0) + 1, "height": bits(v, 31, 16) + 1}


def decode_pbe0(v: int) -> dict:
    return {"xmin": bits(v, 12, 0), "ymin": bits(v, 28, 16), **_rest(v, 0x1FFF1FFF)}


def decode_pbe1(v: int) -> dict:
    return {"xmax": bits(v, 12, 0), "ymax": bits(v, 28, 16), **_rest(v, 0x1FFF1FFF)}


def decode_pbe2(v: int) -> dict:
    """PBE word 2: COUNT (15:8) and the line stride in pairs of pixels - 1 (27:16), for
    both pixel formats (size=*, format=8888)."""
    return {"count": bits(v, 15, 8), "stride_px": 2 * (bits(v, 27, 16) + 1),
            **_rest(v, 0x0FFFFF00)}


def decode_sideband(v: int) -> dict:
    """emitpix2 sideband: pack mode (5:0: 3 RGB565, 0 ARGB8888), tile relative (9),
    dither (10: dither=0 clears it)."""
    return {"packmode": bits(v, 5, 0), "tile_relative": bits(v, 9, 9),
            "dither": bits(v, 10, 10), **_rest(v, 0x63F)}


def decode_float(v: int) -> dict:
    return {"float": struct.unpack("<f", struct.pack("<I", v))[0]}


# kind (the last component of a state path) -> decoder
DECODERS: dict = {
    "isp0": decode_isp0, "isp1": decode_isp1, "isp2": decode_isp2,
    "pds_size": decode_pds_size, "clip0": decode_clip0, "clip1": decode_clip1,
    "term": decode_term, "mtectl": decode_mtectl, "te_screen": decode_te_screen,
    "mte_screen": decode_mte_screen, "pbe0": decode_pbe0, "pbe1": decode_pbe1,
    "pbe2": decode_pbe2, "sideband": decode_sideband,
    **{f"vp{i}": decode_float for i in range(6)}, "wclamp": decode_float,
}


def word_kind(path: str) -> str:
    return path.rsplit(".", 1)[-1]


# --- extraction ------------------------------------------------------------------------

def flatten(prefix: str, d: dict) -> dict[str, int]:
    out = {}

    def put(path, v):
        if isinstance(v, list):
            for i, x in enumerate(v):
                put(f"{path}[{i}]", x)
        else:
            out[path] = v
    for k, v in d.items():
        put(f"{prefix}.{k}", v)
    return out


def _words(raw: bytes) -> list[int]:
    return list(struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4]))


class _Walker:
    def __init__(self, snap, bases):
        self.snap, self.bases, self.out = snap, bases, {}

    def read(self, dva: int, n: int) -> list[int] | None:
        o = self.snap.find(dva)
        if o is None:
            return None
        n = min(n, o["dva"] + o["mapped"] - dva)
        return _words(self.snap.read(dva, n - n % 4)) if n >= 4 else None

    def usse(self, prefix: str, u0: int) -> list[int]:
        a = summary3d.usse_address(u0, self.bases)
        o = self.snap.find(a) if a is not None else None
        if o is None:
            return []
        insns = usse_insns(self.snap.data(o)[a - o["dva"]:])
        for j, insn in enumerate(insns):
            self.out[f"{prefix}.i{j}"] = insn
        return insns

    def pds(self, prefix: str, dva: int, ndata: int, mte: bool = False):
        """Record a PDS program and what it reaches; return its doutd blocks."""
        words = self.read(dva, 4 * (ndata + 64))
        if words is None:
            return []
        for j in range(min(ndata, len(words))):
            self.out[f"{prefix}.d{j}"] = words[j]
        for j, w in enumerate(summary3d.pds_code(words, ndata)):
            self.out[f"{prefix}.c{j}"] = w
        blocks, attrs, nd, nu = [], [], 0, 0
        for dest, vals in pds_outputs(words, ndata):
            if dest == "douta" and None not in vals[:2]:
                attrs.append((vals[0], vals[1]))
            if dest == "doutd" and vals[0] is not None and vals[1] is not None:
                n = doutd(vals[1])["dwords"]
                blk = self.read(vals[0], 4 * n) or []
                for j, w in enumerate(blk):
                    self.out[f"{prefix}.dma{nd}.w{j}"] = w
                blocks.append(blk)
                nd += 1
            elif dest == "doutu" and vals[0] is not None:
                self.usse(f"{prefix}.usse{nu}", vals[0])
                nu += 1
        if mte and not blocks and attrs:
            blocks = [douta_block(attrs)]
        if mte and blocks:          # a long state block is copied in several bursts
            self.mte(f"{prefix.rsplit('.', 1)[0]}.mte", [w for blk in blocks for w in blk])
        return blocks

    def pixel(self, prefix: str, ptr: int):
        dva, data = pixel_pds_ptr(ptr)
        self.pds(prefix, dva, data // 4)

    def mte(self, prefix: str, words: list[int]):
        d = decode_mte_state(words)
        for name, v in d:
            self.out[f"{prefix}.{name}"] = v
        names = dict(d)
        if "pds_sec" in names:
            self.pixel(f"{prefix}.sec", names["pds_sec"])
        if "pds_pri" in names:
            self.pixel(f"{prefix}.pri", names["pds_pri"])

    def vdm(self, dva: int):
        words = self.read(dva, 0x1000) or []
        for i, b in enumerate(kick3d.decode_vdm(words)):
            p = f"vdm.b{i}"
            for j in range(b["words"]):
                self.out[f"{p}.w{j}"] = words[b["offset"] + j]
            if "pds_base" in b:
                n = b["data_bytes"] // 4 if b["data_bytes"] is not None else 8
                self.pds(f"{p}.pds", b["pds_base"], n, mte="MTE emit" in b["flags"])
            if b["type"] == "INDEXLIST":
                count = min(b["count"], 32)
                idx = self.read(b["index_base"], 4 * ((count + 1) // 2)) or []
                found = []
                for j in range(count):
                    if j // 2 < len(idx):
                        found.append(idx[j // 2] >> (16 * (j % 2)) & 0xFFFF)
                        self.out[f"{p}.idx{j}"] = found[-1]
                if b.get("vertex"):
                    v = b["vertex"]
                    self.vertex(f"{p}.vtx", v["pds_base"], v["data_bytes"] // 4,
                                max(found, default=0) + 1)

    def vertex(self, prefix: str, dva: int, ndata: int, nverts: int):
        """A vertex PDS program: each doutd fetches one attribute of one vertex (base +
        vertex index x stride), so record nverts tightly packed elements of each
        stream."""
        words = self.read(dva, 4 * (ndata + 64)) or []
        self.pds(prefix, dva, ndata)
        n = 0
        for dest, vals in pds_outputs(words, ndata):
            if dest == "doutd" and None not in vals[:2]:
                size = 4 * doutd(vals[1])["dwords"] * min(nverts, 16)
                for j, w in enumerate(self.read(vals[0], size) or []):
                    self.out[f"{prefix}.stream{n}.w{j}"] = w
                n += 1

    def eot(self):
        """The end-of-tile USSE task of the end-of-render PDS program: PBE words."""
        for k, v in list(self.out.items()):
            if not (k.startswith("eor.d") and v and summary3d.usse_address(v, self.bases)):
                continue
            a = summary3d.usse_address(v, self.bases)
            o = self.snap.find(a)
            if o is None or o["heap_name"] != "PIXELSHADER":
                continue
            insns = usse_insns(self.snap.data(o)[a - o["dva"]:])
            regs = {}
            for insn in insns:
                m = usse_limm(insn)
                if m:
                    regs[m[0]] = m[1]
                if is_emitpix2(insn) and len(regs) == 4:
                    for r in range(4):
                        self.out[f"eot.pbe{r}"] = regs.get(r, 0)
                    self.out["eot.sideband"] = emitpix2_sideband(insn)
                    for j, x in enumerate(insns):
                        self.out[f"eot.i{j}"] = x
                    return


def extract(kick_in: bytes, snap, bases=summary3d.DEFAULT_CODE_BASES) -> dict[str, int]:
    w = _Walker(snap, bases)
    k = kick3d.decode("PVRSRV_BRIDGE_IN_DOKICK", kick_in)
    ccb = snap.by_handle(k["sCCBKick.hCCBKernelMemInfo"])
    cmd_dva = ccb + k["sCCBKick.ui32CCBOffset"] - kick3d.offset("SGXMKIF_CMDTA", "shared_state")
    cmd = kick3d.decode("SGXMKIF_CMDTA", snap.read(cmd_dva, kick3d.size("SGXMKIF_CMDTA")))
    w.out.update(flatten("cmd", cmd))
    rd_dva = cmd["hw_render_details_dev_addr"]
    rd = kick3d.decode("hw_render_details",
                       snap.read(rd_dva, kick3d.size("hw_render_details")))
    w.out.update(flatten("rd", rd))
    # The command's 3D register updates: {render details word address, value} pairs the
    # EDM writes into the render details before the render [CAP: depth adds six].
    regs = kick3d.offset("SGXMKIF_CMDTA", "reg_updates_3d")
    upd = _words(snap.read(cmd_dva + regs, 8 * min(cmd["num_3d_regs"], 64)))
    names = {4 * off: path for path, off, *_ in kick3d._layout(kick3d.RENDERDETAILS)}
    for addr, value in zip(upd[::2], upd[1::2]):
        w.out[f"upd.{names.get(addr - rd_dva, f'{addr:#x}')}"] = value
    rtds = kick3d.decode("hw_rt_data_set",
                         snap.read(cmd["sHWRTDataSetDevAddr"], kick3d.size("hw_rt_data_set")))
    w.out.update(flatten("rtds", rtds))
    rt = kick3d.decode("hw_rt_data",
                       snap.read(cmd["sHWRTDataDevAddr"], kick3d.size("hw_rt_data")))
    w.out.update(flatten("rt", rt))
    for j, v in enumerate(w.read(rt["bg_obj_base_dev_addr"], 0x80) or []):
        w.out[f"bg.w{j}"] = v
    for name in ("hw_pix_event_update_list_dev_addr", "hw_bg_obj_prim_pds_update_list_dev_addr"):
        for j, v in enumerate(w.read(rd[name], 16) or []):
            w.out[f"rd.{name}.w{j}"] = v
    spec = cmd["spec_object"]
    w.pixel("spec.sec", spec[0])
    w.pixel("spec.pri", spec[2])
    w.pds("eor", rd["regs_3d.edm_pixel_pds_exec"] & ~0xF,
          4 * rd["regs_3d.edm_pixel_pds_data"])
    w.eot()
    w.vdm(cmd["ta_regs.vdm_ctrl_stream_base"])
    return w.out


def extract_capture(path, bases=summary3d.DEFAULT_CODE_BASES) -> dict[str, int]:
    """Both kicks of a capture directory (<path>/dump3d/kickNN_in.bin + kickNN/), with
    paths prefixed k0., k1., ..."""
    d = pathlib.Path(path) / "dump3d"
    out = {}
    for kin in sorted(d.glob("kick*_in.bin")):
        n = int(kin.name[4:6])
        snap = kick3d.Snapshot(d / kin.name.replace("_in.bin", ""))
        for k, v in extract(kin.read_bytes(), snap, bases).items():
            out[f"k{n}.{k}"] = v
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    for k, v in extract_capture(argv[0]).items():
        print(f"{k:48s} {v:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
