<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float dot products: `fdp`, `fdpc`, `fddp`, `fddpc`

Class 2 **[RE]**: `enc[42:41]` picks `fdp` (with `enc[14]` set, `fdpc` and a clip-plane number
`cp0`-`cp7`), `fddp` and `fddpc`. The `fddp` forms write an internal register `i0`/`i1`
besides the destination and so lose the destination's bank extension bit; `fddpc` takes two
`cp` operands and has no `.incs`. Half-float reading with `enc[54]`.

- **`fdp.repeatN d, a, b`** sums the `N` products `(a+k)·(b+k)` and writes the sum to
  register `d + N - 1` only **[HW]**: `fdp.repeat3` is a three-component dot product. Each
  product is rounded, then the sums in order, to nearest even, as chained `fmad`s would; denormals
  are flushed and an invalid result is the NaN `0x7fc00000` **[HW]** (*Float dot products*).
- **`.incsXYZ`**: X, Y and Z let source 0, 1 and 2 advance **[HW]** (*`.incsXYZ`*).
- **`fddp d, iM, s0, s1, s2`** writes `d + N - 1 = s0·s1` and `iM = s0·s2` **[HW]**; `fddpc`
  the same with its sources held still **[HW]**.
- **`fdpc`, `fddpc`**: the value is `fdp`'s and `fddp`'s **[HW]**; where the clip-plane test
  (`cpN`) lands is **[open]**: not in `p0`-`p3` **[HW]**.
- **In the firmware:** not used.

<!-- @example usse fdp r0, r4, r8 -->

<!-- @group usse class=2 -->
