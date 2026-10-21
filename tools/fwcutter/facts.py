"""Our reconstructed tables for sgx545-fwcutter: the FACT side of the firmware file.

Nothing here is copied from the closed library. The known-input table holds offsets,
sizes and hashes; everything else is what srvinit's code computes at runtime,
reconstructed from the harness captures and the ukernel analysis
(design/sgx545_hw_spec.md §13, design/sgx545_fw_format.md). test_fwload.py proves the
tables complete: with them, the reference loader reproduces the captures exactly.
"""
from __future__ import annotations

import dataclasses
from dataclasses import dataclass

from fwformat import (ENC_CODE_BASE, ENC_EMITPIX_SB, ENC_USSE_LIMM, ENC_WORD32, K_ADDR,
                      K_ADDR_SHR4, K_CODE_WORD, K_CONST, K_DOUTU, K_HEAP, K_PARAM, K_PD_PHYS,
                      K_PDS_CTL, K_PDS_ROWS, NO_BUFFER, TQ_NO_STRADDLE, V_DEINIT, V_DEV_DATA,
                      V_PART1, V_PART2, Buffer, Expr, TQMeta)

# Heap ids (sgxapi_km.h).
KERNEL_CODE, KERNEL_DATA, PIXELSHADER, VERTEXSHADER, PDSPIXEL = 2, 3, 4, 5, 6

# Buffer ids (format §3).
UKERNEL, PDS10, PDS11, PDS12, PDS13, PDS14 = range(6)
CCB, CCB_CTL, KICKER, TA3D_CTL, HOST_CTL, MISC, HWPERF, TA_SIG, TD_SIG = range(6, 15)
BUFFER_NAMES = ("ukernel", "pds10", "pds11", "pds12", "pds13", "pds14", "ccb", "ccb_ctl",
                "event_kicker", "ta3d_ctl", "host_ctl", "misc", "hwperf", "ta_sig", "3d_sig")

# Allocations srvinit requests: (id, heap, PDS data rows, size, alignment).
BUFFERS = [Buffer(*b) for b in [
    (UKERNEL, KERNEL_CODE, 0, 0xCF68, 8),
    (PDS10, KERNEL_CODE, 1, 0x18, 16), (PDS11, KERNEL_CODE, 2, 0x38, 16),
    (PDS12, KERNEL_CODE, 1, 0x18, 16), (PDS13, KERNEL_CODE, 6, 0x100, 16),
    (PDS14, KERNEL_CODE, 3, 0x68, 16),
    (CCB, KERNEL_DATA, 0, 0x2000, 0x1000), (CCB_CTL, KERNEL_DATA, 0, 0x8, 0x1000),
    (KICKER, KERNEL_DATA, 0, 0x4, 16), (TA3D_CTL, KERNEL_DATA, 0, 0xEC, 32),
    (HOST_CTL, KERNEL_DATA, 0, 0x44, 32), (MISC, KERNEL_DATA, 0, 0x54, 32),
    (HWPERF, KERNEL_DATA, 0, 0x640C, 32), (TA_SIG, KERNEL_DATA, 0, 0x620, 32),
    (TD_SIG, KERNEL_DATA, 0, 0x2088, 32),
]]


@dataclass(frozen=True)
class CutSection:
    buffer: int
    offset: int
    size: int
    sha256: str


@dataclass(frozen=True)
class Piece:
    """size bytes at src in the library go to dst in the template (format §9.2)."""
    src: int
    size: int
    dst: int


@dataclass(frozen=True)
class TQCut:
    template: int
    size: int
    pieces: tuple[Piece, ...]
    sha256: str


@dataclass(frozen=True)
class KnownInput:
    name: str
    sha256: str
    size: int
    ddk: tuple[int, int, int]
    build_options: int
    package: str
    sections: tuple[CutSection, ...]
    role: str = "init"                       # "init": libsrv_init.so, "um": libsrv_um.so
    templates: tuple[TQCut, ...] = ()


