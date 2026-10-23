<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Repeat order and overlapping ranges

*Measured on the hardware (2026-10-22).* With the default MOE state a `.repeatN` steps every operand
by one register per iteration, and **the iterations run one after the other, in ascending order**
**[HW]**: `mov.repeat2 r2, r1` leaves `r3 = r1` (overlapping ranges propagate), and
`mov.repeat3 r1, r2` gives `r1 = r2`, `r2 = r3`, `r3 = r4` (probe `repeat-order`;
`design/sgx545_hw_spec.md` §14). A repeat of `mov` behaves like N instructions in sequence;
other forms are taken to as well **[infer]**, as their own probes confirm for `fmad`, the dot
products and the colour group (*`.incsXYZ`*, *Colour operations*).
