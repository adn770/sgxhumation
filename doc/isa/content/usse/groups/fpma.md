<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Fixed-point multiply-add: `fpma`

Class 25 **[RE]**: destination and three sources in the c10 reading, then colour selections
(`s0`/`s1`/`s0a`/`s1a`, `s1`/`s1a`, `s2`/`s2a`) and alpha selections, each with a `.comp`
complement; the flags carry a three-bit repeat and `.sat`.

- **Semantics [infer].** A colour multiply-add on 10-bit fixed-point channels, `s0 × s1 + s2`
  with the selections choosing colour or alpha channels; **[open]**.
- **In the firmware:** not used.

<!-- @group usse class=25 -->
