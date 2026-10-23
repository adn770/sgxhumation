<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `smlsi` increments

*Measured on the hardware (2026-10-22).* `smlsi #I0, #I1, #I2, #I3, ...` sets four signed 8-bit
increments **[RE]**: before `mov.repeat2 r20, r10`, **`I0` is the destination's increment and `I2`
the source's** (0 repeats a register, 2 skips one); `I1` and `I3` change nothing there **[HW]**
(probe `repeat-smlsi`; `design/sgx545_hw_spec.md` §14). **On a three-source repeat (`fmad.repeat2`)
`I1` is source 0's increment, `I2` source 1's and `I3` source 2's** **[HW]** (round 4, probe
`smlsi-three-sources`); `mov`'s source is source 1. With the firmware's `0, 0, 0, 1` at every task
entry a following `mov` repeat would hold its destination and its source still; the firmware repeats
nothing itself, so what its 1 is for stays **[open]**.

The recorded `isa4/left4` run of `smlsi-three-sources` used the table's first version, which began
with a case `(1, 1, 1, 1)` (dropped in 2b5cfd2, `(1, 1, 2, 2)` appended): each of its readbacks is
the previous case's, and with that shift every case fits the rule above.

**Undecided:** swizzle mode, the limits, and how long the state lasts.