KNOWN_INPUTS = [
    KnownInput(
        name="libsrv_init.so.1.7.862890",
        sha256="8c5a59c43f793f731199455f41ec4f065c63ecdbd36f72e4ea7501e2e327869a",
        size=63524, ddk=(1, 7, 862890), build_options=0x60218,
        package="cdv-gfx-drivers-1.0.3_bee.tar.bz2 (sha256 2e662b25...0317a), "
                "cedarview-userspace-v1.0.3_bee, usr/lib/",
        sections=(
            CutSection(UKERNEL, 0x2080, 0xCF68,
                       "73dce0885c45ecbe691f86305b3645e525e7a5e18de3de3b5a469d0df8850349"),
            CutSection(PDS10, 0x2008, 0x18,
                       "0963b59aa21ddb00386bb5fa8fad3bab0cd50806da4b3513ce5b362c55005092"),
            CutSection(PDS11, 0x2020, 0x38,
                       "fd5e85010911f8e806da1bbfc4cc1af54b873fb9b19885aba6ab731c229e010f"),
            CutSection(PDS12, 0x2058, 0x18,
                       "0963b59aa21ddb00386bb5fa8fad3bab0cd50806da4b3513ce5b362c55005092"),
            CutSection(PDS13, 0x1EA0, 0x100,
                       "6dd6f63bd18eb71e3a6a0059f843400b6b98507b475922bae99e9f70a628a684"),
            CutSection(PDS14, 0x1FA0, 0x68,
                       "3a8cdac3f4858b495e09962653d761e8212ccfd45a6b7f3c5bae942f626b6e88"),
        ),
    ),
]

# Transfer-queue template ids (format §9.2, §10.2).
(TQ_USE_FILL, TQ_USE_SEC, TQ_USE_EOT, TQ_USE_EOR, TQ_PDS_PRIM, TQ_PDS_SEC,
 TQ_PDS_PIXEVENT, TQ_USE_COPY, TQ_PDS_PRIM_1SRC) = range(9)
TQ_NAMES = ("USE_FILL", "USE_SEC", "USE_EOT", "USE_EOR", "PDS_PRIM", "PDS_SEC",
            "PDS_PIXEVENT", "USE_COPY", "PDS_PRIM_1SRC")
TQ_COPY = (TQ_USE_COPY, TQ_PDS_PRIM_1SRC)        # format v3 (§10): left out of a v2 cut


def _imm(*pairs: tuple[int, int]) -> tuple[Piece, ...]:
    return tuple(Piece(src, 4, dst) for src, dst in pairs)


KNOWN_INPUTS.append(KnownInput(
    name="libsrv_um.so.1.7.862890",
    sha256="2b2c09866ca372c06b9e20472caeb43edf0d776a25fa63d30979121f11988a44",
    size=196152, ddk=(1, 7, 862890), build_options=0x60218,
    package="cdv-gfx-drivers-1.0.3_bee.tar.bz2 (sha256 2e662b25...0317a), "
            "cedarview-userspace-v1.0.3_bee, usr/lib/",
    sections=(), role="um",
    templates=(
        TQCut(TQ_USE_FILL, 0x10, (Piece(0x22218, 8, 0),) + _imm((0x1D1CC, 12)),
              "7727cde6c86f89289c02a2c24a807c2112d33438e01c6bc19bda1b49a7a400b0"),
        TQCut(TQ_USE_SEC, 0x10, (Piece(0x223D8, 16, 0),),
              "74c3e748098b2bb8929d17726a235565ab9d36ace0bd4897604ecc6e1847cece"),
        TQCut(TQ_USE_EOT, 0x38, _imm((0x2001E, 4), (0x20062, 12), (0x20062, 20),
                                     (0x20062, 28), (0x20062, 36), (0x2007C, 40),
                                     (0x20083, 44), (0x20092, 48), (0x200B3, 52)),
              "d03fa3dcad8f058155bee1532d2432158920ef65249130fd9a5e90c35a6667be"),
        TQCut(TQ_USE_EOR, 0x18, _imm((0x1FD9D, 4), (0x1FDAB, 12), (0x1FDB2, 16),
                                     (0x1FDB9, 20)),
              "dfbb3b5d76d2776de4aad6a5760a52b7253d9f9a0dc3eb7090dbe1b1a4ccddaa"),
        TQCut(TQ_PDS_PRIM, 0x18, (Piece(0x227E0, 24, 0),),
              "0963b59aa21ddb00386bb5fa8fad3bab0cd50806da4b3513ce5b362c55005092"),
        TQCut(TQ_PDS_SEC, 0x18, (Piece(0x229A0, 24, 0),),
              "0963b59aa21ddb00386bb5fa8fad3bab0cd50806da4b3513ce5b362c55005092"),
        TQCut(TQ_PDS_PIXEVENT, 0xC4, (Piece(0x23080, 196, 0),),
              "65e78fa8304ae7ef41064086db9ca16a3b2da3ec701b5f61fa076c7c19881671"),
        # format v3: USE resource 0 (the copy shader) and the single_source PDS primary
        TQCut(TQ_USE_COPY, 0x10, (Piece(0x21D60, 16, 0),),
              "9d0dfd83a18e929ed4da9bd5363bedd84c26e5d652515c8d646a545d5a78b297"),
        TQCut(TQ_PDS_PRIM_1SRC, 0x40, (Piece(0x22960, 64, 0),),
              "e3f3a906453236f89efa7cdd4132db26a2f2cb45010dd532da0e4acf13da3758"),
    ),
))


