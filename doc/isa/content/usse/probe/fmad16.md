<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Half-float multiply-add

*Measured on the hardware (2026-10-22).* **`fmad16 d, a, b, c` computes `a × b + c` in each
16-bit half** (low half from the low halves, high from the high) **[HW]**: the product rounded to
binary32, then the sum to a half, both to nearest even. A product of two halves fits a binary32,
so for halves this is one rounding, fused (20 random cases tell it from rounding the product to a
half first). Half denormals are kept, zeros keep their IEEE signs, overflow gives infinity and an
invalid result the half NaN `0x7fff`. **`.swzll`** feeds both lanes the operand's low half,
**`.swzhh`** its high half, and **`.swzf32`** the whole register as a binary32 value (not rounded
to a half: `8209.25 × 1.5 + 3` gives `0x7204`, not the `0x7203` of rounding 8209.25 to a half
first); with it the product can need more than 24 bits, and the hardware rounds it to binary32:
of 992 `.swzf32` results one tells this from a single rounding (`-0.41455 × 40.8136 + 2.0625`,
probe `x-fmad16`, checked by `crates/sgx-sim`). `.neg`, `.abs` act on the operand;
`.repeat2.incsXYZ` advances `a`, `b`, `c` as `fmad` does (probes `fmad16-lanes`, `fmad16-incs`,
`fmad16-specials`, `fmad16-rounding`; `design/sgx545_hw_spec.md` §14).

**Undecided:** a denormal or NaN `binary32` behind `.swzf32` (flushed or not).
