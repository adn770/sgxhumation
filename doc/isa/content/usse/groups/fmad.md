<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float multiply-add: `fmad`, `fadm`, `fmsa`, `fsubflr`

Class 0 **[RE]**. `enc[42:41]` picks `fmad`, `fadm`, `fmsa` (three sources) or `fsubflr` (two);
`enc[54]` reads the sources as half floats (*Source modifiers*; what it computes is **[open]**); the
float flags, a two-bit repeat (`.repeat2`-`.repeat4`) and, when `enc[53]` is clear, `.incsXYZ`.
Sources carry `.neg`/`.abs` (source 0 `.abs` only).

- **`fmad d, a, b, c`** computes `a × b + c` in IEEE binary32 **[infer]**: the firmware's six
  uses fit, and our emulation models it so. **[HW]** It rounds to nearest even, rounds the
  product before adding (not fused) and flushes denormals to a zero of their sign (*Float
  rounding and NaN*, *Denormals*). `fmad` with no source modifiers is also the usual way to
  write a float move or add: `a × 1 + -0` gives `a` for normals, infinities and zeros
  **[HW]**, flushing denormals and turning NaNs into `0x7fc00000` (*Denormals*).
- **`fadm d, a, b, c` = `a·c + b·c`**, **`fmsa d, a, b, c` = `a·a + b·c`** (products then
  the sum rounded) and **`fsubflr d, a, b` = `a - floor(b)`** **[HW]** (*`fadm`, `fmsa`,
  `fsubflr`*).
- **`.incsXYZ`**: X, Y and Z let `a`, `b` and `c` advance in a repeat; the destination always
  advances, and without `.incs` everything does **[HW]** (*`.incsXYZ`*).
- **The SGX540** reads these classes differently (a write mask, source-0 negation, no
  `.incs`) **[RE]**; the decoder only decodes them for the SGX545.

<!-- @example usse fmad r0, r1, r2, r3 -->

<!-- @example usse fmad.repeat2.incs011 r0, r2, r4, r6 -->

<!-- @group usse class=0 -->
