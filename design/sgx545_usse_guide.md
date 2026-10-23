# SGX545 USSE programmer's guide

The **USSE** (universal scalable shader engine) is the programmable core of the PowerVR SGX
Series 5: on the SGX545 of Intel's Cedar Trail it runs every vertex, pixel and state program of
a scene, and the firmware. This guide is for writing and reading those programs: the machine a
program runs on, its operands, the assembler syntax, what every group of instructions does, and
what the hardware was measured to do.

It is the reference's text without its encodings. The instruction set reference
(`design/sgx545_usse_isa.md`) is built from the same snippets and adds, for every group, the bit
layouts, field lists and an appendix of every form; read it to decode or encode a word by hand.
Both state behaviour with the conventions below, and both end every group with what the
simulator (`crates/sgx-sim`) models of it, checked against the hardware vectors: a rule the
simulator holds is one a program can rely on, and one it stops on is still open.

> **Generated file.** This document is assembled by `doc/isa/build.py` from the snippet library
> in `doc/isa/` (see `doc/isa/README.md`). Edit the snippets, not this file, and rebuild; the
> test `doc/isa/test_build.py` fails while the committed copy is stale. Bit diagrams, field
> lists, form lists and encoding examples are generated from the form tables of our clean-room
> disassemblers and assemblers (`tools/usse-dis`, `tools/usse-asm`), so they cannot drift from
> the tools; the prose around them is handwritten.

## 1. Conventions

### 1.1 Where each fact comes from

This is a reconstruction, not a vendor document. Every statement about what an instruction
*does* carries a tag, as in `design/sgx545_hw_spec.md`:

- **[HW]** measured on the laptop: our own programs or probes ran and behaved as stated.
- **[CAP]** captured from the closed driver running under the extraction harness (its
  programs, the words it writes), so exact for that build.
- **[RE]** read from the closed microkernel's code with our disassembler, or established by
  black-box probing of the reference disassemblers (bit flips and value sweeps; the source of
  those tools was never used for the layouts). Encodings and assembler spellings are **[RE]**
  in this sense: the reference disassembler prints them, our decoder matches it on every
  word (`WRONG` = 0), and nothing more is implied about behaviour.
- **[DDK]** read in the GPL DDK sources (register names, structure layouts).
- **[infer]** an inference from names, context or how the firmware uses an instruction;
  verify before relying on it.
- **[open]** not known. Said plainly rather than guessed.

Facts asked of the hardware with our probe programs are collected in one section, one snippet
per fact, marked *Measured on the hardware (2026-10-22)* (with what stays undecided) or *Not
probed yet*.

### 1.2 Clean room

Everything here was written from our own decoders, our own programs, the GPL DDK and our
hardware runs. No vendor ISA text is quoted. Examples are either our own programs
(`tools/usse-asm/progs/`) or instructions assembled fresh for this document; no instruction
word of the closed firmware appears. Where the firmware's use of an instruction is described,
it is described in words and counts, not quoted.

## 2. Machine model

A USSE program runs as a **task** over a set of **instances** (pixels of a batch, vertices,
or the single instance of a firmware or state task) **[infer]**. Each instruction is executed
for the instances of the task; the predicates and `.skipinv` decide which. The model below is
what our programs, the firmware and the captures show; the parts nobody has measured are
marked.

### 2.1 Register banks

| bank | names | what it holds |
|------|-------|---------------|
| temporaries | `r0`-`r123` (`r0`-`r127` as load destinations) | per-task scratch |
| primary attributes | `pa0`-`pa127` | per-instance inputs written by the PDS |
| secondary attributes | `sa0`-`sa127` | per-task (shared) inputs written by the PDS |
| outputs | `o0`-`o127` | results handed to the next unit by the emits |
| constants, globals | `c0`-`c63`, `g0`-`g63` | not established (one bank code; 64 up is `g`) |
| internal | `i0`-`i3` | the accumulators of `efo`, `fddp`, the dual issue and the carries |
| index | `i.l`, `i.h` (`il`, `ih` in operands) | the bases of indexed operands |
| link | `pclink` | the return address of a call |

- **Temporaries [HW].** The number of temporaries a task gets is set when it is started: the
  PDS's `doutu` word U0 carries it in fours (`TRC`, `design/sgx545_transfer.md` §4.2.4), and
  `phas` names it for the next phase. Our end-of-tile program asks for four and uses `r0`-`r3`.
  Temporaries are **not cleared** between tasks: a client shader that reads one before
  writing it sees what an earlier task left there, firmware data included
  (`design/sgx545_render_uapi.md` §7 Q4).
- **Primary attributes [HW].** The PDS fills them per task: `doutd` DMAs memory into them
  (the vertex fetch of our triangle lands in `pa0`-`pa3`), `douti` has the iterator write
  them and `doutt` returns a texture fetch into them (our copy shader reads its texel from
  `pa0`). Programs may also write them: our MTE state tasks load into `pa0` again.
- **Secondary attributes [CAP/RE].** Shared by the instances of a task and written by a
  secondary PDS program. The firmware keeps its base pointers there (`sa0`-`sa2`, DMAed from
  the TA/3D control block at boot) and some of its state (`sa8`, `sa9`, `sa19`), and it does
  write them (`design/sgx545_hw_spec.md` §13.9, `design/sgx545_3d.md` §12.1).
- **Outputs [HW].** Pixel programs leave the colour in `o0` for the end-of-tile emit; the
  vertex shader of our triangle writes `o0`-`o3`, and the state tasks copy whole state blocks
  through `o0`-`o17` before emitting them.
- **Constants and globals [open].** What `c` and `g` hold, and whether `g` is writable, is not
  established. The firmware reads two globals once each (as a source of an `xor` and of a
  `shl`) and otherwise names `c0` only as the dummy destination of a test that does not write
  its result (`!c0`, see the test group) **[RE]**.
- **Internal registers [RE].** `i0`-`i3` print where a temporary number reaches past the
  temporaries (bank 0, numbers 124-127; in the half-float and c10 readings, earlier). They
  are the accumulators of the dual-issue and `efo` groups and the carry registers of
  `imae`/`ima32`. **[HW]** `i0`-`i2` are per instance, hold what `mov` or `fddp` writes and keep it
  from one task to the next, also another client's; `i3` reads `i0` of the next instance of
  the 2x2 quad and drops writes, and `efo.gpiext` reaches across the quad (*Internal
  registers*, *`efo`*).
- **Index registers [RE].** A destination bank writes `i.l`, `i.h` or both (`i.lh`); an
  indexed operand `r[il + #n]` names a register of a bank (`r`, `o`, `pa`, `sa`) by index plus
  offset. The firmware loads `i.l` from a temporary 79 times and uses 79 indexed operands,
  all on the `sa` bank, as tables kept in the secondary attributes **[RE]**. **[HW]**
  `r[il + #n]` and `sa[il + #n]` are the bank's register `i.l + n`, counted from `r0`/`sa0`,
  and bits 10 and up of `i.l` are ignored; `i.h` and `ih` work the same way, and `mov i.lh, s`
  loads `i.l` from the low and `i.h` from the high half of `s` (*Index registers*).
- **The register file.** SGX's own MMIO registers are reached with `ldr`/`str` by dword
  index, not through a bank (*Register file access*).

### 2.2 Predicates

There are four predicate registers, `p0`-`p3`, per instance **[RE]**. Most instructions take an
execution predicate in `enc[58:56]`, printed before the mnemonic:

| value | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|-------|---|---|---|---|---|---|---|---|
| prints | (always) | `p0` | `p1` | `p2` | `p3` | `!p0` | `!p1` | `Pn` |

The integer multiply-add, colour, dot-product and video groups have only a two-bit predicate
`enc[58:57]`: always, `p0`, `p1`, `!p0`; the dual-issue group has the same four in
`enc[57:56]`, and they act as printed on both operations **[HW]** (*Dual issue*).

- An instruction whose predicate is false does nothing for that instance (**[HW]**, if
  indirectly): our vertex and state programs issue a four-register load into `pa0`-`pa3`,
  predicated on `p0`, right after a test that leaves `p0` false; had it run, it would have
  replaced the vertex the PDS had just DMAed there, and the triangle renders exactly.
- Predicates are written by the **test group** (`p0`-`p3`, chosen by `enc[35:34]`), by
  `rlp` (a seven-bit predicate number) and by `atst8` (`p0`-`p3`, optionally negated)
  **[RE]**. **[HW]** `rlp`'s predicate numbers 4 and up alias `p0`-`p3` (*`rlp`*); whether
  `atst8` reaches more is **[open]**.
- `Pn` (value 7) **never runs** the instruction, whatever `p0`-`p3` hold **[HW]** (*`br`
  offsets and the `Pn` predicate*): the reference disassembler prints it for value 7 of the
  full field and nothing in the firmware or the captures uses it.
- A repeated instruction tests its predicate once, for the whole instruction **[HW]**
  (*Predicates on repeated instructions*).

**In the firmware [RE].** The microkernel's predicates guard branches above all: 591 of its
954 `ba` are predicated (`p0` 318, `!p0` 94, `p2` 76, `p1` 72, `!p1` 26, `p3` 5). A few dozen
stores, moves, loads and bitwise operations are predicated too. A typical sequence tests a
flag into `p0` without writing a register and branches on it.

### 2.3 Program counter, calls and the end of a program

- **The PC counts instructions** from the code base the task was started from **[HW]**. The
  PDS starts a task with a `doutu` word naming a code base (`USE_CODE_BASE_n`, n in
  `U0[3:0]`) and an instruction index; the SGX545 has 18 PC bits, which is also the width of
  the branch target fields **[CAP/RE]**.
- **`ba #n`** jumps to instruction `n` of the code base (absolute) **[RE/HW]**. **`br`** takes
  a signed 18-bit offset **counted from the `br` itself** (`br #1` is the next instruction)
  **[HW]** (*`br` offsets and the `Pn` predicate*); the assembler accepts only a number for
  `br`, not a label (`tools/usse-asm/README.md`).
- **Calls.** `ba.savelink` (and `br.savelink`) also write the return address to `pclink`;
  `lapc` jumps to `pclink`. `pclink` holds the **index of the instruction after the call**
  **[RE]**: the firmware builds return addresses as instruction indices (it loads an index
  into a temporary and moves it to `pclink` before a `lapc`), and the kernel CCB dispatch
  jumps to a handler the same way (`mov pclink, r4; lapc`, `design/sgx545_hw_spec.md`
  §13.10). There is one link register, so a routine that calls another saves `pclink` in a
  temporary first (`mov rN, pclink` ... `mov pclink, rN`) **[RE]**.
- **Branch flags [open].** `.nosched`, `.synce`/`.syncent` and `.mon` decode on `ba`, `br`
  and `lapc`; their effect is not established, and the firmware uses none of them.
- **`bexception` [open].** Decodes; never used.
- **The end of a program [HW].** A program ends at an instruction carrying `.end`, which
  still executes. There is no return to the PDS: every one of our programs ends this way
  (`mov.skipinv.end`, `nop.end`, `emitpix2.end.freep`, `ptoff.end`, ...). The firmware's tasks
  end in a shared routine with a `nop.end` or `mov.end` (eight `.end` in all).

**In the firmware [RE].** 954 `ba` (232 of them `.savelink` calls), 12 `br`, 74 `lapc`, 205
moves into `pclink` and 193 out of it.

### 2.4 Repeats and the MOE state

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

### 2.5 Scheduling flags

Most groups carry a handful of one-bit flags printed after the mnemonic. Their bit positions
are in each group's field list in the reference; what is known of their effect:

- **`.end`** ends the program after this instruction **[HW]** (*Program counter, calls and
  the end of a program*).
- **`.skipinv`** executes the instruction for every instance, invalid ones included
  **[infer]**: the name, and the fact that the closed compiler's pixel and state programs
  (and ours, which follow them) set it on the instructions that must run regardless of
  coverage. Whether leaving it clear really skips instances the hardware marks invalid is
  **[open]**; the firmware, whose tasks have a single instance, uses it only twice.
- **`.nosched`** is a scheduling hint, probably "do not switch to another task after this
  instruction" **[infer]**; it changes nothing a fill's pixels see **[HW]** (*`.tpres`,
  `.partial`, `.nosched`, `.syncs`*). Our programs carry it only where the captured programs do
  (`nop.nosched` before the DMA wait); the firmware uses it once, in the same idiom.
- **`.syncs`** decodes in most groups; **in a fill it hangs the task** **[HW]** (on `mov` and
  `fmad`), so it waits for something a fill never gives; what is **[open]**. Unused by the
  firmware and by us.
- **`.partial`** (the bitwise group) changes nothing a fill sees **[HW]**; **`.onceonly`** and
  `.partial` on the test group are **[open]**.
- **`.freep`** on the emits frees the task's resources once the emit is done **[infer]**:
  every final emit of our pixel and state programs carries it, as the captured ones do.
- **`.tog`** and **`.trigger`** (`nop`), **`.tpres`** (single-source floats), **`.gpiext`**
  (`efo`), **`.twopart`**/**`.threepart`**/**`.cut`** (the vertex and MTE emits) and
  **`.tasks`**/**`.taske`** (`emitpds`) are discussed with their groups; most are **[open]**.

The radare2 emulation of the firmware assumes that `.skipinv`, `.syncs`, `.nosched` and `.end`
do not change what one instance computes, and ignores them (`tools/usse-dis/r2/README.md`);
nothing contradicts that so far.

### 2.6 Tasks and phases

A task is started by the PDS (`doutu`, `design/sgx545_pds_isa.md`), by the microkernel's
`emitpds` (which runs a PDS program that starts one), or by the event machinery that runs the
microkernel's own PDS programs (`design/sgx545_hw_spec.md` §13.7-§13.8) **[CAP/RE]**. The
`doutu` words give the program's code base and first instruction, its temporaries and flags
such as "wait for the texture result" (`design/sgx545_transfer.md` §4.2.4, §4.4.3) **[HW]**.

A program can be split into **phases**. `phas` is the phase marker and is the first
instruction of every pixel, vertex and state program we run **[HW]**:

```
phas #<next>, #<temps>, <rate>, <wait>, <mode>
```

- `#<next>`: the instruction index (from the code base) where the next phase starts;
- `#<temps>`: the temporaries of the next phase, a multiple of four;
- `<rate>`: `pixel`, `selective` or `sample`;
- `<wait>`: what the next phase waits for: `none`, `pt`, `vcull` or `end` (no next phase);
- `<mode>`: `parallel` or `perinstance`.

**[HW]** for what our programs do with it: a single-phase program says
`phas #0, #0, pixel, end, parallel`; our vertex shader says `phas #7, #0, pixel, vcull,
parallel`, runs its first phase (copy the vertex to the outputs, `emitvcbvtx`), and its second
phase at instruction 7 (`emitmtevtx`) runs once vertex culling has seen the vertex. The driver
adds the program's own instruction index to `#<next>` when it places the program, as the
operand counts from the code base (`design/sgx545_3d.md` §14.2). `pt` (punch-through?),
`selective`/`sample` rates and `perinstance` are **[open]**, as is the register form
`phas <src1>, <src2>`.

**In the firmware [RE].** Seven `phas`, all of them the single-phase marker
`phas #0, #0, pixel, end, parallel`.

### 2.7 Memory, data fences and the register file

- **Loads complete asynchronously.** A load (`ld*`, `eld*`, `ldr`, the samplers) names a data
  fence counter, `drc0` or `drc1` (two bits for `ldr` and the samplers, `drc0`-`drc3`), and
  `wdf drcN` waits until the loads counted on it have landed **[RE/HW]**. The firmware almost
  always follows its loads with a `wdf` before using the data (711 `wdf drc0`, 96 `wdf drc1`),
  and our programs wait with `wdf drc0` for the attribute DMA the PDS started.
- **`idf drcN, bif`** appears 124 times in the firmware, mostly right before a `wdf` on the
  same counter, after stores **[RE]**: it looks like a fence that makes earlier memory writes
  visible (through the BIF, the memory interface) and is counted on `drcN` **[infer]**. The
  `pixelbe` and reserved modes of `idf` are **[open]**.
- **Addresses.** Loads and stores take a base register and an offset counted in elements of
  the access size (4 bytes for `d`, 2 for `w`, 1 for `b`) **[DDK/HW]**; the addressing modes
  and the fetch rule are in *Loads and stores*. Addresses are device virtual addresses of the
  task's memory context **[HW]**.
- **The SGX register file** is not memory: `ldr rD, #idx` and `str #idx, src` read and write
  the SGX register at offset `idx × 4` **[RE/HW]**. A client pixel shader on our render node
  reads any register this way; a client store to the one register tried was dropped
  (`design/sgx545_render_uapi.md` §7 Q4).
- **Caches.** `ci`, `cf` and `cfi` invalidate, flush, or flush and invalidate the levels their
  last operand names (L0 and L1, L2, or all three) **[infer]**, from the names the reference
  disassembler prints; the cache mode flags of loads and stores (`.bpcache`, `.fcfill`,
  `.bypassl1`) are discussed under *Loads and stores*.

## 3. Operand encodings

### 3.1 Register operands

A register operand is a seven-bit number plus bank bits. Destinations, sources 1 and 2, and
source 0 number their banks differently **[RE]** (the printers were derived by sweeping every
number and bank through the reference disassembler, and hold in every form checked). The
tables below are generated from the decoder's printers: each row is a bank code (the value
bits above the number) and what numbers 0, 1 and 127 print in it.

**Destination** (`enc[27:21]` number, `enc[33:32]` bank, `enc[51]` bank extension):

| bank | number 0 | number 1 | number 127 |
|---|---|---|---|
| 0 | `r0` | `r1` | `i3` |
| 1 | `o0` | `o1` | `o127` |
| 2 | `pa0` | `pa1` | `pa127` |
| 3 | `r[il + #0]` | `r[il + #1]` | `sa[ih + #15]` |
| 4 | `sa0` | `sa1` | `sa127` |
| 5 | `c0` | `c1` | `g63` |
| 6 | `i.` | `i.l` | `i.lh` |
| 7 | `i0` | `i1` | `i127` |

**Sources 1 and 2** (source 1: `enc[13:7]`, `enc[31:30]`, `enc[49]`; source 2: `enc[6:0]`,
`enc[29:28]`, `enc[48]`). Bank 6 is a seven-bit immediate, printed in decimal or, by some
instructions (`mov`, the bitwise group's source 1), in hex:

| bank | number 0 | number 1 | number 127 |
|---|---|---|---|
| 0 | `r0` | `r1` | `i3` |
| 1 | `o0` | `o1` | `o127` |
| 2 | `pa0` | `pa1` | `pa127` |
| 3 | `sa0` | `sa1` | `sa127` |
| 4 | `r[il + #0]` | `r[il + #1]` | `sa[ih + #15]` |
| 5 | `c0` | `c1` | `g63` |
| 6 | `#0` | `#1` | `#127` |
| 7 | `i0` | `i1` | `i127` |

**Source 0** (`enc[20:14]`, `enc[34]`, and `enc[51]` where the instruction has no
destination extension): temporaries, `pa`, `o` and `sa` only; no immediates.

| bank | number 0 | number 1 | number 127 |
|---|---|---|---|
| 0 | `r0` | `r1` | `i3` |
| 1 | `pa0` | `pa1` | `pa127` |
| 2 | `o0` | `o1` | `o127` |
| 3 | `sa0` | `sa1` | `sa127` |

- **Temporaries and internal registers.** In bank 0, numbers 124-127 print as `i0`-`i3`; in
  the 6-bit readings below (half floats, c10, the colour groups) the temporaries stop at `r59`
  (`r29` for half-float halves) and the internal registers follow. A load destination is
  either `r0`-`r127` or `pa0`-`pa127` (one bank bit, `enc[39]`).
- **Bank 7** prints `i0`-`i127` in both tables. Numbers past 3 read the other instances of
  the 2x2 quad, modulo 16, and writes to them are dropped **[HW]** (*Internal registers*).
- **Indexed operands** `bank[il + #n]` and `bank[ih + #n]`: the bank is `r`, `o`, `pa` or `sa`,
  the base the index register `i.l` or `i.h`, and `n` 0-15 (0-7 in the 6-bit readings)
  **[RE]**; the register used is the index plus `n`, in registers, for `r` and `sa` **[HW]**
  (*Index registers*).
- **Destination bank 6** writes the index registers: `i.l`, `i.h`, both (`i.lh`), or neither
  (`i.`, value 0).
- **Constants** `c0`-`c63` and **globals** `g0`-`g63` share bank 5 (numbers 64 up are `g`).

### 3.2 Immediates

- **Seven-bit immediates** are source bank 6 (`#0`-`#127`), in sources 1 and 2.
- **The 32-bit move** (`mov dst, #imm32`, class 31) scatters its immediate over three ranges:
  value bits 20-0 in `enc[20:0]`, 25-21 in `enc[40:36]` and 31-26 in `enc[49:44]` **[RE/HW]**.
  The driver patches colours and PBE words into our programs exactly there
  (`design/sgx545_fw_format.md` §4, the `USSE_LIMM` patch). For example:

`mov.skipinv.end o0, #0x12345678` assembles with form #0: enc[63:32] = `0xFCA44111`, enc[31:0] =
`0x00145678`; in memory (little endian) `78 56 14 00 11 41 A4 FC`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `skipinv` | 0x1 | `.skipinv` |
| `nosched` | 0x0 | (none) |
| `end` | 0x1 | `.end` |
| `dst` | 0x80 | `o0` |
| `imm` | 0x12345678 | `0x12345678` |

- **The rotated immediate** of the bitwise group (source 2 written as `#imm`) is a 16-bit
  value rotated left by a five-bit amount, so any 32-bit value with at most 16 significant
  contiguous bits (cyclically) can be written directly; the assembler finds the rotation
  **[RE]**:

`and r1, r2, #0x00FF0000` assembles with form #15: enc[63:32] = `0x50011230`, enc[31:0] =
`0x203F8100`; in memory (little endian) `00 81 3F 20 30 12 01 50`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `and` |
| `flags` | 0x0 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `src1` | 0x2 | `r2` |
| `inv` | 0x0 | (none) |
| `imm` | 0x8ff00 | `0x00FF0000` |

- **Register indices** of `ldr`/`str` are 14 bits in two seven-bit halves (`enc[6:0]` low,
  `enc[20:14]` high), counted in dwords (`ldr r0, #7, drc0` reads `CORE_ID` at offset `0x1c`)
  **[RE/HW]**. A register index is masked to 14 bits **[HW]** (*The 14-bit `ldr`/`str`
  index*).
- **Branch targets** are 18 bits (`enc[17:0]`): absolute for `ba`, signed for `br`.
- Smaller numbers (counts, `phas` operands, the MOE values, emit sidebands) are described with
  their groups.

### 3.3 Write masks, byte masks and repeat counts

- **Destination write mask.** `enc[47:44]` (bits w, z, y, x from high to low) prints after the
  destination: `.x` alone prints nothing, `0` prints a lone `.`, the rest name the enabled
  components (`r1.xz`). It shares its bits with the repeat count, chosen by `enc[53]` **[RE]**.
  A component is a register, not a byte: component k writes `d + k` from every register source
  `+ k`, as a repeat of four would, and mask 0 writes nothing **[HW]** (*Write masks*).

`mov r1.xz, r2` assembles with form #1: enc[63:32] = `0x28015000`, enc[31:0] = `0x20200100`; in
memory (little endian) `00 01 20 20 00 50 01 28`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `skipinv` | 0x0 | (none) |
| `syncs` | 0x0 | (none) |
| `nosched` | 0x0 | (none) |
| `end` | 0x0 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x5 | `.xz` |
| `src1` | 0x2 | `r2` |

- **Byte masks.** The pack/unpack group writes `.bytemaskXXXX` (`enc[37:34]`, printed only
  when not all four bytes are enabled) and `sop2wm` always prints one (`.bytemask0000`-
  `.bytemask1111`) **[RE]**. On a pack, bit N (the Nth digit from the right) enables byte N of the
  destination, and the others keep their values **[HW]** (*Pack and unpack lanes*); `sop2wm`'s
  works the same way **[HW]** (*Colour operations*).
- **Repeat counts** (`.repeatN`) are described in *Repeats and the MOE state*.

### 3.4 Source modifiers and data readings

The same register bits are read in several ways, chosen by the instruction **[RE]**:

- **Float sources** take `.neg` and `.abs` (`-|x|` when both, as the emulation models it),
  one bit each next to the operand. Source 0 of the float groups has `.abs` only.
- **Half floats.** With the group's half-float bit (`enc[54]` in classes 0-4) a source number
  of 64-127 names a 16-bit half of a 32-bit register, `(n - 64) / 2`, printed with a
  component suffix: `.flt16.0`/`.flt16.2` for the low and high half (`fsubflr` and the third
  operand of the dot products print `.flt16` and `.flt16.2`). Numbers below 64 keep their
  32-bit reading, and in that reading the temporaries stop at `r29`.

`fmad r0, r1.flt16.0, r2.flt16.2, r3.flt16.0` assembles with form #41: enc[63:32] = `0x00600000`,
enc[31:0] = `0x0010A2C6`; in memory (little endian) `C6 A2 10 00 00 00 60 00`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `fmad` |
| `flags` | 0x0 | (none) |
| `repeat` | 0x0 | (none) |
| `incs` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `src0` | 0x42 | `r1.flt16.0` |
| `src1` | 0x45 | `r2.flt16.2` |
| `src2` | 0x46 | `r3.flt16.0` |

- **c10** is a 10-bit fixed-point colour reading (`.c10`): number bit 6 selects it in the
  colour, dot-product and `fpma` groups and in some float tests. It reads three signed 10-bit
  lanes, 1.0 = 255 **[HW]** (*Colour operations*).
- **Integer halves and components.** `.low`/`.high` pick a 16-bit half (`imae`, `ima16`,
  `adifsum`); `.0`-`.3` pick a byte or half of a pack/unpack source; `.neg` negates an
  integer source of `ima16`/`ima32`; `.comp` complements a colour-group selector (`1 - x`
  **[infer]**).
- **Swizzles** of `fmad16` (`.swzll`, `.swzhh`, `.swzf32`) select which halves of a source
  feed the two 16-bit lanes **[infer]**.
- **Inverted source.** `~src2` in the bitwise group is the bitwise complement **[RE]** (the
  firmware builds masks with an `xor` against an inverted immediate; the emulation models it).

What each modifier does to a value was never measured on the hardware except where a group
section says so.

## 4. Assembler syntax

The syntax is the reference disassembler's, which our disassembler reproduces exactly, so
that `asm(disasm(word))` decodes as `word` and `disasm(asm(text))` gives `text` back
(`tools/usse-asm/README.md`). One statement per line:

| statement | meaning |
|-----------|---------|
| `p0 mov.skipinv.end o0, #0x12345678` | an instruction: predicate, mnemonic, modifiers, operands |
| `.long 0x0123456789ABCDEF` | a raw 64-bit word (several, comma-separated) |
| `name:` | a label, an instruction index; it may share the line with a statement |
| `; ...`, `/* ... */` | comments |
| `12: 0x...\tphas ...` | a disassembler listing line: index and hex columns are ignored |

- **Order.** Predicate, mnemonic, then modifiers in the order the decoder prints them (flags
  before the repeat count, the repeat count before `.incsXYZ`, ...), then the operands. The
  destination's write mask follows the destination (`r1.xz`).
- **Leniency.** Letters match in either case, blanks may vary (around commas too), numbers
  may be decimal or hex, and the `/* incp */` comments the decoder prints are optional.
  Mnemonics, modifiers and register names are otherwise exact.
- **Labels.** `ba #label` takes a label as an absolute instruction index; `br` takes only a
  number, since its offset base is not established.
