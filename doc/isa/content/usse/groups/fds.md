<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Screen-space derivatives: `fdsx`, `fdsy`

Class 4 **[RE]**: two sources in the class-0 layout, the operation in `enc[41]`.

- **Semantics [infer].** The x and y screen-space derivatives of a value across neighbouring
  pixels (GLSL `dFdx`/`dFdy`); how the two sources are used is **[open]**.
- **In the firmware:** not used.

<!-- @example usse fdsx r1, r2, r3 -->

<!-- @group usse class=4 -->
