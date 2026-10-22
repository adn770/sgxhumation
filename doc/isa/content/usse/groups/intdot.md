<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer dot products: `u8dot3`, `u8dot4`, `u16dot3`, `u16dot4`

Class 24 **[RE]**: three- and four-component dot products (`enc[56]` picks dot4, `enc[53:52]`
the result width), `off` variants (`enc[38]`), colour and alpha scales `Cx1`-`Cx8` and
`Ax1`-`Ax8`, complemented sources, and for `u8dot3` an alpha operation (`aadd`/`asub` with
selectors, or `aintrp1`/`aintrp2` with a source 0).

- **Semantics [HW].** `S = Σ s1[i] × s2[i]` over bytes 0-2 (dot3) or 0-3 (dot4), or with `off`
  over `(s1[i] - 128)(s2[i] - 128)`. `u8dot` writes `S × Cx / 255` to every colour byte and
  `S × Ax / 255` (or the alpha operation) to byte 3, rounded, saturated to 0..255; `u16dot`
  writes `S × Cx × 32767 / 255²` to both halves, saturated to `0x7fff` (±`0x7fff` with `off`).
  The alpha operations add, subtract or interpolate the source alphas; a repeat advances
  the destination and both sources (*Integer dot products*).
- **In the firmware:** not used.

<!-- @group usse class=24 -->
