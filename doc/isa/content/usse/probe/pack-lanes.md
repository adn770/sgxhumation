<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Pack and unpack lanes

*Not probed yet.* A pack or unpack takes two sources with a component select each (`.0`-`.3`), a
destination byte mask and a rounding mode **[RE]**. Which source and component feed which
destination lane, the bit order of `.bytemaskXXXX`, the rounding modes and `.scale` are **[open]**:
the probes of 2026-10-22 did not cover them. The closed fragment program of our capture packs four
f16 colour components to 8-bit with `pcku8f16` **[CAP]**, a starting point; `pcku8f16` and
`unpckf32u8` over sources with a distinct value per component, one byte-mask bit at a time and ties
for each rounding mode, would settle it.
