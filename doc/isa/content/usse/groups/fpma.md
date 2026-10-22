<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Fixed-point multiply-add: `fpma`

Class 25 **[RE]**: destination and three sources in the c10 reading, then colour selections
(`s0`/`s1`/`s0a`/`s1a`, `s1`/`s1a`, `s2`/`s2a`) and alpha selections, each with a `.comp`
complement; the flags carry a three-bit repeat and `.sat`.

- **Semantics [HW].** A colour multiply-add, `c0 + c1 × c2` per channel with the first
  selection the addend, on 8-bit channels or, with `.c10`, signed 10-bit ones (*`fpma`*);
  `.sat` is **[open]**.
- **In the firmware:** not used.

<!-- @group usse class=25 -->
