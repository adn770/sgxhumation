<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Shift amounts of 32 and over

*Measured on the hardware (2026-10-22).* By a **register** amount of 32 and over, `shl` and `shr`
give 0, `asr` gives the sign (0 or `0xffffffff`) and `rol` takes the amount modulo 32 **[HW]**:
amounts 32, 33, 64, `0x100` and `0xffffffff` behave so, and `shl.testnat` tests the same result
(probes `shl-reg`, `shr-reg`, `asr-reg`, `rol-reg`, `shl-test-reg`; `design/sgx545_hw_spec.md` §14).

**Undecided:** immediate amounts of 32 and over (the rotated immediate can encode them) and `~` on a
shift amount; the same probes with `#imm` and `~r` sources would settle them.
