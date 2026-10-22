<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Colour operations

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `sop2-selectors`,
`sop2-ops`, `sop2-random`, `sop3-lrp`, `sop3-random`, `sop2wm`, `colour-c10`;
`design/sgx545_hw_spec.md` §14). Bytes 0-2 are the colour channels and byte 3 the alpha; a
channel `x` stands for `x/255`:

- **`sop2 d, s1, s2, f1, f2, op+asop2 g1, g2, aop`** computes per colour channel
  `op(s1 × f1, s2 × f2)` and per alpha `aop(s1a × g1, s2a × g2)`: `add` and `sub` sum the two
  exact products, divide by 255 once, round to nearest and saturate to 0..255 (a negative
  difference is 0); `min` and `max` take the smaller or larger **source**, ignoring the
  factors. Factors: `zero`, `s1`, `s2` (the same channel), `s1a`, `s2a`, `asat` =
  `min(s1a, 1 - s2a)`, `s2scale` = `s2` and the whole result doubled (also for `min`/`max`),
  and as second factor `zeros` = 0, which makes `s2` signed (`s2 - 0.5`) in `add`/`sub`.
  `.comp` gives `1 - f` (`asat.comp` = `min(1 - s1a, s2a)`, `s2scale.comp` = `2(1 - s2)`).
  A leading `comp, ` complements `s1` itself in `add`/`sub`; the alpha's `, neg` negates its
  result before saturating (so `sub, neg` is `s2a × g2 - s1a × g1`).
- **`sop3 d, s0, s1, s2, f1, f2, op`** is the same colour operation (`add`/`sub`) with
  factors `zero`, `asat`, `s0`, `s0a`, `s1`, `s1a`, `s2`, `s2a`; with `, neg` (the `alrp`
  form) it is `-op(s1 × f2, s2 × f1)`, the factors swapped. Alpha: `asop a, f2, op` =
  `op(s1a × a, s2a × f2)`; `arsop a, op` = `-op(s1a × f2, s2a × a)`; `alrp a, b` =
  `a × (1 - s0a) + b × s0a`. A factor read for the alpha takes the alpha channels; `asat`
  reads 1 there.
- **`lrp1 d, s0, s1, s2, x, y, t`**: colour `x × (1 - t) + y × t`, `x` one of `zero`/`s1`, `y`
  one of `zero`/`s2` (each with `.comp`), `t` = `s0` or `s0a`; its alpha is `asop` with two
  free factors. **`lrp2 d, s0, s1, s2, f1, f2, m1, m2`**: `f1 × m1 + f2 × m2` with the pairs
  (`s1`, `s2`), (`s1`, `1 - s2`), (`1`, `s2`), (`1 - s1`, `s2`); alpha `alrp`.
- **`sop2wm d.bytemaskN, s1, s2, f1, f2, cop, aop`**: `sop3`'s factors without `s0`, the
  colour operation on bytes 0-2 and the alpha one on byte 3; mask bit N writes byte N.
- **`.c10` operands** are three signed 10-bit lanes (bits 0, 10, 20), 1.0 = 255, clamped to
  ±510 when read and when written; a `c10` source's alpha is its lane 0; a `c10` destination
  gets bits 30-31 clear and an 8-bit destination the saturated channels. The colour formulas
  above hold unchanged.

- **`sop2.repeatN`** (round 3) computes each iteration on the next registers: the destination
  and both sources advance by one **[HW]** (probe `colour-dot-repeats`). `sop3`, `lrp1`,
  `lrp2` and `sop2wm` have no repeat form.

**Undecided:** `.c10` operands on `sop3`, `lrp1`, `lrp2` and `sop2wm` (presumably read as for
`sop2` **[infer]**).
