<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `smlsi` increments

*Measured on the hardware (2026-10-22).* `smlsi #I0, #I1, #I2, #I3, ...` sets four signed 8-bit
increments **[RE]**: before `mov.repeat2 r20, r10`, **`I0` is the destination's increment and `I2`
the source's** (0 repeats a register, 2 skips one); `I1` and `I3` change nothing there **[HW]**
(probe `repeat-smlsi`; `design/sgx545_hw_spec.md` §14). `I1` and `I3` are presumably the other two
sources' **[infer]**. With the firmware's `0, 0, 0, 1` at every task entry a following `mov` repeat
would hold its destination and its source still; the firmware repeats nothing itself, so what its 1
is for stays **[open]**.

**Undecided:** `I1`/`I3` on a three-source instruction, swizzle mode, the limits, and how long the
state lasts; the same probe with `fmad.repeat2` and with a later task would settle them.
