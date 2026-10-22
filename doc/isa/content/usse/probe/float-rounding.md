<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float rounding

*Pending hardware probe (2026-10-22).* Floats are IEEE binary32 bit patterns in the 32-bit
registers **[RE]**. The rounding mode of `fmad` and the other float operations, whether
`fmad` rounds once (fused) or twice, and how NaN and infinity propagate are not pinned. The
radare2 emulation rounds to nearest through doubles, an approximation.
