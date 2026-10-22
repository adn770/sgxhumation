<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float min/max: `fminmax`, `fmaxmin`

Class 3 **[RE]**: the class-0 layout with three sources, the operation in `enc[41]` (`enc[42]`
does not decode).

- **Semantics [infer].** By the names, a clamp: `min(max(a, b), c)` and `max(min(a, b), c)`,
  or with the roles in another order. **[open]**.
- **In the firmware:** not used.

<!-- @example usse fminmax r0, r1, r2, r3 -->

<!-- @group usse class=3 -->
