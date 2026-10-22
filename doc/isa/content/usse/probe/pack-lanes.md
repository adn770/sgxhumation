<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Pack and unpack lanes

*Measured on the hardware (2026-10-22).* A pack or unpack converts **two values, source 1 and
source 2, and lays them out in the destination format** **[HW]** (probes `pack-u8-bytemask`,
`pack-u16-bytemask`, `pack-c10`, `unpack-select`, `wmask-other`; `design/sgx545_hw_spec.md`
§14):

- **8-bit results** (`u8`, `s8`, `o8`): bytes 0-3 take source 1, 2, 1, 2. **`.bytemaskXXXX`**
  enables byte N with bit N (the rightmost digit is byte 0); a disabled byte keeps its value, and
  an enabled byte takes source 1 or 2 by this rule (bN: byte N enabled): byte 0 source 1, byte 1
  `s[b0]`, byte 2 `s[b0 xor b1]`, byte 3 `s[b0 or b1 or b2]`, where `s[0]` is source 1. So
  `.bytemask0011` writes source 1 and 2 into bytes 0 and 1, `.bytemask1100` into bytes 2 and 3.
- **16-bit results** (`u16`, `s16`, `f16`): the low half source 1, the high half source 2; under
  a byte mask the high half takes source 2 only when byte 0 is enabled.
- **`c10`**: three 10-bit lanes at bits 0, 10 and 20 take source 1, 2, 1; bits 30-31 are 0.
- **Component selects** read a source: byte `c` for 8-bit formats; for 16-bit formats `.0` bits
  0-15, `.1` bits 8-23, `.2` bits 16-31, `.3` nothing (0); the `c10` lane `c` (`.3` is 0); an
  `f32` source ignores its select. An unpack to `f32` converts source 1 only.
- **The write mask** and **`.repeatN`** advance the register sources as everywhere (*Write
  masks*).

**`statomic d, s1.c, s2.c`** (format value 8) writes, in both 16-bit halves, byte `c` of `s1` in
the low byte and byte `c` of `s2` in the high one, each read as a signed byte with negatives
made 0 (`0x80`, `0x7f` give `0x7f00`); the rounding mode and `.scale` change nothing (probe
`statomic`, 2026-10-22). Round 1's `0x78007800` was such a value.

**Undecided:** a byte mask on `c10` results, `.1`/`.3` of an `f32` source on `unpckf32f32`
(`.1` read a shifted value), and what `statomic` is for.
