<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Repeat order and overlapping ranges

*Pending hardware probe (2026-10-22).* With the default MOE state a `.repeatN` steps every
operand by one register per iteration **[HW]** (non-overlapping copies). Open: the order of
the iterations, and what an iteration reads when its source was written by an earlier one
(`mov.repeat4 r1, r0`), i.e. whether a repeat behaves like N instructions in sequence.
