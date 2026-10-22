<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `rlp`

Class 14 **[RE]**: `rlp d, s1, pN` with the bitwise group's flags, a write mask or repeat
count, one source and a seven-bit predicate number in `enc[20:14]`; the reference
disassembler reads no source 2.

- **Semantics [open].** The name and operands suggest a move or rotate through predicates
  ("rotate left through predicate"?), but nothing confirms it. The predicate field is wider
  than the four predicates the rest of the instruction set names.
- **In the firmware:** not used.

<!-- @example usse rlp r1, r2, p0 -->

<!-- @group usse class=14 -->
