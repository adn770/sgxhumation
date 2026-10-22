<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `.incsXYZ`

*Pending hardware probe (2026-10-22).* The float multiply-add, dot-product, min/max and
derivative groups print `.incsXYZ` (`enc[47]`, `enc[46]`, `enc[40]`) when `enc[53]` is clear,
and `fmad16` with its repeat. Three bits suggest per-operand increment enables for the
repeat **[infer]**; which operand each bit governs, and what `enc[53]` set (no `.incs`)
means, are not pinned. The emulation models `fmad` only without `.incs` or with `.incs000`.