- **Canonical words.** Text that several words print assembles to one of them: the first form
  in table order and, within it, zeros for ignored bits, except where the closed toolchain's
  code shows a convention (an unused source 2 written as immediate 0, the unprinted sources of
  the vertex-side emits). With those, every decoded word of the microkernel and of our
  programs reassembles bit for bit.

Our programs in `tools/usse-asm/progs/` are written in this syntax; `make -C tools/usse-asm`
builds the assembler, `tools/usse-asm/usse-asm -l prog.s` prints a listing.

## 5. Assembling, disassembling and running

Three tools cover a program's life, each in C (`tools/`) and in Rust (`crates/sgx-tools`), the
two held byte for byte to each other:

- **`usse-asm`** assembles source in the syntax above into raw little-endian 64-bit words;
  `-l` lists each word with its text. Labels name instruction indices, so branches and
  `lapc` targets can be written by name.
- **`usse-dis`** prints a blob of words, one instruction per line, as the assembler reads it
  back; any decodable word round-trips through the two.
- **`sgx-sim`** runs a program on the simulator, as one instance or as the 2x2 quad of a pixel
  (`--quad X,Y`), and prints every register it wrote and every scratch dword it changed;
  `--trace` shows each instruction with its writes, `--set REG=VALUE` presets registers. It
  stops, with the reason, at the first instruction whose behaviour is not established.

```
$ printf 'mov r1, #0x5caf0010\nmov r2, #7\nstad [r1,+#0], r2\nmov.end o0, r2\n' |
    cargo run -q -p sgx-tools --bin sgx-sim -- --trace -
0: mov r1, #0x3E003010
    r1 = 0x3e003010
1: mov r2, #0x00000007
    r2 = 0x00000007
2: stad [r1,+#0], r2
    [0x3e003010] = 0x00000007
3: mov.end o0, r2
    o0 = 0x00000007
ended after 4 steps
```

The simulator's memory is the probe harness's scratch buffer (64 KiB at `0x3e003000`, and a
`limm` of `0x5cafnnnn` loads its address + `nnnn`), so a probe program runs unchanged; our own
pixel, vertex and state programs are in `tools/usse-asm/progs/`.

## 6. Syntax index

Every mnemonic the assembler reads, with the operand syntax of each form shape it can
take, generated from the encoder's own patterns. `usse-asm --explain` prints the closest
of these lines when a source line does not assemble.

Fields are `<name>`; a mnemonic in `<op>` is printed by an operation field, and
`+name` is the second operation of a dual-issue or dot form.

```
+aadd        <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> Ax<sax>, <sop2>
adif         <pred>adif<flags> <dst>, <src1>, <src2>
adifsum      <pred>adifsum<flags> <dst>, <src0>.<half>, <src1>, <src2>
+aintrp1     <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> <ssrc0>, <sop2>
+aintrp2     <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> <ssrc0>, <sop2>
+alrp        <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
+alrp        <pred>lrp2<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
and          <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
and          <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
and          <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
and          <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
and          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
and          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
+arsop       <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+arsop <sop>
+asop        <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
+asop        <pred>lrp1<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
+asop2       <pred>sop2<flags> <dst>, <src1>, <src2>, <op>+asop2 <sop>
asr          <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
asr          <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
asr          <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
asr          <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
asr          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
asr          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
+asub        <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> Ax<sax>, <sop2>
atst8        <pred>atst8<flags> <dst>, <w>p<p>, <src0>, <src1>, <src2><extra><op>
ba           <pred>ba<flags> #<imm>
bexception   bexception<nosched>
bilin        <pred>bilin<flags> <dst>, <src0>, <src1>, <src2>, <op1>, <op2>, <op3><extra>
br           <pred>br<flags> <imm>
cf           <op><flags> <src1>, <src2>, #<imm>
cfi          <op><flags> <src1>, <src2>, #<imm>
ci           <op><flags> <src1>, <src2>, #<imm>
depthf       <pred>depthf<flags> <src0>, <src1>, <src2><extra><op>
efo          <pred>efo<flags> <dst>= <reg>, <f4>, <op1>, <op2>, <src0>, <src1>, <src2>
eldd         <pred>eld<op><flags> <dst>, [<base>, <src0>, #<imm>], <src2>, drc<drc>
eldq         <pred>eld<op><flags> <dst>, [<base>, <src0>, #<imm>], <src2>, drc<drc>
emitmtest    emitmtest<nosched><end><threepart><freep> #<imm> /* incp */, <src1>, <src2>
emitmtevtx   emitmtevtx<nosched><end><twopart><cut><freep> #<imm> /* incp */, <src1>, <src2>
emitpds      emitpds<nosched><end><tasks><taske><freep> #<imm1> /* incp */, <src0>, <src1>,
                <src2>, #<imm2>
emitpix1     emitpix1<nosched><end><freep> #<imm> /* incp */, <src0>, <src1>
emitpix2     emitpix2<nosched><end><freep> #<imm1> /* incp */, <src0>, <src1>, <src2>, #<imm2>
emitvcbst    emitvcbst<nosched><end><threepart><freep> #<imm> /* incp */, <src2>
emitvcbvtx   emitvcbvtx<nosched><end><twopart><freep> #<imm> /* incp */, <src2>
+fadd        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fadd        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fadd        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fadd        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fadd         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fadd         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fadm         <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
fadm         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fadm         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fadm         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fadm         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
fadm         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+fcos        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+fcos        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+fcos        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+fcos        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fcos         <pred><op><flags1> <dst><wmask>, <src1><flags2>
fcos         <pred><op><flags1><repeat> <dst>, <src1><flags2>
fddp         <pred>fddp<flags><repeat><incs> <dst>, i<i>, <src0>, <src1>, <src2>
fddp         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fddp         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fddp         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fddp         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
fddp         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
fddpc        <pred>fddpc<flags><repeat> <dst>, i<i>, cp<cp1>, cp<cp2>, <src0>, <src1>, <src2>
fdp          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fdp          <pred>fdp<flags><repeat><incs> <dst>, <src1>, <src2>
fdp          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fdpc         <pred>fdpc<flags><repeat><incs> <dst>, cp<cp>, <src1>, <src2>
fdsx         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fdsx         <pred><op><flags><repeat><incs> <dst>, <src1>, <src2>
fdsx         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fdsy         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fdsy         <pred><op><flags><repeat><incs> <dst>, <src1>, <src2>
fdsy         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
+fexp        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+fexp        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+fexp        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+fexp        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fexp         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
fexp         <pred><op><flags1> <dst><wmask>, <src1><flags2>
fexp         <pred><op><flags1><repeat> <dst>, <src1><flags2>
fexp         <pred><op><flags>.<test> <w><dst><wmask>, <src1>
firh         <pred>firh<flags> <dst>, <src0>, <src2>, <op1>, <op2>, #<imm1>, #<imm2>
firv         <pred>firv<flags> <w><dst>, <src0>, <src2>, <op><extra>
+flog        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+flog        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+flog        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+flog        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
flog         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
flog         <pred><op><flags1> <dst><wmask>, <src1><flags2>
flog         <pred><op><flags1><repeat> <dst>, <src1><flags2>
flog         <pred><op><flags>.<test> <w><dst><wmask>, <src1>
+fmad        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmad        <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmad        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmad        <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fmad         <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
fmad         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fmad         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fmad         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fmad         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
fmad         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+fmad16      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmad16      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmad16      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmad16      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fmad16       <pred>fmad16<flags1><flags2> <dst>, <src0><flags3>, <src1><flags4>, <src2><flags5>
fmax         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fmax         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
+fmaxmin     <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmaxmin     <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmaxmin     <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmaxmin     <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fmaxmin      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
fmin         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fmin         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fminmax      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
fminmax      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fminmax      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fminmax      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fminmax      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
fminmax      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+fmsa        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmsa        <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmsa        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+fmsa        <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fmsa         <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
fmsa         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fmsa         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fmsa         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fmsa         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
fmsa         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+fmul        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fmul        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fmul        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fmul        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fmul         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fmul         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fpadd8       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fpadd8       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fpma         <pred>fpma<flags> <dst>, <src0>, <src1>, <src2>, <op1>, <op2>
fpmul8       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fpmul8       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
fpsub8       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fpsub8       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
+frcp        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+frcp        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+frcp        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+frcp        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
frcp         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
frcp         <pred><op><flags1> <dst><wmask>, <src1><flags2>
frcp         <pred><op><flags1><repeat> <dst>, <src1><flags2>
frcp         <pred><op><flags>.<test> <w><dst><wmask>, <src1>
+frsq        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+frsq        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+frsq        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+frsq        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
frsq         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
frsq         <pred><op><flags1> <dst><wmask>, <src1><flags2>
frsq         <pred><op><flags1><repeat> <dst>, <src1><flags2>
frsq         <pred><op><flags>.<test> <w><dst><wmask>, <src1>
+fsin        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+fsin        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+fsin        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+fsin        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fsin         <pred><op><flags1> <dst><wmask>, <src1><flags2>
fsin         <pred><op><flags1><repeat> <dst>, <src1><flags2>
fsqrt        <pred><op><flags1> <dst><wmask>, <src1><flags2>
fsqrt        <pred><op><flags1><repeat> <dst>, <src1><flags2>
fssq         <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
fssq         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fssq         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
fssq         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
fssq         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+fsub        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fsub        <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fsub        <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fsub        <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fsub         <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fsub         <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
+fsubflr     <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fsubflr     <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fsubflr     <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
+fsubflr     <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
fsubflr      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
fsubflr      <pred>fsubflr<flags><repeat><incs> <dst>, <src1>, <src2>
fsubflr      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iadd16       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iadd16       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iadd32       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iadd32       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iadd8        <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iadd8        <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iadd8        <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iadd8        <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
iaddu16      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iaddu16      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iaddu32      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iaddu32      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iaddu8       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iaddu8       <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
iaddu8       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
iaddu8       <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
idf          idf drc<drc>, bif
idf          idf<nosched> drc<drc>, <op>
idiv         <pred>idiv<flags> <dst>, <src1>, <src2>, drc<drc>
ima16        <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2><neg>, u16, u16
ima16        <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
+ima32       <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+ima32       <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+ima32       <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
+ima32       <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
ima32        <pred>ima32<flags>.<type> <dst>, <src0>, <src1><neg1>, <src2><neg2>
ima32        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
ima32        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
ima32        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
ima32        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
ima32        <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
ima32        <pred>ima32<flags>.<type> <dst><extra1>, <src0>, <src1>, <src2><extra2>
ima8         <pred>ima8<flags><sat> <dst>, <src0>, <src1>, <src2>, <op>
imae         <pred>imae<flags>.<type><rs> <dst>, <src0>.<half1>, <src1>.<half2>, <src2>, u32
imae         <pred>imae<flags>.<type><rs> <dst>, <src0>, <src1>, <src2>, <op>
imo          imo<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
imul16       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
imul16       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
imul8        <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
imul8        <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
imul8        <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
imul8        <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
imulu16      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
imulu16      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
imulu8       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
imulu8       <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
imulu8       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
imulu8       <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
isub16       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
isub16       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
isub8        <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
isub8        <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
isub8        <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
isub8        <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
isubu16      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
isubu16      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
isubu8       <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
isubu8       <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
isubu8       <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
isubu8       <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
lapc         <pred>lapc<flags>
ldab         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldab         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldab         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldad         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldad         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldad         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldaw         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldaw         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldaw         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldlb         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldlb         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldlb         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldld         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldld         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldld         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldlw         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldlw         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldlw         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldr          ldr<flags> <dst>, #<imm>, drc<drc>
ldr          ldr<flags> <dst>, <src2>, drc<drc>
ldtb         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldtb         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldtb         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldtd         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldtd         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldtd         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
ldtw         <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
ldtw         <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
ldtw         <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
lock         <op><nosched> #<imm>
lrp1         <pred>lrp1<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
lrp2         <pred>lrp2<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
moest        moest<skipinv><nosched> <dst>, #<imm>
+mov         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
+mov         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+mov         <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+mov         <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc0>
+mov         <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
+mov         <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc2>
+mov         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
+mov         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+mov         <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
+mov         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
+mov         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
+mov         <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
mov          <pred>mov<skipinv><nosched><end> <dst>, #<imm>
mov          <pred>mov<skipinv><syncs><nosched><end> <dst><wmask>, <src1>
mov          <pred>mov<skipinv><syncs><nosched><end><repeat> <dst>, <src1>
mov          mov pclink, <src1>
mov          mov <dst>, pclink
mov          <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
mov          <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
mov          <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
mov          <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc0>
mov          <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc2>
mov          <pred>mov<nosched> pclink, <src1>
mov          <pred>mov<nosched> <dst>, pclink
movc         <pred>movc<flags1><flags2><flags3> <dst><wmask>, <src0>, <src1>, <src2>
movc         <pred>movc<flags1><flags2><repeat><flags3> <dst>, <src0>, <src1>, <src2>
movc         <pred>movc<flags1><repeat>.i8<flags2> <dst>, <src0>, <src1>, <src2>
movmsk       <pred>movmsk<flags1><flags2> <dst><wmask>, <src1>, <src2><flags3>
movmsk       <pred>movmsk<flags1><flags2><repeat> <dst>, <src1>, <src2><flags3>
movmsk       <pred>movmsk<flags1><flags2>.smsk <dst><wmask>, <src0>, <src1>, <src2><flags3>
movmsk       <pred>movmsk<flags1><flags2>.smsk<repeat> <dst>, <src0>, <src1>, <src2><flags3>
movmsk       <pred>movmsk<flags1>.u8 <dst><wmask>, <src1>, <src2><flags2>
movmsk       <pred>movmsk<flags1><repeat>.u8 <dst>, <src1>, <src2><flags2>
movmsk       <pred>movmsk<flags1>.smsk.u8 <dst><wmask>, <src0>, <src1>, <src2><flags2>
movmsk       <pred>movmsk<flags1>.smsk<repeat>.u8 <dst>, <src0>, <src1>, <src2><flags2>
nop          nop<syncs><nosched><end><tog><trigger>
or           <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
or           <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
or           <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
or           <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
or           <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
or           <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
pckc10f16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckc10f16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckc10f32    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckc10f32    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckc10s16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckc10s16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckc10u16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckc10u16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckf16f16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckf16f16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckf16f32    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckf16f32    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckf16s16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckf16s16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pckf16u16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pckf16u16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcko8f16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcko8f16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcko8f32     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcko8f32     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcko8s16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcko8s16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcko8u16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcko8u16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks16f16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks16f16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks16f32    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks16f32    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks16s16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks16s16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks16u16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks16u16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks8f16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks8f16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks8f32     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks8f32     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks8s16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks8s16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcks8u16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcks8u16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku16f16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku16f16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku16f32    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku16f32    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku16s16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku16s16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku16u16    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku16u16    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku8f16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku8f16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku8f32     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku8f32     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku8s16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku8s16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcku8u16     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
pcku8u16     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
pcoeff       pcoeff<nosched><end> <src0>, <src1>, <src2>
phas         phas<end> #<imm1>, #<imm2>, <op1>, <op2>, <op3>
phas         phas<nosched><end> <src1>, <src2>
ptoff        ptoff<nosched><end>
release      <op><nosched> #<imm>
rlp          <pred>rlp<flags> <dst><wmask>, <src1>, p<p>
rlp          <pred>rlp<flags><repeat> <dst>, <src1>, p<p>
rol          <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
rol          <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
rol          <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
rol          <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
rol          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
rol          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
setfc        setfc<nosched> #<imm1>, #<imm2>
setm         setm<nosched> #<imm>
shl          <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
shl          <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
shl          <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
shl          <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
shl          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
shl          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
shr          <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
shr          <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
shr          <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
shr          <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
shr          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
shr          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
smbo         smbo<nosched> <src1>, <src2>
smbo         smbo<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
smlsi        smlsi<nosched> <src1>, <src2>
smlsi        smlsi<nosched> <f1>, <f2>, <f3>, <f4><extra>, #<imm1>, #<imm2>, #<imm3>
smoa         smoa<nosched> <src1>, <src2>
smoa         smoa<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>, <op1>, <op2>, <op3>, <op4>
smp1d        <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, drc<drc>
smp1d        <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, drc<drc>
smp1dbias    <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp1dbias    <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp1dgrad    <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp1dgrad    <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp1dreplace <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp1dreplace <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp2d        <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, drc<drc>
smp2d        <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, drc<drc>
smp2dbias    <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp2dbias    <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp2dgrad    <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp2dgrad    <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp2dreplace <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp2dreplace <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp3d        <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, drc<drc>
smp3d        <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, drc<drc>
smp3dbias    <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp3dbias    <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp3dgrad    <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp3dgrad    <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp3dreplace <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
smp3dreplace <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
smr          smr<nosched> <src1>, <src2>
smr          smr<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
sop2         <pred>sop2<flags> <dst>, <src1>, <src2>, <op>+asop2 <sop>
sop2wm       <pred>sop2wm<flags> <dst><bytemask>, <src1>, <src2>, <op>
sop3         <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
sop3         <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+arsop <sop>
sop3         <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
stab         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
stab         <pred>st<op><flags> [<base>,<step><src1>], <src2>
stab         <pred>st<op><flags> [<base>,<src1><step>], <src2>
stad         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
stad         <pred>st<op><flags> [<base>,<step><src1>], <src2>
stad         <pred>st<op><flags> [<base>,<src1><step>], <src2>
statomic     <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
statomic     <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
staw         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
staw         <pred>st<op><flags> [<base>,<step><src1>], <src2>
staw         <pred>st<op><flags> [<base>,<src1><step>], <src2>
stlb         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
stlb         <pred>st<op><flags> [<base>,<step><src1>], <src2>
stlb         <pred>st<op><flags> [<base>,<src1><step>], <src2>
stld         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
stld         <pred>st<op><flags> [<base>,<step><src1>], <src2>
stld         <pred>st<op><flags> [<base>,<src1><step>], <src2>
stlw         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
stlw         <pred>st<op><flags> [<base>,<step><src1>], <src2>
stlw         <pred>st<op><flags> [<base>,<src1><step>], <src2>
str          str<flags> #<imm>, <src1>
str          str<flags> <src2>, <src1>
sttb         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
sttb         <pred>st<op><flags> [<base>,<step><src1>], <src2>
sttb         <pred>st<op><flags> [<base>,<src1><step>], <src2>
sttd         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
sttd         <pred>st<op><flags> [<base>,<step><src1>], <src2>
sttd         <pred>st<op><flags> [<base>,<src1><step>], <src2>
sttw         <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
sttw         <pred>st<op><flags> [<base>,<step><src1>], <src2>
sttw         <pred>st<op><flags> [<base>,<src1><step>], <src2>
u16dot3      <pred><op1>dot3<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u16dot3      <pred>u16dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2><mode>
u16dot3off   <pred><op1>dot3<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u16dot3off   <pred>u16dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2><mode>
u16dot4      <pred><op1>dot4<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u16dot4off   <pred><op1>dot4<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u8dot3       <pred><op1>dot3<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u8dot3       <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> Ax<sax>, <sop2>
u8dot3       <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> <ssrc0>, <sop2>
u8dot3off    <pred><op1>dot3<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u8dot3off    <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> Ax<sax>, <sop2>
u8dot3off    <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> <ssrc0>, <sop2>
u8dot4       <pred><op1>dot4<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
u8dot4off    <pred><op1>dot4<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
unpckc10c10  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckc10c10  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf16c10  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf16c10  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf16o8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf16o8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf16s8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf16s8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf16u8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf16u8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32c10  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32c10  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32f16  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32f16  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32f32  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32f32  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32o8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32o8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32s16  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32s16  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32s8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32s8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32u16  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32u16  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpckf32u8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpckf32u8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcks16c10  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcks16c10  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcks16o8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcks16o8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcks16s8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcks16s8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcks16u8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcks16u8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcku16c10  <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcku16c10  <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcku16o8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcku16o8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcku16s8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcku16s8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcku16u8   <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcku16u8   <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcku8s8    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcku8s8    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
unpcku8u8    <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
unpcku8u8    <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
wdf          wdf drc<drc>
wdf          wdf<nosched> drc<drc>
wop          wop<nosched><twopart> #<imm>
xor          <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
xor          <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
xor          <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
xor          <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
xor          <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
xor          <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
```

