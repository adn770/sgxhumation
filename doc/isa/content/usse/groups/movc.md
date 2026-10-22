<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Conditional and masked moves: `movc`, `movmsk`

Class 5 with a type `enc[42:40]` other than 0. `movc` (types 1-5: `.i8`, `.i16`, `.i32`,
`.flt`, `.i10`) takes three sources and a test (`.tz`, `.tnz`, `.tn`, `.tp|z`); `movmsk`
(types 6 and 7, the second adding `.smsk` and a source 0) takes a mask format (`.mask`,
`.flt16`, `.flt`, `.u8`) and a component select on its last source **[RE]**.

- **Semantics [infer].** By the names, `movc` moves source 1 or source 2 depending on a test
  of source 0 in the named data type (a select, `d = test(s0) ? s1 : s2`), and `movmsk` moves
  under a mask taken from a source. Neither has been run; the operand roles are **[open]**.
- **In the firmware:** not used. Neither do our programs use them.

<!-- @example usse movc.i32.tz r1, r2, r3, r4 -->

<!-- @group usse class=5 pat='movc|movmsk' -->
