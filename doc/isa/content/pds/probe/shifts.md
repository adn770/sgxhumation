<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Shift amounts and the `shr` fill

*Not probed yet.* `pds-shifts` shifts `0x80000001` and `0xc0000003` by 0, 1, 4, 31, 32, 33, 63
and by -1, -4, -31, -32, -64 (readings: logical, 32 and over giving 0 and a negative amount
shifting the other way; `shr` arithmetic; the amount modulo 32). `pds-alum-shr` asks whether
`alum signed` makes `shr` arithmetic.
