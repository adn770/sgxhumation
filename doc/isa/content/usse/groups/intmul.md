<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer multiply-add: `imae`, `ima16`, `ima32`, `ima8`, `idiv`

Four classes **[RE]**: 21 `imae` (16 × 16 + 32), 20 `ima16` (16-bit), 26 `ima32` (32-bit) and
`idiv`, 19 `ima8` (four 8-bit lanes with colour-style selectors). They carry the multiply
flags (`.skipinv`, `.nosched`, `.end`, a three-bit repeat) and a two-bit predicate (`ima32`
and `idiv` the full one).

- **`imae.{u,s} d, a.{low,high}, b.{low,high}, c, u32`** computes `d = a16 × b16 + c`, the
  halves zero- or sign-extended **[infer]**: the firmware's addition idiom is
  `imae.u d, a.low, #1.low, c, u32`, an add of a 16-bit value, 205 times, and its address
  arithmetic multiplies indices by structure sizes the same way. **[HW]** `.usat`/`.ssat`
  saturate, `.rsN` shifts the result right, a `u16` source 2 is sign-extended and a `z16`
  one zero-extended, `coe, iN` writes the carry out and `cie, iN` adds it (*Integer
  multiply-add modes*).
- **`ima32.{u,s} d, a, b, c`** computes the low 32 bits of `a × b + c` **[infer]** (the
  firmware's ten uses fit). `cie, iN` adds `iN` **[HW]**; the optional `i` register after the
  destination is **[open]**. As a dual-issue secondary it works on two 16-bit
  lanes (*Dual issue*).
- **`ima16`** multiplies 16-bit values, its sources read as `u16`, `u8`, `s8` or `o8` with a
  half select. With `u16, u16` it computes `a × b + c` (or `- c`) in each 16-bit lane,
  wrapping **[HW]** (*`ima16` lanes and source formats*); with `u8`, `s8`, `o8` each lane reads
  the `.low`/`.high` byte of its own half **[HW]** (*Integer multiply-add modes*).
  The firmware has two, both `.s` with a negated source.
- **`idiv d, a, b, drcN`** writes the unsigned quotient to `d` and the remainder to `d + 1`,
  asynchronously: `wdf drcN` waits for them **[HW]** (*Integer multiply-add modes*).
- **`ima8`** computes `x + y × z` per byte from its selectors (`s0`, `s1`, `s2` and their alpha
  forms), wrapping or with `.sat` clamping **[HW]** (*Integer multiply-add modes*).

<!-- @example usse imae.u r2, r4.low, #1.low, r2, u32 -->

<!-- @example usse ima32.u r1, r2, r3, r4 -->

<!-- @group usse class=19-21,26 -->
