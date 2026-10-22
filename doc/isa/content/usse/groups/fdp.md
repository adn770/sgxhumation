<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float dot products: `fdp`, `fdpc`, `fddp`, `fddpc`

Class 2 **[RE]**: `enc[42:41]` picks `fdp` (with `enc[14]` set, `fdpc` and a clip-plane number
`cp0`-`cp7`), `fddp` and `fddpc`. The `fddp` forms write an internal register `i0`/`i1`
besides the destination and so lose the destination's bank extension bit; `fddpc` takes two
`cp` operands and has no `.incs`. Half-float reading with `enc[54]`.

- **Semantics [infer].** A dot product accumulated over the repeat (`fdp` with `.repeat3`
  being a three-component dot product?), `fddp` a double dot product into `d` and `iN`, the
  `c` forms tied to clip planes (the `cp` operand). Not run; **[open]**.
- **In the firmware:** not used.

<!-- @example usse fdp r0, r4, r8 -->

<!-- @group usse class=2 -->
