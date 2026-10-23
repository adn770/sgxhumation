<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float dot products

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `fdp-count`, `fdp-incs`,
`fdp-rounding`, `fdp-specials`, `fdpc`, `fddp`, `fddpc`; `design/sgx545_hw_spec.md` §14):

- **`fdp.repeatN d, a, b`** sums `N` products `(a+k)·(b+k)` (`N` = 1 without a repeat, up to 4)
  and **writes only register `d + N - 1`**; `d` to `d + N - 2` keep their values. `.incsXYZ`
  chooses which sources advance (*`.incsXYZ`*).
- **Rounding:** each product is rounded to binary32, then they are added in order, each sum
  rounded to nearest even, as a chain of `fmad` would (6 crafted and 42 random cases).
  Denormal inputs and results are flushed, `x - x` is +0, `-0 + -0` is -0, an overflow is `+inf`
  and an invalid operation (`inf × 0`) gives the NaN **`0x7fc00000`**, not the `0x7fffffff` of
  `frsq`.
- **`fdpc d, cpN, a, b`** gives `fdp`'s value and leaves `p0`-`p3` alone.
- **`fddp.repeatN d, iM, s0, s1, s2`** writes `d + N - 1 = s0·s1` and `iM = s0·s2` (`iM` is `i0`
  or `i1`). **`fddpc`** does the same but never advances its sources.

- **`fdpc` writes only its destination** **[HW]**: for every `cp0`-`cp7` and either sign, `d + 1`
  to `d + 3`, `i0`-`i2` and `p0`-`p3` keep their values (probe `fdpc-clip`).

**Undecided:** the rounding of `fdpc`, `fddp` and `fddpc` when a product or a sum is inexact
(measured on `fdp`; their exact results agree with it), the half-float source forms, and where
the clip-plane result of `fdpc`/`fddpc` (`cpN`) goes. Presumably to the
clipper with a vertex's outputs **[infer]**; a vertex program that emits after `fdpc`, read
back by the host, would settle it.
