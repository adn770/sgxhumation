<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer dot products: `u8dot3`, `u8dot4`, `u16dot3`, `u16dot4`

Class 24 **[RE]**: three- and four-component dot products of 8-bit or 16-bit lanes (`enc[56]`
picks dot4, `enc[53:52]` the lane width), `off` variants (`enc[38]`), colour and alpha scales
`Cx1`-`Cx8` and `Ax1`-`Ax8`, complemented sources, and for `u8dot3` an alpha operation
(`aadd`/`asub` with selectors, or `aintrp1`/`aintrp2` with a source 0).

- **Semantics [infer].** By the name, `d = Σ s1[i] × s2[i]` over 3 or 4 lanes, scaled by
  `Cx`. Nothing has run; the lane order, `off`, the scales and the alpha part are **[open]**.
- **In the firmware:** not used.

<!-- @group usse class=24 -->