# The same DDK build as shipped by Ubuntu 12.04 (multiverse, cedarview-graphics-drivers
# 20120717, Intel's 1.0.2 drop): different files, the same code. libsrv_init.so carries every
# section 0x80 bytes lower (the ukernel at 0x2000); libsrv_um.so carries the code immediates
# 0x1d0 and the whole programs 0x200 bytes higher. Each cut piece is hash-checked as above.
UBUNTU_PACKAGE = ("cedarview-graphics-drivers_20120717.orig.tar.gz (sha256 c23e7261...8fd83), "
                  "usr/lib/")


def _ubuntu(k: KnownInput, sha256: str, size: int) -> KnownInput:
    def moved(p: Piece) -> Piece:
        return dataclasses.replace(p, src=p.src + (0x1D0 if p.src < 0x21000 else 0x200))
    return dataclasses.replace(
        k, sha256=sha256, size=size, package=UBUNTU_PACKAGE,
        sections=tuple(dataclasses.replace(s, offset=s.offset - 0x80) for s in k.sections),
        templates=tuple(dataclasses.replace(t, pieces=tuple(map(moved, t.pieces)))
                        for t in k.templates))


KNOWN_INPUTS += [
    _ubuntu(KNOWN_INPUTS[0], "4d3df850191ac363b085afd025982f5eace64977edd5483466649e9effe53b6d",
            66976),
    _ubuntu(KNOWN_INPUTS[1], "e19402fce177c72e0c76f0c92247e02f83141aa753fe67b18cf896d619ad0dce",
            198204),
]


def _limm(insn: int, kind: int, target: int = 0, addend: int = 0) -> Expr:
    return Expr(UKERNEL, ENC_USSE_LIMM, kind, 0, insn * 8, target, addend)


def _word(buf: int, dword: int, kind: int, target: int = 0, addend: int = 0) -> Expr:
    return Expr(buf, ENC_WORD32, kind, 0, dword * 4, target, addend)


def _const(buf: int, dword: int, value: int) -> Expr:
    return _word(buf, dword, K_CONST, 0, value)


CODE_TAG = 0x0800000F        # USSE code-address word (spec §13.8)


def _entry(buf: int, dword: int, insn: int, tag: int = CODE_TAG) -> Expr:
    return _word(buf, dword, K_CODE_WORD, insn, tag)


