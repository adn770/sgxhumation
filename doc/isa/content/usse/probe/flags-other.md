<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `.tpres`, `.partial`, `.nosched`, `.syncs`

*Measured on the hardware (2026-10-22).* In a fill, **`.tpres` (`frcp`, `frsq`, `fsqrt`),
`.partial` (`and`, `or`) and `.nosched` change nothing a pixel sees, while `.syncs` (on `mov`
and `fmad`) hangs the task**, which the driver recovers from with `-ETIMEDOUT` **[HW]** (probe
`flags-other`; `design/sgx545_hw_spec.md` §14). `.syncs` presumably waits for something a
fill never provides; what is open.
