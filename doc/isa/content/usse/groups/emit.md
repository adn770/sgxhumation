<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Emits: `emitpix1`, `emitpix2`, `emitvcb*`, `emitmte*`, `emitpds`

Class 31, other group, operation 3 **[RE]**. An emit hands words (and, by its kind, the task's
output registers) to another unit. All take an `#incp` number (three bits) and `.nosched`,
`.end` and `.freep`; the sources are register operands, and the pixel and PDS emits end with a
14-bit sideband immediate (scattered over `enc[27:22]`, `enc[40:35]` and `enc[55:54]`).

- **`emitpix1 #incp, s0, s1`** and **`emitpix2 #incp, s0, s1, s2, #sideband`** feed the pixel
  back end (PBE) **[HW]**: our end-of-tile program sends PBE words 0 and 1 (the clip
  rectangle) with `emitpix1`, then words 2 and 3 (count and line stride, the surface address)
  with `emitpix2`, whose sideband carries the pack mode (bits 5-0) and `TILERELATIVE`
  (bit 9); its third source is not used by that emit. The end-of-render program sends an
  `emitpix2` with zero words and sideband `0x200` (`design/sgx545_transfer.md` §4.2.4-§4.2.5).
- **`emitvcbvtx`** hands the vertex in the outputs to the vertex cache and **`emitmtevtx`** to
  the MTE; **`emitvcbst`** and **`emitmtest`** do the same for a state block **[HW]**: our
  first triangle's vertex shader and state tasks (`design/sgx545_3d.md` §14.2). `.twopart`
  and `.threepart` match the number of phases or parts of the task in the captured programs
  **[CAP]**; their exact effect and `.cut` are **[open]**.
- **`emitpds.tasks.taske #incp, s0, s1, s2, #sideband`** starts a PDS program **[CAP/RE]**:
  the microkernel's boot task launches PDS programs #11 and #12 this way and its request path
  #14; the control word carries the program's data size in four-dword units at bits 27-23
  (`design/sgx545_hw_spec.md` §13.7-§13.9). `.tasks`/`.taske` (task start and end?) are
  **[open]**.
- **`.freep`** frees the task's resources after the emit **[infer]**; **`#incp`** is always 0
  in our programs and its meaning is **[open]**.
- The assembler writes the unprinted source of `emitvcbst`/`emitvcbvtx` and the unprinted
  bank bit of `emitmtest` as the closed compiler does, so that the hardware sees the words it
  was tested with (`tools/usse-asm/README.md`).

**In the firmware [RE].** Three `emitpds`, all `.tasks.taske`; no other emit (the firmware
does not render).

<!-- @prog use_eot.s match='emit' -->

<!-- @prog use_vtx.s match='emit' -->

<!-- @group usse class=31 pat='^emit' -->
