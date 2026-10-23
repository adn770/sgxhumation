<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `abs`, `not`

Opcodes 13 and 27 **[RE]**: one source, either `ds1[n]` (`w[8]` set) or a first source (`ds0`,
then `ir0`, `ir1`, and for `not` also `tim`).

- **Semantics.** `not` is the bitwise complement **[infer]**, measured on one bit **[HW]**: our
  pixel-event program tests "bit 8 clear" as `not` then `and` then `tstz`, and runs the right
  task. `abs` is the
  absolute value by name **[infer]**.

<!-- @prog pds_pixevent.s match='^\s+not ' -->

<!-- @group pds pat='(abs|not) ' -->
