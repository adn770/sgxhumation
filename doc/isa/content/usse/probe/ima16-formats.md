<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `ima16` lanes and source formats

*Measured on the hardware (2026-10-22).* **`ima16 ..., u16, u16` computes `a × b + c`** (`a × b - c`
with `.neg` on source 2) **in each 16-bit lane, wrapping** **[HW]**: `0x00020005 × #3 - 0x00010007`
is `0x00050008` and `0x0000ffff × #3` is `0x0000fffd`; an immediate `b` or `c` is in both lanes, a
register one is taken lane by lane (probes `ima16`, `ima16-plain`, `ima16-imm-c`, `ima16-reg-b`;
`design/sgx545_hw_spec.md` §14). This is the firmware's `u16, u16` use.

**Undecided:** the 8-bit source formats `u8`, `s8`, `o8` (`enc[39:38]`, `enc[41:40]`) with their
`.low`/`.high` select, saturation, `.rsN` and `.abs` on the destination; the same probe template
with those formats and lane values that tell the bytes apart would settle them.
