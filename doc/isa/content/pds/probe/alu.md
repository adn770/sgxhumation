<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Bitwise ops, `add` and `sub`

*Not probed yet.* `pds-bitwise` runs `xor`, `nor`, `nand`, `and` and `or` on three bit patterns
(readings: as named; `nor` and `nand` swapped); `pds-add-sub` runs `add` and `sub` on small,
wrapping and signed-edge values (readings: modular with `dst = src - ds1[n]`; unsigned
saturating; `sub` computing `ds1[n] - src`).
