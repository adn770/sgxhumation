# Writing USSE by hand

This tutorial walks through the USSE programs that run our transfer queue and our first
triangle on the real SGX545: a solid fill, a copy, the end-of-tile and end-of-render programs,
a vertex shader in two phases, and the template we test single instructions with. Each chapter
shows the program as it is in `tools/usse-asm/progs/`, explains it line by line, and runs it on
the simulator; the code and the runs in this document are generated from the files and the
tools, so they cannot drift from them.

It assumes the machine model of the programmer's guide (`design/sgx545_usse_guide.md`: the
register banks, predicates, phases and the assembler syntax); every instruction used here has a
section there, and its encoding is in the reference (`design/sgx545_usse_isa.md`).

The tools are those of `crates/sgx-tools` (or their C twins in `tools/`): `usse-asm` assembles a
source into raw words, `usse-dis` prints them back, and `sgx-sim` runs a program and prints what
it wrote. Build them with `cargo build --release -p sgx-tools`.

> **Generated file.** This document is assembled by `doc/isa/build.py` from the snippet library
> in `doc/isa/` (see `doc/isa/README.md`). Edit the snippets, not this file, and rebuild; the
> test `doc/isa/test_build.py` fails while the committed copy is stale. Bit diagrams, field
> lists, form lists and encoding examples are generated from the form tables of our clean-room
> disassemblers and assemblers (`tools/usse-dis`, `tools/usse-asm`), so they cannot drift from
> the tools; the prose around them is handwritten.

## 1. A solid fill

A transfer fill draws a rectangle in one colour. The ISP covers the rectangle with a
background object, and the pixel shader runs once for every pixel of it, as one instance of a
task (`design/sgx545_transfer.md` §4.2.2). Its job is a single write: the colour, into output
register `o0`, which the end-of-tile program later packs into the surface.

From `tools/usse-asm/progs/use_fill.s`:

```
; use_fill.s -- the solid-fill pixel shader (transfer template USE_FILL, id 0).
;
; The ISP background object covers the fill rectangle, and this program runs
; once per pixel of it (design/sgx545_transfer.md 4.2.2). It writes the fill
; colour to output register o0; the end-of-tile program (use_eot.s) then packs
; o0 into the surface with pack mode 0, so the colour is the 0xAARRGGBB dword.
;
; Per transfer, the driver patches the colour into the immediate of the second
; instruction (sgx545_fw_format.md 9.4: PARAM 0, a USSE_LIMM patch at byte 8).
; Each colour therefore needs its own copy of the program. The template keeps
; 0 there.

        ; The phase marker, the first instruction of every pixel program
        ; here: next-phase program #0 with #0 temporaries, pixel rate, wait
        ; condition "end", instances in parallel. [infer] "end" with #0 means
        ; no further phase follows.
        phas #0, #0, pixel, end, parallel

        ; o0 = the colour. A long immediate: the 32 bits are scattered over
        ; the word (the USSE_LIMM split). .skipinv runs it for every
        ; instance, .end ends the program.
        mov.skipinv.end o0, #0x00000000
```

Two instructions:

- **`phas`** opens the program's only phase. Every pixel program here starts with one; its
  operands name the next phase's program and temporaries (none), the rate (`pixel`), what the
  next phase would wait for (`end`: there is none) and that instances run in `parallel`.
- **`mov.skipinv.end o0, #imm`** writes the colour. A 32-bit immediate does not fit one field: the
  encoder scatters it over the word, as the generated example below shows. `.skipinv` is set as on
  every instruction our pixel programs must run regardless of coverage (what it does exactly is
  inferred: *Scheduling flags* in the guide), and `.end` makes it the program's last.

`mov.skipinv.end o0, #0xFF00FF00` assembles with form #0: enc[63:32] = `0xFCA7F181`, enc[31:0] =
`0x0000FF00`; in memory (little endian) `00 FF 00 00 81 F1 A7 FC`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `skipinv` | 0x1 | `.skipinv` |
| `nosched` | 0x0 | (none) |
| `end` | 0x1 | `.end` |
| `dst` | 0x80 | `o0` |
| `imm` | 0xff00ff00 | `0xFF00FF00` |

