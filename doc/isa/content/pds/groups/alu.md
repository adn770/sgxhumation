<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer ALU: `add`, `sub`, `adc`, `sbc`, `and`, `or`, `xor`, `nor`, `nand`

Opcodes 8-11 (`add`, `sub`, `adc`, `sbc`) and 24-26, 28-29 (`or`, `and`, `xor`, `nor`, `nand`)
share one layout: `op dst, src, ds1[n]` with a seven-bit destination, a first source from
`ds0` or `ir0`/`ir1`/`tim`, and `ds1[n]` **[RE]**. Opcode 27 is `not` (*`abs`, `not`*) and
opcode 14 does not decode.

- **Semantics.** `dst = src op ds1[n]` on 32 bits: `and` and `or` **[HW]** (our pixel-event
  program builds the end-of-tile task's `U1` from the tile position with `and`/`or` masks,
  and the tiles land where they should); `xor`, `nor`, `nand`, `add`, `sub` by name
  **[infer]**; `adc`/`sbc` add and subtract with a carry or borrow whose source is **[open]**.
  Whether `alum` makes them signed, and how they set `aluz`/`alun`, are **[open]**.
- **In the microkernel's programs [CAP].** The event program masks the event bits of `ir1`
  with `and` and compares groups of them with `xor` before its tests; the deferred-work
  program masks the request bits of `ir0` with `and`.

<!-- @prog pds_pixevent.s match='^\s+(and|or) ' -->

<!-- @group pds pat='^\{0\}\{1\} \{2\}, \{3\}, ds1' -->
