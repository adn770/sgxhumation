<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float rounding and NaN

*Measured on the hardware (2026-10-22).* Floats are IEEE binary32 bit patterns in the 32-bit
registers **[RE]**. **`fmad` rounds to nearest even** and **is not fused**: it rounds `a × b` to
binary32, then the sum **[HW]**. `(1+2^-12)^2` and `(1+2^-12)(1+3×2^-12)`, both ties, round to even,
also when negative; `(1+2^-12)^2 - (1+2^-11)` is 0, not 2^-24; 48 random triples agree (probes
`fmad-round`, `fmad-fused`, `sweep-fmad`; `design/sgx545_hw_spec.md` §14). An invalid result is the
NaN **`0x7fffffff`** (`frsq` and `fsqrt` of -1), not the IEEE default `0x7fc00000`. The radare2 ESIL
rounds to nearest even but forms `fmad`'s sum through a double, so a sum of very different
magnitudes can round twice there (`tools/usse-dis/r2/README.md`).

The dot products round each product, then each sum, the same way, but their invalid result is
the NaN `0x7fc00000` **[HW]** (*Float dot products*).

**Undecided:** the remaining float groups (presumably the same rounding **[infer]**), and their
NaN and infinite inputs; value probes on those would settle them.
