<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float multiply-add: `fmad`, `fadm`, `fmsa`, `fsubflr`

Class 0 **[RE]**. `enc[42:41]` picks `fmad`, `fadm`, `fmsa` (three sources) or `fsubflr` (two);
`enc[54]` reads the sources as half floats (*Source modifiers*); the float flags, a
two-bit repeat (`.repeat2`-`.repeat4`) and, when `enc[53]` is clear, `.incsXYZ`. Sources
carry `.neg`/`.abs` (source 0 `.abs` only).

- **`fmad d, a, b, c`** computes `a × b + c` in IEEE binary32 **[RE]**: the firmware's six
  uses fit, and our emulation models it so. The rounding, whether the multiply-add is fused,
  and denormals are pending a probe. `fmad` with no source modifiers is also the usual way
  to write a float move or add (`a × 1 + c`) **[infer]**.
- **`fadm`, `fmsa`** (by the names an add-multiply and a multiply-subtract-add?) and
  **`fsubflr`** (subtract and floor?) have their operand roles **[open]**.
- **`.incsXYZ`**: pending probe (*Repeats and the MOE state*).
- **The SGX540** reads these classes differently (a write mask, source-0 negation, no
  `.incs`); the decoder only decodes them for the SGX545.

<!-- @example usse fmad r0, r1, r2, r3 -->

<!-- @example usse fmad.repeat2.incs011 r0, r2, r4, r6 -->

<!-- @group usse class=0 -->
