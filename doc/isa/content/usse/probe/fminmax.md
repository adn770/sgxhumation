<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float min/max

*Measured on the hardware (2026-10-22).* **`fminmax d, a, b, c` is `min(max(a, b), c)` and
`fmaxmin d, a, b, c` is `max(min(a, c), b)`** **[HW]**: both clamp `a` to `[b, c]` when
`b <= c`; when `b > c`, `fminmax` gives `c` and `fmaxmin` gives `b` (six orders of 1, 2, 3 and
ten random triples; probes `fminmax-roles`, `fmaxmin-roles`). **min and max are IEEE minNum
and maxNum**: a NaN operand loses to a number, `-0` is below `+0`, infinities order as numbers
and denormals are flushed to a zero of their sign (22 special triples, probes
`fminmax-specials`, `fmaxmin-specials`; `design/sgx545_hw_spec.md` §14). `.incsXYZ` as for
`fmad` (probe `fminmax-incs`).

When both operands of a step are NaN, the step returns its **second operand**, payload and
signalling bit kept (`fminmax` of three NaNs gives `c`, `fmaxmin` gives `b`; probe
`minmax-allnan`, 2026-10-22).

**Undecided:** the half-float source forms (`enc[54]`), as for `fmad`.
