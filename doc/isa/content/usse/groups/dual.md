<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Dual issue: two operations in one word

Class 27 **[RE]** packs a **primary** operation on the usual operands and a **secondary**
operation on internal registers into one word, printed `primary + secondary`:

```
fmad r0, r1, r2, r3+fmad i0, i0, i1, i2
mov r0, r1+frcp i0, i1
```

- **Primary** (`enc[51:50]` with `enc[58]`): `fmad`, `fadm`, `fmsa`, `fminmax`, `ima32`,
  `mov`, `fddp` or `fssq`, with `.skipinv`, `.syncs`, `.nosched` and `.repeat2`, a two-bit
  predicate and a destination without the bank extension.
- **Secondary** (`enc[41:38]`): `fmad`, `fmad16`, `fmsa`, `fmaxmin`, `ima32`, `fsubflr`,
  `fadd`, `fsub`, `fmul`, `frcp`, `frsq`, `fexp`, `flog`, `fsin`, `fcos` or `mov`. Its
  destination is `i0`-`i2`; its operands are `i0`-`i2` or, for selection 3, the primary's
  source in the same position. Because of that shared reference, one form exists per
  secondary operand shape and per selection, which is why the class has some sixty forms.
- `enc[54]` reads the sources as half floats, as in class 0.
- **Semantics [infer].** Two independent operations issued together, the secondary on the
  internal registers; whether the secondary sees the primary's result or the old values is
  **[open]**, as is `fssq` (a sum of squares?).
- **In the firmware:** not used.

<!-- @example usse mov r0, r1+frcp i0, i1 -->

<!-- @group usse class=27 -->
