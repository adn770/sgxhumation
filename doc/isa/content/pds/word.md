<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Programs and the instruction word

A PDS program is a **data segment** followed by **instructions**, both 32-bit little-endian
words **[CAP/HW]**. The data segment holds the program's constants (code addresses, DMA control
words, masks); the hardware is told its size separately, in rows of four dwords (16 bytes):
in a `doutu`-started program's resource record, in the VDM state words, or in the
`EVENT_*_PDS_DATA` registers (`design/sgx545_hw_spec.md` §13.8, `design/sgx545_3d.md` §9.1).
Branch targets count instructions from the first instruction after the data segment
**[RE/CAP]**.

An instruction word has the **opcode** in `w[31:27]` and, in most groups, the **predicate** in
`w[26:24]` (7 = always); the remaining 24 bits are the operands, laid out per group **[RE]**.
The opcodes and the mnemonics each one holds (the integer ALU group takes its operation from
the opcode field itself, so its single form spans several opcodes):

<!-- @classmap pds -->
