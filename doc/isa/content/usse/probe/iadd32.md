<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The 32-bit adds `iadd32` and `iaddu32`

*Measured on the hardware (2026-10-22).* **`iadd32` and `iaddu32` are not full 32-bit adds**
**[HW]**: they add **source 1's low 16 bits**, sign-extended (`iadd32`) or zero-extended
(`iaddu32`), to the 32-bit source 2, modulo 2^32. `0x12345678 + 0x11111111` gives `0x11116789` and
`iadd32 0xffff + 1` gives 0, while `#1 + r` is the plain increment the firmware uses
(`iaddu32 r3, p0, #1, r3`). The test reads the 32-bit sum: `z` and `n` as usual for `iadd32`;
`iaddu32` is unsigned, its `n` is never and its `p` always true, also for `0x80000000` (probes
`iadd32-value`, `iaddu32-value`, `iaddu32-imm`, `iadd32-src1-imm`, `iadd32-z`, `iadd32-n`,
`iadd32-p`, `iaddu32-tests`, `iaddu32-np`; `design/sgx545_hw_spec.md` §14).
