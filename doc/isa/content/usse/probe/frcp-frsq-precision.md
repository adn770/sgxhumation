<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `frcp` and `frsq` precision

*Pending hardware probe (2026-10-22).* `frcp` and `frsq` compute approximations of `1/x` and
`1/√x` **[RE/infer]**. Their precision (exact to the last bit, or a table-based estimate),
their results for 0, negative numbers and infinities, and the precision of `flog`, `fexp`,
`fsin` and `fcos` are not pinned.
