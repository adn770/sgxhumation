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

- **Semantics [HW].** A small dataflow of two multiplies and two adds per instruction,
  with `i0`/`i1` as accumulators across instructions: the text the decoder prints is the
  dataflow. Each product and each sum is rounded to binary32; the destination's `i0`/`i1`
  are the values before the instruction's own writes. Sources print both their 32-bit and
  half-float readings (`r1/r1.flt16.0`) where the two differ; the 32-bit reading is the one we
  probed.
- **`.gpiext` [HW]** works across the 2x2 quad: the multipliers read the instance's `i2` and
  `i0` for `i0` and `i1`, an `i0` write goes to its `i2`, and the four `i1` writes end in the
  top-right instance's `i0`, whose reads during the instruction see the top-left instance's
  `i0` or, by timing, its write (*`efo`*).
- **In the firmware:** not used.

<!-- @group usse class=7 -->
