<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Bitwise operations and shifts: `and`, `or`, `xor`, `shl`, `shr`, `rol`, `asr`

Classes 10-13 share one layout. `enc[63:60]` selects the pair of classes (5 for `and`/`or`
and `xor`, 6 for the shifts) and the operation is `enc[59]` with `enc[35]` **[RE]**:

| `enc[63:60]` | `enc[59]`, `enc[35]` = 00 | 01 | 10 | 11 |
|---|---|---|---|---|
| 5 | `and` | `or` | `xor` | (does not decode) |
| 6 | `shl` | `rol` | `shr` | `asr` |

- **Semantics [infer].** `d = s1 op s2` on 32 bits: `and`, `or`, `xor`; `shl` and `shr` shift
  left and right (logical), `rol` rotates left, `asr` shifts right arithmetically, by source
  2. `~` before source 2 complements it first. This is the reading the radare2 emulation of
  the firmware uses, and the firmware's code makes sense under it throughout. **[HW]** By a
  register amount of 32 and over, `shl`/`shr` give 0, `asr` the sign and `rol` takes the
  amount mod 32, immediate amounts the same, and `~` complements an amount as any source 2
  (*Shift amounts of 32 and over*).
- **Immediates.** Source 2 written `#imm` is the rotated 16-bit immediate (*Immediates*); a
  source-1 immediate is the usual seven-bit one, printed in hex.
- **Write mask or repeat**, as for `mov` (`enc[53]`).
- **`.partial`** (`enc[34]`) changes nothing a fill sees **[HW]** (*`.tpres`, `.partial`,
  `.nosched`, `.syncs`*).

**In the firmware [RE].** The workhorses of its flag logic: 602 `and`, 208 `or`, 199 `xor`,
95 `shl`, 60 `shr`, 1 `rol`, mostly on temporaries and on words loaded from its control
blocks; masks are often built with `xor` against an inverted immediate.

<!-- @example usse and r1, r2, #0x00FF0000 -->

<!-- @example usse xor.repeat2 r4, r6, ~r8 -->

<!-- @group usse class=10-13 -->
