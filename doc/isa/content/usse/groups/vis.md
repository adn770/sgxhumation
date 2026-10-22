<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Visibility tests: `pcoeff`, `ptoff`, `atst8`, `depthf`

Class 31 with `enc[53:52]` = 3 **[RE]**: operation 0 is `pcoeff` or `ptoff` (by `enc[47]`), 1
`atst8`, 3 `depthf`. `atst8` writes a destination and a predicate `p0`-`p3` (or not, `!`),
with `.c10`; `atst8` and `depthf` test three sources with `twosided` and a feedback mode
(`fb`, `nfb`, `optdwd`) and take a two-bit predicate.

- **`ptoff`** signals the end of the punch-through phase for the pixels of the task
  **[infer]**: our 3D render's PTOFF task is the single instruction `ptoff.end`, the one the
  closed driver's PTOFF task runs, and renders run with it **[CAP/HW]**.
- **`atst8`** (an 8-bit alpha test with feedback to the ISP?), **`depthf`** (depth feedback?)
  and **`pcoeff`** (plane coefficients?) are **[infer]** by name only and **[open]**.
- **In the firmware:** not used.

<!-- @prog use_ptoff.s -->

<!-- @example usse ptoff.end -->

<!-- @group usse class=31 pat='pcoeff|ptoff|atst8|depthf' -->
