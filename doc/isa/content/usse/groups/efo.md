<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Extended float operation: `efo`

Class 7 **[RE]**. Every bit is a field: two multipliers `m0`, `m1` and two adders `a0`, `a1`
whose inputs are selections among the three sources and the internal registers `i0`, `i1`;
the destination takes one of `i0`, `i1`, `a0`, `a1`; `i0` and `i1` are written (or not,
`!i0`) from the adders or multipliers; each source has a `.neg`; flags `.skipinv`,
`.nosched`, `.gpiext` and a repeat.

```
efo d= <i0|i1|a0|a1>, <i0/i1 writes>, a0=<x>+<y>, a1=<x>+<y>, m0=<x>*<y>, m1=<x>*<y>, s0, s1, s2
```

- **Semantics [infer].** A small dataflow of two multiplies and two adds per instruction,
  with `i0`/`i1` as accumulators across instructions: the text the decoder prints is the
  dataflow. Whether the products and sums are rounded between stages is **[open]**, and so
  is `.gpiext`. Sources print both their 32-bit and half-float readings (`r1/r1.flt16.0`)
  where the two differ, because the instruction itself does not say.
- **In the firmware:** not used.

<!-- @group usse class=7 -->
