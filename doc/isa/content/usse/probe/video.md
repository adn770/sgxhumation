<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Video `adif`, `adifsum`

*Measured on the hardware (2026-10-22).* **`adif d, a, b` = `|a − b|` per byte; `adifsum d,
s0.low|high, a, b` = the sum of the four byte differences plus that 16-bit half of `s0`**, as a
32-bit value; a repeat advances the destination and the sources **[HW]** (probe `video-adif`).

**Undecided:** `bilin`, `firv` and `firh` (probe `video-filters`, recorded): no single or
two-weight interpolation of the source bytes fits `bilin`, and the FIR filters read
coefficient sets (`fcsN`) we do not set.
