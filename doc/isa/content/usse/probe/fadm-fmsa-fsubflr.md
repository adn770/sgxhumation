<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `fadm`, `fmsa`, `fsubflr`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fadm-fmsa-fsubflr`,
`fadm-fmsa-fsubflr-specials`; `design/sgx545_hw_spec.md` §14):

- **`fadm d, a, b, c` = `a·c + b·c`**, each product and then the sum rounded to binary32 (not
  `(a + b) × c`: 24 random cases fit only this order), like a two-term `fdp`;
- **`fmsa d, a, b, c` = `a·a + b·c`**, rounded the same way;
- **`fsubflr d, a, b` = `a - floor(b)`**, one rounding.

Denormal inputs are flushed; an invalid result (`inf × 0`, `inf - inf`, a NaN input) is the
NaN `0x7fc00000`, as for the dot products.

**Undecided:** the half-float source forms (`enc[54]`), as for `fmad`.
