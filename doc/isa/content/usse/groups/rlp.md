<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `rlp`

Class 14 **[RE]**: `rlp d, s1, pN` with the bitwise group's flags, a write mask or repeat
count, one source and a seven-bit predicate number in `enc[20:14]`; the reference
disassembler reads no source 2.

- **Semantics [HW].** Rotate left through the predicate: `d = s << 1 | pN`, then `pN` takes
  bit 31 of `s`. Predicate numbers 4 and up alias `p0`-`p3` (number mod 4) (*`rlp`*).
- **In the firmware:** not used.

<!-- @example usse rlp r1, r2, p0 -->

<!-- @group usse class=14 -->
