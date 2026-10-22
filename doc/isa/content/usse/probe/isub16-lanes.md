<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `isub16`/`isubu16` lanes and saturation

*Pending hardware probe (2026-10-22).* The 16-bit operations of the test group (`iadd16`,
`isub16`, `imul16` and their unsigned forms) take 32-bit registers. Open: whether they work
on the low halves only or on both halves as two lanes, what reaches the destination's upper
half, whether results saturate or wrap, and which lane the test reads. The firmware uses 40
`isub16` tests (with `testtanz`, `testpat`, `testnat`, `testtaz`) and one `isubu16`, mostly to
compare small counters, which a low-half reading would serve.
