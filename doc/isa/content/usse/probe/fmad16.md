<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Half-float multiply-add

*Measured on the hardware (2026-10-22).* **`fmad16 d, a, b, c` computes `a × b + c` in each
16-bit half** (low half from the low halves, high from the high) **[HW]**, exactly, rounded
once to nearest even (fused: 20 random cases tell it from rounding the product first). Half
denormals are kept, zeros keep their IEEE signs, overflow gives infinity and an invalid result
the half NaN `0x7fff`. **`.swzll`** feeds both lanes the operand's low half, **`.swzhh`** its
high half, and **`.swzf32`** the whole register as a binary32 value, used exactly (only the
result is rounded: `8209.25 × 1.5 + 3` gives `0x7204`, not the `0x7203` of rounding 8209.25 to
a half first). `.neg`, `.abs` act on the operand; `.repeat2.incsXYZ` advances `a`, `b`, `c` as
`fmad` does (probes `fmad16-lanes`, `fmad16-incs`, `fmad16-specials`, `fmad16-rounding`;
`design/sgx545_hw_spec.md` §14).

**Undecided:** a denormal or NaN `binary32` behind `.swzf32` (flushed or not).