The driver writes each fill's colour into that immediate before it submits the program
(`design/sgx545_fw_format.md` §9.4), so every colour needs its own copy. Run with a colour in
place of the template's 0, the program leaves `o0` as the colour and ends:

```
$ cat prog.s
phas #0, #0, pixel, end, parallel
mov.skipinv.end o0, #0xff00ff00
$ sgx-sim prog.s
ended after 2 steps
o0 = 0xff00ff00
p = 0000 (p3-p0)
```

## 2. Reading what the PDS fetched

A copy reads a source surface. The task's data comes from the PDS, the data sequencer that
starts every USSE task: for a copy its program (`pds_prim_1src.s`) has the texture unit fetch the
source texel of each pixel into primary attribute `pa0` and makes the task wait for it. The
shader only passes the texel on (`design/sgx545_transfer.md` §4.4.3):

From `tools/usse-asm/progs/use_copy.s`:

```
; use_copy.s -- the copy pixel shader (template USE_COPY, id 7).
;
; For a copy, the PDS primary program (pds_prim_1src.s) has the texture unit
; fetch the source texel of each pixel into primary attribute pa0, and makes
; this task wait for it (DOUTU U1 TEXTUREDEPENDENCY). The shader passes the
; texel on to o0; the end-of-tile program packs it as for a fill
; (design/sgx545_transfer.md 4.4.3). Never patched: one copy serves every blit.

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        mov.skipinv.end o0, pa0                 ; o0 = the fetched texel; end
```

The primary attributes `pa0`-`pa127` are per instance and filled before the task runs; here the
simulator takes them from `--set`. A register no one has written is not defined, and the
simulator stops on a read of one rather than guess its value:

```
$ sgx-sim --set pa0=0x80402010 use_copy.s
ended after 2 steps
pa0 = 0x80402010
o0 = 0x80402010
p = 0000 (p3-p0)
```
```
$ cat prog.s
mov.skipinv.end o0, pa1
$ sgx-sim prog.s
stopped after 1 steps
p = 0000 (p3-p0)
unsupported: 0: mov.skipinv.end o0, pa1: reads pa1 before the program writes it
```

## 3. Leaving a tile

The pixel programs above only fill output registers. When a tile is finished, the end-of-tile
program hands those registers to the pixel back end (PBE), with the five state words that say
where and how to write them (`design/sgx545_transfer.md` §4.2.5):

From `tools/usse-asm/progs/use_eot.s`:

```
; use_eot.s -- the end-of-tile program (template USE_EOT, id 2).
;
; Runs when a tile is finished and hands the tile's output registers to the
; pixel back end (PBE) with the five PBE state words of the destination
; (design/sgx545_transfer.md 4.2.5): emitpix1 sends words 0 and 1, emitpix2
; words 2 and 3 plus the sideband word 4 as an immediate. The task gets four
; temporaries (TQ_META temps 4) for words 0-3.
;
; Per transfer the driver patches (sgx545_fw_format.md 9.4):
;   - the destination register of the movs at bytes 16, 24 and 32 to r1, r2
;     and r3 (CONST 1 << 21, 2 << 21, 3 << 21 added to the word): the closed
;     generator adds the register number in a loop, so the template's four
;     movs all name r0 and so do ours, to reproduce it bit for bit;
;   - their immediates to PBE words 0-3 (PARAM 1-4, USSE_LIMM patches);
;   - the sideband of the emitpix2 (PARAM 5, EMITPIX_SB: value bits [5:0]
;     to e[27:22], [11:6] to e[40:35], [13:12] to e[55:54]).

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s

        ; Load words 0-3 into r0-r3 (the last three r0 until patched).
        mov.skipinv r0, #0x00000000             ; r0 = word 0: XMIN [12:0], YMIN [28:16]
        mov.skipinv r0, #0x00000000             ; -> r1 = word 1: XMAX, YMAX (inclusive)
        mov.skipinv r0, #0x00000000             ; -> r2 = word 2: COUNT [15:8], LINESTRIDE [27:16]
        mov.skipinv r0, #0x00000000             ; -> r3 = word 3: FBADDR [31:2]

        ; Emit to the PBE: incp #0, sources r0 and r1 (words 0 and 1).
        ; [infer] .freep frees the task's resources once the emit is done.
        emitpix1.freep #0 /* incp */, r0, r1

        ; Words 2 and 3 from r2 and r3, the sideband word as an immediate
        ; (PACKMODE [5:0], TILERELATIVE [9]: 0x200 for ARGB8888), and end.
        ; The third source (r0) is not used for this emit.
        emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000000
```

