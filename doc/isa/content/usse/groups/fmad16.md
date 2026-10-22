<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Half-float multiply-add: `fmad16`

Class 6 **[RE]**: three sources, each with a swizzle (`.swzll`, `.swzhh`, `.swzf32`) and
`.neg`/`.abs` (source 0 `.abs` only); `enc[44]` is a repeat of two, which brings `.incsXYZ`;
no `.end`.

- **Semantics [infer].** Two 16-bit multiply-adds at once on the halves of 32-bit registers,
  the swizzles choosing the halves (or a 32-bit float converted, `.swzf32`). **[open]**.
- **In the firmware:** not used.

<!-- @example usse fmad16 r0, r1, r2, r3 -->

<!-- @group usse class=6 -->
