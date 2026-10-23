<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Data rows, and the banks past the data segment

*Not probed yet.* `pds-readback` checks that sixteen data words come back unchanged through
`douta`; `pds-banks` puts different words in the two banks to confirm the row rule (*Data
registers*) for every dword of the segment; `pds-uninit` (recorded) reads `ds0[60]`/`ds1[60]`
before writing them, to see whether the banks past the data segment are cleared between runs.