PATCHES = [
    # ukernel: the 9 `mov rN, #0xDEADBEEF` placeholders (spec §13.9).
    _limm(428, K_PDS_CTL, PDS11, 0x1000),    # edm.task.boot: emitpds PDS #11
    _limm(430, K_ADDR_SHR4, PDS11),
    _limm(431, K_ADDR, TA3D_CTL),
    _limm(438, K_PDS_CTL, PDS12, 0x100),     # edm.task.boot: emitpds PDS #12
    _limm(440, K_ADDR_SHR4, PDS12),
    _limm(456, K_ADDR, PDS13),               # edm.task.init_done -> EVENT_OTHER_PDS_EXEC
    _limm(458, K_PDS_ROWS, PDS13),           #                     -> EVENT_OTHER_PDS_DATA
    _limm(5983, K_PDS_CTL, PDS14, 0x100),    # edm.request: emitpds PDS #14
    _limm(5986, K_ADDR_SHR4, PDS14),

    # PDS data segments, as dword indices (rows of ds0[2r], ds0[2r+1], ds1[2r], ds1[2r+1]).
    _entry(PDS10, 0, 0x1AC), _const(PDS10, 1, 2),             # -> edm.task.boot
    _word(PDS11, 0, K_ADDR, TA3D_CTL),                        # DMA source: TA3DCtl -> sa
    _const(PDS11, 1, 0x03C0003F),                             # DMA control
    _entry(PDS11, 4, 0x1BE, tag=0xF), _const(PDS11, 5, 2),    # -> edm.task.pds11
    _entry(PDS12, 0, 0x1C4), _const(PDS12, 1, 2),             # -> edm.task.init_done
    _const(PDS13, 0, 2), _const(PDS13, 3, 0x100), _const(PDS13, 6, 0x200),
    _entry(PDS13, 7, 0xE0A),                                  # -> edm.event.dpm_store
    _entry(PDS13, 10, 0x59A),                                 # -> edm.event.dpm_clear
    _entry(PDS13, 11, 0xACE),                                 # -> edm.event.csw_flush
    _entry(PDS13, 14, 0x6),                                   # -> edm.event.general
    _entry(PDS14, 0, 0xE74), _const(PDS14, 1, 2),             # -> edm.work.dpm_store
    _const(PDS14, 3, 0x200),
    _entry(PDS14, 4, 0x5D4), _const(PDS14, 5, 2),             # -> edm.work.dpm_clear
    _const(PDS14, 7, 0x0004000F),                             # dpm_store request group
    _entry(PDS14, 8, 0xAE8), _const(PDS14, 9, 2),             # -> edm.work.csw_flush
    _const(PDS14, 10, 0x00120080),                            # dpm_clear request group

    # TA3DCtl: the sa register image PDS #11 loads (spec §13.9).
    _word(TA3D_CTL, 0, K_ADDR, TA3D_CTL), _word(TA3D_CTL, 1, K_ADDR, HOST_CTL),
    _word(TA3D_CTL, 2, K_ADDR, CCB_CTL), _word(TA3D_CTL, 22, K_ADDR, CCB),
    _word(TA3D_CTL, 23, K_PD_PHYS),
    _word(TA3D_CTL, 43, K_ADDR, HWPERF), _word(TA3D_CTL, 44, K_ADDR, TA_SIG),
    _word(TA3D_CTL, 45, K_ADDR, TD_SIG),
]

# Signature buffers: a register count, a zero word, then the register offsets to sample.
TA_SIG_REGS = [0x304, 0x26C, 0x270, 0x2A0, 0x2A4, 0x378]
TD_SIG_REGS = [0x438, 0x4CC, 0x4D0, 0x4D4, 0x4D8, 0x510, 0x514, 0x518, 0x51C, 0x524, 0x528,
               0x52C, 0x530, 0x534, 0x538, 0x53C, 0x540, 0x544, 0x550, 0x554, 0x558, 0x55C,
               0x84C, 0x850, 0x854, 0x858, 0x560, 0x564, 0x568, 0x56C, 0x308, 0x30C]
for _buf, _regs in ((TA_SIG, TA_SIG_REGS), (TD_SIG, TD_SIG_REGS)):
    PATCHES.append(_const(_buf, 0, len(_regs)))
    PATCHES += [_const(_buf, 2 + i, r) for i, r in enumerate(_regs)]


