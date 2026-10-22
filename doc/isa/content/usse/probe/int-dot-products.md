<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer dot products

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `intdot-lanes`,
`intdot-alpha`; `design/sgx545_hw_spec.md` §14):

- **`u8dot4 d, Cx, Ax, s1, s2`** sums the four byte products `s1[i] × s2[i]` and writes the
  same value to every byte: `S × Cx / 255` to bytes 0-2 and `S × Ax / 255` to byte 3, each
  rounded to nearest and saturated to 0..255. **`u8dot3`** sums bytes 0-2 only. `.comp`
  complements a source's bytes first.
- **`off`** makes every byte signed around 128: the products are `(s1[i] - 128)(s2[i] - 128)`,
  and a negative result saturates to 0.
- **`u16dot4`, `u16dot3`** read the same bytes but write a 16-bit fraction to both halves:
  `S × Cx × 32767 / 255²` rounded, saturated to `0x7fff`, or to ±`0x7fff` for `off` (signed);
  `Ax` changes nothing.
- **`u8dot3`'s alpha** (byte 3), when it has an operation: `aadd Ax, f1, a1, f2, a2` =
  `(f1 × a1 + f2 × a2) × Ax / 255` and `asub` the difference, with `a1` = `s1a`, `a2` = `s2a`
  (each possibly `.comp`) and factors `zero`, `s1a`, `s2a`; `aintrp1 s0, x, y` = `x × (1 -
  s0a) + y × s0a` and `aintrp2 s0, x, y` = `(1 - x)(1 - s0a) + y × s0a`.
