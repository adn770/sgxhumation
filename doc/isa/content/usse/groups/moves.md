<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Moves: `mov`, the 32-bit immediate and `pclink`

- **`mov dst, src`** (class 5, type `enc[42:40]` = 0) copies a source-1 operand to the
  destination, under a write mask or repeated **[HW]**: our fill shader, copy shader and
  state copies are built from it. A source immediate prints in hex.
- **`mov dst, #imm32`** (class 31) loads a full 32-bit immediate **[HW]**; the driver patches
  colours and PBE words into it.
- **`mov pclink, src`** and **`mov dst, pclink`** save and restore the link register
  **[RE]** (*Program counter, calls and the end of a program*). Two encodings exist for each:
  a plain one with a temporary, and a general one with a predicate, `.nosched` and any
  operand.

**In the firmware [RE].** `mov` is its most common instruction (1497): 626 immediates into
temporaries (constants, addresses, and the nine address-patch sites the loader fills), moves
between temporaries and the `sa`/`pa` banks, 79 into `i.l`, 79 through indexed `sa`
operands, and the `pclink` traffic of its calls.

Examples, from our programs and assembled fresh:

<!-- @prog use_copy.s match='mov' -->

<!-- @example usse mov.skipinv.repeat4 o0, pa0 -->

<!-- @group usse class=5,31 pat='^(\{0\})?mov(\{| )' -->
