<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Half-float multiply-add: `fmad16`

Class 6 **[RE]**: three sources, each with a swizzle (`.swzll`, `.swzhh`, `.swzf32`) and
`.neg`/`.abs` (source 0 `.abs` only); `enc[44]` is a repeat of two, which brings `.incsXYZ`;
no `.end`.

- **Semantics [HW].** Two half-float multiply-adds at once, `a × b + c` on the low and on the
  high halves: the product rounded to binary32, the sum to a half, both to nearest even (one
  rounding for two halves), half denormals kept. `.swzll`/`.swzhh` feed both lanes one half;
  `.swzf32` feeds the 32-bit register as a binary32 value. `.incsXYZ` as for `fmad`
  (*Half-float multiply-add*).
- **In the firmware:** not used.

<!-- @example usse fmad16 r0, r1, r2, r3 -->

<!-- @group usse class=6 -->