## 7. Instruction groups

One section per opcode group: what the group does and what is known of it, how the firmware
uses it, its mnemonics and modifiers, and what the simulator models of it. The bit layouts are
in the reference, group by group under the same titles.

### 7.1 Moves: `mov`, the 32-bit immediate and `pclink`

- **`mov dst, src`** (class 5, type `enc[42:40]` = 0) copies a source-1 operand to the
  destination, under a write mask or repeated **[HW]**: our fill shader, copy shader and
  state copies are built from it. A source immediate prints in hex.
- **`mov dst, #imm32`** (class 31) loads a full 32-bit immediate **[HW]**; the driver patches
  colours and PBE words into it.
- **`mov pclink, src`** and **`mov dst, pclink`** save and restore the link register
  **[RE]** (*Program counter, calls and the end of a program*). Two encodings exist for each:
  a plain one with a temporary, and a general one with a predicate, `.nosched` and any
  operand.

**In the firmware [RE].** `mov` is its most common instruction (1497): 626 immediates into
temporaries (constants, addresses, and the nine address-patch sites the loader fills), moves
between temporaries and the `sa`/`pa` banks, 79 into `i.l`, 79 through indexed `sa`
operands, and the `pclink` traffic of its calls.

Examples, from our programs and assembled fresh:

From `tools/usse-asm/progs/use_copy.s`:

```
        mov.skipinv.end o0, pa0                 ; o0 = the fetched texel; end
```

`mov.skipinv.repeat4 o0, pa0` assembles with form #2: enc[63:32] = `0x28A13001`, enc[31:0] =
`0xA0000000`; in memory (little endian) `00 00 00 A0 01 30 A1 28`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `skipinv` | 0x1 | `.skipinv` |
| `syncs` | 0x0 | (none) |
| `nosched` | 0x0 | (none) |
| `end` | 0x0 | (none) |
| `repeat` | 0x3 | `.repeat4` |
| `dst` | 0x80 | `o0` |
| `src1` | 0x100 | `pa0` |

Mnemonics: `mov`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 7 forms have semantics. The
probe vectors that test them: 960 match the hardware, 122 stop unsupported, 0 mismatch
(`flags-other` 1/0/0, `index-high` 0/12/0, `index-store` 56/0/0, `index-unit` 98/0/0, `index-width`
42/0/0, `index-width2` 98/0/0, `index-width3` 84/0/0, `internal-registers` 0/58/0,
`internal-registers-across-tasks` 18/18/0, `internal-registers-bank7` 29/0/0,
`internal-registers-per-pixel` 54/0/0, `predicate-pn` 0/6/0, `repeat-order` 42/0/0, `repeat-pred`
56/0/0, `test-lanes-other` 74/0/0, `wmask-mov` 224/0/0, `x-bank7` 24/0/0, `x-gpiext-map` 4/0/0,
`x-gpiext-map2` 4/0/0, `x-gpiext-xtask` 6/4/0, `x-misc` 46/24/0); unsupported mostly for reads iN
before the program writes it (41); a write of iN (30); rN is past the task's N temporaries (24).

### 7.2 Conditional and masked moves: `movc`, `movmsk`

Class 5 with a type `enc[42:40]` other than 0. `movc` (types 1-5: `.i8`, `.i16`, `.i32`,
`.flt`, `.i10`) takes three sources and a test (`.tz`, `.tnz`, `.tn`, `.tp|z`); `movmsk`
(types 6 and 7, the second adding `.smsk` and a source 0) takes a mask format (`.mask`,
`.flt16`, `.flt`, `.u8`) and a component select on its last source **[RE]**.

- **`movc`** is a select per lane: `d = test(s0) ? s1 : s2` in each lane of the type (`.i32`
  one, `.i16` two, `.i8` four, `.i10` three 10-bit lanes with bits 30-31 cleared, `.flt` the
  float with denormals as zero and a NaN failing all but `.tnz`) **[HW]**.
- **`movmsk.<fmt> d, s1, s2.c`** writes `s1` into `d` when `s2` passes: `.mask` bit 0 of byte
  `c`, `.u8` bit 7 of byte `c`, `.flt` `s2 >= 0.5`, `.flt16` half `c >= 0.5` **[HW]**. With
  `.smsk` the move also needs bit 0 of source 0, except for `.mask` **[HW]** (*Conditional and
  masked moves*).
- **Write masks** pick registers as for `mov` **[HW]** (*Write masks*).
- **In the firmware:** not used. Neither do our programs use them.

`movc.i32.tz r1, r2, r3, r4` assembles with form #187: enc[63:32] = `0x28001300`, enc[31:0] =
`0x00208184`; in memory (little endian) `84 81 20 00 00 13 00 28`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags1` | 0x0 | (none) |
| `flags2` | 0x3 | `.i32` |
| `flags3` | 0x0 | `.tz` |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `src0` | 0x2 | `r2` |
| `src1` | 0x3 | `r3` |
| `src2` | 0x4 | `r4` |

Mnemonics: `movc`, `movmsk`.

Modifiers: `.end`, `.flt`, `.flt16`, `.i10`, `.i16`, `.i32`, `.i8`, `.mask`, `.nosched`, `.repeatN`,
`.skipinv`, `.smsk`, `.syncs`, `.tn`, `.tnz`, `.tp|z`, `.tz`, `.u8`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 11 of 11 forms have semantics. The
probe vectors that test them: 1030 match the hardware, 0 stop unsupported, 0 mismatch (`movc-flt`
112/0/0, `movc-i10` 112/0/0, `movc-i16` 112/0/0, `movc-i32` 112/0/0, `movc-i8` 112/0/0, `movmsk-flt`
28/0/0, `movmsk-flt16` 56/0/0, `movmsk-mask` 56/0/0, `movmsk-smsk` 28/0/0, `movmsk-smsk2` 192/0/0,
`movmsk-u8` 56/0/0, `wmask-other` 14/0/0, `x-smsk` 16/0/0, `x-smsk2` 24/0/0).

### 7.3 Bitwise operations and shifts: `and`, `or`, `xor`, `shl`, `shr`, `rol`, `asr`

Classes 10-13 share one layout. `enc[63:60]` selects the pair of classes (5 for `and`/`or`
and `xor`, 6 for the shifts) and the operation is `enc[59]` with `enc[35]` **[RE]**:

| `enc[63:60]` | `enc[59]`, `enc[35]` = 00 | 01 | 10 | 11 |
|---|---|---|---|---|
| 5 | `and` | `or` | `xor` | (does not decode) |
| 6 | `shl` | `rol` | `shr` | `asr` |

- **Semantics [RE].** `d = s1 op s2` on 32 bits: `and`, `or`, `xor`; `shl` and `shr` shift
  left and right (logical), `rol` rotates left, `asr` shifts right arithmetically, by source
  2. `~` before source 2 complements it first. This is the reading the radare2 emulation of
  the firmware uses, and the firmware's code makes sense under it throughout. **[HW]** By a
  register amount of 32 and over, `shl`/`shr` give 0, `asr` the sign and `rol` takes the
  amount mod 32, immediate amounts the same, and `~` complements an amount as any source 2
  (*Shift amounts of 32 and over*).
- **Immediates.** Source 2 written `#imm` is the rotated 16-bit immediate (*Immediates*); a
  source-1 immediate is the usual seven-bit one, printed in hex.
- **Write mask or repeat**, as for `mov` (`enc[53]`).
- **`.partial`** (`enc[34]`) changes nothing a fill sees **[HW]** (*`.tpres`, `.partial`,
  `.nosched`, `.syncs`*).

**In the firmware [RE].** The workhorses of its flag logic: 602 `and`, 208 `or`, 199 `xor`,
95 `shl`, 60 `shr`, 1 `rol`, mostly on temporaries and on words loaded from its control
blocks; masks are often built with `xor` against an inverted immediate.

`and r1, r2, #0x00FF0000` assembles with form #15: enc[63:32] = `0x50011230`, enc[31:0] =
`0x203F8100`; in memory (little endian) `00 81 3F 20 30 12 01 50`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `and` |
| `flags` | 0x0 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `src1` | 0x2 | `r2` |
| `inv` | 0x0 | (none) |
| `imm` | 0x8ff00 | `0x00FF0000` |

`xor.repeat2 r4, r6, ~r8` assembles with form #18: enc[63:32] = `0x58201800`, enc[31:0] =
`0x00800308`; in memory (little endian) `08 03 80 00 00 18 20 58`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x2 | `xor` |
| `flags` | 0x0 | (none) |
| `dst` | 0x4 | `r4` |
| `repeat` | 0x1 | `.repeat2` |
| `src1` | 0x6 | `r6` |
| `inv` | 0x1 | `~` |
| `src2` | 0x8 | `r8` |

Mnemonics: `and`, `asr`, `or`, `rol`, `shl`, `shr`, `xor`.

Modifiers: `.end`, `.nosched`, `.partial`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 8 of 8 forms have semantics. The
probe vectors that test them: 1089 match the hardware, 28 stop unsupported, 0 mismatch (`asr-reg`
98/0/0, `br-offset` 5/0/0, `fdpc` 84/0/0, `fdpc-clip` 80/0/0, `flags-other` 0/2/0, `rol-reg` 98/0/0,
`shift-immediates` 25/0/0, `shl-reg` 98/0/0, `shr-reg` 98/0/0, `wmask-and-imm` 224/0/0, `wmask-or`
224/0/0, `x-fdpc` 24/24/0, `x-m2` 5/0/0, `x-misc` 26/2/0); unsupported mostly for reads rN before
the program writes it (24); flag .partial (4).

### 7.4 The test group: ALU operations with a test

Classes 9 and 15 run an ALU operation (`enc[19:14]`) and **test its result**: class 9 into a
predicate `p0`-`p3` (`enc[35:34]`), class 15 into a byte, word or dword mask (`.bmsk`,
`.wmsk`, `.dmsk`). With `enc[20]` clear the result itself is not written (`!` before the
destination) **[RE]**. The operations: the bitwise group and shifts, integer adds, subtracts
and multiplies on 8, 16 and 32 bits (`iadd8` ... `iaddu32`, the 8-bit ones with `.sat`), and
float adds, multiplies, min/max, derivatives, dot product and reciprocal-type operations,
with the operand formats of their own groups when `enc[50]` is set.

The test is `.test<S><J><Z>.<chan>`, from `enc[43:36]`:

| part | bits | values |
|---|---|---|
| sign test `S` | `enc[43:42]` | `t`, `n` (negative), `p` (positive) |
| join `J` | `enc[39]` | `o` (or), `a` (and) |
| zero test `Z` | `enc[41:40]` | `t`, `z` (zero), `nz` (not zero) |
| channel | `enc[38:36]` | `chan0`-`chan3`, `andall`, `orall`, `and02`, `or02` |

- **The tests [HW].** On 32-bit results tested on `chan0`: `z` and `nz` test the result for
  zero, `n` tests bit 31 and `p` is its complement (`>= 0`, so zero passes), and `t` is true
  in either join: `testtaz`/`testtanz` are "zero" and "not zero", an `o` join with a `t` side
  is always true (*The test sign `p`*, *`t` in the `a`/`o` joins of a test*). The firmware
  tests flags this way hundreds of times, and sign bits by shifting the flag to bit 31 first
  (`shl.testnat`); our state and vertex programs rely on `and.testtanz` of 0 and 0 leaving
  the predicate false (*Predicates*).
- **The 32-bit adds [HW]** are not full 32-bit adds: `iadd32`/`iaddu32` add source 1's low 16
  bits, sign- or zero-extended, to the 32-bit source 2, and `iaddu32`'s `n` is never true
  (*The 32-bit adds `iadd32` and `iaddu32`*).
- **The 16-bit operations [HW]:** `isub16`/`isubu16` subtract both lanes, wrapping, and `chan0`
  tests the low lane (*`isub16`/`isubu16` lanes*). `iadd16`, `iaddu16`, `imul16`, `imulu16`,
  `iadd8`, `isub8`, `imul8`, `fadd`, `fmul` and `fsub` test their lanes as the channels say, a
  16-bit lane on its full result before it is cut **[HW]** (*Test channels*).
- **Channels [HW]:** a test has one result per channel: bytes 0-3 for the 8-bit operations,
  the low and high halves as channels 0 and 2 for the 16-bit ones, the value as channel 0
  otherwise; a channel without a lane tests as a positive non-zero value. `chan0`-`chan3`
  pick one, `andall`/`orall` combine all four, `and02`/`or02` channels 0 and 2 (*Test
  channels*). The masks of class 15, `.partial` and `.onceonly` on tests are **[open]**.

**In the firmware [RE].** 671 tests, every one on `chan0` and into a predicate: `and.testtaz`
246, `and.testtanz` 163, `xor.testtaz` 108, `xor.testtanz` 67, `shl.testnat` 16,
`shl.testpat` 12, `isub16` 40 (with `testtanz`, `testpat`, `testnat`, `testtaz`), `iaddu32`
8, `or` 9, `isubu16` 1. Most discard the result (`!c0`): test, then branch on the predicate.

From `tools/usse-asm/progs/use_mtest18.s`:

```
        and.skipinv.testtanz.chan0 !r0, p0, #0x00000000, #0
```

`xor.testtaz.chan0 r1, p2, r1, r3` assembles with form #23: enc[63:32] = `0x48001188`, enc[31:0] =
`0x003C8083`; in memory (little endian) `83 80 3C 00 88 11 00 48`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x32 | `xor` |
| `flags` | 0x0 | (none) |
| `test` | 0x18 | `testtaz.chan0` |
| `w` | 0x1 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `p` | 0x2 | `2` |
| `src1` | 0x1 | `r1` |
| `src2` | 0x3 | `r3` |

`isub16.testpat.chan0 r1, p2, r3, #4` assembles with form #24: enc[63:32] = `0x48011888`, enc[31:0]
= `0x2035C184`; in memory (little endian) `84 C1 35 20 88 18 01 48`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x17 | `isub16` |
| `flags` | 0x0 | (none) |
| `test` | 0x88 | `testpat.chan0` |
| `w` | 0x1 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `p` | 0x2 | `2` |
| `src1` | 0x3 | `r3` |
| `src2` | 0x304 | `#4` |

Mnemonics: `and`, `asr`, `fadd`, `fdp`, `fdsx`, `fdsy`, `fexp`, `flog`, `fmax`, `fmin`, `fmul`,
`fpadd8`, `fpmul8`, `fpsub8`, `frcp`, `frsq`, `fsub`, `fsubflr`, `iadd16`, `iadd32`, `iadd8`,
`iaddu16`, `iaddu32`, `iaddu8`, `imul16`, `imul8`, `imulu16`, `imulu8`, `isub16`, `isub8`,
`isubu16`, `isubu8`, `or`, `rol`, `shl`, `shr`, `xor`.

