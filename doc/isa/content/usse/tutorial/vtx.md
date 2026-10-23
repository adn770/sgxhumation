<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## A vertex shader in two phases

The first triangle's vertex shader (`design/sgx545_3d.md` §9.2) gets one vertex, already in clip
space, in `pa0`-`pa3`, copies it to the outputs `o0`-`o3`, and hands it on twice: to the
vertex cache, then, once vertex culling has seen it, to the MTE. Those are two phases of one
task, the second waiting for `vcull`:

<!-- @prog use_vtx.s -->

- **`phas #7, ...`** names the next phase's first instruction, index 7 here. The index counts
  from the code base the task runs from, so the driver adds where it places the program.
- **The predicated load and `wdf`.** `and.testtanz` with two zero immediates sets `p0` false;
  `p0 ldad` therefore never loads, but it gives `wdf drc0` an access to wait for, which is the
  shape the closed compiler uses so that the task does not read `pa0`-`pa3` before the PDS's
  attribute DMA has landed.
- **`mov.repeat4 o0, pa0`** is four moves in one instruction, `o0 = pa0` to `o3 = pa3`: a
  repeat advances every register operand by one per iteration (*Repeats* in the guide).
- **`emitvcbvtx`** ends phase 0; phase 1 is `emitmtevtx` with `.end`.

The simulator runs a task from its first instruction to the first `.end`, so here phase 0:
with a vertex in `pa0`-`pa3`, it ends at the vertex cache emit:

<!-- @sim --set pa0=1 --set pa1=2 --set pa2=3 --set pa3=4 prog=use_vtx.s -->
