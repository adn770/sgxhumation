<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Data moves: `mov16`, `mov32`, `mov64`, `mov128`

Opcodes 1-4 **[RE]**: `mov16 dsB[n], src.h|l` moves a 16-bit half (sources: `ds0`, `ds1`, then
`ir0`, `ir1`, `pc`, `tim`, `ir2`, each `.l` or `.h`); `mov32 dsB[n], src` a dword (the same
sources, whole); `mov64 dsB[2k], src` an aligned pair (from a `ds0`/`ds1` pair, `mr01` or
`mr23`); `mov128 ds[2k], ds[2j]` four dwords, optionally `, swap`.

- **Semantics [infer].** Copies of the named width. Which half of the destination `mov16`
  writes, how `mov128` addresses two banks with one index, and what `swap` swaps are
  **[open]**.
- **In the microkernel's programs:** not used; nor in ours.

<!-- @example pds mov16 ds0[3], ds1[2].h -->

<!-- @group pds pat='mov(16|32|64|128) ' -->
