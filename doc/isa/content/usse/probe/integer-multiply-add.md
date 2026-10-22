<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Integer multiply-add modes

*Measured on the hardware (2026-10-22).* Round 4 (table `leftovers4.toml`;
`design/sgx545_hw_spec.md` §14) **[HW]**:

- **`idiv d, a, b, drcN`** writes the **unsigned quotient to `d` and the remainder to `d + 1`**,
  `0xffffffff` to both for `b = 0`; the results arrive once `wdf drcN` returns (without the
  wait the remainder was still missing at the next store) (probe `idiv`).
- **`ima16` with 8-bit formats** reads, in each 16-bit lane, the byte `.low` or `.high` of that
  lane's half of `b` and `c`: `u8` unsigned, `s8` signed, `o8` the byte minus 128; `a` stays
  `u16` (probe `ima16-8bit`).
- **`ima8 d, a, b, c, x, y, z, xa, ya, za`** computes **`x + y × z` per byte**, the colour
  selectors `x`, `y`, `z` for bytes 0-2 and the alpha ones for byte 3 (`s0`, `s1`, `s2` = `a`,
  `b`, `c`; `.comp` = 255 − the byte), wrapping, or clamping to 255 with `.sat` (probe
  `ima8`).
- **`imae`**: `.usat`/`.ssat` saturate the 32-bit result (`.ssat` signed), `.rsN` shifts it right
  by N, source 2 typed `u16` is its half **sign**-extended and `z16` zero-extended, **`coe, iN`
  writes the carry out to `iN` and `cie, iN` adds `iN`**; `ima32`'s `cie, iN` adds `iN` too
  (probe `imae-modes`).
