<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Immediates

- **Seven-bit immediates** are source bank 6 (`#0`-`#127`), in sources 1 and 2.
- **The 32-bit move** (`mov dst, #imm32`, class 31) scatters its immediate over three ranges:
  value bits 20-0 in `enc[20:0]`, 25-21 in `enc[40:36]` and 31-26 in `enc[49:44]` **[RE/HW]**.
  The driver patches colours and PBE words into our programs exactly there
  (`design/sgx545_fw_format.md` §4, the `USSE_LIMM` patch). For example:

<!-- @example usse mov.skipinv.end o0, #0x12345678 -->

- **The rotated immediate** of the bitwise group (source 2 written as `#imm`) is a 16-bit
  value rotated left by a five-bit amount, so any 32-bit value with at most 16 significant
  contiguous bits (cyclically) can be written directly; the assembler finds the rotation
  **[RE]**:

<!-- @example usse and r1, r2, #0x00FF0000 -->

- **Register indices** of `ldr`/`str` are 14 bits in two seven-bit halves (`enc[6:0]` low,
  `enc[20:14]` high), counted in dwords (`ldr r0, #7, drc0` reads `CORE_ID` at offset `0x1c`)
  **[RE/HW]**. Whether the hardware honours all 14 bits is pending a probe.
- **Branch targets** are 18 bits (`enc[17:0]`): absolute for `ba`, signed for `br`.
- Smaller numbers (counts, `phas` operands, the MOE values, emit sidebands) are described with
  their groups.
