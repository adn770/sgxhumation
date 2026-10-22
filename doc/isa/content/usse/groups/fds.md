<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Screen-space derivatives: `fdsx`, `fdsy`

Class 4 **[RE]**: two sources in the class-0 layout, the operation in `enc[41]`.

- **Semantics [HW].** Coarse derivatives over the 2x2 quad (GLSL `dFdx`/`dFdy`): `fdsx d, a, b`
  = `a`(top-right) − `b`(top-left), `fdsy d, a, b` = `a`(bottom-left) − `b`(top-left), the
  same value in all four pixels (*Derivatives*).
- **In the firmware:** not used.

<!-- @example usse fdsx r1, r2, r3 -->

<!-- @group usse class=4 -->
