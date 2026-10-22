<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Write masks

*Pending hardware probe (2026-10-22).* The destination write mask (`.x`, `.y`, `.z`, `.w` and
combinations, `enc[47:44]`) decodes on moves, the bitwise group, the tests and the
single-source floats **[RE]**. What a component is for a 32-bit register (one byte each,
or one register of a four-register group), and what the lone `.` (mask 0) does, are not
pinned. All our programs and the whole firmware use the default mask (`.x`, printed as
nothing).
