<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## A solid fill

A transfer fill draws a rectangle in one colour. The ISP covers the rectangle with a
background object, and the pixel shader runs once for every pixel of it, as one instance of a
task (`design/sgx545_transfer.md` §4.2.2). Its job is a single write: the colour, into output
register `o0`, which the end-of-tile program later packs into the surface.

<!-- @prog use_fill.s -->

Two instructions:

- **`phas`** opens the program's only phase. Every pixel program here starts with one; its
  operands name the next phase's program and temporaries (none), the rate (`pixel`), what the
  next phase would wait for (`end`: there is none) and that instances run in `parallel`.
- **`mov.skipinv.end o0, #imm`** writes the colour. A 32-bit immediate does not fit one field: the
  encoder scatters it over the word, as the generated example below shows. `.skipinv` is set as on
  every instruction our pixel programs must run regardless of coverage (what it does exactly is
  inferred: *Scheduling flags* in the guide), and `.end` makes it the program's last.

<!-- @example usse mov.skipinv.end o0, #0xff00ff00 -->

The driver writes each fill's colour into that immediate before it submits the program
(`design/sgx545_fw_format.md` §9.4), so every colour needs its own copy. Run with a colour in
place of the template's 0, the program leaves `o0` as the colour and ends:

<!-- @sim -- phas #0, #0, pixel, end, parallel | mov.skipinv.end o0, #0xff00ff00 -->
