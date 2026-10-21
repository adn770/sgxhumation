#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""Decode SGX545 PDS programs and the PDS-related fields around them.

The EDM is entered through small PDS programs (KERNEL_CODE allocations #10-#14 in the
harness capture). A PDS program is a data segment followed by 32-bit PDS instructions;
the instructions are disassembled by refdisasm/refpds545 (PVR_PSP2's pdsdisasm). This
module handles the parts that tool does not:

- the data segment, which is stored in rows of four dwords holding two banks:
  ds0[2r], ds0[2r+1], ds1[2r], ds1[2r+1];
- USSE code-address words (doutu operands): 0x08000000 | (pc << 4) | 0xf, where pc is
  an instruction index from the EDM code base; srvinit relocates the template by the
  ukernel's offset in that base (0x200 instructions on the captured build);
- the emitpds control word (data-segment size, in 4-dword units, at bits [27:23]);
- EUR_CR_EVENT_OTHER_PDS_EXEC / _DATA / _INFO (sgx545defs.h field layout).

    usage: pdsprog.py <program.bin> <data_dwords> [base_insn]
"""
from __future__ import annotations

import struct
import sys

CODE_WORD_MASK, CODE_WORD_TAG = 0xF800000F, 0x0800000F


def banks(data: list[int]) -> tuple[list[int], list[int]]:
    if len(data) % 4:
        raise ValueError("PDS data segment must be whole rows of four dwords")
    ds0, ds1 = [], []
    for r in range(0, len(data), 4):
        ds0 += data[r:r + 2]
        ds1 += data[r + 2:r + 4]
    return ds0, ds1


def code_entry(word: int, base_insn: int = 0) -> int | None:
    """USSE instruction index named by a PDS code-address word, or None."""
    if (word & CODE_WORD_MASK) != CODE_WORD_TAG:
        return None
    return ((word & 0x007FFFF0) >> 4) - base_insn


def emitpds_data_size(ctl: int) -> int:
    return (ctl >> 23) & 0x1F


def event_pds(exec_: int, data: int, info: int) -> dict[str, int]:
    return {"exec": exec_ & 0xFFFFFFF0, "data_size": data & 0x3F,
            "attr_size": (info >> 2) & 0xFF, "sd": (info >> 1) & 1, "rotate": info & 1}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    raw = open(argv[0], "rb").read()
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4]))
    ndata, base = int(argv[1], 0), int(argv[2], 0) if len(argv) > 2 else 0
    ds0, ds1 = banks(words[:ndata])
    for name, bank in (("ds0", ds0), ("ds1", ds1)):
        for i, w in enumerate(bank):
            pc = code_entry(w, base)
            note = f"  -> USSE entry {pc:#x}" if pc is not None else ""
            print(f"{name}[{i:2d}] = {w:#010x}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
