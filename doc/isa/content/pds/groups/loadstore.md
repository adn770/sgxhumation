<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Memory: `load`, `store`

Opcodes 6 and 7 **[RE]**: `load addr, dsB[48+i ... ]` and `store addr, dsB[48+i ...]` move one to
eight dwords between memory and the registers from `ds0[48]` or `ds1[48]` up. The address is an
18-bit dword index (printed as a byte address, `× 4`); the count reuses the predicate bits
`w[26:24]` (1-8, 0 meaning 8), so these instructions cannot be predicated.

- **Semantics [infer].** The address is relative to a base that is **[open]** (18 bits of
  dwords reach 1 MiB); whether the transfer completes before the next instruction is
  **[open]** (the fences may be for that).
- **In the microkernel's programs:** not used; nor in ours.

<!-- @example pds load 0x00001000, ds0[48-51] -->

<!-- @group pds pat='^(load|store) ' -->
