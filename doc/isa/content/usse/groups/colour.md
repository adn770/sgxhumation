<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Colour operations: `sop2`, `sop3`, `lrp1`, `lrp2`, `sop2wm`

Classes 16-18 combine 8-bit colour channels, a colour part and an alpha part in one
instruction, with selectors naming what feeds each multiplier (`zero`, `s1`, `s2a`, `asat`,
`s2scale`, ...), `.comp` complements and an operation (`add`, `sub`, `min`, `max`) **[RE]**:

- `sop2 d, s1, s2, <colour>+asop2 <alpha>`: two sources;
- `sop3 d, s0, s1, s2, <colour>+asop|arsop|alrp <alpha>`: three sources;
- `lrp1`, `lrp2`: interpolations between sources (`lrp` = linear interpolation **[infer]**);
- `sop2wm`: `sop2` with a destination byte mask and separate colour and alpha operations.

Operands use the 6-bit c10 reading (`.c10` after the register, *Source modifiers*).

- **Semantics [infer].** These look like the fixed-function blend and texture-combine
  operations of a GLES 1 era pipeline (`d = op(s1 × sel1, s2 × sel2)` per channel, `a` the
  alpha channel). Nothing has run; operand roles, saturation and rounding are **[open]**.
- **In the firmware:** not used; nor in the closed GLES2 programs we captured.

<!-- @example usse sop2 r0, r1, r2, s1, s2, add+asop2 s1a, s2a, add -->

<!-- @group usse class=16-18 -->
