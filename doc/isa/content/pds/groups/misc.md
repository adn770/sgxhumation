<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Fences and ALU mode: `rfence`, `rwfence`, `alum`

Opcode 15 is a fence, `rfence` or `rwfence` by `w[0]`, without a predicate; opcode 23 is
`alum signed` or `alum unsigned` (`w[0]`) **[RE]**.

- **Semantics [infer].** By the names, fences that order reads (and writes) of the PDS's
  memory accesses, and a mode switch for the ALU's signedness. Neither the microkernel's
  programs nor ours use them; **[open]**.

<!-- @group pds pat='^\{0\}$|alum' -->
