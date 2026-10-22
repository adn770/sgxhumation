<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Single-source float functions: `frcp`, `frsq`, `flog`, `fexp`, `fsqrt`, `fsin`, `fcos`

Class 1 **[RE]**: one source (source 1) with `.neg`, `.abs` and a data type (`.flt16.0`/`.2`
for a half, `.c10.0`-`.3` for a c10 component), a destination with a write mask or a repeat
count, the float flags plus `.tpres`. The operation is `enc[42:41]` with `enc[48]`.

- **Semantics [HW].** `frcp` = 1/x, `frsq` = 1/√x, `fsqrt` = √x, within one ulp, not
  always correctly rounded; their sources are flushed if denormal and an invalid result is the
  NaN `0x7fffffff` (*`frcp`, `frsq` and `fsqrt` precision*). `flog` = log2 and `fexp` = 2^x
  within an ulp or two; `fsin`/`fcos` take quarter turns (`sin(πx/2)`) and return 15
  fraction bits (*`flog`, `fexp`, `fsin`, `fcos`*).
- **`.tpres`** changes nothing in a fill **[HW]** (*`.tpres`, `.partial`, `.nosched`,
  `.syncs`*); its purpose is unknown.
- **In the firmware:** not used.

<!-- @example usse frcp r1, r2 -->

<!-- @example usse frsq.repeat2 r4, r2.abs -->

<!-- @group usse class=1 -->
