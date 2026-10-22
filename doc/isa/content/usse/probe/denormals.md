<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Denormals

*Measured on the hardware (2026-10-22).* **`fmad` flushes denormal inputs, product and result to a
zero of their sign** **[HW]**: `2^-70 × 2^-70` is 0, `-2^-70 × 2^-70 + -0` is -0, `2^-140 × 2^100`
is 0 (probes `fmad-denormal`, `fmad-zero-sign`; `design/sgx545_hw_spec.md` §14). The sources of
`frcp`, `frsq` and `fsqrt` are flushed too (`frcp` of a denormal is `+inf`). No denormal result was
seen.

The dot products and the min/max flush their inputs and results the same way **[HW]** (*Float dot
products*, *Float min/max*); pack and unpack do not: `pckf16f32` keeps a denormal half and
`unpckf32f16` converts one exactly **[HW]** (*Pack and unpack conversions*).

`fadm`, `fmsa`, `fsubflr` flush their inputs too, `flog` of a denormal is `-inf` and `fexp`
flushes results below 2^-126 **[HW]**, while **`fmad16` keeps half denormals** (*`fadm`, `fmsa`,
`fsubflr`*, *`flog`, `fexp`, `fsin`, `fcos`*, *Half-float multiply-add*).

**Undecided:** `efo` (presumably flushing as `fmad` **[infer]**).
