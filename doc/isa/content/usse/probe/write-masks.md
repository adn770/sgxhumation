<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Write masks

*Measured on the hardware (2026-10-22).* The destination write mask (`.x`, `.y`, `.z`, `.w` and
combinations, `enc[47:44]`) **is a repeat of four with iterations left out: component k writes
register `d + k`, computed from every register source `+ k`; an immediate stays the same, and the
lone `.` (mask 0) writes nothing** **[HW]**. `mov r10.xz, r1` writes `r10 = r1` and `r12 = r3`,
`frcp r10.y, r1` writes `r11 = 1/r2`; a component is not a byte. All 16 masks on `mov`, `or`,
`and` with an immediate and `frcp`, and masks on `movc` and on a pack together with its byte
mask, agree (probes `wmask-mov`, `wmask-or`, `wmask-and-imm`, `wmask-frcp`, `wmask-other`;
`design/sgx545_hw_spec.md` §14). The firmware **[RE]** and our programs use only the default `.x`.

**Undecided:** the test group's masks (with a predicate result) and the 540's reading; the same
probe would settle them.
