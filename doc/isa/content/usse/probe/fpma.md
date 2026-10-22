<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `fpma`

*Measured on the hardware (2026-10-22).* **`fpma d, s0, s1, s2, c0, c1, c2, a0, a1, a2` is
`c0 + c1 × c2`** per colour channel and `a0 + a1 × a2` for the alpha **[HW]**: the first
selection is the addend (`s0`, `s1`, `s0a`, `s1a`), the other two the factors (`s1`/`s1a`,
`s2`/`s2a`), each `.comp` giving `1 - x`; with channels as `x/255`, the result is
`(255 c0 + c1 c2) / 255` rounded to nearest and saturated to 0..255. `.neg` negates `s0`.
`.c10` operands read and write the signed 10-bit lanes of the colour operations (±510);
`.sat` changes nothing that we saw (probe `fpma`; `design/sgx545_hw_spec.md` §14).

Round 3: **`.sat` changes nothing** also for `c10` results below 0 and above 1.0, which stay
clamped to ±2.0 only (probe `fpma-sat`), and **`fpma.repeatN` advances the destination and the
three sources by one per iteration** (probe `colour-dot-repeats`) **[HW]**.
