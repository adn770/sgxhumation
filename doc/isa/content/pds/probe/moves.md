<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `mov16`, `mov64`, `mov128` and `swap`

*Not probed yet.* `pds-mov16` moves high and low halves (readings: into the low half keeping
the high one; zero-extended; into the same half as the source's). `pds-mov-wide` runs
`mov64` across the banks and `mov128` with and without `swap` (readings: `ds[2k]` is the row
`ds0[2k]`, `ds0[2k+1]`, `ds1[2k]`, `ds1[2k+1]` and `swap` exchanges its two pairs; `swap`
exchanges the words of each pair; `ds[2k]` is four `ds0` dwords; `mov64` lands its pair
reversed).