- The four `mov`s load PBE words 0-3 into temporaries. The task is given four temporaries
  (its `temps` in the transfer's meta data); a program that used more would reach other
  instances' registers.
- **`emitpix1`** and **`emitpix2`** send the words to the PBE: the first words 0 and 1, the
  second words 2 and 3 and, as its last immediate, the sideband word 4 (the pack mode and
  `TILERELATIVE`). `.freep` frees the task's resources once the emit is done, and the `.end`
  of the second ends the program.
- The driver patches this program per transfer: the immediates become the PBE words, and the
  destination registers of the last three `mov`s, r0 in the template, become r1-r3. The
  template keeps r0 because the closed generator builds it that way, and ours reproduces its
  words bit for bit (`design/sgx545_fw_format.md` §9.4).

Patched by hand, with made-up PBE words, the simulator shows the two emits and the values each
sends; it models what the USSE hands over, not what the PBE does with it:

```
$ sgx-sim tutorial/eot_patched.s
ended after 7 steps
r0 = 0x00000000
r1 = 0x001f003f
r2 = 0x01001000
r3 = 0x12340000
p = 0000 (p3-p0)
emit emitpix1 0x00000000, 0x001f003f, 0x00000000
emit emitpix2 0x01001000, 0x12340000, 0x00000000
```

## 4. Ending a render, and the secondary program

Two more programs complete a transfer. After the last tile of a render, the end-of-render
program sends one more `emitpix2`, with zero words and the sideband `0x200`, which the closed
driver issues to finish the render; the secondary program, which the PDS kicks once per
transfer, has nothing to do and ends (`design/sgx545_transfer.md` §4.2.4):

From `tools/usse-asm/progs/use_eor.s`:

```
; use_eor.s -- the end-of-render program (template USE_EOR, id 3).
;
; Runs once after the last tile of a render: an emitpix2 to the PBE with zero
; words and sideband 0x200, which the closed driver issues to finish the
; render (design/sgx545_transfer.md 4.2.4). One temporary (TQ_META temps 1).
; Never patched.

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        mov.skipinv r0, #0x00000000             ; r0 = 0
        ; Emit r0 (0) and the immediate #0 as words 2 and 3, sideband 0x200; end.
        emitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200
```
From `tools/usse-asm/progs/use_sec.s`:

```
; use_sec.s -- the USE secondary program of a transfer (template USE_SEC, id 1).
;
; The PDS secondary program (pds_sec.s) kicks this once; a transfer has no
; per-tile secondary work, so it only ends (design/sgx545_transfer.md 4.2.4).

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        nop.end                                 ; nothing to do: end the program
```

`nop.end` is the shortest program there is: one phase marker, then an instruction that only
ends. Programs always end on an instruction with `.end`; one that runs past its last word
would execute whatever follows it in memory.

## 5. A vertex shader in two phases

The first triangle's vertex shader (`design/sgx545_3d.md` §9.2) gets one vertex, already in clip
space, in `pa0`-`pa3`, copies it to the outputs `o0`-`o3`, and hands it on twice: to the
vertex cache, then, once vertex culling has seen it, to the MTE. Those are two phases of one
task, the second waiting for `vcull`:

From `tools/usse-asm/progs/use_vtx.s`:

```
; use_vtx.s -- the vertex shader of the first triangle (design/sgx545_3d.md S.1, 9.2).
;
; pds_vtx.s DMAs one vertex (x, y, z, w, already in clip space) into pa0-pa3 and starts
; this task. It copies the position to the outputs o0-o3 and hands the vertex first to
; the vertex cache (emitvcbvtx) and then, once the vertex culling stage has seen it, to
; the MTE (emitmtevtx): two phases, the second waiting for "vcull" [CAP: the shape of
; every vertex program the closed compiler emits].
;
; The phas operand is the instruction index of the next phase, counted from the code
; base the task runs from (USE_CODE_BASE_10). This source writes it relative to the
; program's first instruction; the driver adds the index at which it places the
; program (sgx_tri.c).

        ; Phase 0: wait for the attribute DMA, copy the position, emit to the VCB.
        phas #7, #0, pixel, vcull, parallel
        nop.nosched
        ; A load that never runs (p0 is false: 0 & 0 is zero) followed by a wait on the
        ; data fence: the task does not read pa0-pa3 before the PDS DMA has landed.
        and.skipinv.testtanz.chan0 !r0, p0, #0x00000000, #0
        p0 ldad.skipinv.fetch4 pa0, [pa0,+#0], drc0
        wdf drc0
        mov.skipinv.repeat4 o0, pa0                     ; o0-o3 = x, y, z, w
        emitvcbvtx.end.twopart #0, #0
        ; Phase 1 (instruction 7): emit the vertex to the MTE and end.
        phas #0, #0, pixel, end, parallel
        emitmtevtx.end.twopart.freep #0, #0, #0
```

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

```
$ sgx-sim --set pa0=1 --set pa1=2 --set pa2=3 --set pa3=4 use_vtx.s
ended after 7 steps
pa0 = 0x00000001
pa1 = 0x00000002
pa2 = 0x00000003
pa3 = 0x00000004
o0 = 0x00000001
o1 = 0x00000002
o2 = 0x00000003
o3 = 0x00000004
p = 0000 (p3-p0)
emit emitvcbvtx 0x00000000, 0x00000000, 0x00000000
```

## 6. Testing an instruction yourself

What an instruction does is settled on the hardware, not in the text. The probe harness
(`tools/usse-asm/probe.py`) runs a case as the pixel shader of a 64 x 32 fill on the real GPU:
a template sets inputs, runs the instruction under test, and stores registers to a scratch
buffer the host reads back. This is the template most cases use:

From `tools/usse-asm/progs/probes/dump.s`:

```
phas #0, #0, pixel, end, parallel
mov.skipinv r1, #{A}
mov.skipinv r10, #0xA0A1A2A3
{OP}
mov.skipinv r29, #0x5CAF0000
stad [r29,+#0], r10
stad [r29,+#8], r1
```

`{OP}` is the instruction under test, and `#0x5CAF0000` a marker the harness replaces by the
scratch buffer's address; the simulator does the same, so the template runs on it unchanged.
Here it asks how `fmad` rounds a tie: 1 + 2^-12 squared, written to `r10` and stored:

```
$ sgx-sim tutorial/fmad_tie.s
ended after 5 steps
r1 = 0x3f800800
r10 = 0x3f801000
r29 = 0x3e003000
o0 = 0x3f801000
p = 0000 (p3-p0)
[0x3e003000] = 0x3f801000 (was 0xd0000000)
```

The product, 1 + 2^-11 + 2^-24, is a tie between two binary32 values; `fmad` rounds it to even
(*Float rounding and NaN*). On the hardware, the same case is a line of a probe table in
`tools/usse-asm/progs/probes/`; `probe.py run` sends it to the GPU, and
`tools/usse-asm/vectors.py` turns the readback into a vector the simulator is tested against
(`design/sgx545_vectors.md`), so a rule found this way is held to the hardware from then on:

```
$ cargo run --release -p sgx-tools --bin sgx-sim -- --vectors data/vectors/vec1.vec.json
```

## 7. Where to go next

- **The programmer's guide** (`design/sgx545_usse_guide.md`) for every group of instructions,
  with what the hardware was measured to do and what the simulator models.
- **The reference** (`design/sgx545_usse_isa.md`) to encode or decode a word by hand: every
  group's bit layouts and fields, and every form.
- **The PDS reference** (`design/sgx545_pds_isa.md`) for the programs that start these tasks
  (`tools/usse-asm/progs/pds_*.s`).
- **The open questions** at the end of the reference: what is not settled yet, and what the
  simulator stops on.
