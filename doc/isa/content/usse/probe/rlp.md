<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `rlp`

*Measured on the hardware (2026-10-22).* **`rlp d, s, pN` rotates left through the predicate**:
`d = s << 1 | pN`, then `pN` = bit 31 of `s` **[HW]** (probe `rlp`, 32 cases over every
predicate). **Predicate numbers from 4 up alias `p0`-`p3`** (number mod 4: `rlp ..., p4` reads
and writes `p0`) **[HW]** (probe `rlp-high-predicates`).
