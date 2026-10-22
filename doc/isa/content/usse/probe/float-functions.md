<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `flog`, `fexp`, `fsin`, `fcos`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fsingle-base`, `fsingle-specials`,
`sweep-flog`, `sweep-fexp`, `sweep-fsin`, `sweep-fcos`; `design/sgx545_hw_spec.md` §14):

- **`flog` is `log2`**: exact on powers of two; within one ulp elsewhere, or within 2^-22
  near 1 (`log2(1 + 2^-23)` gives 2^-23); 0 and denormals give `-inf`, `+inf` gives `+inf`,
  negatives and NaN the NaN `0x7fffffff`.
- **`fexp` is `2^x`**: exact on integers, within two ulps elsewhere (78 of 95 exploration
  cases within one); results below 2^-126 are 0, from 128 up `+inf`; `-inf` gives 0.
- **`fsin`, `fcos` take the argument in quarter turns**: `sin(πx/2)`, `cos(πx/2)`, period 4,
  exact at multiples of 1/2 (`fcos(1)` is `-0`); the result is a fixed-point value with 15
  fraction bits (absolute error up to about 2^-15.5, so tiny results are 0 or 2^-15); an
  infinite or NaN argument gives `0x7fffffff`.

**Undecided:** `.flt16` and `.c10` sources, write masks and repeats on these, and `.tpres`.
