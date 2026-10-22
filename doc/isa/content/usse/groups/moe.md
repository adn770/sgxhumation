<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### MOE control: `smoa`, `smr`, `smlsi`, `smbo`, `imo`, `setfc`, `moest`

Class 31 with `enc[53:52]` = 1, the operation in `enc[58:56]`, no predicate and `.nosched` in
`enc[50]` **[RE]**. `smoa`, `smr`, `smlsi` and `smbo` take either two register operands
(`enc[51]` set) or immediates:

- `smoa #o1, #o2, #o3, #o4, m1, m2, m3, m4`: four signed 10-bit offsets and four address modes
  (`none`, `repeat`, `clamp`, `mirror`);
- `smr #a, #b, #c, #d` and `smbo #a, #b, #c, #d`: four 12-bit values (ranges and base
  offsets);
- `smlsi i1, i2, i3, i4, <modes>, #l1, #l2, #l3`: four 8-bit operands, each a signed increment
  or, in swizzle mode, a four-component swizzle, then three four-bit limits times four;
- `imo #a, #b, #c, #d` (four 12-bit immediates), `setfc #a, #b` (two bits) and
  `moest d, #sel`, which reads a word of the MOE state back into a register: the selector
  names *SMOA*, *SMLSI*, *SMBO* and *SMR* low and high data and an index word.

- **Semantics [infer].** These set the per-operand register stepping that repeats and fetches
  use (*Repeats and the MOE state*): `smoa` an offset and an address mode per operand, `smbo`
  base offsets, `smr` ranges, `smlsi` increments or swizzles. **[HW]** `smlsi`'s four
  increments are the destination's and sources 0, 1 and 2's for a following repeat (*`smlsi`
  increments*); the rest is **[open]**.

**In the firmware [RE].** Only `smlsi`, eight times, at task entry points, always with
increments 0, 0, 0, 1 in increment mode and limits 0.

<!-- @example usse smr #4, #0, #0, #0 -->

<!-- @group usse class=31 pat='^(smoa|smr|smlsi|smbo|imo|setfc|moest)' -->
