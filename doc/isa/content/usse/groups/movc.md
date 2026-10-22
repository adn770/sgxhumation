<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Conditional and masked moves: `movc`, `movmsk`

Class 5 with a type `enc[42:40]` other than 0. `movc` (types 1-5: `.i8`, `.i16`, `.i32`,
`.flt`, `.i10`) takes three sources and a test (`.tz`, `.tnz`, `.tn`, `.tp|z`); `movmsk`
(types 6 and 7, the second adding `.smsk` and a source 0) takes a mask format (`.mask`,
`.flt16`, `.flt`, `.u8`) and a component select on its last source **[RE]**.

- **`movc`** is a select per lane: `d = test(s0) ? s1 : s2` in each lane of the type (`.i32`
  one, `.i16` two, `.i8` four, `.i10` three 10-bit lanes with bits 30-31 cleared, `.flt` the
  float with denormals as zero and a NaN failing all but `.tnz`) **[HW]**.
- **`movmsk.<fmt> d, s1, s2.c`** writes `s1` into `d` when `s2` passes: `.mask` bit 0 of byte
  `c`, `.u8` bit 7 of byte `c`, `.flt` `s2 >= 0.5`, `.flt16` half `c >= 0.5` **[HW]**. With
  `.smsk` the move also needs bit 0 of source 0, except for `.mask` **[HW]** (*Conditional and
  masked moves*).
- **Write masks** pick registers as for `mov` **[HW]** (*Write masks*).
- **In the firmware:** not used. Neither do our programs use them.

<!-- @example usse movc.i32.tz r1, r2, r3, r4 -->

<!-- @group usse class=5 pat='movc|movmsk' -->
