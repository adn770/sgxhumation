<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Index registers

*Measured on the hardware (2026-10-22).* `mov i.l, src` loads the index register and **`r[il + #n]`
is the temporary `i.l + n`, counted from `r0`** in units of one register, as a source and as a
destination **[HW]**; `sa[il + #n]` is the same on the secondaries, counted from `sa0`. **Bits 10
and up of `i.l` are ignored**: `0x401`, `0x40001` and `0xfffffc02` index `r1`, `r1` and `r2` (probes
`index-unit`, `index-width`, `index-width2`, `index-width3`, `index-store`, `index-sa-unit`,
`index-sa-store`, `sa-values`; `design/sgx545_hw_spec.md` §14). An index past the task's temporaries
reaches the next instances' registers **[infer]**: the share of pixels reading other values grows
with it. The firmware's `0x00040000 | index` (bit 18) **[RE]** is the stride of a register
**offset** of a load or store (*Register offsets of loads and stores*), not a mode of an index
register.

*Round 4 (2026-10-22).* **`i.h` and `r[ih + #n]` work as `i.l` does** (one register from `r0`,
bits 10 and up ignored), and **`mov i.lh, s` loads `i.l` from the low and `i.h` from the high 16
bits of `s`** **[HW]** (round 4, probe `index-high`).

**Undecided:** the `o` and `pa` banks through an index, and indexed operands of anything but
`mov`; probes like `index-unit` with `o`/`pa` and an ALU instruction would settle them.
