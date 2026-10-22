<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Video: `adif`, `adifsum`, `bilin`, `firv`, `firh`

Classes 22 and 23 **[RE]**. Class 22 picks by `enc[53:52]`: `adif` (or `adifsum` with
`enc[38]`), `bilin`, `firv`; class 23 is `firh`. All take the multiply flags and a two-bit
predicate, and work on 8-bit data (`u8`, `s8`, `o8`).

- **Semantics.** `adif` is the per-byte absolute difference and `adifsum` the sum of the four
  plus a 16-bit half of `s0` **[HW]** (*Video*). By the names, `bilin` is a bilinear
  interpolation (`planar` or `interleaved` data, source and destination pairs) and `firv`/
  `firh` vertical and horizontal FIR filters (`firh` with an edge mode `rep`/`msc`/`mdc`, a
  coefficient set `fcsN` and two small numbers) **[infer]**; they ran without a fault but
  their results fit no model yet **[open]**.
- **In the firmware:** not used.

<!-- @example usse adif r1, r2, r3 -->

<!-- @group usse class=22,23 -->
