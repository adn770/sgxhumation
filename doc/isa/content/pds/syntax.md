<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Assembler syntax

`pds-asm` reads the syntax `pds-dis` prints, with the rules of the USSE assembler (comments,
labels, lenient blanks and number bases, listings; `tools/usse-asm/README.md`) and two of its
own:

- **The data segment** is the raw words (`.word 0x...`) before the first instruction, in file
  order: dword 0 first, so `ds0[0]`, `ds0[1]`, `ds1[0]`, `ds1[1]`, `ds0[2]`, ... by the row
  layout above. In a listing, a `data` line takes its value from the hex column.
- **Labels** count instructions from the first one after the data segment, as `bra` and `call`
  do, so `(p0) bra not_eot` works without counting data words.

A predicate is written in parentheses before the mnemonic: `(p0) bra done`, `(if0) movs ...`.

<!-- @prog pds_prim.s -->
