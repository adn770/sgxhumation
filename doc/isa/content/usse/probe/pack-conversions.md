<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Pack and unpack conversions

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `pack-*`, `unpack-*`;
`design/sgx545_hw_spec.md` §14):

- **From `f32`, without `.scale`:** round to an integer in the instruction's mode (`nearest` ties
  to even, `down`, `up`, `zero`), then saturate to the format: `u8` 0..255, `s8` -128..127, `o8`
  `x + 128` in 0..255, `u16`, `s16`. 300 packs to 255, -5 to 0 (`u8`) or 123 (`o8`).
- **With `.scale`:** `u8` `x × 255`, `s8` `x × 127` clamped to ±127, `o8` `x × 127 + 128` clamped
  to 1..255, `u16` `x × 65535`, `s16` `x × 32767` clamped to ±32767; the product is exact and then
  rounded to nearest even **whatever the mode** (96 random values agree).
- **`c10`** is `x × 255` in a signed 10-bit lane, to nearest even, clamped to ±510 (±2.0),
  with or without `.scale`.
- **`pckf16f32`:** without `.renable` it truncates toward zero whatever the mode, keeps denormal
  results and saturates to 65504; **`.renable` rounds to an integer** in the mode (`up` turns
  1.0005 into 2.0).
- **To `f32`:** integers exactly; with `.scale` `u8/255`, `s8/127`, `o8 (x - 128)/127`,
  `u16/65535`, `s16/32767`, correctly rounded, with -128, 0 (`o8`) and -32768 giving -1.0 (every
  byte value checked); `c10` lanes are always `/255`, clamped to ±2.0; `f16` converts exactly,
  denormals and infinities included, and a NaN becomes `0x7fffffff`.
- **Integer to integer:** `unpcks16s8` sign-extends, `unpcks16u8` and `unpcku16u8`
  zero-extend, `unpcku16s8` turns a negative into 0, and `unpcku8s8` turns a negative into
  `0x7f`, not 0.

**Undecided:** `unpck` from `c10` to the 16-bit integers: `unpcku16c10` and `unpcks16c10`, with
or without `.scale`, give the same values, and no conversion we tried fits them (`0x0ff` gives
`0xfffe`, `0x100` gives 1, `0x080` 0; probe `unpack-c10-int`, recorded); `.scale` on the
integer-to-integer forms, and the other `f16` sources.
