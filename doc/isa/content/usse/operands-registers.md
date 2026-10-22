<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Operand encodings

### Register operands

A register operand is a seven-bit number plus bank bits. Destinations, sources 1 and 2, and
source 0 number their banks differently **[RE]** (the printers were derived by sweeping every
number and bank through the reference disassembler, and hold in every form checked). The
tables below are generated from the decoder's printers: each row is a bank code (the value
bits above the number) and what numbers 0, 1 and 127 print in it.

**Destination** (`enc[27:21]` number, `enc[33:32]` bank, `enc[51]` bank extension):

<!-- @banks usse pat='^\{0\}mov\{1\}\{2\}\{3\}\{4\} \{5\}\{6\}, \{7\}$' field=5 -->

**Sources 1 and 2** (source 1: `enc[13:7]`, `enc[31:30]`, `enc[49]`; source 2: `enc[6:0]`,
`enc[29:28]`, `enc[48]`). Bank 6 is a seven-bit immediate, printed in decimal or, by some
instructions (`mov`, the bitwise group's source 1), in hex:

<!-- @banks usse pat='^\{0\}\{1\}\{2\} \{3\}\{4\}, \{5\}, \{6\}\{7\}$' field=5 -->

**Source 0** (`enc[20:14]`, `enc[34]`, and `enc[51]` where the instruction has no
destination extension): temporaries, `pa`, `o` and `sa` only; no immediates.

<!-- @banks usse pat='^emitpix1' field=4 -->

- **Temporaries and internal registers.** In bank 0, numbers 124-127 print as `i0`-`i3`; in
  the 6-bit readings below (half floats, c10, the colour groups) the temporaries stop at `r59`
  (`r29` for half-float halves) and the internal registers follow. A load destination is
  either `r0`-`r127` or `pa0`-`pa127` (one bank bit, `enc[39]`).
- **Bank 7** prints `i0`-`i127` in both tables. Numbers past 3 read the other instances of
  the 2x2 quad, modulo 16, and writes to them are dropped **[HW]** (*Internal registers*).
- **Indexed operands** `bank[il + #n]` and `bank[ih + #n]`: the bank is `r`, `o`, `pa` or `sa`,
  the base the index register `i.l` or `i.h`, and `n` 0-15 (0-7 in the 6-bit readings)
  **[RE]**; the register used is the index plus `n`, in registers, for `r` and `sa` **[HW]**
  (*Index registers*).
- **Destination bank 6** writes the index registers: `i.l`, `i.h`, both (`i.lh`), or neither
  (`i.`, value 0).
- **Constants** `c0`-`c63` and **globals** `g0`-`g63` share bank 5 (numbers 64 up are `g`).
