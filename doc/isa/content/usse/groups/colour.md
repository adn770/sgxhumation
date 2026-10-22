<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Colour operations: `sop2`, `sop3`, `lrp1`, `lrp2`, `sop2wm`

Classes 16-18 combine 8-bit colour channels, a colour part and an alpha part in one
instruction, with selectors naming what feeds each multiplier (`zero`, `s1`, `s2a`, `asat`,
`s2scale`, ...), `.comp` complements and an operation (`add`, `sub`, `min`, `max`) **[RE]**:

- `sop2 d, s1, s2, <colour>+asop2 <alpha>`: two sources;
- `sop3 d, s0, s1, s2, <colour>+asop|arsop|alrp <alpha>`: three sources;
- `lrp1`, `lrp2`: interpolations between sources;
- `sop2wm`: `sop2` with a destination byte mask and separate colour and alpha operations.

Operands use the 6-bit c10 reading (`.c10` after the register, *Source modifiers*).

- **Semantics [HW].** They are the fixed-function blend and texture-combine operations of a
  GLES 1 era pipeline: per colour channel (bytes 0-2) `op(s1 × f1, s2 × f2)`, per alpha (byte
  3) the same with the alpha selectors, the products summed exactly, divided by 255 once,
  rounded and saturated; `min`/`max` compare the sources themselves; `asat` is
  `min(s1a, 1 - s2a)`, `s2scale` doubles the result and `zeros` makes `s2` signed. `lrp1` is a
  lerp by `s0` or `s0a`, `lrp2` a sum of two selected products, `alrp` a lerp by `s0a`.
  `.c10` operands are signed 10-bit lanes with 1.0 = 255, clamped to ±510 (*Colour
  operations*). Repeats are **[open]**.
- **In the firmware:** not used; nor in the closed GLES2 programs we captured.

<!-- @example usse sop2 r0, r1, r2, s1, s2, add+asop2 s1a, s2a, add -->

<!-- @group usse class=16-18 -->
