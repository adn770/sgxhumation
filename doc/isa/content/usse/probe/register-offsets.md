<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Register offsets of loads and stores

*Pending hardware probe (2026-10-22).* An immediate offset counts elements of the access size
**[HW/RE]**. For a **register** offset (`[b,+rN]`) the unit (elements or bytes), and whether a
fetch adds the same extra element as with an immediate, are not pinned. The firmware has four
such stores; the radare2 emulation leaves them out.
