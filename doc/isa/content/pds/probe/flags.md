<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `aluz`, `alun`, `tstn` and predicated tests

*Not probed yet.* `pds-flags` runs one op of eight (`add`, `sub`, `and`, `xor`, `shl`, `not`,
`mov32`, `tstz`) with a zero, a negative and a positive result after a known `add`, then marks
under `(aluz)` and `(alun)` (readings: every ALU op, shift and `not`/`abs` sets them and moves
and tests do not; only `add`/`sub`; the logic ops but not the shifts; never true).
`pds-tst` runs `tstn` on 0, 1, `0x7fffffff`, `0x80000000` and `0xffffffff`, `tstz` on a `ds1`
source, and a test under a false predicate (readings: `tstn` is bit 31 and a test obeys its
predicate; bit 31 and the predicate is ignored; signed `<= 0`). `pds-if` (recorded, per pixel)
shows which runs see `if0` and `if1`.
