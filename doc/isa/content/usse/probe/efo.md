<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `efo`

*Measured on the hardware (2026-10-22).* **`efo` does what its text prints** **[HW]**: `m0` and
`m1` multiply the named sources (`src0`-`src2`, each with its `.neg`, or `i0`/`i1`); `a0` and
`a1` add (or subtract) the named values; the destination takes `a0`, `a1`, or the **old**
`i0`/`i1`; `i0` and `i1` are written after, unless marked `!`. Every product and every sum is
rounded to binary32 (24 random cases); a repeat's second iteration sees the first one's `i0`
and `i1` (probes `efo-dataflow`, `efo-rounding`; `design/sgx545_hw_spec.md` §14).

**`.gpiext`** sends the writes to `i2` (the `i1` write is lost) and, for the instance that
stores our dump, makes the multipliers read `i2` and `i0` for `i0` and `i1`, while the adders
and the destination keep `i0`/`i1` (probe `efo-gpiext`). But when the multipliers or the
destination read `i0`/`i1`, **one pixel in four (512 of 2048) gets another value, every
run**: `d = i0` gives 11 in 1536 pixels and the 56 that `.gpiext` wrote in 512, and
`src1·i0 + src2·i1` gives 162 and, in 512 pixels, `1944 = 12 × 162` (probe
`efo-gpiext-instances`, recorded). `.gpiext` therefore reaches registers that other instances
write: `i2`/`i3` look like another instance's `i0`/`i1`, or registers shared by four.

**Undecided:** repeats beyond two, and which instance's registers `.gpiext` reaches; a probe
that writes a different `i0` per pixel (from the pixel's coordinates) and reads it back after
an `efo.gpiext` would settle it.
