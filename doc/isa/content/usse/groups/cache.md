<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Cache control: `ci`, `cf`, `cfi`

Class 31 with `enc[54:52]` = 4, operation 0 **[RE]**: `ci`, `cf` or `cfi` by `enc[45:44]` (0
does not decode), flags `.nosched`, `.global`, `.dm_nomatch`, two sources and a cache-level
operand that the decoder annotates: 1 `L0 and L1`, 2 `L2`, 3 `L0, L1 and L2`.

- **Semantics [infer].** Cache invalidate, flush, and flush-and-invalidate over the named
  levels, for the address range the sources give (or everything, `.global`?). **[open]**
  beyond the names.

**In the firmware [RE].** Four `ci.global` and one `cfi.global`, all on the three levels.

<!-- @example usse ci r1, r2, #1 -->

<!-- @group usse class=31 pat='^\{0\}\{1\} \{2\}, \{3\}, #\{4\}$' -->
