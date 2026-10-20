"""Our reconstructed tables for sgx545-fwcutter: the FACT side of the firmware file.

Nothing here is copied from the closed library. The known-input table holds offsets,
sizes and hashes; everything else is what srvinit's code computes at runtime,
reconstructed from the harness captures and the ukernel analysis
(design/sgx545_hw_spec.md §13, design/sgx545_fw_format.md). test_fwload.py proves the
tables complete: with them, the reference loader reproduces the captures exactly.
"""
from __future__ import annotations

from dataclasses import dataclass

from fwformat import (ENC_CODE_BASE, ENC_USSE_LIMM, ENC_WORD32, K_ADDR, K_ADDR_SHR4,
                      K_CODE_WORD, K_CONST, K_HEAP, K_PD_PHYS, K_PDS_CTL, K_PDS_ROWS,
                      NO_BUFFER, V_DEINIT, V_DEV_DATA, V_PART1, V_PART2, Buffer, Expr)

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
class KnownInput:
    name: str
    sha256: str
    size: int
    ddk: tuple[int, int, int]
    build_options: int
    package: str
    sections: tuple[CutSection, ...]


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