def _reg(reg: int, value: int) -> Expr:
    return Expr(NO_BUFFER, ENC_WORD32, K_CONST, 0, reg, 0, value)


def _code_base(n: int, heap: int, dm: int, addend: int = 0) -> Expr:
    return Expr(NO_BUFFER, ENC_CODE_BASE, K_HEAP, dm, 0x0A0C + 4 * n, heap, addend)


DM_VERTEX, DM_PIXEL, DM_EDM = 0, 1, 3

PART2 = [_reg(r, v) for r, v in [
    (0x0818, 0x7FF), (0x0810, 0x1), (0x0814, 0xFFFF), (0x0130, 0x4000), (0x0110, 0),
    (0x013C, 0), (0x0A7C, 0), (0x0A80, 0), (0x0A74, 0x208200), (0x04E8, 0x100),
    (0x0A78, 0x2C), (0x0AAC, 0x45), (0x0ABC, 0xE), (0x0800, 0x30), (0x0804, 0xFFFF),
    (0x0A00, 0x7C000),
]]
PART2 += [_code_base(n, PIXELSHADER, DM_PIXEL, n * 0x80000) for n in range(10)]
PART2 += [_code_base(n, VERTEXSHADER, DM_VERTEX, (n - 10) * 0x80000) for n in range(10, 14)]
PART2 += [
    _code_base(15, KERNEL_CODE, DM_EDM),
    Expr(NO_BUFFER, ENC_WORD32, K_HEAP, 0, 0x0AB8, PDSPIXEL, 0),       # PDS_EXEC_BASE
    _reg(0x0B30, 0x100),                                               # USE_MOE
    Expr(NO_BUFFER, ENC_WORD32, K_ADDR, 0, 0x0AC4, KICKER, 0),         # EVENT_KICKER
    Expr(NO_BUFFER, ENC_WORD32, K_ADDR, 0, 0x0A68, PDS10, 0),          # EVENT_OTHER_PDS_EXEC
    Expr(NO_BUFFER, ENC_WORD32, K_PDS_ROWS, 0, 0x0A6C, PDS10, 0),      # EVENT_OTHER_PDS_DATA
    _reg(0x0A70, 0x6),                                                 # EVENT_OTHER_PDS_INFO
    _reg(0x0A58, 0x2204B00F),                                          # EVENT_PDS_ENABLE
]

VALUES = {
    V_PART1: [_reg(0x0A58, 0), _reg(0x0ACC, 0)],
    V_PART2: PART2,
    V_DEINIT: [_reg(r, 0) for r in (0x0A58, 0x0A68, 0x0A6C, 0x0A70, 0x0A48, 0x0130, 0x0110,
                                    0x0000, 0x0004)],
    V_DEV_DATA: [
        Expr(NO_BUFFER, ENC_WORD32, K_CONST, 0, 8, 0, 2),
        Expr(NO_BUFFER, ENC_WORD32, K_CONST, 0, 10, 0, 0x300),
        Expr(NO_BUFFER, ENC_WORD32, K_CONST, 0, 11, 0, 0x940),
        Expr(NO_BUFFER, ENC_WORD32, K_HEAP, 0, 12, PDSPIXEL, 0),
        Expr(NO_BUFFER, ENC_WORD32, K_HEAP, 0, 13, PIXELSHADER, 0),
    ],
}

# EDM_META (format §5).
KICK = [0x11D, 0x103, 0, 0xD4, 0xEB, 0x13A, 0x173, 0x156, 0, 0x157, 0]
STRUCT_SIZES = [0x2B0, 0x1E0, 0x4C8, 0xDC, 0xD0, 0x3C, 0x44, 0x1EC, 0x38, 0x50, 0xB4, 0x44, 0x20]
EDM_TASK_REGS = [0x0B14, 0x0B20]                    # EUR_CR_USE0/USE1_SERV_EVENT
CLKGATE = [0xA6AA, 0xAAAAAA, 0x8, 0xFFFFFFDF]       # ctl, ctl2, status reg, status mask
CACHE_CONTROL = 0x2
META = KICK + STRUCT_SIZES + EDM_TASK_REGS + CLKGATE + [CACHE_CONTROL]