Modifiers: `.and02`, `.andall`, `.bmsk`, `.chan0`, `.chan1`, `.chan2`, `.chan3`, `.dmsk`,
`.nosched`, `.onceonly`, `.or02`, `.orall`, `.partial`, `.sat`, `.skipinv`, `.syncs`, `.testnanz`,
`.testnat`, `.testnaz`, `.testnonz`, `.testnot`, `.testnoz`, `.testpanz`, `.testpat`, `.testpaz`,
`.testponz`, `.testpot`, `.testpoz`, `.testtanz`, `.testtat`, `.testtaz`, `.testtonz`, `.testtot`,
`.testtoz`, `.wmsk`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 3 of 18 forms have semantics (not
yet: #198, #199, #200, #201, #202, #203, #204, #205, #206, #207, #208, #209, #210, #211, #212). The
probe vectors that test them: 4564 match the hardware, 10 stop unsupported, 0 mismatch (`chan-and`
168/0/0, `chan-isub16` 70/0/0, `chan-preds` 112/0/0, `iadd32-n` 56/0/0, `iadd32-p` 42/0/0,
`iadd32-src1-imm` 28/0/0, `iadd32-value` 56/0/0, `iadd32-z` 70/0/0, `iaddu32-imm` 56/0/0,
`iaddu32-np` 42/0/0, `iaddu32-tests` 84/0/0, `iaddu32-value` 56/0/0, `isub16-reg` 42/0/0,
`isub16-sign-n` 56/0/0, `isub16-sign-p` 56/0/0, `isub16-value` 84/0/0, `isub16-zero` 84/0/0,
`isubu16-value` 42/0/0, `isubu16-zero` 42/0/0, `join-t-not` 42/0/0, `join-t-tonz` 28/0/0,
`join-t-toz` 28/0/0, `predicate-pn` 0/6/0, `shl-test-reg` 56/0/0, `sign-n-iadd32` 56/0/0,
`sign-n-iaddu32` 56/0/0, `sign-p-and` 42/0/0, `sign-p-shl` 60/0/0, `test-channels` 1800/0/0,
`test-lanes-other` 74/0/0, `x-chan-and` 80/0/0, `x-chan-iadd16` 80/0/0, `x-chan-iadd32` 80/0/0,
`x-chan-iadd8` 80/0/0, `x-chan-or` 80/0/0, `x-chan2-iadd16-testnanz` 48/0/0,
`x-chan2-iadd16-testnat` 48/0/0, `x-chan2-iadd16-testpat` 48/0/0, `x-chan2-iadd16-testtanz` 48/0/0,
`x-chan2-iadd32-testnanz` 48/0/0, `x-chan2-iadd32-testnat` 48/0/0, `x-chan2-iadd32-testpat` 48/0/0,
`x-chan2-iadd32-testtanz` 48/0/0, `x-chan2-iadd8-testnanz` 48/0/0, `x-chan2-iadd8-testnat` 48/0/0,
`x-chan2-iadd8-testpat` 48/0/0, `x-chan2-iadd8-testtanz` 48/0/0, `x-misc` 44/4/0, `zero-z-iadd32`
56/0/0); unsupported mostly for predicate Pn (10).

### 7.5 Integer multiply-add: `imae`, `ima16`, `ima32`, `ima8`, `idiv`

Four classes **[RE]**: 21 `imae` (16 × 16 + 32), 20 `ima16` (16-bit), 26 `ima32` (32-bit) and
`idiv`, 19 `ima8` (four 8-bit lanes with colour-style selectors). They carry the multiply
flags (`.skipinv`, `.nosched`, `.end`, a three-bit repeat) and a two-bit predicate (`ima32`
and `idiv` the full one).

- **`imae.{u,s} d, a.{low,high}, b.{low,high}, c, u32`** computes `d = a16 × b16 + c`, the
  halves zero- or sign-extended **[RE]**: the firmware's addition idiom is
  `imae.u d, a.low, #1.low, c, u32`, an add of a 16-bit value, 205 times, and its address
  arithmetic multiplies indices by structure sizes the same way. **[HW]** `.usat`/`.ssat`
  saturate, `.rsN` shifts the result right, a `u16` source 2 is sign-extended and a `z16`
  one zero-extended, `coe, iN` writes the carry out and `cie, iN` adds it (*Integer
  multiply-add modes*).
- **`ima32.{u,s} d, a, b, c`** computes the low 32 bits of `a × b + c` **[RE]** (the
  firmware's ten uses fit). `cie, iN` adds `iN` **[HW]**; the optional `i` register after the
  destination is **[open]**. As a dual-issue secondary it works on two 16-bit
  lanes (*Dual issue*).
- **`ima16`** multiplies 16-bit values, its sources read as `u16`, `u8`, `s8` or `o8` with a
  half select. With `u16, u16` it computes `a × b + c` (or `- c`) in each 16-bit lane,
  wrapping **[HW]** (*`ima16` lanes and source formats*); with `u8`, `s8`, `o8` each lane reads
  the `.low`/`.high` byte of its own half **[HW]** (*Integer multiply-add modes*).
  The firmware has two, both `.s` with a negated source.
- **`idiv d, a, b, drcN`** writes the unsigned quotient to `d` and the remainder to `d + 1`,
  asynchronously: `wdf drcN` waits for them **[HW]** (*Integer multiply-add modes*).
- **`ima8`** computes `x + y × z` per byte from its selectors (`s0`, `s1`, `s2` and their alpha
  forms), wrapping or with `.sat` clamping **[HW]** (*Integer multiply-add modes*).

`imae.u r2, r4.low, #1.low, r2, u32` assembles with form #26: enc[63:32] = `0xA8020080`, enc[31:0] =
`0x80410082`; in memory (little endian) `82 00 41 80 80 00 02 A8`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `type` | 0x0 | `u` |
| `rs` | 0x0 | (none) |
| `dst` | 0x2 | `r2` |
| `src0` | 0x4 | `r4` |
| `half1` | 0x0 | `low` |
| `src1` | 0x301 | `#1` |
| `half2` | 0x0 | `low` |
| `src2` | 0x2 | `r2` |

`ima32.u r1, r2, r3, r4` assembles with form #28: enc[63:32] = `0xD0000000`, enc[31:0] =
`0x00208184`; in memory (little endian) `84 81 20 00 00 00 00 D0`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `type` | 0x0 | `u` |
| `dst` | 0x1 | `r1` |
| `src0` | 0x2 | `r2` |
| `src1` | 0x3 | `r3` |
| `neg1` | 0x0 | (none) |
| `src2` | 0x4 | `r4` |
| `neg2` | 0x0 | (none) |

Mnemonics: `idiv`, `ima16`, `ima32`, `ima8`, `imae`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.rsN`, `.s`, `.sat`, `.skipinv`, `.ssat`, `.u`, `.usat`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 4 of 12 forms have semantics (not
yet: #171, #172, #173, #174, #175, #176, #177, #178). The probe vectors that test them: 182 match
the hardware, 45 stop unsupported, 0 mismatch (`idiv` 7/0/0, `ima16` 56/0/0, `ima16-8bit` 0/8/0,
`ima16-imm-c` 28/0/0, `ima16-plain` 42/0/0, `ima16-reg-b` 28/0/0, `ima8` 0/8/0, `imae-modes` 7/7/0,
`x-misc` 14/22/0); unsupported mostly for no semantics yet: ima16 (15); no semantics yet: ima8 (12);
no semantics yet: imae (10).

### 7.6 Colour operations: `sop2`, `sop3`, `lrp1`, `lrp2`, `sop2wm`

Classes 16-18 combine 8-bit colour channels, a colour part and an alpha part in one
instruction, with selectors naming what feeds each multiplier (`zero`, `s1`, `s2a`, `asat`,
`s2scale`, ...), `.comp` complements and an operation (`add`, `sub`, `min`, `max`) **[RE]**:

- `sop2 d, s1, s2, <colour>+asop2 <alpha>`: two sources;
- `sop3 d, s0, s1, s2, <colour>+asop|arsop|alrp <alpha>`: three sources;
- `lrp1`, `lrp2`: interpolations between sources;
- `sop2wm`: `sop2` with a destination byte mask and separate colour and alpha operations.

Operands use the 6-bit c10 reading (`.c10` after the register, *Source modifiers*).

- **Semantics [HW].** They are the fixed-function blend and texture-combine operations of a
  GLES 1 era pipeline: per colour channel (bytes 0-2) `op(s1 × f1, s2 × f2)`, per alpha (byte
  3) the same with the alpha selectors, the products summed exactly, divided by 255 once,
  rounded and saturated; `min`/`max` compare the sources themselves; `asat` is
  `min(s1a, 1 - s2a)`, `s2scale` doubles the result and `zeros` makes `s2` signed. `lrp1` is a
  lerp by `s0` or `s0a`, `lrp2` a sum of two selected products, `alrp` a lerp by `s0a`.
  `.c10` operands are signed 10-bit lanes with 1.0 = 255, clamped to ±510 (*Colour
  operations*). `sop2.repeatN` advances the destination and both sources per iteration
  **[HW]**.
- **In the firmware:** not used; nor in the closed GLES2 programs we captured.

`sop2 r0, r1, r2, s1, s2, add+asop2 s1a, s2a, add` assembles with form #164: enc[63:32] =
`0x80100450`, enc[31:0] = `0x00000082`; in memory (little endian) `82 00 00 00 50 04 10 80`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `dst` | 0x0 | `r0` |
| `src1` | 0x1 | `r1` |
| `src2` | 0x2 | `r2` |
| `op` | 0x42 | `s1, s2, add` |
| `sop` | 0x22 | `s1a, s2a, add` |

Mnemonics: `+alrp`, `+arsop`, `+asop`, `+asop2`, `lrp1`, `lrp2`, `sop2`, `sop2wm`, `sop3`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 7 forms have semantics. The
probe vectors that test them: 5372 match the hardware, 0 stop unsupported, 0 mismatch (`colour-c10`
176/0/0, `colour-c10-other` 9/0/0, `colour-dot-repeats` 30/0/0, `sop2-ops` 504/0/0, `sop2-random`
288/0/0, `sop2-selectors` 1056/0/0, `sop2wm` 440/0/0, `sop2wm-bytemask` 5/0/0, `sop3-lrp` 1672/0/0,
`sop3-random` 176/0/0, `x-arsop` 72/0/0, `x-c10` 24/0/0, `x-misc` 7/0/0, `x-rep` 8/0/0, `x-sop2`
174/0/0, `x-sop2r` 60/0/0, `x-sop2wm` 168/0/0, `x-sop2wmr` 32/0/0, `x-sop3` 471/0/0).

### 7.7 Integer dot products: `u8dot3`, `u8dot4`, `u16dot3`, `u16dot4`

Class 24 **[RE]**: three- and four-component dot products (`enc[56]` picks dot4, `enc[53:52]`
the result width), `off` variants (`enc[38]`), colour and alpha scales `Cx1`-`Cx8` and
`Ax1`-`Ax8`, complemented sources, and for `u8dot3` an alpha operation (`aadd`/`asub` with
selectors, or `aintrp1`/`aintrp2` with a source 0).

- **Semantics [HW].** `S = Σ s1[i] × s2[i]` over bytes 0-2 (dot3) or 0-3 (dot4), or with `off`
  over `(s1[i] - 128)(s2[i] - 128)`. `u8dot` writes `S × Cx / 255` to every colour byte and
  `S × Ax / 255` (or the alpha operation) to byte 3, rounded, saturated to 0..255; `u16dot`
  writes `S × Cx × 32767 / 255²` to both halves, saturated to `0x7fff` (±`0x7fff` with `off`).
  The alpha operations add, subtract or interpolate the source alphas; a repeat advances
  the destination and both sources (*Integer dot products*).
- **In the firmware:** not used.

Mnemonics: `+aadd`, `+aintrp1`, `+aintrp2`, `+asub`, `u16dot3`, `u16dot3off`, `u16dot4`,
`u16dot4off`, `u8dot3`, `u8dot3off`, `u8dot4`, `u8dot4off`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 5 of 5 forms have semantics. The
probe vectors that test them: 1485 match the hardware, 0 stop unsupported, 0 mismatch
(`colour-dot-repeats` 45/0/0, `intdot-alpha` 352/0/0, `intdot-lanes` 792/0/0, `x-aintrp` 8/0/0,
`x-dotcomp` 4/0/0, `x-idot` 272/0/0, `x-rep` 12/0/0).

### 7.8 `rlp`

Class 14 **[RE]**: `rlp d, s1, pN` with the bitwise group's flags, a write mask or repeat
count, one source and a seven-bit predicate number in `enc[20:14]`; the reference
disassembler reads no source 2.

- **Semantics [HW].** Rotate left through the predicate: `d = s << 1 | pN`, then `pN` takes
  bit 31 of `s`. Predicate numbers 4 and up alias `p0`-`p3` (number mod 4) (*`rlp`*).
- **In the firmware:** not used.

`rlp r1, r2, p0` assembles with form #185: enc[63:32] = `0x70001000`, enc[31:0] = `0x00200100`; in
memory (little endian) `00 01 20 00 00 10 00 70`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `src1` | 0x2 | `r2` |
| `p` | 0x0 | `0` |

Mnemonics: `rlp`.

Modifiers: `.end`, `.nosched`, `.partial`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 2 forms have semantics. The
probe vectors that test them: 287 match the hardware, 0 stop unsupported, 0 mismatch (`rlp` 160/0/0,
`rlp-high-predicates` 15/0/0, `x-rlp` 112/0/0).

### 7.9 Pack and unpack: `pck*`, `unpck*`, `statomic`

Class 8 converts between data formats **[RE]**. `enc[43:38]` names the destination and source
formats together: `pck<dst><src>` packs to a narrower format, `unpck<dst><src>` widens, with
`u8`, `s8`, `o8` (offset 8-bit), `u16`, `s16`, `f16`, `f32` and `c10` (value 8 is `statomic`
instead). Each instruction takes two sources, each with a component select (`.0`-`.3`), a
destination byte mask (`.bytemaskXXXX`, all four bytes print nothing), a rounding mode
(`nearest`, `down`, `up`, `zero`) and `.scale` (`.renable` for the float-to-float ones).

- **What is known [CAP].** The closed GLES2 fragment program of our capture converts its four
  f16 colour components to 8-bit with `pcku8f16` into an attribute register
  (`design/sgx545_3d.md` §9.3), and its clear vertex program unpacks the four bytes of a
  vertex colour to floats. So the group does carry colour conversion, as the names say.
- **Lanes [HW].** Source 1 and 2 fill the destination's lanes in turn (bytes 1, 2, 1, 2;
  halves 1, 2; `c10` lanes 1, 2, 1); the byte mask enables byte N with bit N and decides which
  source each enabled byte takes; component selects pick a byte, a half or a `c10` lane
  (*Pack and unpack lanes*).
- **Conversions [HW].** Unscaled conversions round in the instruction's mode and saturate;
  `.scale` maps 1.0 to 255, 127, 65535 or 32767 (`o8`: `x × 127 + 128`), symmetric for the
  signed formats, always to nearest even; `c10` is `x × 255` clamped to ±2.0; `pckf16f32`
  truncates unless `.renable`, which rounds to an integer; unpacks invert these exactly
  (*Pack and unpack conversions*).
- **`statomic`** writes, in both halves, the selected bytes of source 1 (low) and source 2
  (high) read as signed bytes, negatives made 0 **[HW]** (*Pack and unpack lanes*); what it is
  for is unknown.
- **In the firmware:** not used.

`pckf16f32 r0, r1.0, r1.1, nearest` assembles with form #160: enc[63:32] = `0x40001D7C`, enc[31:0] =
`0x00004081`; in memory (little endian) `81 40 00 00 7C 1D 00 40`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op1` | 0x35 | `pckf16f32` |
| `wmask` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `bytemask` | 0xf | (none) |
| `src1` | 0x1 | `r1.0` |
| `src2` | 0x401 | `r1.1` |
| `op2` | 0x0 | `nearest` |

`unpckf32u8 r0, r1.0, r1.1, nearest` assembles with form #160: enc[63:32] = `0x400011BC`, enc[31:0]
= `0x00004081`; in memory (little endian) `81 40 00 00 BC 11 00 40`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op1` | 0x6 | `unpckf32u8` |
| `wmask` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `bytemask` | 0xf | (none) |
| `src1` | 0x1 | `r1.0` |
| `src2` | 0x401 | `r1.1` |
| `op2` | 0x0 | `nearest` |

Mnemonics: `pckc10f16`, `pckc10f32`, `pckc10s16`, `pckc10u16`, `pckf16f16`, `pckf16f32`,
`pckf16s16`, `pckf16u16`, `pcko8f16`, `pcko8f32`, `pcko8s16`, `pcko8u16`, `pcks16f16`, `pcks16f32`,
`pcks16s16`, `pcks16u16`, `pcks8f16`, `pcks8f32`, `pcks8s16`, `pcks8u16`, `pcku16f16`, `pcku16f32`,
`pcku16s16`, `pcku16u16`, `pcku8f16`, `pcku8f32`, `pcku8s16`, `pcku8u16`, `statomic`, `unpckc10c10`,
`unpckf16c10`, `unpckf16o8`, `unpckf16s8`, `unpckf16u8`, `unpckf32c10`, `unpckf32f16`,
`unpckf32f32`, `unpckf32o8`, `unpckf32s16`, `unpckf32s8`, `unpckf32u16`, `unpckf32u8`,
`unpcks16c10`, `unpcks16o8`, `unpcks16s8`, `unpcks16u8`, `unpcku16c10`, `unpcku16o8`, `unpcku16s8`,
`unpcku16u8`, `unpcku8s8`, `unpcku8u8`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.scale`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 4 of 4 forms have semantics. The
probe vectors that test them: 4334 match the hardware, 14 stop unsupported, 0 mismatch (`pack-c10`
140/0/0, `pack-f16` 140/0/0, `pack-o8-int` 98/0/0, `pack-o8-scale` 90/0/0, `pack-round-down` 70/0/0,
`pack-round-nearest` 70/0/0, `pack-round-scale` 56/0/0, `pack-round-up` 70/0/0, `pack-round-zero`
70/0/0, `pack-s16-int` 98/0/0, `pack-s16-scale` 90/0/0, `pack-s8-int` 98/0/0, `pack-s8-scale`
90/0/0, `pack-u16-bytemask` 224/0/0, `pack-u16-int` 98/0/0, `pack-u16-scale` 90/0/0,
`pack-u8-bytemask` 224/0/0, `pack-u8-int` 98/0/0, `pack-u8-scale` 90/0/0, `pack-u8-scale-sweep`
84/0/0, `statomic` 264/0/0, `unpack-16-scale` 58/0/0, `unpack-c10` 14/0/0, `unpack-c10-16` 140/0/0,
`unpack-c10-int` 88/0/0, `unpack-f16` 14/0/0, `unpack-o8-scale` 448/0/0, `unpack-s16s8` 14/0/0,
`unpack-s16u8` 14/0/0, `unpack-s8-scale` 448/0/0, `unpack-select` 0/14/0, `unpack-u16s8` 14/0/0,
`unpack-u16u8` 14/0/0, `unpack-u8-scale` 448/0/0, `unpack-u8s8` 14/0/0, `wmask-other` 28/0/0,
`x-c10u` 192/0/0, `x-statomic` 18/0/0, `x-unpc10` 16/0/0); unsupported mostly for unpckfNuN of
component N (14).

### 7.10 Float multiply-add: `fmad`, `fadm`, `fmsa`, `fsubflr`

Class 0 **[RE]**. `enc[42:41]` picks `fmad`, `fadm`, `fmsa` (three sources) or `fsubflr` (two);
`enc[54]` reads the sources as half floats (*Source modifiers*); the float flags, a
two-bit repeat (`.repeat2`-`.repeat4`) and, when `enc[53]` is clear, `.incsXYZ`. Sources
carry `.neg`/`.abs` (source 0 `.abs` only).

- **`fmad d, a, b, c`** computes `a × b + c` in IEEE binary32 **[RE]**: the firmware's six
  uses fit, and our emulation models it so. **[HW]** It rounds to nearest even, rounds the
  product before adding (not fused) and flushes denormals to a zero of their sign (*Float
  rounding and NaN*, *Denormals*). `fmad` with no source modifiers is also the usual way to
  write a float move or add: `a × 1 + -0` gives `a` for normals, infinities and zeros
  **[HW]**, flushing denormals and turning NaNs into `0x7fc00000` (*Denormals*).
- **`fadm d, a, b, c` = `a·c + b·c`**, **`fmsa d, a, b, c` = `a·a + b·c`** (products then
  the sum rounded) and **`fsubflr d, a, b` = `a - floor(b)`** **[HW]** (*`fadm`, `fmsa`,
  `fsubflr`*).
- **`.incsXYZ`**: X, Y and Z let `a`, `b` and `c` advance in a repeat; the destination always
  advances, and without `.incs` everything does **[HW]** (*`.incsXYZ`*).
- **The SGX540** reads these classes differently (a write mask, source-0 negation, no
  `.incs`); the decoder only decodes them for the SGX545.

`fmad r0, r1, r2, r3` assembles with form #39: enc[63:32] = `0x00200000`, enc[31:0] = `0x00004103`;
in memory (little endian) `03 41 00 00 00 00 20 00`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `fmad` |
| `flags` | 0x0 | (none) |
| `repeat` | 0x0 | (none) |
| `incs` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `src0` | 0x1 | `r1` |
| `src1` | 0x2 | `r2` |
| `src2` | 0x3 | `r3` |

`fmad.repeat2.incs011 r0, r2, r4, r6` assembles with form #39: enc[63:32] = `0x00005100`, enc[31:0]
= `0x00008206`; in memory (little endian) `06 82 00 00 00 51 00 00`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `fmad` |
| `flags` | 0x0 | (none) |
| `repeat` | 0x1 | `.repeat2` |
| `incs` | 0x6 | `.incs011` |
| `dst` | 0x0 | `r0` |
| `src0` | 0x2 | `r2` |
| `src1` | 0x4 | `r4` |
| `src2` | 0x6 | `r6` |

Mnemonics: `fadm`, `fmad`, `fmsa`, `fsubflr`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 4 forms have semantics (not
yet: #41, #42). The probe vectors that test them: 1269 match the hardware, 141 stop unsupported, 0
mismatch (`fadm-fmsa-fsubflr` 198/99/0, `fadm-fmsa-fsubflr-specials` 24/12/0, `fmad-denormal`
42/0/0, `fmad-fused` 14/0/0, `fmad-move` 14/0/0, `fmad-round` 42/0/0, `fmad-zero-sign` 84/0/0,
`incs-fmad` 126/0/0, `smlsi-three-sources` 0/7/0, `sweep-fmad` 672/0/0, `x-dual3` 4/0/0, `x-fadm`
32/16/0, `x-fspec` 4/2/0, `x-misc` 13/5/0); unsupported mostly for fadm (rounding open) (129);
source N/N increment after smlsi (12).

### 7.11 Single-source float functions: `frcp`, `frsq`, `flog`, `fexp`, `fsqrt`, `fsin`, `fcos`

Class 1 **[RE]**: one source (source 1) with `.neg`, `.abs` and a data type (`.flt16.0`/`.2`
for a half, `.c10.0`-`.3` for a c10 component), a destination with a write mask or a repeat
count, the float flags plus `.tpres`. The operation is `enc[42:41]` with `enc[48]`.

- **Semantics [HW].** `frcp` = 1/x, `frsq` = 1/√x, `fsqrt` = √x, within one ulp, not
  always correctly rounded; their sources are flushed if denormal and an invalid result is the
  NaN `0x7fffffff` (*`frcp`, `frsq` and `fsqrt` precision*). `flog` = log2 and `fexp` = 2^x
  within an ulp or two; `fsin`/`fcos` take quarter turns (`sin(πx/2)`) and return 15
  fraction bits (*`flog`, `fexp`, `fsin`, `fcos`*).
- **`.tpres`** changes nothing in a fill **[HW]** (*`.tpres`, `.partial`, `.nosched`,
  `.syncs`*); its purpose is unknown.
- **In the firmware:** not used.

`frcp r1, r2` assembles with form #43: enc[63:32] = `0x08001000`, enc[31:0] = `0x00200100`; in
memory (little endian) `00 01 20 00 00 10 00 08`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `frcp` |
| `flags1` | 0x0 | (none) |
| `dst` | 0x1 | `r1` |
| `wmask` | 0x1 | (none) |
| `src1` | 0x2 | `r2` |
| `flags2` | 0x0 | (none) |

`frsq.repeat2 r4, r2.abs` assembles with form #44: enc[63:32] = `0x08201240`, enc[31:0] =
`0x00800100`; in memory (little endian) `00 01 80 00 40 12 20 08`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x1 | `frsq` |
| `flags1` | 0x0 | (none) |
| `repeat` | 0x1 | `.repeat2` |
| `dst` | 0x4 | `r4` |
| `src1` | 0x2 | `r2` |
| `flags2` | 0x2 | `.abs` |

Mnemonics: `fcos`, `fexp`, `flog`, `frcp`, `frsq`, `fsin`, `fsqrt`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`, `.tpres`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 0 of 2 forms have semantics (not
yet: #43, #44). The probe vectors that test them: 0 match the hardware, 3195 stop unsupported, 0
mismatch (`flags-other` 0/3/0, `frcp` 0/84/0, `frsq` 0/70/0, `fsingle-base` 0/48/0,
`fsingle-specials` 0/48/0, `fsqrt` 0/70/0, `sweep-fcos` 0/144/0, `sweep-fexp` 0/144/0, `sweep-flog`
0/144/0, `sweep-frcp` 0/672/0, `sweep-frsq` 0/672/0, `sweep-fsin` 0/144/0, `sweep-fsqrt` 0/672/0,
`wmask-frcp` 0/224/0, `x-fcos` 0/13/0, `x-fexp` 0/13/0, `x-flog` 0/13/0, `x-fsin` 0/13/0, `x-misc`
0/4/0); unsupported mostly for no semantics yet: frcp (983); no semantics yet: frsq (744); no
semantics yet: fsqrt (744).

### 7.12 Float dot products: `fdp`, `fdpc`, `fddp`, `fddpc`

Class 2 **[RE]**: `enc[42:41]` picks `fdp` (with `enc[14]` set, `fdpc` and a clip-plane number
`cp0`-`cp7`), `fddp` and `fddpc`. The `fddp` forms write an internal register `i0`/`i1`
besides the destination and so lose the destination's bank extension bit; `fddpc` takes two
`cp` operands and has no `.incs`. Half-float reading with `enc[54]`.

- **`fdp.repeatN d, a, b`** sums the `N` products `(a+k)·(b+k)` and writes the sum to
  register `d + N - 1` only **[HW]**: `fdp.repeat3` is a three-component dot product. Each
  product is rounded, then the sums in order, to nearest even, as chained `fmad`s would; denormals
  are flushed and an invalid result is the NaN `0x7fc00000` **[HW]** (*Float dot products*).
- **`.incsXYZ`**: X, Y and Z let source 0, 1 and 2 advance **[HW]** (*`.incsXYZ`*).
- **`fddp d, iM, s0, s1, s2`** writes `d + N - 1 = s0·s1` and `iM = s0·s2` **[HW]**; `fddpc`
  the same with its sources held still **[HW]**.
- **`fdpc`, `fddpc`**: the value is `fdp`'s and `fddp`'s **[HW]**; where the clip-plane test
  (`cpN`) lands is **[open]**: not in `p0`-`p3`, `i0`-`i2` or the registers after `d` **[HW]**.
- **In the firmware:** not used.

`fdp r0, r4, r8` assembles with form #45: enc[63:32] = `0x10200000`, enc[31:0] = `0x00000208`; in
memory (little endian) `08 02 00 00 00 00 20 10`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `repeat` | 0x0 | (none) |
| `incs` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `src1` | 0x4 | `r4` |
| `src2` | 0x8 | `r8` |

Mnemonics: `fddp`, `fddpc`, `fdp`, `fdpc`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 4 of 8 forms have semantics (not
yet: #49, #50, #51, #52). The probe vectors that test them: 1280 match the hardware, 24 stop
unsupported, 0 mismatch (`fddp` 84/0/0, `fddpc` 42/0/0, `fdp-count` 56/0/0, `fdp-incs` 126/0/0,
`fdp-rounding` 672/0/0, `fdp-specials` 112/0/0, `fdpc` 84/0/0, `fdpc-clip` 80/0/0, `x-fdpc`
24/24/0); unsupported mostly for reads rN before the program writes it (24).

### 7.13 Float min/max: `fminmax`, `fmaxmin`

Class 3 **[RE]**: the class-0 layout with three sources, the operation in `enc[41]` (`enc[42]`
does not decode).

- **`fminmax d, a, b, c` = `min(max(a, b), c)`** and **`fmaxmin d, a, b, c` = `max(min(a, c),
  b)`** **[HW]**: both clamp `a` to `[b, c]`, and differ when `b > c` (`c`, `b`). min and max
  are IEEE minNum/maxNum (a NaN loses, `-0 < +0`), denormals flushed **[HW]** (*Float min/max*).
- **`.incsXYZ`** as for `fmad` **[HW]**.
- **In the firmware:** not used.

`fminmax r0, r1, r2, r3` assembles with form #53: enc[63:32] = `0x18200200`, enc[31:0] =
`0x00004103`; in memory (little endian) `03 41 00 00 00 02 20 18`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x1 | `fminmax` |
| `flags` | 0x0 | (none) |
| `repeat` | 0x0 | (none) |
| `incs` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `src0` | 0x1 | `r1` |
| `src1` | 0x2 | `r2` |
| `src2` | 0x3 | `r3` |

Mnemonics: `fmaxmin`, `fminmax`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 2 forms have semantics (not
yet: #54). The probe vectors that test them: 1203 match the hardware, 0 stop unsupported, 0 mismatch
(`fmaxmin-roles` 224/0/0, `fmaxmin-specials` 308/0/0, `fminmax-incs` 126/0/0, `fminmax-roles`
224/0/0, `fminmax-specials` 308/0/0, `minmax-allnan` 12/0/0, `x-allnan` 1/0/0).

### 7.14 Screen-space derivatives: `fdsx`, `fdsy`

Class 4 **[RE]**: two sources in the class-0 layout, the operation in `enc[41]`.

- **Semantics [HW].** Coarse derivatives over the 2x2 quad (GLSL `dFdx`/`dFdy`): `fdsx d, a, b`
  = `a`(top-right) − `b`(top-left), `fdsy d, a, b` = `a`(bottom-left) − `b`(top-left), the
  same value in all four pixels (*Derivatives*).
- **In the firmware:** not used.

`fdsx r1, r2, r3` assembles with form #55: enc[63:32] = `0x20200000`, enc[31:0] = `0x00200103`; in
memory (little endian) `03 01 20 00 00 00 20 20`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `fdsx` |
| `flags` | 0x0 | (none) |
| `repeat` | 0x0 | (none) |
| `incs` | 0x1 | (none) |
| `dst` | 0x1 | `r1` |
| `src1` | 0x2 | `r2` |
| `src2` | 0x3 | `r3` |

Mnemonics: `fdsx`, `fdsy`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 2 forms have semantics (not
yet: #56). The probe vectors that test them: 42 match the hardware, 22 stop unsupported, 0 mismatch
(`fds-incs` 0/12/0, `fds-quad` 30/0/0, `x-fds` 12/0/0, `x-m2` 0/7/0, `x-misc` 0/3/0); unsupported
mostly for fds repeated (22).

### 7.15 Half-float multiply-add: `fmad16`

Class 6 **[RE]**: three sources, each with a swizzle (`.swzll`, `.swzhh`, `.swzf32`) and
`.neg`/`.abs` (source 0 `.abs` only); `enc[44]` is a repeat of two, which brings `.incsXYZ`;
no `.end`.

- **Semantics [HW].** Two half-float multiply-adds at once, `a × b + c` on the low and on the
  high halves: the product rounded to binary32, the sum to a half, both to nearest even (one
  rounding for two halves), half denormals kept. `.swzll`/`.swzhh` feed both lanes one half;
  `.swzf32` feeds the 32-bit register as a binary32 value. `.incsXYZ` as for `fmad`
  (*Half-float multiply-add*).
- **In the firmware:** not used.

`fmad16 r0, r1, r2, r3` assembles with form #57: enc[63:32] = `0x30000000`, enc[31:0] =
`0x00004103`; in memory (little endian) `03 41 00 00 00 00 00 30`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags1` | 0x0 | (none) |
| `flags2` | 0x0 | (none) |
| `dst` | 0x0 | `r0` |
| `src0` | 0x1 | `r1` |
| `flags3` | 0x0 | (none) |
| `src1` | 0x2 | `r2` |
| `flags4` | 0x0 | (none) |
| `src2` | 0x3 | `r3` |
| `flags5` | 0x0 | (none) |

Mnemonics: `fmad16`.

Modifiers: `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 819 match the hardware, 13 stop unsupported, 0 mismatch (`fmad16-incs`
96/0/0, `fmad16-lanes` 396/0/0, `fmad16-rounding` 220/0/0, `fmad16-specials` 24/12/0, `x-fmad16`
52/0/0, `x-fmad16c` 1/0/0, `x-fmad16r` 4/0/0, `x-fmad16r2` 24/0/0, `x-fmad16s` 0/1/0, `x-swzf32`
2/0/0); unsupported mostly for a half infinity or NaN operand (13).

### 7.16 Extended float operation: `efo`

Class 7 **[RE]**. Every bit is a field: two multipliers `m0`, `m1` and two adders `a0`, `a1`
whose inputs are selections among the three sources and the internal registers `i0`, `i1`;
the destination takes one of `i0`, `i1`, `a0`, `a1`; `i0` and `i1` are written (or not,
`!i0`) from the adders or multipliers; each source has a `.neg`; flags `.skipinv`,
`.nosched`, `.gpiext` and a repeat.

```
efo d= <i0|i1|a0|a1>, <i0/i1 writes>, a0=<x>+<y>, a1=<x>+<y>, m0=<x>*<y>, m1=<x>*<y>, s0, s1, s2
```

- **Semantics [HW].** A small dataflow of two multiplies and two adds per instruction,
  with `i0`/`i1` as accumulators across instructions: the text the decoder prints is the
  dataflow. Each product and each sum is rounded to binary32; the destination's `i0`/`i1`
  are the values before the instruction's own writes. Sources print both their 32-bit and
  half-float readings (`r1/r1.flt16.0`) where the two differ; the 32-bit reading is the one we
  probed.
- **`.gpiext` [HW]** works across the 2x2 quad: the multipliers read the instance's `i2` and
  `i0` for `i0` and `i1`, an `i0` write goes to its `i2`, and the four `i1` writes end in the
  top-right instance's `i0`, whose reads during the instruction see the top-left instance's
  `i0` or, by timing, its write (*`efo`*).
- **In the firmware:** not used.

Mnemonics: `efo`.

Modifiers: `.gpiext`, `.nosched`, `.repeatN`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 1023 match the hardware, 73 stop unsupported, 0 mismatch
(`efo-dataflow` 451/0/0, `efo-denormals` 0/4/0, `efo-gpiext` 0/38/0, `efo-gpiext-quad` 45/0/0,
`efo-gpiext-race` 0/10/0, `efo-rounding` 264/0/0, `x-efo` 148/2/0, `x-efor` 48/0/0, `x-gpiext`
1/4/0, `x-gpiext-map` 18/0/0, `x-gpiext-map2` 24/0/0, `x-gpiext-map3` 2/5/0, `x-gpiext-map5` 22/6/0,
`x-gpiext-xtask` 0/2/0, `x-misc` 0/2/0); unsupported mostly for efo.gpiext reaches the other
instances of the quad (43); the top-right iN read races the iN writes (20); reads iN before the
program writes it (8).

### 7.17 Fixed-point multiply-add: `fpma`

Class 25 **[RE]**: destination and three sources in the c10 reading, then colour selections
(`s0`/`s1`/`s0a`/`s1a`, `s1`/`s1a`, `s2`/`s2a`) and alpha selections, each with a `.comp`
complement; the flags carry a three-bit repeat and `.sat`.

- **Semantics [HW].** A colour multiply-add, `c0 + c1 × c2` per channel with the first
  selection the addend, on 8-bit channels or, with `.c10`, signed 10-bit ones (*`fpma`*);
  `.sat` changes nothing we measured, also on `c10` results out of 0..1.0, and a repeat
  advances every operand **[HW]**.
- **In the firmware:** not used.

Mnemonics: `fpma`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.sat`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 543 match the hardware, 0 stop unsupported, 0 mismatch
(`colour-dot-repeats` 15/0/0, `fpma` 396/0/0, `fpma-sat` 60/0/0, `x-fpma` 48/0/0, `x-rep` 24/0/0).

### 7.18 Dual issue: two operations in one word

Class 27 **[RE]** packs a **primary** operation on the usual operands and a **secondary**
operation on internal registers into one word, printed `primary + secondary`:

```
fmad r0, r1, r2, r3+fmad i0, i0, i1, i2
mov r0, r1+frcp i0, i1
```

- **Primary** (`enc[51:50]` with `enc[58]`): `fmad`, `fadm`, `fmsa`, `fminmax`, `ima32`,
  `mov`, `fddp` or `fssq`, with `.skipinv`, `.syncs`, `.nosched` and `.repeat2`, a two-bit
  predicate and a destination without the bank extension.
- **Secondary** (`enc[41:38]`): `fmad`, `fmad16`, `fmsa`, `fmaxmin`, `ima32`, `fsubflr`,
  `fadd`, `fsub`, `fmul`, `frcp`, `frsq`, `fexp`, `flog`, `fsin`, `fcos` or `mov`. Its
  destination is `i0`-`i2`; its operands are `i0`-`i2` or, for selection 3, the primary's
  source in the same position. Because of that shared reference, one form exists per
  secondary operand shape and per selection, which is why the class has some sixty forms.
- `enc[54]` reads the sources as half floats, as in class 0.
- **Semantics [HW].** Two operations issued together: both read the values from before the
  word, the secondary wins a shared destination, a selection-3 source is the primary's register
  without its `.neg`, `.repeat2` repeats both, and the two-bit predicate (`enc[57:56]`: always,
  `p0`, `p1`, `!p0`) covers both; `fssq d, a, b` is `a·a + b·b`. Pairs that need the same unit
  give mixed results, the secondary taking the primary's intermediate value (*Dual issue*).
- **In the firmware:** not used.

`mov r0, r1+frcp i0, i1` assembles with form #68: enc[63:32] = `0xD8044140`, enc[31:0] =
`0x00000080`; in memory (little endian) `80 00 00 00 40 41 04 D8`.

| field | value | prints |
|---|---|---|
| `sop` | 0x5 | `frcp` |
| `sreg` | 0x0 | `i0` |
| `ssrc1` | 0x401 | `i1` |
| `pred` | 0x0 | (none) |
| `op` | 0x1 | `mov` |
| `dst` | 0x0 | `r0` |
| `src1` | 0x1 | `r1` |

Mnemonics: `+fadd`, `+fcos`, `+fexp`, `+flog`, `+fmad`, `+fmad16`, `+fmaxmin`, `+fmsa`, `+fmul`,
`+frcp`, `+frsq`, `+fsin`, `+fsub`, `+fsubflr`, `+ima32`, `+mov`, `fadm`, `fddp`, `fmad`, `fminmax`,
`fmsa`, `fssq`, `ima32`, `mov`.

Modifiers: `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 24 of 59 forms have semantics (not
yet: #84, #85, #86, #87, #88, #89, #90, #91, #92, #93, #94, #95, #96, #97, #98, #99, #100, #101,
#102, #103, #104, #105, #106, #107, #108, #109, #110, #111, #112, #113, #114, #115, #116, #117,
#118). The probe vectors that test them: 204 match the hardware, 318 stop unsupported, 0 mismatch
(`dual-issue` 75/40/0, `dual-issue-conflicts` 0/60/0, `dual-issue-ima32` 0/34/0,
`dual-issue-predicates` 60/0/0, `dual-issue-shared` 0/14/0, `x-dual` 43/164/0, `x-dual2` 10/6/0,
`x-dual3` 16/0/0); unsupported mostly for dual issue secondary imaN (23); dual issue fmad + imaN
(18); dual issue fssq + imaN (17).

### 7.19 Loads and stores: `ld*`, `st*`, `eldd`, `eldq`

Classes 29 (loads) and 30 (stores) **[RE]**. The mnemonic is `ld`/`st` plus an address mode and
a data type: `ldad` = load, absolute (`a`), dword (`d`); `l` and `t` are the other address
modes (local and tiled **[infer]**, both **[open]**), `w` and `b` 16- and 8-bit data. The base
is a source-0 operand, the offset a source-1 operand (an immediate or a register), and
`enc[41:40]` picks plain, pre-increment or post-increment addressing. Flags: `.skipinv`,
`.syncs`, `.nosched` (stores), the cache mode (`.bpcache`, `.fcfill`, `.bypassl1`), `.noread`
and `.rangeenable` (stores), and a count used as `.fetchN` (MOE-expand bit `enc[53]` clear)
or `.repeatN` (set). A load can name a range operand (source 2) and always names a data fence
`drc0`/`drc1`.

**Addressing [HW/RE].** Offsets count elements of the data size (4 bytes for `d`, 2 for `w`):

| form | elements accessed | base writeback |
|------|-------------------|----------------|
| `[b,+#k]` / `[b,-#k]` | `b+k` / `b-k` | none |
| `[b,++#k]` (pre) | `b+k` | `b += k` |
| `[b,#k++]` (post) | `b` | `b += k` |
| `.fetchN [b,+#k]` | **`b+k+1` ... `b+k+N`** | none |
| `.fetchN [b,#k++]` | `b+k` ... `b+k+N-1` | `b += N` |

A fetch with a base-plus-offset address starts **one element after** the printed offset:
`ldad.fetch4 r3, [r2,+#1]` loads dwords 2-5 into `r3`-`r6`. The plain forms and the
post-increment fetch were checked on the laptop (the kernel CCB fetch and the GETMISCINFO
reply, `design/sgx545_hw_spec.md` §13.10a) **[HW]**; the writeback and the base-plus-offset
fetch follow from the firmware's loops and structure offsets **[RE]**. A **register** offset
`o` (`[b,+rN]`) is `(o >> 16) × (o & 0xffff)` bytes, and a fetch starts one element after it
too **[HW]** (*Register offsets of loads and stores*); the firmware has four such stores.

- **Cache modes [open].** The firmware uses `.bpcache` on almost every access; `.fcfill` once.
  The SGX540 swaps the two cache-mode bits; `usse-dis --core 540` follows it.
- **`eldd`/`eldq`** (class 29 with `enc[54]` set) take a base, an index and an 11-bit offset
  and a source-2 operand, and load a dword or a quad **[RE]**; their semantics are **[open]**
  and the firmware does not use them.

**In the firmware [RE].** 598 loads and 349 stores, nearly all `ldad`/`stad` with `.bpcache`
on its control blocks through `sa0`-`sa2` or computed bases; 120 fetches; `ldaw` twice. Its
807 `wdf` (for 598 loads and 196 `ldr`) show the pattern: issue the loads, then wait on the
counter before the first use.

From `tools/usse-asm/progs/use_vtx.s`:

```
        p0 ldad.skipinv.fetch4 pa0, [pa0,+#0], drc0
```

`ldad.fetch4 r3, [r2,+#1], drc0` assembles with form #29: enc[63:32] = `0xE8033000`, enc[31:0] =
`0xA0608080`; in memory (little endian) `80 80 60 A0 00 30 03 E8`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `ad` |
| `flags` | 0x60 | `.fetch4` |
| `dst` | 0x3 | `r3` |
| `base` | 0x2 | `r2` |
| `mode` | 0x0 | (none) |
| `sign` | 0x0 | `+` |
| `src1` | 0x301 | `#1` |
| `extra` | 0x300 | (none) |
| `drc` | 0x0 | `0` |

`stad [r2,+#7], r9` assembles with form #32: enc[63:32] = `0xF0220000`, enc[31:0] = `0x80008389`; in
memory (little endian) `89 83 00 80 00 00 22 F0`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x0 | `ad` |
| `flags` | 0x80 | (none) |
| `base` | 0x2 | `r2` |
| `mode` | 0x0 | (none) |
| `sign` | 0x0 | `+` |
| `src1` | 0x307 | `#7` |
| `src2` | 0x9 | `r9` |

Mnemonics: `eldd`, `eldq`, `ldab`, `ldad`, `ldaw`, `ldlb`, `ldld`, `ldlw`, `ldtb`, `ldtd`, `ldtw`,
`stab`, `stad`, `staw`, `stlb`, `stld`, `stlw`, `sttb`, `sttd`, `sttw`.

Modifiers: `.bpcache`, `.bypassl1`, `.fcfill`, `.fetchN`, `.noread`, `.nosched`, `.rangeenable`,
`.repeatN`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 6 of 7 forms have semantics (not
yet: #123). The probe vectors that test them: 555 match the hardware, 28 stop unsupported, 0
mismatch (`cache-control` 100/0/0, `ldad-reg-off` 224/0/0, `ldad-reg-off-fetch` 0/28/0,
`ldad-reg-off-neg` 56/0/0, `stad-reg-off` 90/0/0, `stad-reg-off-unaligned` 45/0/0, `x-cache2`
40/0/0); unsupported mostly for flag .fetchN with the MOE state unknown (28).

### 7.20 Texture sampling: `smp1d`, `smp2d`, `smp3d`

Class 28 **[RE]**: the dimension (`1d`, `2d`, `3d`, `enc[43:42]`) and a LOD mode (none,
`bias`, `replace`, `grad`, `enc[41:40]`; `3d` with a LOD mode does not decode); flags
`.skipinv`, `.syncs`, `.nosched`, the PCF mode (`.pcf`, `.pcff16`) and the SRD mode (`.sinf`,
`.rsd`); a destination `r` or `pa` with a two-bit write mask or a repeat count; the
coordinates as a source-0 operand with a format (`.flt16`, `.c10`); the texture state as a
source-1 operand; the LOD value as source 2 when a LOD mode is set; a data fence `drc0`-`drc3`.

- **Semantics [HW].** Source 1 holds the four texture state words (`s` to `s + 3`, the `doutt`
  words of our copies), source 0 the normalized coordinates (`c`, `c + 1`; `.flt16`: two halves
  of one register); the texel arrives in the destination once `drcN` is waited for. Point
  sampling clamps to the edge (*Texture sampling*); filters, address modes, formats,
  conversions and `.pcf` follow the state words (*Sampler state*). The LOD values and the SRD
  modes are **[open]**. Our copy path does not use it: the PDS issues the texture fetch
  (`doutt`) and the result arrives in `pa0` before the task starts
  (`design/sgx545_transfer.md` §4.4.3) **[HW]**.
- **In the firmware:** not used.

`smp2d r0, r4, c0, drc0` assembles with form #119: enc[63:32] = `0xE0021400`, enc[31:0] =
`0x40010000`; in memory (little endian) `00 00 01 40 00 14 02 E0`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `op` | 0x1 | `2d` |
| `flags1` | 0x0 | (none) |
| `wmask` | 0x1 | (none) |
| `dst` | 0x0 | `r0` |
| `src0` | 0x4 | `r4` |
| `flags2` | 0x0 | (none) |
| `src1` | 0x280 | `c0` |
| `drc` | 0x0 | `0` |

Mnemonics: `smp1d`, `smp1dbias`, `smp1dgrad`, `smp1dreplace`, `smp2d`, `smp2dbias`, `smp2dgrad`,
`smp2dreplace`, `smp3d`, `smp3dbias`, `smp3dgrad`, `smp3dreplace`.

Modifiers: `.nosched`, `.pcf`, `.pcff16`, `.repeatN`, `.rsd`, `.sinf`, `.skipinv`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 0 of 4 forms have semantics (not
yet: #119, #120, #121, #122). The probe vectors that test them: 0 match the hardware, 726 stop
unsupported, 0 mismatch (`smp-address` 0/40/0, `smp-address-border` 0/4/0, `smp-bilinear` 0/21/0,
`smp-convert` 0/48/0, `smp-filter-modes` 0/8/0, `smp-formats` 0/57/0, `smp-formats-other` 0/40/0,
`smp-lod` 0/11/0, `smp-pcf` 0/28/0, `smp-point` 0/65/0, `smp-twiddled` 0/16/0, `x-smp` 0/108/0,
`x-smp2` 0/132/0, `x-smp3` 0/148/0); unsupported mostly for no semantics yet: smp2d (655); no
semantics yet: smp2dreplace (37); no semantics yet: smp2dgrad (15).

### 7.21 Register file access: `ldr`, `str`

Class 31, other group, operation 6 **[RE]**. `ldr rD, #idx, drcN` reads SGX register `idx`
(dword index, offset `idx × 4`) into a temporary or `pa` register, counted on a data fence;
`str #idx, src` writes it. The index is a 14-bit immediate or a source-2 register; flags
`.skipinv` and `.nosched`.

- **Semantics [HW].** `ldr r0, #7, drc0` returns `CORE_ID` (`0x01150000`) and `ldr r0, #801`
  returns `BIF_DIR_LIST_BASE0`, from a client pixel shader on our render node; a client
  `str #671` (`USE_G0`) changed nothing (`design/sgx545_render_uapi.md` §7 Q4). The firmware
  programs the whole GPU with these two instructions.
- **The index [HW].** `ldr rD, rI` masks a register index to 14 bits (*The 14-bit
  `ldr`/`str` index*); `str` with a register index is **[open]**.
- The decoder holds two copies of these forms (an older one and the class-31 family's); the
  older ones match first.

**In the firmware [RE].** 196 `ldr` and 422 `str`; those with an immediate index touch 191
distinct registers, all of them named SGX545 registers (`design/sgx545_hw_spec.md` §13.6,
`tools/usse-dis/regtouch.py`).

`ldr r0, #7, drc0` assembles with form #35: enc[63:32] = `0xFE210000`, enc[31:0] = `0x20000007`; in
memory (little endian) `07 00 00 20 00 00 21 FE`.

| field | value | prints |
|---|---|---|
| `flags` | 0x0 | (none) |
| `dst` | 0x0 | `r0` |
| `imm` | 0x7 | `7` |
| `drc` | 0x0 | `0` |

`str #671, r1` assembles with form #37: enc[63:32] = `0xFE290000`, enc[31:0] = `0x2001409F`; in
memory (little endian) `9F 40 01 20 00 00 29 FE`.

| field | value | prints |
|---|---|---|
| `flags` | 0x0 | (none) |
| `imm` | 0x29f | `671` |
| `src1` | 0x1 | `r1` |

Mnemonics: `ldr`, `str`.

Modifiers: `.nosched`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 8 of 8 forms have semantics. The
probe vectors that test them: 0 match the hardware, 70 stop unsupported, 0 mismatch
(`ldr-index-mask` 0/70/0); unsupported mostly for no SGX registers on this bus (70).

### 7.22 Flow control: `ba`, `br`, `lapc`, `bexception`

Class 31 with `enc[54:52]` = 0 and the operation in `enc[40:38]` **[RE]**: 0 `ba`, 1 `br`,
2 `lapc`, 3 and 4 the `pclink` moves (*Moves*), 5 `nop` (*Synchronisation and misc*);
`bexception` sits next to them. The branch flags are `.nosched`, `.synce`/`.syncent`,
`.savelink` and `.mon`; `ba` and `br` take the full predicate.

- **`ba #n`** jumps to instruction `n`; **`br #k`** by a signed offset from the `br` itself
  **[HW]**; `.savelink` makes either a call; **`lapc`** returns to `pclink` **[RE/HW]**. See
  *Program counter, calls and the end of a program* for the evidence.
- **`.synce`, `.syncent`, `.mon`, `bexception`** are **[open]**.

**In the firmware [RE].** 954 `ba` (591 predicated, 232 calls), 12 `br`, 74 `lapc`. The kernel
CCB dispatch jumps to its handler with `mov pclink, r4; lapc`.

`ba.savelink #0x00000100` assembles with form #12: enc[63:32] = `0xF8000200`, enc[31:0] =
`0x00000100`; in memory (little endian) `00 01 00 00 00 02 00 F8`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x8 | `.savelink` |
| `imm` | 0x100 | `0x00000100` |

`p0 ba #0x00000010` assembles with form #12: enc[63:32] = `0xF9000000`, enc[31:0] = `0x00000010`; in
memory (little endian) `10 00 00 00 00 00 00 F9`.

| field | value | prints |
|---|---|---|
| `pred` | 0x1 | `p0` |
| `flags` | 0x0 | (none) |
| `imm` | 0x10 | `0x00000010` |

`br -#0x00000003` assembles with form #13: enc[63:32] = `0xF8000040`, enc[31:0] = `0x0003FFFD`; in
memory (little endian) `FD FF 03 00 40 00 00 F8`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `imm` | 0x3fffd | `-#0x00000003` |

Mnemonics: `ba`, `bexception`, `br`, `lapc`.

Modifiers: `.mon`, `.nosched`, `.savelink`, `.synce`, `.syncent`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 3 of 4 forms have semantics (not
yet: #14). The probe vectors that test them: 14 match the hardware, 0 stop unsupported, 0 mismatch
(`br-offset` 5/0/0, `x-m2` 5/0/0, `x-misc` 4/0/0).

### 7.23 Synchronisation and misc: `nop`, `wdf`, `idf`, `setm`, `wop`, `lock`, `release`

Class 31 **[RE]**: `nop` is flow-control operation 5; the rest are the "other" group
(`enc[53:52]` = 2) with the operation in `enc[58:56]`: 0 `idf`, 1 `wdf`, 2 `setm`, 5
`lock`/`release` (by `enc[0]`, a three-bit mutex number), 7 `wop`.

- **`nop`** does nothing **[RE]**; with `.end` it ends the program **[HW]** (our USE
  secondary program). `.tog` and `.trigger` are **[open]**.
- **`wdf drcN`** waits for the loads counted on `drcN` **[RE/HW]**; **`idf drcN, bif`** is a
  fence on memory writes counted the same way **[infer]** (*Memory, data fences and the
  register file*).
- **`wop #n`** waits for the outputs to drain before an emit **[infer]**: our state programs
  issue `wop #0` between `emitvcbst` and `emitmtest`, as the captured ones do **[CAP/HW]**.
  `.twopart` is **[open]**.
- **`lock #n` / `release #n`** take and give back one of eight mutexes **[infer]**. The firmware
  executes `lock` twice and never `release` (whether something else releases the mutex is
  **[open]**).
- **`setm #n`** (a four-bit number) is **[open]**.

**In the firmware [RE].** 807 `wdf` (711 on `drc0`), 124 `idf ..., bif`, 87 `nop` (5 with
`.end`, 1 with `.nosched`), 2 `lock`.

From `tools/usse-asm/progs/use_mtest18.s`:

```
        nop.nosched
        wdf drc0
        wop #0
```

`idf drc1, bif` assembles with form #6: enc[63:32] = `0xF8200001`, enc[31:0] = `0x00000000`; in
memory (little endian) `00 00 00 00 01 00 20 F8`.

| field | value | prints |
|---|---|---|
| `drc` | 0x1 | `1` |

Mnemonics: `idf`, `lock`, `nop`, `release`, `setm`, `wdf`, `wop`.

Modifiers: `.end`, `.nosched`, `.syncs`, `.tog`, `.trigger`, `.twopart`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 9 forms have semantics (not
yet: #141, #142). The probe vectors that test them: 0 match the hardware, 56 stop unsupported, 0
mismatch (`sa-values` 0/56/0); unsupported mostly for reads saN before the program writes it (56).

### 7.24 MOE control: `smoa`, `smr`, `smlsi`, `smbo`, `imo`, `setfc`, `moest`

Class 31 with `enc[53:52]` = 1, the operation in `enc[58:56]`, no predicate and `.nosched` in
`enc[50]` **[RE]**. `smoa`, `smr`, `smlsi` and `smbo` take either two register operands
(`enc[51]` set) or immediates:

- `smoa #o1, #o2, #o3, #o4, m1, m2, m3, m4`: four signed 10-bit offsets and four address modes
  (`none`, `repeat`, `clamp`, `mirror`);
- `smr #a, #b, #c, #d` and `smbo #a, #b, #c, #d`: four 12-bit values (ranges and base
  offsets);
- `smlsi i1, i2, i3, i4, <modes>, #l1, #l2, #l3`: four 8-bit operands, each a signed increment
  or, in swizzle mode, a four-component swizzle, then three four-bit limits times four;
- `imo #a, #b, #c, #d` (four 12-bit immediates), `setfc #a, #b` (two bits) and
  `moest d, #sel`, which reads a word of the MOE state back into a register: the selector
  names *SMOA*, *SMLSI*, *SMBO* and *SMR* low and high data and an index word.

- **Semantics [infer].** These set the per-operand register stepping that repeats and fetches
  use (*Repeats and the MOE state*): `smoa` an offset and an address mode per operand, `smbo`
  base offsets, `smr` ranges, `smlsi` increments or swizzles. **[HW]** `smlsi`'s four
  increments are the destination's and sources 0, 1 and 2's for a following repeat (*`smlsi`
  increments*); the rest is **[open]**.

**In the firmware [RE].** Only `smlsi`, eight times, at task entry points, always with
increments 0, 0, 0, 1 in increment mode and limits 0.

`smr #4, #0, #0, #0` assembles with form #133: enc[63:32] = `0xF9100040`, enc[31:0] = `0x00000000`;
in memory (little endian) `00 00 00 00 40 00 10 F9`.

| field | value | prints |
|---|---|---|
| `nosched` | 0x0 | (none) |
| `imm1` | 0x4 | `4` |
| `imm2` | 0x0 | `0` |
| `imm3` | 0x0 | `0` |
| `imm4` | 0x0 | `0` |

Mnemonics: `imo`, `moest`, `setfc`, `smbo`, `smlsi`, `smoa`, `smr`.

Modifiers: `.nosched`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 11 forms have semantics (not
yet: #128, #129, #130, #131, #132, #133, #134, #136, #137, #138). The probe vectors that test them:
1 match the hardware, 12 stop unsupported, 0 mismatch (`smlsi-three-sources` 0/7/0, `x-misc` 1/5/0);
unsupported mostly for source N/N increment after smlsi (12).

### 7.25 Phases: `phas`

Class 31 **[RE]**. The immediate form is described in *Tasks and phases*: the next phase's
instruction index (18 bits), its temporaries (eight bits, times four), the rate, the wait
condition and the instance mode, plus `.end`. A register form takes two source operands
instead (presumably the same information computed at run time **[infer]**). Both exist twice
in the decoder's table, as for `ldr`/`str`.

From `tools/usse-asm/progs/use_vtx.s`:

```
        phas #7, #0, pixel, vcull, parallel
        phas #0, #0, pixel, end, parallel
```

`phas #7, #0, pixel, vcull, parallel` assembles with form #8: enc[63:32] = `0xFA440200`, enc[31:0] =
`0x00000007`; in memory (little endian) `07 00 00 00 00 02 44 FA`.

| field | value | prints |
|---|---|---|
| `end` | 0x0 | (none) |
| `imm1` | 0x7 | `7` |
| `imm2` | 0x0 | `0` |
| `op1` | 0x0 | `pixel` |
| `op2` | 0x2 | `vcull` |
| `op3` | 0x0 | `parallel` |

Mnemonics: `phas`.

Modifiers: `.end`, `.nosched`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 4 forms have semantics (not
yet: #158, #159).

### 7.26 Emits: `emitpix1`, `emitpix2`, `emitvcb*`, `emitmte*`, `emitpds`

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

From `tools/usse-asm/progs/use_eot.s`:

```
        emitpix1.freep #0 /* incp */, r0, r1
        emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000000
```

From `tools/usse-asm/progs/use_vtx.s`:

```
        emitvcbvtx.end.twopart #0, #0
        emitmtevtx.end.twopart.freep #0, #0, #0
```

Mnemonics: `emitmtest`, `emitmtevtx`, `emitpds`, `emitpix1`, `emitpix2`, `emitvcbst`, `emitvcbvtx`.

Modifiers: `.cut`, `.end`, `.freep`, `.nosched`, `.taske`, `.tasks`, `.threepart`, `.twopart`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 7 forms have semantics.

### 7.27 Visibility tests: `pcoeff`, `ptoff`, `atst8`, `depthf`

Class 31 with `enc[53:52]` = 3 **[RE]**: operation 0 is `pcoeff` or `ptoff` (by `enc[47]`), 1
`atst8`, 3 `depthf`. `atst8` writes a destination and a predicate `p0`-`p3` (or not, `!`),
with `.c10`; `atst8` and `depthf` test three sources with `twosided` and a feedback mode
(`fb`, `nfb`, `optdwd`) and take a two-bit predicate.

- **`ptoff`** signals the end of the punch-through phase for the pixels of the task
  **[infer]**: our 3D render's PTOFF task is the single instruction `ptoff.end`, the one the
  closed driver's PTOFF task runs, and renders run with it **[CAP/HW]**.
- **`atst8`** (an 8-bit alpha test with feedback to the ISP?), **`depthf`** (depth feedback?)
  and **`pcoeff`** (plane coefficients?) are **[infer]** by name only and **[open]**.
- **In the firmware:** not used.

From `tools/usse-asm/progs/use_ptoff.s`:

```
; use_ptoff.s -- the PTOFF task of a 3D render (design/sgx545_3d.md 9.4).
;
; pds_pixevent.s starts it for the PTOFF pixel event (ir1 bit 3). A transfer never
; sees that event and points the PTOFF case at its end-of-tile task; a 3D render
; gives it a task of its own that only signals ptoff and ends [infer: from the name,
; the instruction the closed 3D driver's PTOFF task runs].

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        ptoff.end
```

`ptoff.end` assembles with form #154: enc[63:32] = `0xF8348000`, enc[31:0] = `0x00000000`; in memory
(little endian) `00 00 00 00 00 80 34 F8`.

| field | value | prints |
|---|---|---|
| `nosched` | 0x0 | (none) |
| `end` | 0x1 | `.end` |

Mnemonics: `atst8`, `depthf`, `pcoeff`, `ptoff`.

Modifiers: `.c10`, `.end`, `.nosched`, `.syncs`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 4 forms have semantics (not
yet: #153, #155, #156).

### 7.28 Cache control: `ci`, `cf`, `cfi`

Class 31 with `enc[54:52]` = 4, operation 0 **[RE]**: `ci`, `cf` or `cfi` by `enc[45:44]` (0
does not decode), flags `.nosched`, `.global`, `.dm_nomatch`, two sources and a cache-level
operand that the decoder annotates: 1 `L0 and L1`, 2 `L2`, 3 `L0, L1 and L2`.

- **Semantics [infer].** Cache invalidate, flush, and flush-and-invalidate over the named
  levels, for the address range the sources give (or everything, `.global`?). **[HW]** none
  of the forms writes a register, loses a store or makes the task's next load stale (*Cache
  control*); their effect on other units is **[open]**.

**In the firmware [RE].** Four `ci.global` and one `cfi.global`, all on the three levels.

`ci r1, r2, #1 /* L0 and L1 */` assembles with form #157: enc[63:32] = `0xF8401100`, enc[31:0] =
`0x00000082`; in memory (little endian) `82 00 00 00 00 11 40 F8`.

| field | value | prints |
|---|---|---|
| `op` | 0x1 | `ci` |
| `flags` | 0x0 | (none) |
| `src1` | 0x1 | `r1` |
| `src2` | 0x2 | `r2` |
| `imm` | 0x1 | `1 /* L0 and L1 */` |

Mnemonics: `cf`, `cfi`, `ci`.

Modifiers: `.dm_nomatch`, `.global`, `.nosched`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 68 match the hardware, 0 stop unsupported, 0 mismatch (`cache-control`
50/0/0, `x-cache2` 18/0/0).

### 7.29 Video: `adif`, `adifsum`, `bilin`, `firv`, `firh`

Classes 22 and 23 **[RE]**. Class 22 picks by `enc[53:52]`: `adif` (or `adifsum` with
`enc[38]`), `bilin`, `firv`; class 23 is `firh`. All take the multiply flags and a two-bit
predicate, and work on 8-bit data (`u8`, `s8`, `o8`).

- **Semantics.** `adif` is the per-byte absolute difference and `adifsum` the sum of the four
  plus a 16-bit half of `s0` **[HW]** (*Video*). By the names, `bilin` is a bilinear
  interpolation (`planar` or `interleaved` data, source and destination pairs) and `firv`/
  `firh` vertical and horizontal FIR filters (`firh` with an edge mode `rep`/`msc`/`mdc`, a
  coefficient set `fcsN` and two small numbers) **[infer]**; they ran without a fault but
  their results fit no model yet **[open]**.
- **In the firmware:** not used.

`adif r1, r2, r3` assembles with form #213: enc[63:32] = `0xB0000000`, enc[31:0] = `0x00200103`; in
memory (little endian) `03 01 20 00 00 00 00 B0`.

| field | value | prints |
|---|---|---|
| `pred` | 0x0 | (none) |
| `flags` | 0x0 | (none) |
| `dst` | 0x1 | `r1` |
| `src1` | 0x2 | `r2` |
| `src2` | 0x3 | `r3` |

Mnemonics: `adif`, `adifsum`, `bilin`, `firh`, `firv`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 5 forms have semantics (not
yet: #215, #216, #217). The probe vectors that test them: 132 match the hardware, 135 stop
unsupported, 0 mismatch (`video-adif` 120/0/0, `video-filters` 0/105/0, `x-video` 12/30/0);
unsupported mostly for no semantics yet: bilin (75); no semantics yet: firv (39); no semantics yet:
firh (21).

## 8. Hardware probe results

What the instruction text leaves open was asked of the GPU on 2026-10-22: our own pixel
programs ran each instruction under test on preset inputs and wrote the result to the pixel
(`tools/usse-asm/probe.py`, the probe table in `tools/usse-asm/progs/probes/`). The results are
summarised in `design/sgx545_hw_spec.md` §14 and given with their evidence in
`tools/usse-dis/r2/README.md` ("Hardware probes"), whose radare2 ESIL models them. Each fact is
one snippet in `doc/isa/content/usse/probe/`, marked *Measured on the hardware (2026-10-22)*,
with what stays undecided and what would settle it, or *Not probed yet*:

- §8.1 The test sign `p` **[HW]**
- §8.2 `t` in the `a`/`o` joins of a test **[HW]**
- §8.3 The 32-bit adds `iadd32` and `iaddu32` **[HW]**
- §8.4 Test channels **[HW]**
- §8.5 `isub16`/`isubu16` lanes **[HW]**
- §8.6 `ima16` lanes and source formats **[HW]**
- §8.7 Integer multiply-add modes **[HW]**
- §8.8 Shift amounts of 32 and over **[HW]**
- §8.9 Index registers **[HW]**
- §8.10 Register offsets of loads and stores **[HW]**
- §8.11 The 14-bit `ldr`/`str` index **[HW]**
- §8.12 Repeat order and overlapping ranges **[HW]**
- §8.13 Predicates on repeated instructions **[HW]**
- §8.14 `br` offsets and the `Pn` predicate **[HW]**
- §8.15 `smlsi` increments **[HW]**
- §8.16 Float rounding and NaN **[HW]**
- §8.17 `frcp`, `frsq` and `fsqrt` precision **[HW]**
- §8.18 Denormals **[HW]**
- §8.19 Write masks **[HW]**
- §8.20 `.incsXYZ` **[HW]**
- §8.21 Pack and unpack lanes **[HW]**
- §8.22 Pack and unpack conversions **[HW]**
- §8.23 Float dot products **[HW]**
- §8.24 Internal registers **[HW]**
- §8.25 Float min/max **[HW]**
- §8.26 Conditional and masked moves **[HW]**
- §8.27 Colour operations **[HW]**
- §8.28 Integer dot products **[HW]**
- §8.29 `fpma` **[HW]**
- §8.30 Half-float multiply-add **[HW]**
- §8.31 `efo` **[HW]**
- §8.32 `fadm`, `fmsa`, `fsubflr` **[HW]**
- §8.33 `flog`, `fexp`, `fsin`, `fcos` **[HW]**
- §8.34 Dual issue **[HW]**
- §8.35 Derivatives `fdsx`, `fdsy` **[HW]**
- §8.36 Texture sampling `smp` **[HW]**
- §8.37 Sampler state **[HW]**
- §7.8 `rlp` **[HW]**
- §8.39 Cache control `ci`, `cf`, `cfi` **[HW]**
- §8.40 Video `adif`, `adifsum` **[HW]**
- §8.41 `.tpres`, `.partial`, `.nosched`, `.syncs` **[HW]**

### 8.1 The test sign `p`

*Measured on the hardware (2026-10-22).* The sign part of a test (`.test<S>...`) is `t`, `n` or `p`.
On a 32-bit result `n` is bit 31 and **`p` is its complement, `>= 0`**, so a zero result passes `p`
**[HW]**: `shl.testpat` and `and.testpat` set the predicate for 0, 1 and `0x7fffffff` and not for
`0x80000000` (probes `sign-p-shl`, `sign-p-and`; `design/sgx545_hw_spec.md` §14). The earlier open
reading `> 0` is wrong. The firmware's `testpat` on `shl` and `isub16` results therefore asks "bit
clear" (after the shift) and "not negative". On the 16-bit tests of `chan0`, `p` is `>= 0` on the
low lane (*`isub16`/`isubu16` lanes*); `iaddu32`, unsigned, passes `p` always (*The 32-bit adds*).

### 8.2 `t` in the `a`/`o` joins of a test

*Measured on the hardware (2026-10-22).* A test joins a sign test and a zero test with `a` (and) or
`o` (or), and either can be `t`. **`t` means true** **[HW]**: `and.testnot`, `.testtoz` and
`.testtonz` set the predicate for every input (probes `join-t-not`, `join-t-toz`, `join-t-tonz`;
`design/sgx545_hw_spec.md` §14). So an `o` join with a `t` side is always true, `testtaz` is "zero",
`testtanz` "not zero", `testnat` "negative" and `testpat` "not negative"; the "not tested" reading
is ruled out.

### 8.3 The 32-bit adds `iadd32` and `iaddu32`

*Measured on the hardware (2026-10-22).* **`iadd32` and `iaddu32` are not full 32-bit adds**
**[HW]**: they add **source 1's low 16 bits**, sign-extended (`iadd32`) or zero-extended
(`iaddu32`), to the 32-bit source 2, modulo 2^32. `0x12345678 + 0x11111111` gives `0x11116789` and
`iadd32 0xffff + 1` gives 0, while `#1 + r` is the plain increment the firmware uses
(`iaddu32 r3, p0, #1, r3`). The test reads the 32-bit sum: `z` and `n` as usual for `iadd32`;
`iaddu32` is unsigned, its `n` is never and its `p` always true, also for `0x80000000` (probes
`iadd32-value`, `iaddu32-value`, `iaddu32-imm`, `iadd32-src1-imm`, `iadd32-z`, `iadd32-n`,
`iadd32-p`, `iaddu32-tests`, `iaddu32-np`; `design/sgx545_hw_spec.md` §14).

### 8.4 Test channels

*Measured on the hardware (2026-10-22).* **A test gives one result per channel, and the channel
field picks one or combines them** **[HW]** (probe `test-channels`, 360 cases; `design/
sgx545_hw_spec.md` §14):

- an 8-bit operation (`iadd8`, ...) has four channels, **bytes 0-3**, each tested on its own
  (`n` is the byte's bit 7);
- a 16-bit operation has **the low half as channel 0 and the high half as channel 2**;
- a 32-bit or float operation has its value as channel 0;
- **a channel with no lane tests as a positive non-zero value**: `z` and `n` false, `nz` and `p`
  true. That is why round 1 saw `chan1`-`chan3` clear the predicate on a zero test;
- `chan0`-`chan3` take one channel, **`andall`/`orall` all four** (and/or), **`and02`/`or02`
  channels 0 and 2**.

*Round 4 (2026-10-22).* **`imul16`, `imulu16`, `iadd16` and `iaddu16` test their lanes as
channels 0 and 2 on the full product or sum, before it is cut to 16 bits**: `0x100 × 0x100`
writes 0 but does not test as zero, nor does `iaddu16`'s `0xffff + 1` or `iadd16`'s
`-0x8000 + -0x8000`. `fmul` and `fsub` test channel 0 like `fadd`, and `isub8`/`imul8` a byte per
channel **[HW]** (probe `test-lanes-other`).

**Undecided:** the mask forms of class 15 (`.bmsk`/`.wmsk`/`.dmsk`).

### 8.5 `isub16`/`isubu16` lanes

*Measured on the hardware (2026-10-22).* **`isub16` and `isubu16` subtract both 16-bit lanes**, each
wrapping, with **no saturation** **[HW]**: `0x12340005 - #1` is `0x12330004` (an immediate is
subtracted from both lanes), `0 - #1` is `0xffffffff`, `0x00050007 - 0x00010002` is `0x00040005`,
and `isubu16 0 - #1` is `0xffffffff`. `chan0` tests the **low lane**: `z` from its 16 bits, `n` from
bit 15 (not bit 31), `p` is `>= 0` on it (probes `isub16-value`, `isub16-zero`, `isub16-sign-n`,
`isub16-sign-p`, `isub16-reg`, `isubu16-value`, `isubu16-zero`; `design/sgx545_hw_spec.md` §14). The
firmware's counter comparisons read only the low lane, which fits.

`iadd16`, `iaddu16`, `imul16` and `imulu16` have two lanes too, tested as channels 0 and 2 **[HW]**
(round 4, *Test channels*).

**Undecided:** `isubu16`'s sign tests.

### 8.6 `ima16` lanes and source formats

*Measured on the hardware (2026-10-22).* **`ima16 ..., u16, u16` computes `a × b + c`** (`a × b - c`
with `.neg` on source 2) **in each 16-bit lane, wrapping** **[HW]**: `0x00020005 × #3 - 0x00010007`
is `0x00050008` and `0x0000ffff × #3` is `0x0000fffd`; an immediate `b` or `c` is in both lanes, a
register one is taken lane by lane (probes `ima16`, `ima16-plain`, `ima16-imm-c`, `ima16-reg-b`;
`design/sgx545_hw_spec.md` §14). This is the firmware's `u16, u16` use.

**Undecided:** the 8-bit source formats `u8`, `s8`, `o8` (`enc[39:38]`, `enc[41:40]`) with their
`.low`/`.high` select, saturation, `.rsN` and `.abs` on the destination; the same probe template
with those formats and lane values that tell the bytes apart would settle them.

### 8.7 Integer multiply-add modes

*Measured on the hardware (2026-10-22).* Round 4 (table `leftovers4.toml`;
`design/sgx545_hw_spec.md` §14) **[HW]**:

- **`idiv d, a, b, drcN`** writes the **unsigned quotient to `d` and the remainder to `d + 1`**,
  `0xffffffff` to both for `b = 0`; the results arrive once `wdf drcN` returns (without the
  wait the remainder was still missing at the next store) (probe `idiv`).
- **`ima16` with 8-bit formats** reads, in each 16-bit lane, the byte `.low` or `.high` of that
  lane's half of `b` and `c`: `u8` unsigned, `s8` signed, `o8` the byte minus 128; `a` stays
  `u16` (probe `ima16-8bit`).
- **`ima8 d, a, b, c, x, y, z, xa, ya, za`** computes **`x + y × z` per byte**, the colour
  selectors `x`, `y`, `z` for bytes 0-2 and the alpha ones for byte 3 (`s0`, `s1`, `s2` = `a`,
  `b`, `c`; `.comp` = 255 − the byte), wrapping, or clamping to 255 with `.sat` (probe
  `ima8`).
- **`imae`**: `.usat`/`.ssat` saturate the 32-bit result (`.ssat` signed), `.rsN` shifts it right
  by N, source 2 typed `u16` is its half **sign**-extended and `z16` zero-extended, **`coe, iN`
  writes the carry out to `iN` and `cie, iN` adds `iN`**; `ima32`'s `cie, iN` adds `iN` too
  (probe `imae-modes`).

### 8.8 Shift amounts of 32 and over

*Measured on the hardware (2026-10-22).* By a **register** amount of 32 and over, `shl` and `shr`
give 0, `asr` gives the sign (0 or `0xffffffff`) and `rol` takes the amount modulo 32 **[HW]**:
amounts 32, 33, 64, `0x100` and `0xffffffff` behave so, and `shl.testnat` tests the same result
(probes `shl-reg`, `shr-reg`, `asr-reg`, `rol-reg`, `shl-test-reg`; `design/sgx545_hw_spec.md` §14).

*Round 4 (2026-10-22).* **Immediate amounts of 32 and over act as register ones** (`#32`, `#33`,
`#40`, `#127`), and **`~` complements the 32-bit amount or operand before use**: `shl d, a, ~r2`
with `r2 = 0xfffffffc` shifts by 3, `~#3` is `0xfffffffc` (so `shl`/`shr` give 0 and `rol` turns
by 28), and `and`/`or`/`xor` with `~#3` take the complement as their operand **[HW]** (probe
`shift-immediates`).

### 8.9 Index registers

*Measured on the hardware (2026-10-22).* `mov i.l, src` loads the index register and **`r[il + #n]`
is the temporary `i.l + n`, counted from `r0`** in units of one register, as a source and as a
destination **[HW]**; `sa[il + #n]` is the same on the secondaries, counted from `sa0`. **Bits 10
and up of `i.l` are ignored**: `0x401`, `0x40001` and `0xfffffc02` index `r1`, `r1` and `r2` (probes
`index-unit`, `index-width`, `index-width2`, `index-width3`, `index-store`, `index-sa-unit`,
`index-sa-store`, `sa-values`; `design/sgx545_hw_spec.md` §14). An index past the task's temporaries
reaches the next instances' registers **[infer]**: the share of pixels reading other values grows
with it. The firmware's `0x00040000 | index` (bit 18) is the stride of a register **offset** of a
load or store (*Register offsets of loads and stores*), not a mode of an index register.

*Round 4 (2026-10-22).* **`i.h` and `r[ih + #n]` work as `i.l` does** (one register from `r0`,
bits 10 and up ignored), and **`mov i.lh, s` loads `i.l` from the low and `i.h` from the high 16
bits of `s`** **[HW]** (round 4, probe `index-high`).

**Undecided:** the `o` and `pa` banks through an index, and indexed operands of anything but
`mov`; probes like `index-unit` with `ih`, `o`/`pa` and an ALU instruction would settle them.

### 8.10 Register offsets of loads and stores

*Measured on the hardware (2026-10-22).* An immediate offset counts elements of the access size
**[HW/RE]**. A **register** offset `o` (`ldad d, [b,+o]`, `stad [b,+o], s`) is
**`(o >> 16) × (o & 0xffff)` bytes**, both halves unsigned, added modulo 2^32 **[HW]**: `0x00040003`
is +12, `0x00010001` +1, `0x01000001` +256, `0x00000005` +0, and `0xffffffff` faults at the base
minus `0x1ffff`. `[b,-o]` subtracts; `ldad.fetchN [b,+o]` starts one element after, as the immediate
forms do. A load from an address that is not a multiple of 4 reads those bytes; a store there lands
nowhere and does not fault (probes `ldad-reg-off`, `ldad-reg-off-sign`, `ldad-reg-off-neg`,
`ldad-reg-off-fetch`, `stad-reg-off`, `stad-reg-off-unaligned`; `design/sgx545_hw_spec.md` §14). The
firmware's register offsets OR `0x00040000` (a stride of 4) into a dword index.

**Undecided:** the word and byte forms (`ldaw`/`staw`, ...), the pre- and post-increment forms with
a register offset (what is written back), and the negative fetch `[b,-o]`; the same probes on those
forms would settle them.

### 8.11 The 14-bit `ldr`/`str` index

*Measured on the hardware (2026-10-22).* The immediate register index of `ldr`/`str` has 14 bits (a
reach of 64 KiB of register space) **[RE]**, and **`ldr rD, rI` masks a register index to 14 bits**
**[HW]**: `0x4007`, `0x8007`, `0x10007` and `0x40007` read `CORE_ID` like 7 (probe `ldr-index-mask`;
`design/sgx545_hw_spec.md` §14). A validator can reduce an index modulo `0x4000`.

**Undecided:** `str` with a register index (a client `str` was not seen to land; a probe from the
EDM's own task, on a register that is safe to write, would settle it).

### 8.12 Repeat order and overlapping ranges

*Measured on the hardware (2026-10-22).* With the default MOE state a `.repeatN` steps every operand
by one register per iteration, and **the iterations run one after the other, in ascending order**
**[HW]**: `mov.repeat2 r2, r1` leaves `r3 = r1` (overlapping ranges propagate), and
`mov.repeat3 r1, r2` gives `r1 = r2`, `r2 = r3`, `r3 = r4` (probe `repeat-order`;
`design/sgx545_hw_spec.md` §14). A repeat behaves like N instructions in sequence.

### 8.13 Predicates on repeated instructions

*Measured on the hardware (2026-10-22).* **One predicate covers the whole instruction** **[HW]**:
`p0 mov.repeat2` with `p0 = 1, p1 = 0` writes both registers and with `p0 = 0, p1 = 1` neither
(probe `repeat-pred`; `design/sgx545_hw_spec.md` §14).

**Undecided:** what a repeated test writes into its predicate, and `Pn`; a probe of
`and.testtaz.repeat2` with inputs that differ per iteration would settle the first.

### 8.14 `br` offsets and the `Pn` predicate

*Measured on the hardware (2026-10-22).* **`br #k` jumps `k` instructions on from the `br`
itself**: `br #1` runs the next instruction, `br #2` skips one (probe `br-offset`; `ba` agrees
with the absolute index). **An instruction predicated `Pn` (value 7) never runs**, whatever
`p0`-`p3` hold (probe `predicate-pn`) **[HW]** (`design/sgx545_hw_spec.md` §14).

### 8.15 `smlsi` increments

*Measured on the hardware (2026-10-22).* `smlsi #I0, #I1, #I2, #I3, ...` sets four signed 8-bit
increments **[RE]**: before `mov.repeat2 r20, r10`, **`I0` is the destination's increment and `I2`
the source's** (0 repeats a register, 2 skips one); `I1` and `I3` change nothing there **[HW]**
(probe `repeat-smlsi`; `design/sgx545_hw_spec.md` §14). **On a three-source repeat (`fmad.repeat2`)
`I1` is source 0's increment, `I2` source 1's and `I3` source 2's** **[HW]** (round 4, probe
`smlsi-three-sources`); `mov`'s source is source 1. With the firmware's `0, 0, 0, 1` at every task
entry a following `mov` repeat would hold its destination and its source still; the firmware repeats
nothing itself, so what its 1 is for stays **[open]**.

**Undecided:** swizzle mode, the limits, and how long the state lasts.

### 8.16 Float rounding and NaN

*Measured on the hardware (2026-10-22).* Floats are IEEE binary32 bit patterns in the 32-bit
registers **[RE]**. **`fmad` rounds to nearest even** and **is not fused**: it rounds `a × b` to
binary32, then the sum **[HW]**. `(1+2^-12)^2` and `(1+2^-12)(1+3×2^-12)`, both ties, round to even,
also when negative; `(1+2^-12)^2 - (1+2^-11)` is 0, not 2^-24; 48 random triples agree (probes
`fmad-round`, `fmad-fused`, `sweep-fmad`; `design/sgx545_hw_spec.md` §14). An invalid result is the
NaN **`0x7fffffff`** (`frsq` and `fsqrt` of -1), not the IEEE default `0x7fc00000`. The radare2 ESIL
rounds to nearest even but forms `fmad`'s sum through a double, so a sum of very different
magnitudes can round twice there (`tools/usse-dis/r2/README.md`).

The dot products round each product, then each sum, the same way, but their invalid result is
the NaN `0x7fc00000` **[HW]** (*Float dot products*).

`efo`, `fadm` and `fmsa` round each product and each sum the same way, `fadm`/`fmsa`/`fsubflr`
return the NaN `0x7fc00000` too, and `fmad16` rounds its product to binary32 and its sum to a
half, which for two halves is once, fused **[HW]** (*`efo`*, *`fadm`, `fmsa`, `fsubflr`*,
*Half-float multiply-add*).

**Undecided:** `efo`'s NaN and infinite inputs.

### 8.17 `frcp`, `frsq` and `fsqrt` precision

*Measured on the hardware (2026-10-22).* **`frcp`, `frsq` and `fsqrt` are within one ulp but not
always correctly rounded** **[HW]**: of 48 random normal inputs 46, 42 and 44 are exact and the
others one ulp off (at most 0.65 ulp from the true value); `1/3`, `1/5`, `1/√2` and `√3` are exact.
Specials: `frcp(0) = +inf`, `frcp(-0) = -inf`, `frcp` of a denormal `+inf` (its source is flushed),
`frsq(0) = +inf`, `fsqrt(0) = 0`, `frsq` and `fsqrt` of -1 the NaN `0x7fffffff` (probes `frcp`,
`frsq`, `fsqrt`, `sweep-frcp`, `sweep-frsq`, `sweep-fsqrt`; `design/sgx545_hw_spec.md` §14). The
radare2 ESIL rounds them correctly, so it can differ by one ulp, and does not model the specials.

`flog`, `fexp`, `fsin` and `fcos` are measured in *`flog`, `fexp`, `fsin`, `fcos`*.

*Round 4 (2026-10-22).* The sweeps' expectations now say so: **within one ulp of the exact
value** settles all three in every run, while "correctly rounded" and "rounded toward zero" do
not fit (the same inputs give the same results run after run).

**Undecided:** `frcp`, `frsq`, `fsqrt` of infinite inputs.

### 8.18 Denormals

*Measured on the hardware (2026-10-22).* **`fmad` flushes denormal inputs, product and result to a
zero of their sign** **[HW]**: `2^-70 × 2^-70` is 0, `-2^-70 × 2^-70 + -0` is -0, `2^-140 × 2^100`
is 0 (probes `fmad-denormal`, `fmad-zero-sign`; `design/sgx545_hw_spec.md` §14). The sources of
`frcp`, `frsq` and `fsqrt` are flushed too (`frcp` of a denormal is `+inf`). No denormal result was
seen.

The dot products and the min/max flush their inputs and results the same way **[HW]** (*Float dot
products*, *Float min/max*); pack and unpack do not: `pckf16f32` keeps a denormal half and
`unpckf32f16` converts one exactly **[HW]** (*Pack and unpack conversions*).

`fadm`, `fmsa`, `fsubflr` flush their inputs too, `flog` of a denormal is `-inf` and `fexp`
flushes results below 2^-126 **[HW]**, while **`fmad16` keeps half denormals** (*`fadm`, `fmsa`,
`fsubflr`*, *`flog`, `fexp`, `fsin`, `fcos`*, *Half-float multiply-add*).

**`efo` flushes as `fmad`** **[HW]** (round 4, probe `efo-denormals`): a denormal source, a
denormal product and a denormal sum all give a zero. **`fmad x, 1, -0` is a move** of normals,
infinities and zeros (`+0` as `c` turns `-0` into `+0`), not of denormals (flushed) or NaNs
(`0x7fc00000`) **[HW]** (probe `fmad-move`).

### 8.19 Write masks

*Measured on the hardware (2026-10-22).* The destination write mask (`.x`, `.y`, `.z`, `.w` and
combinations, `enc[47:44]`) **is a repeat of four with iterations left out: component k writes
register `d + k`, computed from every register source `+ k`; an immediate stays the same, and the
lone `.` (mask 0) writes nothing** **[HW]**. `mov r10.xz, r1` writes `r10 = r1` and `r12 = r3`,
`frcp r10.y, r1` writes `r11 = 1/r2`; a component is not a byte. All 16 masks on `mov`, `or`,
`and` with an immediate and `frcp`, and masks on `movc` and on a pack together with its byte
mask, agree (probes `wmask-mov`, `wmask-or`, `wmask-and-imm`, `wmask-frcp`, `wmask-other`;
`design/sgx545_hw_spec.md` §14). The firmware and our programs use only the default `.x`.

**Undecided:** the test group's masks (with a predicate result) and the 540's reading; the same
probe would settle them.

### 8.20 `.incsXYZ`

*Measured on the hardware (2026-10-22).* The float multiply-add, dot-product, min/max and
derivative groups print `.incsXYZ` (`enc[47]`, `enc[46]`, `enc[40]`) when `enc[53]` is clear
**[RE]**. **Each bit lets one source advance by one register per iteration: X source 0, Y source
1, Z source 2; the destination advances whatever the bits say, and without `.incs` (`enc[53]`
set) every source advances** **[HW]**. `fmad.repeat2.incs001 d, a, b, c` computes `d = a·b + c`
and `d+1 = a·b + (c+1)`; `.incs000` repeats the first operation into `d+1` (probes
`incs-fmad`, `fdp-incs`, `fddp`, `fminmax-incs`; `design/sgx545_hw_spec.md` §14). `fdp` has no
source 0, so its X bit does nothing, and `fddpc`, which has no `.incs`, never advances its
sources (probe `fddpc`).

`fmad16.repeat2.incsXYZ` advances `a`, `b`, `c` the same way (probe `fmad16-incs`).

**`fdsx`/`fdsy` advance the same way: their `a` and `b` are sources 1 and 2**, so Y advances `a`,
Z advances `b` and X nothing **[HW]** (round 4, probe `fds-incs`).

### 8.21 Pack and unpack lanes

*Measured on the hardware (2026-10-22).* A pack or unpack converts **two values, source 1 and
source 2, and lays them out in the destination format** **[HW]** (probes `pack-u8-bytemask`,
`pack-u16-bytemask`, `pack-c10`, `unpack-select`, `wmask-other`; `design/sgx545_hw_spec.md`
§14):

- **8-bit results** (`u8`, `s8`, `o8`): bytes 0-3 take source 1, 2, 1, 2. **`.bytemaskXXXX`**
  enables byte N with bit N (the rightmost digit is byte 0); a disabled byte keeps its value, and
  an enabled byte takes source 1 or 2 by this rule (bN: byte N enabled): byte 0 source 1, byte 1
  `s[b0]`, byte 2 `s[b0 xor b1]`, byte 3 `s[b0 or b1 or b2]`, where `s[0]` is source 1. So
  `.bytemask0011` writes source 1 and 2 into bytes 0 and 1, `.bytemask1100` into bytes 2 and 3.
- **16-bit results** (`u16`, `s16`, `f16`): the low half source 1, the high half source 2; under
  a byte mask the high half takes source 2 only when byte 0 is enabled.
- **`c10`**: three 10-bit lanes at bits 0, 10 and 20 take source 1, 2, 1; bits 30-31 are 0.
- **Component selects** read a source: byte `c` for 8-bit formats; for 16-bit formats `.0` bits
  0-15, `.1` bits 8-23, `.2` bits 16-31, `.3` nothing (0); the `c10` lane `c` (`.3` is 0); an
  `f32` source ignores its select. An unpack to `f32` converts source 1 only.
- **The write mask** and **`.repeatN`** advance the register sources as everywhere (*Write
  masks*).

**`statomic d, s1.c, s2.c`** (format value 8) writes, in both 16-bit halves, byte `c` of `s1` in
the low byte and byte `c` of `s2` in the high one, each read as a signed byte with negatives
made 0 (`0x80`, `0x7f` give `0x7f00`); the rounding mode and `.scale` change nothing (probe
`statomic`, 2026-10-22). Round 1's `0x78007800` was such a value.

**Undecided:** a byte mask on `c10` results, `.1`/`.3` of an `f32` source on `unpckf32f32`
(`.1` read a shifted value), and what `statomic` is for.

### 8.22 Pack and unpack conversions

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `pack-*`, `unpack-*`;
`design/sgx545_hw_spec.md` §14):

- **From `f32`, without `.scale`:** round to an integer in the instruction's mode (`nearest` ties
  to even, `down`, `up`, `zero`), then saturate to the format: `u8` 0..255, `s8` -128..127, `o8`
  `x + 128` in 0..255, `u16`, `s16`. 300 packs to 255, -5 to 0 (`u8`) or 123 (`o8`).
- **With `.scale`:** `u8` `x × 255`, `s8` `x × 127` clamped to ±127, `o8` `x × 127 + 128` clamped
  to 1..255, `u16` `x × 65535`, `s16` `x × 32767` clamped to ±32767; the product is exact and then
  rounded to nearest even **whatever the mode** (96 random values agree).
- **`c10`** is `x × 255` in a signed 10-bit lane, to nearest even, clamped to ±510 (±2.0),
  with or without `.scale`.
- **`pckf16f32`:** without `.renable` it truncates toward zero whatever the mode, keeps denormal
  results and saturates to 65504; **`.renable` rounds to an integer** in the mode (`up` turns
  1.0005 into 2.0).
- **To `f32`:** integers exactly; with `.scale` `u8/255`, `s8/127`, `o8 (x - 128)/127`,
  `u16/65535`, `s16/32767`, correctly rounded, with -128, 0 (`o8`) and -32768 giving -1.0 (every
  byte value checked); `c10` lanes are always `/255`, clamped to ±2.0; `f16` converts exactly,
  denormals and infinities included, and a NaN becomes `0x7fffffff`.
- **Integer to integer:** `unpcks16s8` sign-extends, `unpcks16u8` and `unpcku16u8`
  zero-extend, `unpcku16s8` turns a negative into 0, and `unpcku8s8` turns a negative into
  `0x7f`, not 0.

- **`c10` to the 16-bit integers is no conversion** **[HW]**: `unpcku16c10` and
  `unpcks16c10`, with or without `.scale`, read **lane 0 of source 1 whatever the selects** and
  write three of its bits into the low half: bit 15 = lane bit 4, bits 1-14 = lane bit 6, bit 0
  = lane bit 8; the high half is 0 (every lane value, probe `unpack-c10-16`; round 2's
  `unpack-c10-int` recorded the first values). A compiler should not emit them.

**Undecided:** `.scale` on the integer-to-integer forms, and the other `f16` sources.

### 8.23 Float dot products

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `fdp-count`, `fdp-incs`,
`fdp-rounding`, `fdp-specials`, `fdpc`, `fddp`, `fddpc`; `design/sgx545_hw_spec.md` §14):

- **`fdp.repeatN d, a, b`** sums `N` products `(a+k)·(b+k)` (`N` = 1 without a repeat, up to 4)
  and **writes only register `d + N - 1`**; `d` to `d + N - 2` keep their values. `.incsXYZ`
  chooses which sources advance (*`.incsXYZ`*).
- **Rounding:** each product is rounded to binary32, then they are added in order, each sum
  rounded to nearest even, as a chain of `fmad` would (6 crafted and 42 random cases).
  Denormal inputs and results are flushed, `x - x` is +0, `-0 + -0` is -0, an overflow is `+inf`
  and an invalid operation (`inf × 0`) gives the NaN **`0x7fc00000`**, not the `0x7fffffff` of
  `frsq`.
- **`fdpc d, cpN, a, b`** gives `fdp`'s value and leaves `p0`-`p3` alone.
- **`fddp.repeatN d, iM, s0, s1, s2`** writes `d + N - 1 = s0·s1` and `iM = s0·s2` (`iM` is `i0`
  or `i1`). **`fddpc`** does the same but never advances its sources.

- **`fdpc` writes only its destination** **[HW]**: for every `cp0`-`cp7` and either sign, `d + 1`
  to `d + 3`, `i0`-`i2` and `p0`-`p3` keep their values (probe `fdpc-clip`).

**Undecided:** where the clip-plane result of `fdpc`/`fddpc` (`cpN`) goes. Presumably to the
clipper with a vertex's outputs **[infer]**; a vertex program that emits after `fdpc`, read
back by the host, would settle it.

### 8.24 Internal registers

*Measured on the hardware (2026-10-22).* **`i0`-`i2` are ordinary 32-bit registers to `mov`,
one set per instance** **[HW]**: a fill that gives every pixel its own `i0`-`i2` (round 3, from
the pixel's coordinates) reads each pixel's own values back (probe
`internal-registers-per-pixel`). **`i3` reads `i0` of the next instance of the pixel's 2x2
quad**, in the order top-left, top-right, bottom-left, bottom-right and round again, and **a
write to `i3` is dropped** **[HW]**. Round 1 saw `i3` read "`i0`" only because every pixel held
the same value.

**They keep their values from one task to the next**, also from one client to another **[HW]**:
a fill writes `i0`-`i2`, and the next probe, in another process, reads them back (probe
`internal-registers`; `fddp` leaves its result there too). The next task's pixels see the
values of other pixels: a whole quad of instances moves to another quad of pixels, its four
positions kept, so `i3` stays within the quad (probe `internal-registers-across-tasks`,
recorded). They are not cleared by a task start, as the temporaries are not
(`design/sgx545_render_uapi.md` §7).

`efo` reads `i0`/`i1` and writes them after its own reads; **`efo.gpiext`** reaches the other
instances of the quad (*`efo`*).

*Round 4 (2026-10-22).* **Bank 7 numbers past `i3` name the other instances of the quad**: number
`n`, taken modulo 16 (`i31`, `i63` and `i127` read as `i15`), is `4m + j`; `j` = 0-2 reads `ij` of
the instance `m` places on (TL, TR, BL, BR, round again), and `j` = 3 reads `i2` of that instance,
but `i0` of the next one for `i3` itself. **Writes to `i4`-`i7` are dropped**: no instance's
`i0`-`i2` change **[HW]** (probe `internal-registers-bank7`, every pixel). The carries of
`imae`/`ima32` go through `iN` (*Integer multiply-add modes*).

### 8.25 Float min/max

*Measured on the hardware (2026-10-22).* **`fminmax d, a, b, c` is `min(max(a, b), c)` and
`fmaxmin d, a, b, c` is `max(min(a, c), b)`** **[HW]**: both clamp `a` to `[b, c]` when
`b <= c`; when `b > c`, `fminmax` gives `c` and `fmaxmin` gives `b` (six orders of 1, 2, 3 and
ten random triples; probes `fminmax-roles`, `fmaxmin-roles`). **min and max are IEEE minNum
and maxNum**: a NaN operand loses to a number, `-0` is below `+0`, infinities order as numbers
and denormals are flushed to a zero of their sign (22 special triples, probes
`fminmax-specials`, `fmaxmin-specials`; `design/sgx545_hw_spec.md` §14). `.incsXYZ` as for
`fmad` (probe `fminmax-incs`).

When both operands of a step are NaN, the step returns its **second operand**, payload and
signalling bit kept (`fminmax` of three NaNs gives `c`, `fmaxmin` gives `b`; probe
`minmax-allnan`, 2026-10-22).

### 8.26 Conditional and masked moves

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `movc-*`, `movmsk-*`;
`design/sgx545_hw_spec.md` §14):

- **`movc.<type>.<test> d, s0, s1, s2`** is a select per lane: each lane of `d` is `s1`'s if the
  test holds for that lane of `s0`, else `s2`'s. Lanes: `.i32` one, `.i16` two, `.i8` four,
  `.i10` three 10-bit lanes at bits 0, 10, 20 (bits 30-31 of `d` become 0). Tests: `.tz` zero,
  `.tnz` not zero, `.tn` the lane's sign bit set, `.tp|z` clear. **`.flt`** tests the float:
  denormals and `-0` are zero, `.tn` is `< 0`, `.tp|z` `>= 0`, and a NaN fails all but `.tnz`.
- **`movmsk.<fmt> d, s1, s2.c`** moves `s1` into `d` when `s2` passes, else leaves `d` alone:
  `.mask` bit 0 of byte `c`, `.u8` bit 7 of byte `c`, `.flt` `s2 >= 0.5` (a NaN fails, `c`
  ignored), `.flt16` half `c >= 0.5` (`.0` bits 0-15, `.1` bits 8-23, `.2` and `.3` bits 16-31).
- **`.smsk`** adds source 0: the move also needs **bit 0 of `s0`** for `.u8`, `.flt` and
  `.flt16`, while `.mask.smsk` ignores `s0` (16 values of `s0` against a passing and a failing
  `s2` per format; probe `movmsk-smsk2`, 2026-10-22).

### 8.27 Colour operations

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `sop2-selectors`,
`sop2-ops`, `sop2-random`, `sop3-lrp`, `sop3-random`, `sop2wm`, `colour-c10`;
`design/sgx545_hw_spec.md` §14). Bytes 0-2 are the colour channels and byte 3 the alpha; a
channel `x` stands for `x/255`:

- **`sop2 d, s1, s2, f1, f2, op+asop2 g1, g2, aop`** computes per colour channel
  `op(s1 × f1, s2 × f2)` and per alpha `aop(s1a × g1, s2a × g2)`: `add` and `sub` sum the two
  exact products, divide by 255 once, round to nearest and saturate to 0..255 (a negative
  difference is 0); `min` and `max` take the smaller or larger **source**, ignoring the
  factors. Factors: `zero`, `s1`, `s2` (the same channel), `s1a`, `s2a`, `asat` =
  `min(s1a, 1 - s2a)`, `s2scale` = `s2` and the whole result doubled (also for `min`/`max`),
  and as second factor `zeros` = 0, which makes `s2` signed (`s2 - 0.5`) in `add`/`sub`.
  `.comp` gives `1 - f` (`asat.comp` = `min(1 - s1a, s2a)`, `s2scale.comp` = `2(1 - s2)`).
  A leading `comp, ` complements `s1` itself in `add`/`sub`; the alpha's `, neg` negates its
  result before saturating (so `sub, neg` is `s2a × g2 - s1a × g1`).
- **`sop3 d, s0, s1, s2, f1, f2, op`** is the same colour operation (`add`/`sub`) with
  factors `zero`, `asat`, `s0`, `s0a`, `s1`, `s1a`, `s2`, `s2a`; with `, neg` (the `alrp`
  form) it is `-op(s1 × f2, s2 × f1)`, the factors swapped. Alpha: `asop a, f2, op` =
  `op(s1a × a, s2a × f2)`; `arsop a, op` = `-op(s1a × f2, s2a × a)`; `alrp a, b` =
  `a × (1 - s0a) + b × s0a`. A factor read for the alpha takes the alpha channels; `asat`
  reads 1 there.
- **`lrp1 d, s0, s1, s2, x, y, t`**: colour `x × (1 - t) + y × t`, `x` one of `zero`/`s1`, `y`
  one of `zero`/`s2` (each with `.comp`), `t` = `s0` or `s0a`; its alpha is `asop` with two
  free factors. **`lrp2 d, s0, s1, s2, f1, f2, m1, m2`**: `f1 × m1 + f2 × m2` with the pairs
  (`s1`, `s2`), (`s1`, `1 - s2`), (`1`, `s2`), (`1 - s1`, `s2`); alpha `alrp`.
- **`sop2wm d.bytemaskN, s1, s2, f1, f2, cop, aop`**: `sop3`'s factors without `s0`, the
  colour operation on bytes 0-2 and the alpha one on byte 3; mask bit N writes byte N.
- **`.c10` operands** are three signed 10-bit lanes (bits 0, 10, 20), 1.0 = 255, clamped to
  ±510 when read and when written; a `c10` source's alpha is its lane 0; a `c10` destination
  gets bits 30-31 clear and an 8-bit destination the saturated channels. The colour formulas
  above hold unchanged.

- **`sop2.repeatN`** (round 3) computes each iteration on the next registers: the destination
  and both sources advance by one **[HW]** (probe `colour-dot-repeats`). `sop3`, `lrp1`,
  `lrp2` and `sop2wm` have no repeat form.

*Round 4 (2026-10-22).* **`.c10` operands read the same way on `sop3`, `lrp1` and `sop2wm`**
(three signed 10-bit lanes, 1.0 = 255, clamped to ±510, the alpha lane 0) **[HW]** (probe
`colour-c10-other`, against the bytes read as 8-bit channels and an alpha from lane 2), and
**`sop2wm`'s mask bit N writes byte N** as a pack's does (probe `sop2wm-bytemask`).

**Undecided:** `.c10` on `lrp2`.

### 8.28 Integer dot products

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `intdot-lanes`,
`intdot-alpha`; `design/sgx545_hw_spec.md` §14):

- **`u8dot4 d, Cx, Ax, s1, s2`** sums the four byte products `s1[i] × s2[i]` and writes the
  same value to every byte: `S × Cx / 255` to bytes 0-2 and `S × Ax / 255` to byte 3, each
  rounded to nearest and saturated to 0..255. **`u8dot3`** sums bytes 0-2 only. `.comp`
  complements a source's bytes first.
- **`off`** makes every byte signed around 128: the products are `(s1[i] - 128)(s2[i] - 128)`,
  and a negative result saturates to 0.
- **`u16dot4`, `u16dot3`** read the same bytes but write a 16-bit fraction to both halves:
  `S × Cx × 32767 / 255²` rounded, saturated to `0x7fff`, or to ±`0x7fff` for `off` (signed);
  `Ax` changes nothing.
- **`u8dot3`'s alpha** (byte 3), when it has an operation: `aadd Ax, f1, a1, f2, a2` =
  `(f1 × a1 + f2 × a2) × Ax / 255` and `asub` the difference, with `a1` = `s1a`, `a2` = `s2a`
  (each possibly `.comp`) and factors `zero`, `s1a`, `s2a`; `aintrp1 s0, x, y` = `x × (1 -
  s0a) + y × s0a` and `aintrp2 s0, x, y` = `(1 - x)(1 - s0a) + y × s0a`.
- **Repeats** (round 3): `u8dot3.repeat2`, `u8dot4.repeat3`, `u16dot4.repeat2` advance the
  destination and both sources by one per iteration **[HW]** (probe `colour-dot-repeats`).

### 8.29 `fpma`

*Measured on the hardware (2026-10-22).* **`fpma d, s0, s1, s2, c0, c1, c2, a0, a1, a2` is
`c0 + c1 × c2`** per colour channel and `a0 + a1 × a2` for the alpha **[HW]**: the first
selection is the addend (`s0`, `s1`, `s0a`, `s1a`), the other two the factors (`s1`/`s1a`,
`s2`/`s2a`), each `.comp` giving `1 - x`; with channels as `x/255`, the result is
`(255 c0 + c1 c2) / 255` rounded to nearest and saturated to 0..255. `.neg` negates `s0`.
`.c10` operands read and write the signed 10-bit lanes of the colour operations (±510);
`.sat` changes nothing that we saw (probe `fpma`; `design/sgx545_hw_spec.md` §14).

Round 3: **`.sat` changes nothing** also for `c10` results below 0 and above 1.0, which stay
clamped to ±2.0 only (probe `fpma-sat`), and **`fpma.repeatN` advances the destination and the
three sources by one per iteration** (probe `colour-dot-repeats`) **[HW]**.

### 8.30 Half-float multiply-add

*Measured on the hardware (2026-10-22).* **`fmad16 d, a, b, c` computes `a × b + c` in each
16-bit half** (low half from the low halves, high from the high) **[HW]**: the product rounded to
binary32, then the sum to a half, both to nearest even. A product of two halves fits a binary32,
so for halves this is one rounding, fused (20 random cases tell it from rounding the product to a
half first). Half denormals are kept, zeros keep their IEEE signs, overflow gives infinity and an
invalid result the half NaN `0x7fff`. **`.swzll`** feeds both lanes the operand's low half,
**`.swzhh`** its high half, and **`.swzf32`** the whole register as a binary32 value (not rounded
to a half: `8209.25 × 1.5 + 3` gives `0x7204`, not the `0x7203` of rounding 8209.25 to a half
first); with it the product can need more than 24 bits, and the hardware rounds it to binary32:
of 992 `.swzf32` results one tells this from a single rounding (`-0.41455 × 40.8136 + 2.0625`,
probe `x-fmad16`, checked by `crates/sgx-sim`). `.neg`, `.abs` act on the operand;
`.repeat2.incsXYZ` advances `a`, `b`, `c` as `fmad` does (probes `fmad16-lanes`, `fmad16-incs`,
`fmad16-specials`, `fmad16-rounding`; `design/sgx545_hw_spec.md` §14).

**Undecided:** a denormal or NaN `binary32` behind `.swzf32` (flushed or not).

### 8.31 `efo`

*Measured on the hardware (2026-10-22).* **`efo` does what its text prints** **[HW]**: `m0` and
`m1` multiply the named sources (`src0`-`src2`, each with its `.neg`, or `i0`/`i1`); `a0` and
`a1` add (or subtract) the named values; the destination takes `a0`, `a1`, or the **old**
`i0`/`i1`; `i0` and `i1` are written after, unless marked `!`. Every product and every sum is
rounded to binary32 (24 random cases); a repeat's second iteration sees the first one's `i0`
and `i1` (probes `efo-dataflow`, `efo-rounding`; `design/sgx545_hw_spec.md` §14).

**`.gpiext`**, with a different `i0`-`i2` in every pixel (round 3, probe `efo-gpiext-quad`)
**[HW]**:

- the multipliers read the instance's own **`i2` for `i0` and its own `i0` for `i1`**; the
  adders and the destination read its own `i0`/`i1`;
- a write of `i0` goes to the instance's own **`i2`**;
- the four instances' writes of `i1` all end in **one register, the top-right pixel's `i0`**,
  which holds the bottom-right pixel's value afterwards; the other pixels' `i0` and `i1` are
  left alone. Through `i3` (the next instance's `i0`) the top-left pixel then reads it too.

While an instruction writes `i1`, the top-right pixel's reads of `i0` (destination, multiplier
or adder) give the **top-left pixel's `i0`**, and now and then (about 1 read in 3000) the value
the top-left pixel is writing: the result depends on timing (probe `efo-gpiext-race`,
recorded). Every effect stays within the pixel's own 2x2 quad, which belongs to one task:
300 jobs of two clients submitted concurrently never saw the other client's values within a
task. Round 2's "one pixel in four" is the top-right pixel of every quad.

**Undecided:** repeats beyond two, and whether the four `i1` writes go to the top-right `i0`
in order (the bottom-right one last) or only the bottom-right one is kept.

### 8.32 `fadm`, `fmsa`, `fsubflr`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fadm-fmsa-fsubflr`,
`fadm-fmsa-fsubflr-specials`; `design/sgx545_hw_spec.md` §14):

- **`fadm d, a, b, c` = `a·c + b·c`**, each product and then the sum rounded to binary32 (not
  `(a + b) × c`: 24 random cases fit only this order), like a two-term `fdp`;
- **`fmsa d, a, b, c` = `a·a + b·c`**, rounded the same way;
- **`fsubflr d, a, b` = `a - floor(b)`**, one rounding.

Denormal inputs are flushed; an invalid result (`inf × 0`, `inf - inf`, a NaN input) is the
NaN `0x7fc00000`, as for the dot products.

**Undecided:** the half-float source forms (`enc[54]`), as for `fmad`.

### 8.33 `flog`, `fexp`, `fsin`, `fcos`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fsingle-base`, `fsingle-specials`,
`sweep-flog`, `sweep-fexp`, `sweep-fsin`, `sweep-fcos`; `design/sgx545_hw_spec.md` §14):

- **`flog` is `log2`**: exact on powers of two; within one ulp elsewhere, or within 2^-22
  near 1 (`log2(1 + 2^-23)` gives 2^-23); 0 and denormals give `-inf`, `+inf` gives `+inf`,
  negatives and NaN the NaN `0x7fffffff`.
- **`fexp` is `2^x`**: exact on integers, within two ulps elsewhere (78 of 95 exploration
  cases within one); results below 2^-126 are 0, from 128 up `+inf`; `-inf` gives 0.
- **`fsin`, `fcos` take the argument in quarter turns**: `sin(πx/2)`, `cos(πx/2)`, period 4,
  exact at multiples of 1/2 (`fcos(1)` is `-0`); the result is a fixed-point value with 15
  fraction bits (absolute error up to about 2^-15.5, so tiny results are 0 or 2^-15); an
  infinite or NaN argument gives `0x7fffffff`.

**Undecided:** `.flt16` and `.c10` sources, write masks and repeats on these, and `.tpres`.

### 8.34 Dual issue

*Measured on the hardware (2026-10-22).* **A dual-issue word runs two operations that read
before either writes** **[HW]** (probes `dual-issue`, `dual-issue-predicates`; 144 pairings
explored; `design/sgx545_hw_spec.md` §14):

- the primary computes on its operands as alone (`fssq d, a, b` is `a·a + b·b`, each product
  rounded), the secondary on `i0`-`i2` and, for a source naming a register, **the primary's
  source in that position, without its `.neg`**;
- both read the values from before the word: a primary reading `i0` gets the old `i0` while the
  secondary writes it, and a secondary reading `i0` the old one while the primary writes it;
  when both write one register, **the secondary's value stays**;
- `.repeat2` repeats **both**: the primary's operands advance, the secondary runs twice on the
  internal registers (`fadd i0, i0, i1` adds `i1` twice);
- the two-bit predicate (always, `p0`, `p1`, `!p0`) **covers both operations**.

**Pairs that co-issue** cleanly: with a `mov` primary every secondary but `ima32`; with `fmad`
or `ima32` the secondaries `fmad`, `fadd`, `fsub`, `fmul`, `fsubflr`, the single-source
functions and `mov` (`ima32` also `fmad16`, `fmsa`); with `fadm`, `fmsa` or `fssq` the
add-type and single-source secondaries and `mov`; with `fminmax` only `fmul`, the
single-source functions and `mov`. **Other pairs share a unit and give mixed results**
(probe `dual-issue-conflicts`, recorded): the secondary gets the primary's intermediate value
(`fadm` + `fmul` gives the primary's second product, `fminmax` + `fadd` its comparison
difference, `fmad` + `fmsa` the primary's result) or the primary is lost (`fmad` + `fmad16`
writes 0). An `fddp` primary writes its second product to the secondary's destination and
the secondary nothing.

*Round 4 (2026-10-22).* **`ima32` as a secondary computes two 16-bit lanes**, `a × b + c` in
each, wrapping, as `ima16` with `u16` sources; next to an `fmad`, `fadm`, `fmsa` or `fssq`
primary its `b` is **the primary's source 1** whatever it names, next to a `mov` the register it
names **[HW]** (probe `dual-issue-ima32`, integer and float inputs). **The pairs that share a
unit are deterministic** (probe `dual-issue-shared`, new values; round 3's cases fit too):

- a multiplying secondary (`fmul`, `fmad`) of a two-product primary (`fadm`, `fmsa`, `fssq`)
  gets the primary's second product, plus its own `c` for `fmad`;
- an `fmsa` secondary of `fmad`, `fadm` or `fmsa` writes the primary's result, as does an
  `ima32` secondary of an `ima32` primary;
- the add-type and `fmad` secondaries of `fminmax` get `max(a, b) - c` (the primary's last
  comparison difference);
- `fmad` + `fmaxmin` computes `a·b - c` for the primary and `b - c` for the secondary;
- `fmad` + `fmad16`: the secondary as alone, the primary 0.

**Undecided:** the secondaries' half-float (`enc[54]`) forms, `fminmax` + `ima32`.

### 8.35 Derivatives `fdsx`, `fdsy`

*Measured on the hardware (2026-10-22).* **`fdsx d, a, b` = `a`(top-right) − `b`(top-left) and
`fdsy d, a, b` = `a`(bottom-left) − `b`(top-left), the same value for all four pixels of the 2x2
quad** **[HW]** (coarse derivatives; probe `fds-quad`, on per-pixel values from the pixel
coordinates; `design/sgx545_hw_spec.md` §14). The difference is rounded once to binary32;
`.neg` and `.abs` act on the source before. The quads are the aligned 2x2 blocks of the
render target (even x and y at the top left).

**Undecided:** partly covered quads (the probes fill whole quads), and denormal inputs.

### 8.36 Texture sampling `smp`

*Measured on the hardware (2026-10-22).* **`smp2d d, c, s, drcN` samples the texture whose four
state words are in `s`, `s + 1`, `s + 2`, `s + 3` at the normalized coordinates in `c` and
`c + 1`, and writes the texel to `d` when the fence `drcN` is waited for** **[HW]** (probe
`smp-point`; `design/sgx545_hw_spec.md` §14). The state words are those the PDS gives the
texture unit for our copies (`doutt`, `design/sgx545_transfer.md` §4.4.3), built in temporaries
by the program itself:

- point sampling picks texel `floor(u × width)`, `floor(v × height)`; coordinates outside
  `[0, 1)` **clamp to the edge**; the texel arrives as stored (the raw B8G8R8A8 dword);
- `smp1d` reads `u` alone, row 0; `.flt16` takes both coordinates as halves of one register
  (`u` low); `smp2dbias`, `smp2dreplace`, `smp2dgrad` with a LOD of 0 sample the same texel;
- with `.repeat2` or a write mask the job faults at an address made of the state's fourth word,
  as if the next iteration read the state one register on **[infer]** (probe `smp-repeat-mask`).

**Undecided:** mipmaps and what the LOD values do, `.sinf`/`.rsd`, `.c10` coordinates,
`smp3d`, and state read from `sa` or `c` (only temporaries were used); the state fields are
measured in *Sampler state*.

### 8.37 Sampler state

*Measured on the hardware (2026-10-22).* Round 4 varied the four state words an `smp` program
builds (T0-T3, the `doutt` words; `design/sgx545_transfer.md` §4.4.3) on a texture in the
scratch buffer whose dwords count up, so every result names the texels it came from **[HW]**
(table `sampler.toml`; `design/sgx545_hw_spec.md` §14):

- **Filtering (T0 bits 13:12, probes `smp-bilinear`, `smp-filter-modes`).** 1 and 2 are
  bilinear, 0 and 3 point. **Bilinear weights are truncated to 1/64 and each 8-bit channel is
  rounded to nearest, halves up**, before any conversion: between texels 255 and 15 at
  `v + k/256` the f32 result steps every four `k`. The minification field does not act while
  magnifying (every case magnifies: the LOD forms never left level 0).
- **Addressing (T0 bits 8:6 U, 5:3 V; `smp-address`).** A stride texture (T1 type 6, our copy
  sources) clamps to the edge whatever the modes say. On a twiddled texture (type 0) **0
  repeats, 1 mirrors (period twice the size), 2 clamps to the edge, 3 mirrors once** (the
  absolute coordinate, then clamped); bits 2:0 do not act on `u`. Modes 4-7 read other texels
  (`smp-address-border`, recorded).
- **Twiddled layout (`smp-twiddled`).** Texel `(x, y)` lies at the Morton index with **`y` in
  bit 0, `x` in bit 1**, and so on; a non-square texture puts the longer side's extra bits
  above (16 × 16, 8 × 8, 16 × 4 and 4 × 16 tried).
- **Formats (T1 bits 28:24, no conversion; `smp-formats`).** 0 and 1 return the byte (`s8`
  not extended), 7-11 the half, 12, 15-18, 22 and 31 the dword; **2, 3, 4 and 5
  (`4444`, `8332`, `1555`, `565`) expand to B8G8R8A8 by repeating each field's bits**, alpha
  `0xff` when the format has none; 13 swaps red and blue (R8G8B8A8), 19 clears the sign bit,
  21 the top byte, 30 sets it. 6, 14, 20 and 23-29 (signed, 10-bit, compressed or YUV
  layouts) are recorded (`smp-formats-other`); 14 writes two registers.
- **Conversion (T3 bits 17:16, normalization bit 15; `smp-convert`).** 1 gives **c10**: the
  B, G, R channels in the 10-bit fields of the first register (the 8-bit value unscaled,
  1.0 = 255 as `.c10` operands read it) and A in the second; 2 gives **f16** pairs R, G then
  B, A (alpha 1.0 when the format has none); 3 gives **f32** B, G, R, A, one register each.
  Values are the integers (0-255, 0-65535) unless bit 15 is set, which divides by 255 (by
  65535 for `u16`). Data size field (T3 bits 26:23) changes nothing seen.
- **PCF (`smp-pcf`).** `smp2d.pcf` compares **texel OP reference**, the reference being the
  f32 in the register after the coordinates (`.pcff16`: an f16, low half), by T3 bits 31:29
  (0 never, 1 `<`, 2 `==`, 3 `<=`, 4 `>`, 5 `>=`, 6 `!=`, 7 always), and returns 1.0 for an f32
  texture or `0xffff` for `u16` when it holds, else 0; with bilinear filtering the four results
  are averaged (`0x8000` for one of two). A colour format ignores `.pcf`.

**Undecided:** mipmaps and the LOD values (`smp-lod`: neither `replace`, `bias` nor `grad` left
level 0 with T0 bits 20:17 at 0, 12 or 15), the border modes, the recorded formats, `.sinf`/
`.rsd`, `smp3d`, and state from `sa` or `c`.

### 8.38 `rlp`

*Measured on the hardware (2026-10-22).* **`rlp d, s, pN` rotates left through the predicate**:
`d = s << 1 | pN`, then `pN` = bit 31 of `s` **[HW]** (probe `rlp`, 32 cases over every
predicate). **Predicate numbers from 4 up alias `p0`-`p3`** (number mod 4: `rlp ..., p4` reads
and writes `p0`) **[HW]** (probe `rlp-high-predicates`).

### 8.39 Cache control `ci`, `cf`, `cfi`

*Measured on the hardware (2026-10-22).* **A load after `ci`, `cf` or `cfi`, on any of the
three level selections, with `.global` or `.dm_nomatch`, returns the pixel's own store**
**[HW]**: none of them drops a store or makes a later load read stale memory, and none
writes a register (probe `cache-control`, every pixel on its own dword).

**Undecided:** what they do to other units' view of memory (the texture cache, the host),
which our probes cannot see.

### 8.40 Video `adif`, `adifsum`

*Measured on the hardware (2026-10-22).* **`adif d, a, b` = `|a − b|` per byte; `adifsum d,
s0.low|high, a, b` = the sum of the four byte differences plus that 16-bit half of `s0`**, as a
32-bit value; a repeat advances the destination and the sources **[HW]** (probe `video-adif`).

**Undecided:** `bilin`, `firv` and `firh` (probe `video-filters`, recorded): no single or
two-weight interpolation of the source bytes fits `bilin`, and the FIR filters read
coefficient sets (`fcsN`) we do not set.

### 8.41 `.tpres`, `.partial`, `.nosched`, `.syncs`

*Measured on the hardware (2026-10-22).* In a fill, **`.tpres` (`frcp`, `frsq`, `fsqrt`),
`.partial` (`and`, `or`) and `.nosched` change nothing a pixel sees, while `.syncs` (on `mov`
and `fmad`) hangs the task**, which the driver recovers from with `-ETIMEDOUT` **[HW]** (probe
`flags-other`; `design/sgx545_hw_spec.md` §14). `.syncs` presumably waits for something a
fill never provides; what is open.

## 9. Open questions

Beyond the undecided parts of the hardware probe results, by how much they would matter to an
open driver:

1. **The behaviour of the groups no program of ours has run**: the visibility group. Of the
   probed groups, mipmaps and the LOD values of `smp`, the border address modes and the
   compressed and YUV formats, `bilin`/`firv`/`firh` and `fdpc`'s clip-plane result (likely a
   vertex-task output) remain.
2. **The MOE state as a whole**: `smoa`, `smbo`, `smr`, `imo`, `setfc` and what `moest`
   returns, beyond the `smlsi` increments the probes pinned.
3. **`.skipinv`, `.syncs`, `.nosched`** and the other scheduling flags: what changes when they
   are set or clear (performance only, or correctness); `.syncs` hangs a fill's task.
4. **The constant and global banks**: what `c` and `g` hold and who writes them.
5. **Emits**: `#incp`, `.cut`, `.tasks`/`.taske` and the exact role of `.twopart`/
   `.threepart`; the PBE sideband bits beyond the pack mode and `TILERELATIVE`.
6. **`idf` modes** other than `bif`, `lock`/`release`, `setm`, and the cache-control operands.
7. **Instruction timing and hazards**: how many instructions a result takes to be readable,
   whether the hardware interlocks, and what `.nosched` has to do with it.
8. **The destination bank 6 value 0** (`i.`).

The simulator (`crates/sgx-sim`) reproduces 25762 probe vectors and 364 pixel vectors; it stops on
4974 rather than guess, most often because (vectors, from `doc/isa/sim-coverage.json`):

- no semantics yet: frcp (983)
- no semantics yet: frsq (744)
- no semantics yet: fsqrt (744)
- no semantics yet: smp2d (655)
- no semantics yet: fcos (181)
- no semantics yet: fexp (181)
- no semantics yet: flog (181)
- no semantics yet: fsin (181)
- fadm (rounding open) (129)
- no semantics yet: bilin (75)
- no SGX registers on this bus (70)
- reads saN before the program writes it (56)
