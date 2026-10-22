<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The test sign `p`

*Pending hardware probe (2026-10-22).* The sign part of a test (`.test<S>...`) is `t`, `n` or
`p`. `n` tests bit 31 of a 32-bit result **[RE]**. Whether `p` ("positive") means `> 0` or
`>= 0`, and therefore what it says about a zero result, is not pinned; the firmware uses
`testpat` on `shl` and `isub16` results, where either reading fits its control flow.
