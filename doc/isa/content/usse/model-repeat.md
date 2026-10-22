<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Repeats and the MOE state

Many instructions can **repeat**: one instruction stands for N consecutive operations on
consecutive registers. Where a group allows it, `enc[53]` turns the write-mask field
`enc[47:44]` into a repeat count (`.repeat2`-`.repeat16`, the field value plus one); the float
groups have a two-bit count (`.repeat2`-`.repeat4`), the multiply-add and colour groups a
three-bit one (`.repeat2`-`.repeat8`) **[RE]**. Loads and stores use the same count either as
a repeat or, with the MOE-expand bit `enc[53]` clear, as a **fetch** of N registers
(`.fetch1`-`.fetch16`, see *Loads and stores*).

- **What a plain repeat does [HW].** `mov.skipinv.repeat4 o0, pa0` copies `pa0`-`pa3` to
  `o0`-`o3`: our vertex shader writes the position this way, our state tasks copy 16 and 2
  registers with `.repeat16` and `.repeat2`, and the outputs reach the MTE intact. So with
  the default MOE state each operand register number advances by one per iteration. The
  iterations run one after the other in ascending order, so overlapping ranges propagate,
  and one predicate covers the whole instruction (*Repeat order and overlapping ranges*,
  *Predicates on repeated instructions*).
- **The MOE state.** The register advance per iteration is programmable: the MOE group sets
  per-operand increments or swizzles (`smlsi`), base offsets (`smbo`), address modes and
  offsets (`smoa`) and ranges (`smr`) **[RE]**: the reference disassembler names the state
  words they write (*SMOA*, *SMLSI*, *SMBO*, *SMR* low and high data, and an index word) when
  `moest` reads them back. How each value acts on the operand numbers is **[infer]** from those
  names, except that `smlsi`'s four increments are the destination's and sources 0, 1 and
  2's **[HW]** (*`smlsi` increments*).
- **`.incsXYZ`.** The float groups carry three increment bits per instruction when they
  repeat (`.incs000`-`.incs111`): X, Y and Z let source 0, 1 and 2 advance, the destination
  always does, and without `.incs` everything does **[HW]** (*`.incsXYZ`*).
- **Write masks** are repeats of four with iterations left out: component k writes `d + k` from
  every register source `+ k` **[HW]** (*Write masks*).
- **Open:** what a repeated test writes into its predicate, repeats after the other MOE
  instructions, `.partial` and `.onceonly` (*Hardware probe results*).

**In the firmware [RE].** The microkernel never repeats an ALU instruction (no `.repeatN` at
all); it moves blocks with fetches instead (120 `.fetchN` loads and stores, mostly
`.fetch2`). It executes `smlsi` eight times, each time at the start of a task entry point and
each time with the same operands: increments 0, 0, 0 and 1 in increment mode, limits 0. It
uses no other MOE instruction.
