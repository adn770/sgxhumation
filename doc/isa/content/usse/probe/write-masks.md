<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Write masks

*Not probed yet.* The destination write mask (`.x`, `.y`, `.z`, `.w` and combinations, `enc[47:44]`)
decodes on moves, the bitwise group, the tests and the single-source floats **[RE]**. What a
component is for a 32-bit register (one byte each, or one register of a four-register group), and
what the lone `.` (mask 0) does, are **[open]**: the probes of 2026-10-22 did not cover them. All
our programs and the whole firmware use the default mask (`.x`, printed as nothing). A
`mov r1.xz, r2` over a preset `r1`-`r4`, with distinct bytes in `r2`, would settle it.
