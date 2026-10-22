<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Single-source float functions: `frcp`, `frsq`, `flog`, `fexp`, `fsqrt`, `fsin`, `fcos`

Class 1 **[RE]**: one source (source 1) with `.neg`, `.abs` and a data type (`.flt16.0`/`.2`
for a half, `.c10.0`-`.3` for a c10 component), a destination with a write mask or a repeat
count, the float flags plus `.tpres`. The operation is `enc[42:41]` with `enc[48]`.

- **Semantics [RE/infer].** `frcp` = 1/x, `frsq` = 1/√x, `fsqrt` = √x by name and as the
  emulation models them; `flog`/`fexp` presumably base 2, `fsin`/`fcos` with an argument
  scaling that is **[open]**. Pending probe: the precision of `frcp` and `frsq` (and of the
  rest), special values and denormals.
- **`.tpres`** is **[open]**.
- **In the firmware:** not used.

<!-- @example usse frcp r1, r2 -->

<!-- @example usse frsq.repeat2 r4, r2.abs -->

<!-- @group usse class=1 -->