# --- format v2/v3: the transfer-queue templates of a fill and a copy (format §9, §10) -----

# Template records: (id, heap, temps, PDS attributes, size, align, PDS data bytes, flags).
TQ_META = [TQMeta(*m) for m in [
    (TQ_USE_FILL, PIXELSHADER, 0, 0, 0x10, 8, 0, TQ_NO_STRADDLE),
    (TQ_USE_SEC, PIXELSHADER, 0, 0, 0x10, 8, 0, TQ_NO_STRADDLE),
    (TQ_USE_EOT, PIXELSHADER, 4, 0, 0x38, 8, 0, TQ_NO_STRADDLE),
    (TQ_USE_EOR, PIXELSHADER, 1, 0, 0x18, 8, 0, TQ_NO_STRADDLE),
    (TQ_PDS_PRIM, PDSPIXEL, 0, 0, 0x18, 16, 0x10, TQ_NO_STRADDLE),
    (TQ_PDS_SEC, PDSPIXEL, 0, 0, 0x18, 16, 0x10, TQ_NO_STRADDLE),
    (TQ_PDS_PIXEVENT, PDSPIXEL, 0, 1, 0xC4, 16, 0x50, TQ_NO_STRADDLE),
    (TQ_USE_COPY, PIXELSHADER, 0, 0, 0x10, 8, 0, TQ_NO_STRADDLE),
    (TQ_PDS_PRIM_1SRC, PDSPIXEL, 0, 1, 0x40, 16, 0x30, TQ_NO_STRADDLE),
]]

# Per-transfer parameters (format §9.6, §10.4).
P_COLOUR, P_PBE0, P_PBE1, P_PBE2, P_PBE3, P_SIDEBAND, P_TEX0, P_TEX1, P_TEXADDR = range(9)
DOUTU1_PIXEL = 4                     # pds_doutu1_sdsoft
DOUTU1_TEXTURE = 0x02000004          # SDSOFT | TEXTUREDEPENDENCY
DOUTI0_TC0 = 0x40012001              # TEXISSUE TC0 | USEISSUE NONE | TEXLASTISSUE
DOUTT3 = 0xA0800000                  # T3 as set_tag_state sets it (transfer §4.4.3)
DOUTU1_EOT = 0x00800000              # PBENABLE
DOUTU1_EOR = 0x00C00000              # PBENABLE | ENDOFRENDER


def _tq(t: int, enc: int, off: int, kind: int, target: int = 0, addend: int = 0) -> Expr:
    return Expr(t, enc, kind, 0, off, target, addend)


def _tq_word(t: int, dword: int, kind: int, target: int = 0, addend: int = 0) -> Expr:
    return _tq(t, ENC_WORD32, dword * 4, kind, target, addend)


def _kick(t: int, use: int, u1: int) -> list[Expr]:
    """A PDS program's DOUTU words U0, U1, U2 in data dwords 0-2 (transfer §4.2.4)."""
    return [_tq_word(t, 0, K_DOUTU, use), _tq_word(t, 1, K_CONST, 0, u1),
            _tq_word(t, 2, K_CONST, 0, 0)]


