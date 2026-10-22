<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Shift amounts of 32 and over

*Measured on the hardware (2026-10-22).* By a **register** amount of 32 and over, `shl` and `shr`
give 0, `asr` gives the sign (0 or `0xffffffff`) and `rol` takes the amount modulo 32 **[HW]**:
amounts 32, 33, 64, `0x100` and `0xffffffff` behave so, and `shl.testnat` tests the same result
(probes `shl-reg`, `shr-reg`, `asr-reg`, `rol-reg`, `shl-test-reg`; `design/sgx545_hw_spec.md` §14).

*Round 4 (2026-10-22).* **Immediate amounts of 32 and over act as register ones** (`#32`, `#33`,
`#40`, `#127`), and **`~` complements the 32-bit amount or operand before use**: `shl d, a, ~r2`
with `r2 = 0xfffffffc` shifts by 3, `~#3` is `0xfffffffc` (so `shl`/`shr` give 0 and `rol` turns
by 28), and `and`/`or`/`xor` with `~#3` take the complement as their operand **[HW]** (probe
`shift-immediates`).
