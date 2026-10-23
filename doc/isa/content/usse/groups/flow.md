<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Flow control: `ba`, `br`, `lapc`, `bexception`

Class 31 with `enc[54:52]` = 0 and the operation in `enc[40:38]` **[RE]**: 0 `ba`, 1 `br`,
2 `lapc`, 3 and 4 the `pclink` moves (*Moves*), 5 `nop` (*Synchronisation and misc*);
`bexception` sits next to them. The branch flags are `.nosched`, `.synce`/`.syncent`,
`.savelink` and `.mon`; `ba` and `br` take the full predicate.

- **`ba #n`** jumps to instruction `n`; **`br #k`** by a signed offset from the `br` itself
  **[HW]**; `.savelink` makes either a call; **`lapc`** returns to `pclink` **[RE/HW]**. See
  *Program counter, calls and the end of a program* for the evidence.
- **`.synce`, `.syncent`, `.mon`, `bexception`** are **[open]**.

**In the firmware [RE].** 954 `ba` (591 predicated, 232 calls), 12 `br`, 74 `lapc`. The kernel
CCB dispatch jumps to its handler by moving the handler's address into `pclink` and running
`lapc`.

<!-- @example usse ba.savelink #0x00000100 -->

<!-- @example usse p0 ba #0x00000010 -->

<!-- @example usse br -#0x00000003 -->

<!-- @group usse class=31 pat='ba\{|br\{|bexception|lapc' -->