TQ_PATCHES = [
    _tq(TQ_USE_FILL, ENC_USSE_LIMM, 8, K_PARAM, P_COLOUR),
    # EOT: the generator numbers the four movs r0..r3 (i << 21) before the immediates.
    *[_tq(TQ_USE_EOT, ENC_WORD32, 8 + 8 * i, K_CONST, 0, i << 21) for i in (1, 2, 3)],
    *[_tq(TQ_USE_EOT, ENC_USSE_LIMM, 8 + 8 * i, K_PARAM, P_PBE0 + i) for i in range(4)],
    _tq(TQ_USE_EOT, ENC_EMITPIX_SB, 48, K_PARAM, P_SIDEBAND),
    *_kick(TQ_PDS_PRIM, TQ_USE_FILL, DOUTU1_PIXEL),
    *_kick(TQ_PDS_SEC, TQ_USE_SEC, DOUTU1_PIXEL),
    # pixel event: ds0[0], ds0[1] PTOFF task; ds1[0] douta source; ds1[1] EOT U0;
    # ds0[2], ds0[3] EOR task; ds0[6] EOT U1 base (transfer §4.2.4).
    _tq_word(TQ_PDS_PIXEVENT, 0, K_DOUTU, TQ_USE_EOT),
    _tq_word(TQ_PDS_PIXEVENT, 1, K_CONST, 0, DOUTU1_EOT),
    _tq_word(TQ_PDS_PIXEVENT, 2, K_CONST, 0, 0),
    _tq_word(TQ_PDS_PIXEVENT, 3, K_DOUTU, TQ_USE_EOT),
    _tq_word(TQ_PDS_PIXEVENT, 4, K_DOUTU, TQ_USE_EOR),
    _tq_word(TQ_PDS_PIXEVENT, 5, K_CONST, 0, DOUTU1_EOR),
    _tq_word(TQ_PDS_PIXEVENT, 12, K_CONST, 0, DOUTU1_EOT),
    # format v3, the copy's PDS primary (transfer §4.4.3): data dwords 0-2 ds0[0], ds0[1],
    # ds1[0] the DOUTU words; 4, 5 ds0[2], ds0[3] DOUTI; 6, 7 ds1[2], ds1[3] T2, T3;
    # 8, 9 ds0[4], ds0[5] T0, T1.
    *_kick(TQ_PDS_PRIM_1SRC, TQ_USE_COPY, DOUTU1_TEXTURE),
    _tq_word(TQ_PDS_PRIM_1SRC, 4, K_CONST, 0, DOUTI0_TC0),
    _tq_word(TQ_PDS_PRIM_1SRC, 5, K_CONST, 0, 0),
    _tq_word(TQ_PDS_PRIM_1SRC, 6, K_PARAM, P_TEXADDR),
    _tq_word(TQ_PDS_PRIM_1SRC, 7, K_CONST, 0, DOUTT3),
    _tq_word(TQ_PDS_PRIM_1SRC, 8, K_PARAM, P_TEX0),
    _tq_word(TQ_PDS_PRIM_1SRC, 9, K_PARAM, P_TEX1),
]


def _cmd(off: int, kind: int, target: int = 0, addend: int = 0) -> Expr:
    return Expr(NO_BUFFER, ENC_WORD32, kind, 0, off, target, addend)


# The fixed SGXMKIF_TRANSFERCMD words of a fill or a copy (VALUES list 4, format §9.5).
TQ_CMD = [
    _cmd(0x004, K_CONST, 0, 0x200),                  # ISPBgObj: ENABLEBGTAG
    _cmd(0x008, K_CONST, 0, 0x3F800000),             # ISPBgObjDepth: 1.0f
    _cmd(0x00C, K_CONST, 0, 3),                      # ISPRender: FASTSCALE, TL2BR
    _cmd(0x014, K_CONST, 0, 0x100),                  # ISPIPFMisc: PROCESSEMPTY
    _cmd(0x01C, K_CONST, 0, 0x80000000),             # 3DAAMode
    _cmd(0x020, K_CONST, 0, 0x88),                   # ISPMultiSampCtl
    _cmd(0x024, K_ADDR, TQ_PDS_PIXEVENT),            # EDMPixelPDSExec
    _cmd(0x028, K_CONST, 0, 0x50 >> 4),              # EDMPixelPDSData
    _cmd(0x02C, K_CONST, 0, (4 * 1 + 15) >> 4),      # EDMPixelPDSInfo
    _cmd(0x038, K_CONST, 0, 0x80),                   # PixelBE
    _cmd(0x03C, K_HEAP, PDSPIXEL),                   # PDSExecBase
    _cmd(0x194, K_CONST, 0, 0x07C00000),             # num_pixel_partitions
]
