<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer multiply-add: `imae`, `ima16`, `ima32`, `ima8`, `idiv`

Four classes **[RE]**: 21 `imae` (16 × 16 + 32), 20 `ima16` (16-bit), 26 `ima32` (32-bit) and
`idiv`, 19 `ima8` (four 8-bit lanes with colour-style selectors). They carry the multiply
flags (`.skipinv`, `.nosched`, `.end`, a three-bit repeat) and a two-bit predicate (`ima32`
and `idiv` the full one).

- **`imae.{u,s} d, a.{low,high}, b.{low,high}, c, u32`** computes `d = a16 × b16 + c`, the
  halves zero- or sign-extended **[RE]**: the firmware's addition idiom is
  `imae.u d, a.low, #1.low, c, u32`, an add of a 16-bit value, 205 times, and its address
  arithmetic multiplies indices by structure sizes the same way. Saturation (`.usat`,
  `.ssat`), the right shift `.rsN`, the source-2 types `z16`/`u16` and the carries (`coe`,
  `cie` with `i0`/`i1`) are **[open]**.
- **`ima32.{u,s} d, a, b, c`** computes the low 32 bits of `a × b + c` **[RE]** (the
  firmware's ten uses fit). The optional `i` register after the destination (the high half?)
  and the carry-in `cie, iN` are **[infer]** at best.
- **`ima16`** multiplies 16-bit values, its sources read as `u16`, `u8`, `s8` or `o8` with a
  half select. Pending probe: its formats. The firmware has two, both `.s` with a negated
  source.
- **`idiv d, a, b, drcN`** names a data fence like a load, so the quotient presumably arrives
  asynchronously and is waited for with `wdf` **[infer]**.
- **`ima8`** works on four 8-bit lanes with selectors (`s0`, `s1a`, ...) and `.sat` **[open]**.

<!-- @example usse imae.u r2, r4.low, #1.low, r2, u32 -->

<!-- @example usse ima32.u r1, r2, r3, r4 -->

<!-- @group usse class=19-21,26 -->
