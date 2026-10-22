<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Source modifiers and data readings

The same register bits are read in several ways, chosen by the instruction **[RE]**:

- **Float sources** take `.neg` and `.abs` (`-|x|` when both, as the emulation models it),
  one bit each next to the operand. Source 0 of the float groups has `.abs` only.
- **Half floats.** With the group's half-float bit (`enc[54]` in classes 0-4) a source number
  of 64-127 names a 16-bit half of a 32-bit register, `(n - 64) / 2`, printed with a
  component suffix: `.flt16.0`/`.flt16.2` for the low and high half (`fsubflr` and the third
  operand of the dot products print `.flt16` and `.flt16.2`). Numbers below 64 keep their
  32-bit reading, and in that reading the temporaries stop at `r29`.

<!-- @example usse fmad r0, r1.flt16.0, r2.flt16.2, r3.flt16.0 -->

- **c10** is a 10-bit fixed-point colour reading (`.c10`): number bit 6 selects it in the
  colour, dot-product and `fpma` groups and in some float tests. It reads three signed 10-bit
  lanes, 1.0 = 255 **[HW]** (*Colour operations*).
- **Integer halves and components.** `.low`/`.high` pick a 16-bit half (`imae`, `ima16`,
  `adifsum`); `.0`-`.3` pick a byte or half of a pack/unpack source; `.neg` negates an
  integer source of `ima16`/`ima32`; `.comp` complements a colour-group selector (`1 - x`
  **[infer]**).
- **Swizzles** of `fmad16` (`.swzll`, `.swzhh`, `.swzf32`) select which halves of a source
  feed the two 16-bit lanes **[infer]**.
- **Inverted source.** `~src2` in the bitwise group is the bitwise complement **[RE]** (the
  firmware builds masks with an `xor` against an inverted immediate; the emulation models it).

What each modifier does to a value was never measured on the hardware except where a group
section says so.
