<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Multiply: `mul`

Opcode 12 **[RE]**: `mul dst, a.l|h, ds1[n].l|h` multiplies two 16-bit halves (the first from
`ds0` or `ir0`/`ir1`/`tim`) into a 32-bit destination.

- **Semantics [HW].** Our vertex program computes the vertex offset `ir0.l × stride` with it,
  and the vertices are fetched from the right place. Signedness (unsigned by default, `alum`?)
  is **[open]**.
- **In the microkernel's programs:** not used.

<!-- @prog pds_vtx.s match='mul' -->

<!-- @group pds pat='mul ' -->
