<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Port writes: `movs`, `movsa`

Opcodes 0 (`movs`) and 5 (`movsa`) write four source words to a port (`w[3:0]`, *The output
ports*); ports 10-14 do not decode **[RE]**.

- **`movs port, a, b, c, d`** issues the four words **[HW]**; every program we run ends its
  work with one or more (`doutd` the data, then `doutu` the task).
- **`movsa`** adds its third source to the DMA address of a `doutd` **[CAP/HW]**: our vertex
  program fetches the vertex at `base + index × stride` with
  `movsa doutd, ds0[0], ds0[1], ds1[50], ds1[50]`, where `ds1[50]` holds `index × stride`.
  What `movsa` does on other ports is **[open]**.

**In the microkernel's programs [CAP].** Every path through the five programs ends in a
`movs doutu` that starts a USSE task, then `halt`: one in the boot programs, four in the
event program (one per case), three in the deferred-work program. The program that starts
`edm.task.pds11` first `doutd`s the TA/3D control block into the secondary attributes; the
event and deferred-work programs pass their inputs on with `douta` (an input register or the
timer, written as the low half `ir0.l`, `ir1.l`, `tim.l` in every slot).

<!-- @prog pds_vtx.s match='movs' -->

<!-- @group pds pat='^\{0\}movsa? ' -->
