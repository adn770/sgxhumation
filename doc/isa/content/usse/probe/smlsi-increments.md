<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `smlsi` increments

*Pending hardware probe (2026-10-22).* `smlsi` sets four 8-bit values (signed increments, or
swizzles in swizzle mode) and three limits **[RE]**. Which operand (destination, sources 0-2)
each position governs, how an increment applies per repeat iteration, what the limits do,
and how long the state lasts (per task?) are not pinned. The firmware sets
`0, 0, 0, 1` in increment mode at every task entry, which would read naturally as "the last
position steps by one, the others stay": the probe should say which operand that is.
