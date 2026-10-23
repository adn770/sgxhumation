<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `flog`, `fexp`, `fsin`, `fcos`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fsingle-base`, `fsingle-specials`,
`sweep-flog`, `sweep-fexp`, `sweep-fsin`, `sweep-fcos`; `design/sgx545_hw_spec.md` §14):

- **`flog` is `log2`**: exact on powers of two; within one ulp elsewhere, or within 2^-22
  near 1 (`log2(1 + 2^-23)` gives 2^-23); 0, `-0` and denormals give `-inf`, `+inf` gives
  `+inf`, negatives and NaN the NaN `0x7fffffff`.
- **`fexp` is `2^x`**: exact on integers, within two ulps elsewhere (78 of 95 exploration
  cases within one); results below 2^-126 are 0, from 128 up `+inf`; `-inf` gives 0, a NaN
  `0x7fffffff`.
- **`fsin`, `fcos` take the argument in quarter turns**: `sin(πx/2)`, `cos(πx/2)`, period 4,
  exact on integers; the result is a fixed-point value with 15 fraction bits (absolute error up
  to about 2^-15.5, so tiny results are 0 or 2^-15); an infinite or NaN argument gives
  `0x7fffffff`. Odd multiples of 1/2 are not exact: `fcos(0.5)` is 23171/2^15, the nearest
  being 23170/2^15 (probe `x-fcos`).
- **The zeros at integers have a sign** (probes `x-fsin`, `x-fcos`, `fsingle-specials`):
  `fsin` is odd and `fcos` even, and in quarter turns `r = |x| mod 4` `fsin` gives `+0` at
  `r = 0` and `-0` at `r = 2`, `fcos` `-0` at `r = 1` and `+0` at `r = 3`: `fsin(-0)` is `-0`,
  `fsin(2)` `-0`, `fsin(4)`, `fsin(100)` and `fsin(10^6)` `+0`, `fcos(1)` and `fcos(-1)` `-0`,
  `fcos(3)` `+0`. A denormal argument is flushed (`fcos` of one is 1).

*Simulator check, 2026-10-23.* The simulator (`crates/sgx-sim`, `func.rs`)
gives these functions and `frcp`, `frsq`, `fsqrt` exactly where the hardware is exact and
otherwise, as a *near* value, the nearest one with the bound above; the vector tests accept a
near value within its bound and stop on anything computed from one. All the recorded vectors
of the seven functions fit, write masks included (as *Write masks*). The `isa2/f1` runs of the
four sweeps read back other inputs than the rebuilt table's (the inputs they store differ).

**Undecided:** `.flt16` and `.c10` sources, repeats on these, and the sign of a tiny
result rounded to zero.
