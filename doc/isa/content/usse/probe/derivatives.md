<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Derivatives `fdsx`, `fdsy`

*Measured on the hardware (2026-10-22).* **`fdsx d, a, b` = `a`(top-right) − `b`(top-left) and
`fdsy d, a, b` = `a`(bottom-left) − `b`(top-left), the same value for all four pixels of the 2x2
quad** **[HW]** (coarse derivatives; probe `fds-quad`, on per-pixel values from the pixel
coordinates; `design/sgx545_hw_spec.md` §14). The difference is rounded once to binary32;
`.neg` and `.abs` act on the source before. The quads are the aligned 2x2 blocks of the
render target (even x and y at the top left).

**Undecided:** partly covered quads (the probes fill whole quads), and denormal inputs.
