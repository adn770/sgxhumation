<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `.incsXYZ`

*Measured on the hardware (2026-10-22).* The float multiply-add, dot-product, min/max and
derivative groups print `.incsXYZ` (`enc[47]`, `enc[46]`, `enc[40]`) when `enc[53]` is clear
**[RE]**. **Each bit lets one source advance by one register per iteration: X source 0, Y source
1, Z source 2; the destination advances whatever the bits say, and without `.incs` (`enc[53]`
set) every source advances** **[HW]**. `fmad.repeat2.incs001 d, a, b, c` computes `d = a·b + c`
and `d+1 = a·b + (c+1)`; `.incs000` repeats the first operation into `d+1` (probes
`incs-fmad`, `fdp-incs`, `fddp`, `fminmax-incs`; `design/sgx545_hw_spec.md` §14). `fdp` has no
source 0, so its X bit does nothing, and `fddpc`, which has no `.incs`, never advances its
sources (probe `fddpc`).

**Undecided:** `fmad16` and the derivatives, presumably the same **[infer]**; a probe of each
would settle it.
