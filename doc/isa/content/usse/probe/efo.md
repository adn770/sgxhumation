<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `efo`

*Measured on the hardware (2026-10-22).* **`efo` does what its text prints** **[HW]**: `m0` and
`m1` multiply the named sources (`src0`-`src2`, each with its `.neg`, or `i0`/`i1`); `a0` and
`a1` add (or subtract) the named values; the destination takes `a0`, `a1`, or the **old**
`i0`/`i1`; `i0` and `i1` are written after, unless marked `!`. Every product and every sum is
rounded to binary32 (24 random cases); a repeat's second iteration sees the first one's `i0`
and `i1` (probes `efo-dataflow`, `efo-rounding`; `design/sgx545_hw_spec.md` §14).

**`.gpiext`**, with a different `i0`-`i2` in every pixel (round 3, probe `efo-gpiext-quad`)
**[HW]**:

- the multipliers read the instance's own **`i2` for `i0` and its own `i0` for `i1`**; the
  adders and the destination read its own `i0`/`i1`;
- a write of `i0` goes to the instance's own **`i2`**;
- the four instances' writes of `i1` all end in **one register, the top-right pixel's `i0`**,
  which holds the bottom-right pixel's value afterwards; the other pixels' `i0` and `i1` are
  left alone. Through `i3` (the next instance's `i0`) the top-left pixel then reads it too.

While an instruction writes `i1`, the top-right pixel's reads of `i0` (destination, multiplier
or adder) give the **top-left pixel's `i0`**, and now and then (about 1 read in 3000) the value
the top-left pixel is writing: the result depends on timing (probe `efo-gpiext-race`,
recorded). Every effect stays within the pixel's own 2x2 quad, which belongs to one task:
300 jobs of two clients submitted concurrently never saw the other client's values within a
task. Round 2's "one pixel in four" is the top-right pixel of every quad.

**Undecided:** repeats beyond two, and whether the four `i1` writes go to the top-right `i0`
in order (the bottom-right one last) or only the bottom-right one is kept.
