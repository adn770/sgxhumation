<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Operand encodings

The PDS has no single operand encoding: each group lays out its own fields **[RE]**. The
recurring ones:

- **Seven-bit destination** (`w[6:0]`): `ds0[0]`-`ds0[63]`, then `ds1[0]`-`ds1[63]`.
- **Seven-bit first source** (`w[23:17]`): `ds0[0]`-`ds0[63]`, then the first two or three of
  `ir0`, `ir1`, `tim`; the second source of the ALU group is always `ds1[n]` (`w[15:10]`).
- **Halves** (`.l`, `.h`): `mov16` and `mul` read 16-bit halves of a register; the `movs`
  sources read halves of an input register.
- **The `movs` source list** (`w[23:5]`): a `ds1` pair (`w[17:13]` × 2) and either a `ds0`
  pair (`w[22:18]` × 2) or, with `w[23]` set, the input register numbered by `w[22:18]`;
  then four two-bit selectors, one per slot (`w[12:5]`), each picking the `ds1` pair or the
  other one, and its first or second dword (the low or high half of an input register). So
  the four sources of one `movs` come from at most two `ds0` and two `ds1` dwords.

<!-- @example pds movsa doutd, ds0[0], ds0[1], ds1[50], ds1[50] -->
