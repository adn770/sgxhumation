<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `ima16` source formats

*Pending hardware probe (2026-10-22).* `ima16` reads its sources 1 and 2 as `u16`, `u8`, `s8`
or `o8` (`enc[39:38]`, `enc[41:40]`), an 8-bit source with a `.low`/`.high` select, and has
`.u`/`.s`, saturation, `.rsN`, `.abs` on the destination and `.neg` on source 2. What each
format reads from the register, how the result is formed and what the destination receives
are not pinned; the firmware's two uses are `.s` with a negated source and `u16, u16`.
