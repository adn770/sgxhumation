<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Shifts: `shl`, `shr`

Opcodes 30 and 31 **[RE]**: `shl dst, src, n` and `shr dst, src, n` with a source from `ds0`,
`ds1`, `ir0` or `ir1` and a signed seven-bit amount, which the decoder prints as an unsigned
32-bit number (so `-3` prints `4294967293`).

- **Semantics.** `shl` shifts left **[HW]**: our pixel-event program places the tile position with
  `shl` by 3 and the end-of-render bit with `shl` by 20. `shr` shifts right **[infer]** (by its
  name); what a negative amount does (the other direction?) and whether `shr` is logical are
  **[open]**.

<!-- @prog pds_pixevent.s match='^\s+shl ' -->

<!-- @group pds pat='sh[lr] ' -->
