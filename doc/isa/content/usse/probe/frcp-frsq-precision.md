<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `frcp`, `frsq` and `fsqrt` precision

*Measured on the hardware (2026-10-22).* **`frcp`, `frsq` and `fsqrt` are within one ulp but not
always correctly rounded** **[HW]**: of 48 random normal inputs 46, 42 and 44 are exact and the
others one ulp off (at most 0.65 ulp from the true value); `1/3`, `1/5`, `1/√2` and `√3` are exact.
Specials: `frcp(0) = +inf`, `frcp(-0) = -inf`, `frcp` of a denormal `+inf` (its source is flushed),
`frsq(0) = +inf`, `fsqrt(0) = 0`, `frsq` and `fsqrt` of -1 the NaN `0x7fffffff` (probes `frcp`,
`frsq`, `fsqrt`, `sweep-frcp`, `sweep-frsq`, `sweep-fsqrt`; `design/sgx545_hw_spec.md` §14). The
radare2 ESIL rounds them correctly, so it can differ by one ulp, and does not model the specials.

`flog`, `fexp`, `fsin` and `fcos` are measured in *`flog`, `fexp`, `fsin`, `fcos`*.

**Undecided:** `frcp`, `frsq`, `fsqrt` of infinite inputs.
