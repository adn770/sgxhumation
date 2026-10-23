# SGX545 USSE instruction set reference

The **USSE** (universal scalable shader engine) is the programmable core of the PowerVR SGX
Series 5. On the SGX545 of Intel's Cedar Trail it runs every vertex, pixel and state program
of a scene and, as the **EDM** microkernel, the firmware itself. Its programs are started by
the PDS, the data sequencer, whose own instruction set has a companion reference,
`design/sgx545_pds_isa.md`.

This reference covers the whole instruction set as our disassembler `tools/usse-dis` decodes
it and our assembler `tools/usse-asm` writes it: the machine model, the operand encodings,
the assembler syntax, every opcode group with its bit layouts, fields, mnemonics, modifiers,
what is known of its behaviour and how the firmware uses it, and an appendix of every
instruction form. The decoding is complete and checked against the reference disassembler
(`tools/usse-dis/README.md`); the *behaviour* of many groups is not, and the text says so.

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

### 1.3 Reading the encodings

The decoders are tables of **forms**. A form is an assembler pattern, the bits that identify
it (`(word & mask) == value`) and up to ten **fields**. A field is one or more bit ranges of
the word, concatenated lowest first into a field value `v`, and a printer that turns `v` into
text. The decoder tries the forms in table order and prints the first whose fields all print;
the assembler reads the same table backwards and accepts a word only if the decoder prints it
back. Form numbers (`#12`) are table indices; they change when the table grows.

In the generated parts:

- **Patterns** name each field `<name>`, as in `<pred>ba<flags> #<imm>`. Names are derived
  from what a field prints: `pred` for an execution predicate, `dst` and `src0`-`src2` for
  register operands (by where the operand sits in the word), a flag's own name such as `end`,
  `op` for an operation, `imm` for a number, `extra` for an optional trailing operand. A field
  after the `+` of a two-operation instruction gets an `s` prefix; repeated names are numbered.
- **Diagrams** draw the word as boxes, highest bit first, each box headed by its highest and
  lowest bit. A box holds a field name, the value of fixed bits (`?` where the forms sharing
  the diagram differ) or `-` for bits the instruction ignores. A field split over several
  places appears once per place.
- **Field lists** say where each field's bits are (`27:21 → v[6:0], 51 → v[9]`: word bits
  27-21 are value bits 6-0, word bit 51 is value bit 9) and what its values print. `(none)` is
  a value that prints nothing.
- **Mnemonics and modifiers** are collected from every value the fields can print; numbered
  families are written once (`.repeatN`, `.fetchN`, `.incsXYZ`).

## 2. The instruction word

A USSE instruction is one **64-bit word**, stored little endian: the low dword `enc[31:0]`
first, then the high dword `enc[63:32]` **[RE]**. The decoder, its comments and this document
number the bits of the whole word, `enc[63:0]`; the diagrams draw the high dword above the low
one. Instruction addresses everywhere (branch targets, the link register, the code-address
words of the PDS) count **instructions**, not bytes, from the start of a code base
**[RE/HW]**: the microkernel only boots when it sits exactly at its code base, because its
branch targets and the kernel CCB's service addresses are absolute instruction indices
(`design/sgx545_hw_spec.md` §13.10a).

The top five bits `enc[63:59]` are the **major opcode class**. Most classes hold one family;
class 31 is the control and special group, subdivided by `enc[54:52]` and further bits. A few
forms leave part of the class to a field: the bitwise group fixes only `enc[63:60]` and reads
`enc[59]` as part of its operation.

Some bits recur with one meaning across most classes **[RE]**:

- `enc[58:56]`, the execution predicate (two bits, `enc[58:57]`, in the multiply-add and
  colour groups);
- `enc[55]` `.skipinv`, `enc[52]` `.syncs`, `enc[50]` `.end` and either `enc[54]` or `enc[43]`
  `.nosched` (the scheduling flags);
- `enc[27:21]` the destination number, `enc[20:14]` source 0, `enc[13:7]` source 1 and
  `enc[6:0]` source 2, their bank bits in `enc[34:28]` and the bank extension bits `enc[51]`,
  `enc[49]` and `enc[48]`;
- `enc[53]`, which turns a write mask in `enc[47:44]` into a repeat count.

The classes and the mnemonics each one holds:

- Class 0 (`0x00`): `fadm`, `fmad`, `fmsa`, `fsubflr`
- Class 1 (`0x01`): `fcos`, `fexp`, `flog`, `frcp`, `frsq`, `fsin`, `fsqrt`
- Class 2 (`0x02`): `fddp`, `fddpc`, `fdp`, `fdpc`
- Class 3 (`0x03`): `fmaxmin`, `fminmax`
- Class 4 (`0x04`): `fdsx`, `fdsy`
- Class 5 (`0x05`): `mov`, `movc`, `movmsk`
- Class 6 (`0x06`): `fmad16`
- Class 7 (`0x07`): `efo`
- Class 8 (`0x08`): `pckc10f16`, `pckc10f32`, `pckc10s16`, `pckc10u16`, `pckf16f16`, `pckf16f32`,
  `pckf16s16`, `pckf16u16`, `pcko8f16`, `pcko8f32`, `pcko8s16`, `pcko8u16`, `pcks16f16`,
  `pcks16f32`, `pcks16s16`, `pcks16u16`, `pcks8f16`, `pcks8f32`, `pcks8s16`, `pcks8u16`,
  `pcku16f16`, `pcku16f32`, `pcku16s16`, `pcku16u16`, `pcku8f16`, `pcku8f32`, `pcku8s16`,
  `pcku8u16`, `statomic`, `unpckc10c10`, `unpckf16c10`, `unpckf16o8`, `unpckf16s8`, `unpckf16u8`,
  `unpckf32c10`, `unpckf32f16`, `unpckf32f32`, `unpckf32o8`, `unpckf32s16`, `unpckf32s8`,
  `unpckf32u16`, `unpckf32u8`, `unpcks16c10`, `unpcks16o8`, `unpcks16s8`, `unpcks16u8`,
  `unpcku16c10`, `unpcku16o8`, `unpcku16s8`, `unpcku16u8`, `unpcku8s8`, `unpcku8u8`
- Class 9 (`0x09`): `and`, `asr`, `fadd`, `fdp`, `fdsx`, `fdsy`, `fexp`, `flog`, `fmax`, `fmin`,
  `fmul`, `fpadd8`, `fpmul8`, `fpsub8`, `frcp`, `frsq`, `fsub`, `fsubflr`, `iadd16`, `iadd32`,
  `iadd8`, `iaddu16`, `iaddu32`, `iaddu8`, `imul16`, `imul8`, `imulu16`, `imulu8`, `isub16`,
  `isub8`, `isubu16`, `isubu8`, `or`, `rol`, `shl`, `shr`, `xor`
- Class 10 (`0x0a`): `and`, `or`, `xor`
- Class 11 (`0x0b`): `and`, `or`, `xor`
- Class 12 (`0x0c`): `asr`, `rol`, `shl`, `shr`
- Class 13 (`0x0d`): `asr`, `rol`, `shl`, `shr`
- Class 14 (`0x0e`): `rlp`
- Class 15 (`0x0f`): `and`, `asr`, `fadd`, `fdp`, `fdsx`, `fdsy`, `fexp`, `flog`, `fmax`, `fmin`,
  `fmul`, `fpadd8`, `fpmul8`, `fpsub8`, `frcp`, `frsq`, `fsub`, `fsubflr`, `iadd16`, `iadd32`,
  `iadd8`, `iaddu16`, `iaddu32`, `iaddu8`, `imul16`, `imul8`, `imulu16`, `imulu8`, `isub16`,
  `isub8`, `isubu16`, `isubu8`, `or`, `rol`, `shl`, `shr`, `xor`
- Class 16 (`0x10`): `sop2`
- Class 17 (`0x11`): `lrp1`, `lrp2`, `sop3`
- Class 18 (`0x12`): `sop2wm`
- Class 19 (`0x13`): `ima8`
- Class 20 (`0x14`): `ima16`
- Class 21 (`0x15`): `imae`
- Class 22 (`0x16`): `adif`, `adifsum`, `bilin`, `firv`
- Class 23 (`0x17`): `firh`
- Class 24 (`0x18`): `u16dot3`, `u16dot3off`, `u16dot4`, `u16dot4off`, `u8dot3`, `u8dot3off`,
  `u8dot4`, `u8dot4off`
- Class 25 (`0x19`): `fpma`
- Class 26 (`0x1a`): `idiv`, `ima32`
- Class 27 (`0x1b`): `fadm`, `fddp`, `fmad`, `fminmax`, `fmsa`, `fssq`, `ima32`, `mov`
- Class 28 (`0x1c`): `smp1d`, `smp1dbias`, `smp1dgrad`, `smp1dreplace`, `smp2d`, `smp2dbias`,
  `smp2dgrad`, `smp2dreplace`, `smp3d`, `smp3dbias`, `smp3dgrad`, `smp3dreplace`
- Class 29 (`0x1d`): `eldd`, `eldq`, `ldab`, `ldad`, `ldaw`, `ldlb`, `ldld`, `ldlw`, `ldtb`, `ldtd`,
  `ldtw`
- Class 30 (`0x1e`): `stab`, `stad`, `staw`, `stlb`, `stld`, `stlw`, `sttb`, `sttd`, `sttw`
- Class 31 (`0x1f`): `atst8`, `ba`, `bexception`, `br`, `cf`, `cfi`, `ci`, `depthf`, `emitmtest`,
  `emitmtevtx`, `emitpds`, `emitpix1`, `emitpix2`, `emitvcbst`, `emitvcbvtx`, `idf`, `imo`, `lapc`,
  `ldr`, `lock`, `moest`, `mov`, `nop`, `pcoeff`, `phas`, `ptoff`, `release`, `setfc`, `setm`,
  `smbo`, `smlsi`, `smoa`, `smr`, `str`, `wdf`, `wop`

## 3. Machine model

A USSE program runs as a **task** over a set of **instances** (pixels of a batch, vertices,
or the single instance of a firmware or state task) **[infer]**. Each instruction is executed
for the instances of the task; the predicates and `.skipinv` decide which. The model below is
what our programs, the firmware and the captures show; the parts nobody has measured are
marked.

### 3.1 Register banks

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

### 3.2 Predicates

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

### 3.3 Program counter, calls and the end of a program

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

### 3.4 Repeats and the MOE state

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

### 3.5 Scheduling flags

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

### 3.6 Tasks and phases

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

### 3.7 Memory, data fences and the register file

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

## 4. Operand encodings

### 4.1 Register operands

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

### 4.2 Immediates

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

### 4.3 Write masks, byte masks and repeat counts

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

### 4.4 Source modifiers and data readings

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

## 5. Assembler syntax

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

## 6. Instruction groups

One section per opcode group. Each starts with what the group does and what is known of it,
how the firmware uses it and examples, then the generated part: the group's forms, its
mnemonics and modifiers, and a bit diagram with the field list for every distinct word layout.
Forms that differ only in fixed bits share a diagram. Every form of the decoder appears in
exactly one group (a test checks it) and again in the appendix.

### 6.1 Moves: `mov`, the 32-bit immediate and `pclink`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#0    class 31       mask FF30000000000000  value FC20000000000000  mov
      <pred>mov<skipinv><nosched><end> <dst>, #<imm>
#1    class 5        mask F820070000000000  value 2800000000000000  mov
      <pred>mov<skipinv><syncs><nosched><end> <dst><wmask>, <src1>
#2    class 5        mask F820070000000000  value 2820000000000000  mov
      <pred>mov<skipinv><syncs><nosched><end><repeat> <dst>, <src1>
#3    class 31       mask FFFFFFFFFFFFC07F  value F80000C000000000  mov
      mov pclink, <src1>
#4    class 31       mask FFFFFFFFF01FFFFF  value F800010000000000  mov
      mov <dst>, pclink
#125  class 31       mask F87001C000000000  value F80000C000000000  mov
      <pred>mov<nosched> pclink, <src1>
#126  class 31       mask F87001C000000000  value F800010000000000  mov
      <pred>mov<nosched> <dst>, pclink
```

Mnemonics: `mov`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 7 (form #0):

```
 63      56 55        54        53 52 51    50    49 44 43  41 40 36 35 34 33 32
+----------+---------+---------+-----+-----+-----+-----+------+-----+-----+-----+
| 11111100 | skipinv | nosched |  10 | dst | end | imm | pred | imm |  -  | dst |
+----------+---------+---------+-----+-----+-----+-----+------+-----+-----+-----+
 31 28 27 21 20  0
+-----+-----+-----+
|  -  | dst | imm |
+-----+-----+-----+
```

- `pred` — bits 43:41. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `skipinv` — bit 55. Values: 0 (none), 1 `.skipinv`.
- `nosched` — bit 54. Values: 0 (none), 1 `.nosched`.
- `end` — bit 50. Values: 0 (none), 1 `.end`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `imm` — bits 20:0 → v[20:0], 40:36 → v[25:21], 49:44 → v[31:26]. A 32-bit number, printed in hex.

Layout 2 of 7 (form #1):

```
 63   59 58  56 55        54  53  52      51    50    49     48
+-------+------+---------+---+---+-------+-----+-----+------+---+
| 00101 | pred | skipinv | - | 0 | syncs | dst | end | src1 | - |
+-------+------+---------+---+---+-------+-----+-----+------+---+
 47   44 43        42 40 39 34 33 32
+-------+---------+-----+-----+-----+
| wmask | nosched | 000 |  -  | dst |
+-------+---------+-----+-----+-----+
 31  30 29 28 27 21 20 14 13   7 6 0
+------+-----+-----+-----+------+---+
| src1 |  -  | dst |  -  | src1 | - |
+------+-----+-----+-----+------+---+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `skipinv` — as in layout 1.
- `syncs` — bit 52. Values: 0 (none), 1 `.syncs`.
- `nosched` — bit 43. Values: 0 (none), 1 `.nosched`.
- `end` — as in layout 1.
- `dst` — as in layout 1.
- `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5 `.xz`, 6 `.yz`, 7
  `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15 `.xyzw`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2, with a seven-bit immediate printed in hex (*Register operands*).

Layout 3 of 7 (form #2):

```
 63   59 58  56 55        54  53  52      51    50    49     48
+-------+------+---------+---+---+-------+-----+-----+------+---+
| 00101 | pred | skipinv | - | 1 | syncs | dst | end | src1 | - |
+-------+------+---------+---+---+-------+-----+-----+------+---+
 47    44 43        42 40 39 34 33 32
+--------+---------+-----+-----+-----+
| repeat | nosched | 000 |  -  | dst |
+--------+---------+-----+-----+-----+
 31  30 29 28 27 21 20 14 13   7 6 0
+------+-----+-----+-----+------+---+
| src1 |  -  | dst |  -  | src1 | - |
+------+-----+-----+-----+------+---+
```

- `pred` — as in layout 2.
- `skipinv` — as in layout 1.
- `syncs` — as in layout 2.
- `nosched` — as in layout 2.
- `end` — as in layout 1.
- `repeat` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `dst` — as in layout 1.
- `src1` — as in layout 2.

Layout 4 of 7 (form #3):

```
 63                              32
+----------------------------------+
| 11111000000000000000000011000000 |
+----------------------------------+
 31                14 13   7 6       0
+--------------------+------+---------+
| 000000000000000000 | src1 | 0000000 |
+--------------------+------+---------+
```

- `src1` — bits 13:7. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `r0`…`i3`.

Layout 5 of 7 (form #4):

```
 63                              32
+----------------------------------+
| 11111000000000000000000100000000 |
+----------------------------------+
 31  28 27 21 20                    0
+------+-----+-----------------------+
| 0000 | dst | 000000000000000000000 |
+------+-----+-----------------------+
```

- `dst` — bits 27:21. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `r0`…`i3`.

Layout 6 of 7 (form #125):

```
 63   59 58  56 55  54 52 51 50 49     48 44 43        42 41 40 38 37 32
+-------+------+---+-----+-----+------+-----+---------+-----+-----+-----+
| 11111 | pred | - | 000 |  -  | src1 |  -  | nosched |  -  | 011 |  -  |
+-------+------+---+-----+-----+------+-----+---------+-----+-----+-----+
 31  30 29 14 13   7 6 0
+------+-----+------+---+
| src1 |  -  | src1 | - |
+------+-----+------+---+
```

- `pred` — as in layout 2.
- `nosched` — as in layout 2.
- `src1` — as in layout 2.

Layout 7 of 7 (form #126):

```
 63   59 58  56 55  54 52 51    50 44 43        42 41 40 38 37 34 33 32
+-------+------+---+-----+-----+-----+---------+-----+-----+-----+-----+
| 11111 | pred | - | 000 | dst |  -  | nosched |  -  | 100 |  -  | dst |
+-------+------+---+-----+-----+-----+---------+-----+-----+-----+-----+
 31 28 27 21 20 0
+-----+-----+----+
|  -  | dst | -  |
+-----+-----+----+
```

- `pred` — as in layout 2.
- `nosched` — as in layout 2.
- `dst` — as in layout 1.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 7 forms have semantics. The
probe vectors that test them: 964 match the hardware, 112 stop unsupported, 0 mismatch
(`flags-other` 1/0/0, `index-high` 0/12/0, `index-store` 56/0/0, `index-unit` 98/0/0, `index-width`
42/0/0, `index-width2` 98/0/0, `index-width3` 84/0/0, `internal-registers` 0/58/0,
`internal-registers-across-tasks` 18/18/0, `internal-registers-bank7` 29/0/0,
`internal-registers-per-pixel` 54/0/0, `repeat-order` 42/0/0, `repeat-pred` 56/0/0,
`test-lanes-other` 74/0/0, `wmask-mov` 224/0/0, `x-bank7` 24/0/0, `x-gpiext-map` 4/0/0,
`x-gpiext-map2` 4/0/0, `x-gpiext-xtask` 6/4/0, `x-misc` 50/20/0); unsupported mostly for reads iN
before the program writes it (55); rN is past the task's N temporaries (24); iN reads another
instance of the quad (23).

### 6.2 Conditional and masked moves: `movc`, `movmsk`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#187  class 5        mask F820000000000000  value 2800000000000000  movc
      <pred>movc<flags1><flags2><flags3> <dst><wmask>, <src0>, <src1>, <src2>
#188  class 5        mask F820000000000000  value 2820000000000000  movc
      <pred>movc<flags1><flags2><repeat><flags3> <dst>, <src0>, <src1>, <src2>
#189  class 5        mask F820070000000000  value 2820010000000000  movc
      <pred>movc<flags1><repeat>.i8<flags2> <dst>, <src0>, <src1>, <src2>
#190  class 5        mask F820070000000000  value 2800060000000000  movmsk
      <pred>movmsk<flags1><flags2> <dst><wmask>, <src1>, <src2><flags3>
#191  class 5        mask F820070000000000  value 2820060000000000  movmsk
      <pred>movmsk<flags1><flags2><repeat> <dst>, <src1>, <src2><flags3>
#192  class 5        mask F820070000000000  value 2800070000000000  movmsk
      <pred>movmsk<flags1><flags2>.smsk <dst><wmask>, <src0>, <src1>, <src2><flags3>
#193  class 5        mask F820070000000000  value 2820070000000000  movmsk
      <pred>movmsk<flags1><flags2>.smsk<repeat> <dst>, <src0>, <src1>, <src2><flags3>
#194  class 5        mask F860078000000000  value 2800068000000000  movmsk
      <pred>movmsk<flags1>.u8 <dst><wmask>, <src1>, <src2><flags2>
#195  class 5        mask F860078000000000  value 2820068000000000  movmsk
      <pred>movmsk<flags1><repeat>.u8 <dst>, <src1>, <src2><flags2>
#196  class 5        mask F860078000000000  value 2800078000000000  movmsk
      <pred>movmsk<flags1>.smsk.u8 <dst><wmask>, <src0>, <src1>, <src2><flags2>
#197  class 5        mask F860078000000000  value 2820078000000000  movmsk
      <pred>movmsk<flags1>.smsk<repeat>.u8 <dst>, <src0>, <src1>, <src2><flags2>
```

Mnemonics: `movc`, `movmsk`.

Modifiers: `.end`, `.flt`, `.flt16`, `.i10`, `.i16`, `.i32`, `.i8`, `.mask`, `.nosched`, `.repeatN`,
`.skipinv`, `.smsk`, `.syncs`, `.tn`, `.tnz`, `.tp|z`, `.tz`, `.u8`.

Layout 1 of 7 (form #187):

```
 63   59 58  56 55       54       53  52       51    50       49     48
+-------+------+--------+--------+---+--------+-----+--------+------+------+
| 00101 | pred | flags1 | flags3 | 0 | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+--------+---+--------+-----+--------+------+------+
 47   44 43       42    40 39       38 35 34     33 32
+-------+--------+--------+--------+-----+------+-----+
| wmask | flags1 | flags2 | flags3 |  -  | src0 | dst |
+-------+--------+--------+--------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `flags1` — bits 55 → v[0], 52 → v[1], 43 → v[2], 50 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`.
- `flags2` — bits 42:40. Values: 1 `.i8`, 2 `.i16`, 3 `.i32`, 4 `.flt`, 5 `.i10`. Other values do
  not decode.
- `flags3` — bits 39 → v[0], 54 → v[1]. Values: 0 `.tz`, 1 `.tnz`, 2 `.tn`, 3 `.tp|z`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5 `.xz`, 6 `.yz`, 7
  `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15 `.xyzw`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 7 (form #188):

```
 63   59 58  56 55       54       53  52       51    50       49     48
+-------+------+--------+--------+---+--------+-----+--------+------+------+
| 00101 | pred | flags1 | flags3 | 1 | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+--------+---+--------+-----+--------+------+------+
 47    44 43       42    40 39       38 35 34     33 32
+--------+--------+--------+--------+-----+------+-----+
| repeat | flags1 | flags2 | flags3 |  -  | src0 | dst |
+--------+--------+--------+--------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags1` — as in layout 1.
- `flags2` — bits 42:40. Values: 2 `.i16`, 3 `.i32`, 4 `.flt`, 5 `.i10`. Other values do not decode.
- `repeat` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `flags3` — as in layout 1.
- `dst` — as in layout 1.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.

Layout 3 of 7 (form #189):

```
 63   59 58  56 55       54       53  52       51    50       49     48
+-------+------+--------+--------+---+--------+-----+--------+------+------+
| 00101 | pred | flags1 | flags2 | 1 | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+--------+---+--------+-----+--------+------+------+
 47    44 43       42 40 39       38 35 34     33 32
+--------+--------+-----+--------+-----+------+-----+
| repeat | flags1 | 001 | flags2 |  -  | src0 | dst |
+--------+--------+-----+--------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags1` — as in layout 1.
- `repeat` — as in layout 2.
- `flags2` — bits 39 → v[0], 54 → v[1]. Values: 0 `.tz`, 1 `.tnz`, 2 `.tn`, 3 `.tp|z`.
- `dst` — as in layout 1.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.

Layout 4 of 7 (forms #190, #191):

```
 63   59 58  56 55       54       53  52       51    50       49     48
+-------+------+--------+--------+---+--------+-----+--------+------+------+
| 00101 | pred | flags1 | flags2 | ? | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+--------+---+--------+-----+--------+------+------+
 47   44 43       42 40 39       38    37 36 34 33 32
+-------+--------+-----+--------+--------+-----+-----+
| wmask | flags1 | 110 | flags2 | flags3 |  -  | dst |
+-------+--------+-----+--------+--------+-----+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 1.
- `flags1` — as in layout 1.
- `flags2` — bits 39 → v[0], 54 → v[1]. Values: 0 `.mask`, 2 `.flt16`, 3 `.flt`. Other values do not
  decode.
- `dst` — as in layout 1.
- (form #190) `wmask` — as in layout 1.
- (form #191) `wmask` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `flags3` — bits 38:37. Values: 0 (none), 1-3 `.1`…`.3`.

Layout 5 of 7 (forms #192, #193):

```
 63   59 58  56 55       54       53  52       51    50       49     48
+-------+------+--------+--------+---+--------+-----+--------+------+------+
| 00101 | pred | flags1 | flags2 | ? | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+--------+---+--------+-----+--------+------+------+
 47   44 43       42 40 39       38    37 36 35 34     33 32
+-------+--------+-----+--------+--------+-----+------+-----+
| wmask | flags1 | 111 | flags2 | flags3 |  -  | src0 | dst |
+-------+--------+-----+--------+--------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags1` — as in layout 1.
- `flags2` — as in layout 4.
- `dst` — as in layout 1.
- (form #192) `wmask` — as in layout 1.
- (form #193) `wmask` — as in layout 4.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `flags3` — as in layout 4.

Layout 6 of 7 (forms #194, #195):

```
 63   59 58  56 55       54 53 52       51    50       49     48
+-------+------+--------+-----+--------+-----+--------+------+------+
| 00101 | pred | flags1 |  0? | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+-----+--------+-----+--------+------+------+
 47   44 43       42  39 38    37 36 34 33 32
+-------+--------+------+--------+-----+-----+
| wmask | flags1 | 1101 | flags2 |  -  | dst |
+-------+--------+------+--------+-----+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 1.
- `flags1` — as in layout 1.
- (form #194) `wmask` — as in layout 1.
- (form #195) `wmask` — as in layout 4.
- `dst` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `flags2` — bits 38:37. Values: 0 (none), 1-3 `.1`…`.3`.

Layout 7 of 7 (forms #196, #197):

```
 63   59 58  56 55       54 53 52       51    50       49     48
+-------+------+--------+-----+--------+-----+--------+------+------+
| 00101 | pred | flags1 |  0? | flags1 | dst | flags1 | src1 | src2 |
+-------+------+--------+-----+--------+-----+--------+------+------+
 47   44 43       42  39 38    37 36 35 34     33 32
+-------+--------+------+--------+-----+------+-----+
| wmask | flags1 | 1111 | flags2 |  -  | src0 | dst |
+-------+--------+------+--------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags1` — as in layout 1.
- (form #196) `wmask` — as in layout 1.
- (form #197) `wmask` — as in layout 4.
- `dst` — as in layout 1.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `flags2` — as in layout 6.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 11 of 11 forms have semantics. The
probe vectors that test them: 1030 match the hardware, 0 stop unsupported, 0 mismatch (`movc-flt`
112/0/0, `movc-i10` 112/0/0, `movc-i16` 112/0/0, `movc-i32` 112/0/0, `movc-i8` 112/0/0, `movmsk-flt`
28/0/0, `movmsk-flt16` 56/0/0, `movmsk-mask` 56/0/0, `movmsk-smsk` 28/0/0, `movmsk-smsk2` 192/0/0,
`movmsk-u8` 56/0/0, `wmask-other` 14/0/0, `x-smsk` 16/0/0, `x-smsk2` 24/0/0).

### 6.3 Bitwise operations and shifts: `and`, `or`, `xor`, `shl`, `shr`, `rol`, `asr`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#15   classs 10-11   mask F021000030000000  value 5001000020000000  and or xor
      <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
#16   classs 10-11   mask F021000030000000  value 5021000020000000  and or xor
      <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
#17   classs 10-11   mask F020000000000000  value 5000000000000000  and or xor
      <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
#18   classs 10-11   mask F020000000000000  value 5020000000000000  and or xor
      <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
#19   classs 12-13   mask F021000030000000  value 6001000020000000  asr rol shl shr
      <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
#20   classs 12-13   mask F021000030000000  value 6021000020000000  asr rol shl shr
      <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
#21   classs 12-13   mask F020000000000000  value 6000000000000000  asr rol shl shr
      <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
#22   classs 12-13   mask F020000000000000  value 6020000000000000  asr rol shl shr
      <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
```

Mnemonics: `and`, `asr`, `or`, `rol`, `shl`, `shr`, `xor`.

Modifiers: `.end`, `.nosched`, `.partial`, `.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 2 (forms #15, #16, #19, #20):

```
 63  60 59   58  56 55   54 53  52      51    50      49     48
+------+----+------+-------+---+-------+-----+-------+------+---+
| 01?? | op | pred | flags | ? | flags | dst | flags | src1 | 1 |
+------+----+------+-------+---+-------+-----+-------+------+---+
 47   44 43    42 36 35   34      33 32
+-------+-----+-----+----+-------+-----+
| wmask | inv | imm | op | flags | dst |
+-------+-----+-----+----+-------+-----+
 31  30 29 28 27 21 20 14 13   7 6   0
+------+-----+-----+-----+------+-----+
| src1 |  10 | dst | imm | src1 | imm |
+------+-----+-----+-----+------+-----+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- (forms #15, #16) `op` — bits 35 → v[0], 59 → v[1]. Values: 0 `and`, 1 `or`, 2 `xor`. Other values
  do not decode.
- (forms #19, #20) `op` — bits 35 → v[0], 59 → v[1]. Values: 0 `shl`, 1 `rol`, 2 `shr`, 3 `asr`.
- `flags` — bits 55 → v[0], 52 → v[1], 54 → v[2], 50 → v[3], 34 → v[4]. Flags, printed in this
  order. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`, v[4]
  `.partial`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- (forms #15, #19) `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5 `.xz`,
  6 `.yz`, 7 `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15
  `.xyzw`.
- (forms #16, #20) `wmask` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `inv` — bit 43. Values: 0 (none), 1 `~`.
- `imm` — bits 6:0 → v[6:0], 19:14 → v[12:7], 20 → v[13], 37:36 → v[15:14], 42:38 → v[20:16]. A
  21-bit value printed as a number by the field's own printer (the group's text says how), for
  example 0 `0x00000000`, 1 `0x00000001`, 1048576 `0x00000000`, 2097151 `0x80007FFF`.

Layout 2 of 2 (forms #17, #18, #21, #22):

```
 63  60 59   58  56 55   54 53  52      51    50      49     48
+------+----+------+-------+---+-------+-----+-------+------+------+
| 01?? | op | pred | flags | ? | flags | dst | flags | src1 | src2 |
+------+----+------+-------+---+-------+-----+-------+------+------+
 47   44 43    42 36 35   34      33 32
+-------+-----+-----+----+-------+-----+
| wmask | inv |  -  | op | flags | dst |
+-------+-----+-----+----+-------+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 1.
- (forms #17, #18) `op` — as in layout 1.
- (forms #21, #22) `op` — as in layout 1.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- (forms #17, #21) `wmask` — as in layout 1.
- (forms #18, #22) `wmask` — as in layout 1.
- `src1` — as in layout 1.
- `inv` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 8 of 8 forms have semantics. The
probe vectors that test them: 1089 match the hardware, 28 stop unsupported, 0 mismatch (`asr-reg`
98/0/0, `br-offset` 5/0/0, `fdpc` 84/0/0, `fdpc-clip` 80/0/0, `flags-other` 0/2/0, `rol-reg` 98/0/0,
`shift-immediates` 25/0/0, `shl-reg` 98/0/0, `shr-reg` 98/0/0, `wmask-and-imm` 224/0/0, `wmask-or`
224/0/0, `x-fdpc` 24/24/0, `x-m2` 5/0/0, `x-misc` 26/2/0); unsupported mostly for reads rN before
the program writes it (24); flag .partial (4).

### 6.4 The test group: ALU operations with a test

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#23   class 9        mask F800000000000000  value 4800000000000000  and asr or rol shl shr xor
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#24   class 9        mask F804000000000000  value 4800000000000000  fadd fdp fdsx fdsy fmax fmin
      fmul fpadd8 fpmul8 fpsub8 fsub fsubflr iadd16 iadd32 iadd8 iaddu16 iaddu32 iaddu8 imul16 imul8
      imulu16 imulu8 isub16 isub8 isubu16 isubu8
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#25   class 9        mask F804000000000000  value 4800000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
#198  class 9        mask F804000000000000  value 4804000000000000  iadd16 iadd32 iaddu16 iaddu32
      imul16 imulu16 isub16 isubu16
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#199  class 9        mask F804000000000000  value 4804000000000000  iadd8 iaddu8 imul8 imulu8 isub8
      isubu8
      <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#200  class 9        mask F804000000000000  value 4804000000000000  fadd fmul fsub fsubflr
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#201  class 9        mask F804000000000000  value 4804000000000000  fdp fdsx fdsy fmax fmin
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#202  class 9        mask F804000000000000  value 4804000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
#203  class 9        mask F804000000000000  value 4804000000000000  fpadd8 fpmul8 fpsub8
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#204  class 15       mask F800000000000000  value 7800000000000000  and asr or rol shl shr xor
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#205  class 15       mask F804000000000000  value 7800000000000000  fadd fdp fdsx fdsy fmax fmin
      fmul fpadd8 fpmul8 fpsub8 fsub fsubflr iadd16 iadd32 iadd8 iaddu16 iaddu32 iaddu8 imul16 imul8
      imulu16 imulu8 isub16 isub8 isubu16 isubu8
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#206  class 15       mask F804000000000000  value 7800000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>
#207  class 15       mask F804000000000000  value 7804000000000000  iadd16 iadd32 iaddu16 iaddu32
      imul16 imulu16 isub16 isubu16
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#208  class 15       mask F804000000000000  value 7804000000000000  iadd8 iaddu8 imul8 imulu8 isub8
      isubu8
      <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
#209  class 15       mask F804000000000000  value 7804000000000000  fadd fmul fsub fsubflr
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#210  class 15       mask F804000000000000  value 7804000000000000  fdp fdsx fdsy fmax fmin
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#211  class 15       mask F804000000000000  value 7804000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>
#212  class 15       mask F804000000000000  value 7804000000000000  fpadd8 fpmul8 fpsub8
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
```

Mnemonics: `and`, `asr`, `fadd`, `fdp`, `fdsx`, `fdsy`, `fexp`, `flog`, `fmax`, `fmin`, `fmul`,
`fpadd8`, `fpmul8`, `fpsub8`, `frcp`, `frsq`, `fsub`, `fsubflr`, `iadd16`, `iadd32`, `iadd8`,
`iaddu16`, `iaddu32`, `iaddu8`, `imul16`, `imul8`, `imulu16`, `imulu8`, `isub16`, `isub8`,
`isubu16`, `isubu8`, `or`, `rol`, `shl`, `shr`, `xor`.

Modifiers: `.and02`, `.andall`, `.bmsk`, `.chan0`, `.chan1`, `.chan2`, `.chan3`, `.dmsk`,
`.nosched`, `.onceonly`, `.or02`, `.orall`, `.partial`, `.sat`, `.skipinv`, `.syncs`, `.testnanz`,
`.testnat`, `.testnaz`, `.testnonz`, `.testnot`, `.testnoz`, `.testpanz`, `.testpat`, `.testpaz`,
`.testponz`, `.testpot`, `.testpoz`, `.testtanz`, `.testtat`, `.testtaz`, `.testtonz`, `.testtot`,
`.testtoz`, `.wmsk`.

Layout 1 of 6 (form #23):

```
 63   59 58  56 55   52 51    50      49     48     47   44 43  36 35 34 33 32
+-------+------+-------+-----+-------+------+------+-------+------+-----+-----+
| 01001 | pred | flags | dst | flags | src1 | src2 | wmask | test |  p  | dst |
+-------+------+-------+-----+-------+------+------+-------+------+-----+-----+
 31  30 29  28 27 21 20  19 14 13   7 6    0
+------+------+-----+---+-----+------+------+
| src1 | src2 | dst | w |  op | src1 | src2 |
+------+------+-----+---+-----+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 19:14. Values: 48 `and`, 49 `or`, 50 `xor`, 51 `shl`, 52 `shr`, 53 `rol`, 55 `asr`.
  Other values do not decode.
- `flags` — bits 55 → v[0], 52 → v[1], 54 → v[2], 50 → v[3], 53 → v[4]. Flags, printed in this
  order. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.partial`, v[4]
  `.onceonly`.
- `test` — bits 43:36. 144 distinct texts, for example 0 `testtot.chan0`, 1 `testtot.chan1`, 2
  `testtot.chan2`, 3 `testtot.chan3`, 4 `testtot.andall`, 5 `testtot.orall`.
- `w` — bit 20. Values: 0 `!`, 1 (none).
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5 `.xz`, 6 `.yz`, 7
  `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15 `.xyzw`.
- `p` — bits 35:34. A 2-bit number, printed in decimal.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2, with a seven-bit immediate printed in hex (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 6 (forms #24, #198, #199, #200, #201, #203):

```
 63   59 58  56 55   52 51    50  49     48     47   44 43  36 35 34 33 32
+-------+------+-------+-----+---+------+------+-------+------+-----+-----+
| 01001 | pred | flags | dst | ? | src1 | src2 | wmask | test |  p  | dst |
+-------+------+-------+-----+---+------+------+-------+------+-----+-----+
 31  30 29  28 27 21 20  19 14 13   7 6    0
+------+------+-----+---+-----+------+------+
| src1 | src2 | dst | w |  op | src1 | src2 |
+------+------+-----+---+-----+------+------+
```

- `pred` — as in layout 1.
- (form #24) `op` — bits 19:14. Values: 0 `fadd`, 3 `fsubflr`, 8 `fdp`, 9 `fmin`, 10 `fmax`, 11
  `fdsx`, 12 `fdsy`, 13 `fmul`, 14 `fsub`, 22 `iadd16`, 23 `isub16`, 24 `imul16`, 25 `iaddu16`, 26
  `isubu16`, 27 `imulu16`, 28 `iadd32`, 29 `iaddu32`, 32 `iadd8`, 33 `isub8`, 34 `iaddu8`, 35
  `isubu8`, 36 `imul8`, 37 `fpmul8`, 38 `imulu8`, 39 `fpadd8`, 40 `fpsub8`. Other values do not
  decode.
- (form #198) `op` — bits 19:14. Values: 22 `iadd16`, 23 `isub16`, 24 `imul16`, 25 `iaddu16`, 26
  `isubu16`, 27 `imulu16`, 28 `iadd32`, 29 `iaddu32`. Other values do not decode.
- (form #199) `op` — bits 19:14. Values: 32 `iadd8`, 33 `isub8`, 34 `iaddu8`, 35 `isubu8`, 36
  `imul8`, 38 `imulu8`. Other values do not decode.
- (form #200) `op` — bits 19:14. Values: 0 `fadd`, 3 `fsubflr`, 13 `fmul`, 14 `fsub`. Other values
  do not decode.
- (form #201) `op` — bits 19:14. Values: 8 `fdp`, 9 `fmin`, 10 `fmax`, 11 `fdsx`, 12 `fdsy`. Other
  values do not decode.
- (form #203) `op` — bits 19:14. Values: 37 `fpmul8`, 39 `fpadd8`, 40 `fpsub8`. Other values do not
  decode.
- `flags` — bits 55 → v[0], 52 → v[1], 54 → v[2], 53 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.onceonly`.
- `test` — as in layout 1.
- `w` — as in layout 1.
- (forms #24, #198, #199) `dst` — as in layout 1.
- (forms #200, #201) `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1
  `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `r[il + #0]`…`sa[ih + #7].flt16`, 4
  `sa0`…`sa31.flt16.2`, 5 `c0`…`g63`, 6 `i.`…`i.lh`, 7 `i0`…`i31.flt16.2`.
- (form #203) `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`,
  2 `pa0`…`pa63.c10`, 3 `r[il + #0]`…`sa[ih + #7].c10`, 4 `sa0`…`sa63.c10`, 5 `c0`…`g63`, 6
  `i.`…`i.lh`, 7 `i0`…`i63.c10`.
- `wmask` — as in layout 1.
- `p` — as in layout 1.
- (forms #24, #198, #199) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register
  operand: the standard source 1 or 2 (*Register operands*).
- (form #200) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1
  `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`.
- (form #201) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1
  `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`.
- (form #203) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`,
  2 `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- (forms #24, #198, #199) `src2` — as in layout 1.
- (form #200) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1
  `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`.
- (form #201) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1
  `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`.
- (form #203) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`,
  2 `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.

Layout 3 of 6 (forms #25, #202):

```
 63   59 58  56 55   52 51    50  49     48  47   44 43  36 35 34 33 32
+-------+------+-------+-----+---+------+---+-------+------+-----+-----+
| 01001 | pred | flags | dst | ? | src1 | - | wmask | test |  p  | dst |
+-------+------+-------+-----+---+------+---+-------+------+-----+-----+
 31  30 29 28 27 21 20  19 14 13   7 6 0
+------+-----+-----+---+-----+------+---+
| src1 |  -  | dst | w |  op | src1 | - |
+------+-----+-----+---+-----+------+---+
```

- `pred` — as in layout 1.
- `op` — bits 19:14. Values: 4 `frcp`, 5 `frsq`, 6 `flog`, 7 `fexp`. Other values do not decode.
- `flags` — as in layout 2.
- `test` — as in layout 1.
- `w` — as in layout 1.
- (form #25) `dst` — as in layout 1.
- (form #202) `dst` — as in layout 2.
- `wmask` — as in layout 1.
- `p` — as in layout 1.
- (form #25) `src1` — as in layout 2.
- (form #202) `src1` — as in layout 2.

Layout 4 of 6 (form #204):

```
 63   59 58  56 55   52 51    50      49     48     47   44 43  39 38  37  36 35 34 33 32
+-------+------+-------+-----+-------+------+------+-------+------+---+------+-----+-----+
| 01111 | pred | flags | dst | flags | src1 | src2 | wmask | test | - | test |  -  | dst |
+-------+------+-------+-----+-------+------+------+-------+------+---+------+-----+-----+
 31  30 29  28 27 21 20  19 14 13   7 6    0
+------+------+-----+---+-----+------+------+
| src1 | src2 | dst | w |  op | src1 | src2 |
+------+------+-----+---+-----+------+------+
```

- `pred` — as in layout 1.
- `op` — as in layout 1.
- `flags` — as in layout 1.
- `test` — bits 37:36 → v[1:0], 43:39 → v[6:2]. 54 distinct texts, for example 0 `testtot.bmsk`, 1
  `testtot.wmsk`, 2 `testtot.dmsk`, 4 `testtat.bmsk`, 5 `testtat.wmsk`, 6 `testtat.dmsk`.
- `w` — as in layout 1.
- `dst` — as in layout 1.
- `wmask` — as in layout 1.
- `src1` — as in layout 2.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2, with a seven-bit immediate printed in hex (*Register operands*).

Layout 5 of 6 (forms #205, #207, #208, #209, #210, #212):

```
 63   59 58  56 55   52 51    50  49     48     47   44 43  39 38  37  36 35 34 33 32
+-------+------+-------+-----+---+------+------+-------+------+---+------+-----+-----+
| 01111 | pred | flags | dst | ? | src1 | src2 | wmask | test | - | test |  -  | dst |
+-------+------+-------+-----+---+------+------+-------+------+---+------+-----+-----+
 31  30 29  28 27 21 20  19 14 13   7 6    0
+------+------+-----+---+-----+------+------+
| src1 | src2 | dst | w |  op | src1 | src2 |
+------+------+-----+---+-----+------+------+
```

- `pred` — as in layout 1.
- (form #205) `op` — as in layout 2.
- (form #207) `op` — as in layout 2.
- (form #208) `op` — as in layout 2.
- (form #209) `op` — as in layout 2.
- (form #210) `op` — as in layout 2.
- (form #212) `op` — as in layout 2.
- `flags` — as in layout 2.
- `test` — as in layout 4.
- `w` — as in layout 1.
- (forms #205, #207, #208) `dst` — as in layout 1.
- (forms #209, #210) `dst` — as in layout 2.
- (form #212) `dst` — as in layout 2.
- `wmask` — as in layout 1.
- (forms #205, #207, #208) `src1` — as in layout 2.
- (form #209) `src1` — as in layout 2.
- (form #210) `src1` — as in layout 2.
- (form #212) `src1` — as in layout 2.
- (forms #205, #207, #208) `src2` — as in layout 1.
- (form #209) `src2` — as in layout 2.
- (form #210) `src2` — as in layout 2.
- (form #212) `src2` — as in layout 2.

Layout 6 of 6 (forms #206, #211):

```
 63   59 58  56 55   52 51    50  49     48  47   44 43  39 38  37  36 35 34 33 32
+-------+------+-------+-----+---+------+---+-------+------+---+------+-----+-----+
| 01111 | pred | flags | dst | ? | src1 | - | wmask | test | - | test |  -  | dst |
+-------+------+-------+-----+---+------+---+-------+------+---+------+-----+-----+
 31  30 29 28 27 21 20  19 14 13   7 6 0
+------+-----+-----+---+-----+------+---+
| src1 |  -  | dst | w |  op | src1 | - |
+------+-----+-----+---+-----+------+---+
```

- `pred` — as in layout 1.
- `op` — as in layout 3.
- `flags` — as in layout 2.
- `test` — as in layout 4.
- `w` — as in layout 1.
- (form #206) `dst` — as in layout 1.
- (form #211) `dst` — as in layout 2.
- `wmask` — as in layout 1.
- (form #206) `src1` — as in layout 2.
- (form #211) `src1` — as in layout 2.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 3 of 18 forms have semantics (not
yet: #198, #199, #200, #201, #202, #203, #204, #205, #206, #207, #208, #209, #210, #211, #212). The
probe vectors that test them: 4568 match the hardware, 0 stop unsupported, 0 mismatch (`chan-and`
168/0/0, `chan-isub16` 70/0/0, `chan-preds` 112/0/0, `iadd32-n` 56/0/0, `iadd32-p` 42/0/0,
`iadd32-src1-imm` 28/0/0, `iadd32-value` 56/0/0, `iadd32-z` 70/0/0, `iaddu32-imm` 56/0/0,
`iaddu32-np` 42/0/0, `iaddu32-tests` 84/0/0, `iaddu32-value` 56/0/0, `isub16-reg` 42/0/0,
`isub16-sign-n` 56/0/0, `isub16-sign-p` 56/0/0, `isub16-value` 84/0/0, `isub16-zero` 84/0/0,
`isubu16-value` 42/0/0, `isubu16-zero` 42/0/0, `join-t-not` 42/0/0, `join-t-tonz` 28/0/0,
`join-t-toz` 28/0/0, `shl-test-reg` 56/0/0, `sign-n-iadd32` 56/0/0, `sign-n-iaddu32` 56/0/0,
`sign-p-and` 42/0/0, `sign-p-shl` 60/0/0, `test-channels` 1800/0/0, `test-lanes-other` 74/0/0,
`x-chan-and` 80/0/0, `x-chan-iadd16` 80/0/0, `x-chan-iadd32` 80/0/0, `x-chan-iadd8` 80/0/0,
`x-chan-or` 80/0/0, `x-chan2-iadd16-testnanz` 48/0/0, `x-chan2-iadd16-testnat` 48/0/0,
`x-chan2-iadd16-testpat` 48/0/0, `x-chan2-iadd16-testtanz` 48/0/0, `x-chan2-iadd32-testnanz` 48/0/0,
`x-chan2-iadd32-testnat` 48/0/0, `x-chan2-iadd32-testpat` 48/0/0, `x-chan2-iadd32-testtanz` 48/0/0,
`x-chan2-iadd8-testnanz` 48/0/0, `x-chan2-iadd8-testnat` 48/0/0, `x-chan2-iadd8-testpat` 48/0/0,
`x-chan2-iadd8-testtanz` 48/0/0, `x-misc` 48/0/0, `zero-z-iadd32` 56/0/0).

### 6.5 Integer multiply-add: `imae`, `ima16`, `ima32`, `ima8`, `idiv`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#26   class 21       mask F80083C000000000  value A800008000000000  imae
      <pred>imae<flags>.<type><rs> <dst>, <src0>.<half1>, <src1>.<half2>, <src2>, u32
#27   class 20       mask F80083E000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2><neg>, u16, u16
#28   class 26       mask F8300C4000000000  value D000000000000000  ima32
      <pred>ima32<flags>.<type> <dst>, <src0>, <src1><neg1>, <src2><neg2>
#171  class 19       mask F800000000000000  value 9800000000000000  ima8
      <pred>ima8<flags><sat> <dst>, <src0>, <src1>, <src2>, <op>
#172  class 20       mask F80003C000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#173  class 20       mask F800030000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#174  class 20       mask F80000C000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#175  class 20       mask F800000000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#176  class 21       mask F800008000000000  value A800008000000000  imae
      <pred>imae<flags>.<type><rs> <dst>, <src0>, <src1>, <src2>, <op>
#177  class 21       mask F800008000000000  value A800000000000000  imae
      <pred>imae<flags>.<type><rs> <dst>, <src0>, <src1>, <src2>, <op>
#178  class 26       mask F830000000000000  value D000000000000000  ima32
      <pred>ima32<flags>.<type> <dst><extra1>, <src0>, <src1>, <src2><extra2>
#179  class 26       mask F830000000000000  value D010000000000000  idiv
      <pred>idiv<flags> <dst>, <src1>, <src2>, drc<drc>
```

Mnemonics: `idiv`, `ima16`, `ima32`, `ima8`, `imae`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.rsN`, `.s`, `.sat`, `.skipinv`, `.ssat`, `.u`, `.usat`.

Layout 1 of 12 (form #26):

```
 63   59 58  57 56      55   54 53      52  51    50      49     48
+-------+------+-------+-------+-------+---+-----+-------+------+------+
| 10101 | pred | half1 | flags | half2 | - | dst | flags | src1 | src2 |
+-------+------+-------+-------+-------+---+-----+-------+------+------+
 47  46   44 43  42 41  38 37 35 34     33 32
+---+-------+------+------+-----+------+-----+
| 0 | flags | type | 0010 |  rs | src0 | dst |
+---+-------+------+------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:57. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2], 46:44 → v[5:3]. Flags, printed in this order. Each
  bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.end`, v[5:3] a count, `.repeat2`…`.repeat8`.
- `type` — bits 42 → v[0], 43 → v[1]. Values: 0 `u`, 1 `usat`, 2 `s`, 3 `ssat`.
- `rs` — bits 37:35. Values: 0 (none), 1-7 `.rs1`…`.rs7`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- `half1` — bit 56. Values: 0 `low`, 1 `high`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `half2` — bit 53. Values: 0 `low`, 1 `high`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 12 (form #27):

```
 63   59 58  57 56    55   54 53    52  51    50      49     48
+-------+------+-----+-------+-----+---+-----+-------+------+------+
| 10100 | pred | abs | flags | neg | - | dst | flags | src1 | src2 |
+-------+------+-----+-------+-----+---+-----+-------+------+------+
 47  46   44 43  42 41   37 36 35 34     33 32
+---+-------+------+-------+-----+------+-----+
| 0 | flags | type | 00000 |  rs | src0 | dst |
+---+-------+------+-------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — bits 36:35. Values: 0 (none), 1-3 `.rs1`…`.rs3`.
- `dst` — as in layout 1.
- `abs` — bit 56. Values: 0 (none), 1 `.abs`.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `neg` — bit 53. Values: 0 (none), 1 `.neg`.

Layout 3 of 12 (form #28):

```
 63   59 58  56 55   54 53 52 51    50      49     48
+-------+------+-------+-----+-----+-------+------+------+
| 11010 | pred | flags |  00 | dst | flags | src1 | src2 |
+-------+------+-------+-----+-----+-------+------+------+
 47     46   44 43 42 41     40     39     38  37 35 34     33 32
+------+-------+-----+------+------+------+---+-----+------+-----+
| src0 | flags |  00 | type | neg1 | neg2 | 0 |  -  | src0 | dst |
+------+-------+-----+------+------+------+---+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `flags` — as in layout 1.
- `type` — bit 41. Values: 0 `u`, 1 `s`.
- `dst` — as in layout 1.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 47 → v[8]. A register operand: the standard source 0
  (*Register operands*).
- `src1` — as in layout 1.
- `neg1` — bit 40. Values: 0 (none), 1 `.neg`.
- `src2` — as in layout 1.
- `neg2` — bit 39. Values: 0 (none), 1 `.neg`.

Layout 4 of 12 (form #171):

```
 63   59 58  57 56   55   54 53 52 51    50      49     48
+-------+------+----+-------+-----+-----+-------+------+------+
| 10011 | pred | op | flags |  op | dst | flags | src1 | src2 |
+-------+------+----+-------+-----+-----+-------+------+------+
 47   46   44 43    42 36 35  34 33 32
+----+-------+-----+-----+------+-----+
| op | flags | sat |  op | src0 | dst |
+----+-------+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `sat` — bit 43. Values: 0 (none), 1 `.sat`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `r[il + #0]`…`sa[ih + #7].c10`, 4 `sa0`…`sa63.c10`, 5 `c0`…`g63`, 6
  `i.`…`i.lh`, 7 `i0`…`i63.c10`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 35 → v[8]. A register operand, v = bank·128 + number (the
  bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `pa0`…`pa63.c10`, 2
  `r0.neg`…`i3.neg.c10`, 3 `pa0.neg`…`pa63.neg.c10`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- `op` — bits 53:52 → v[1:0], 42 → v[2], 37 → v[3], 56 → v[4], 36 → v[5], 47 → v[6], 41 → v[7], 38 →
  v[8], 39 → v[9], 40 → v[10]. An operand list (see *Operand encodings*), for example 0 `s0, s1, s2,
  s0a, s1a, s2a`, 1 `s1, s1, s2, s0a, s1a, s2a`, 2 `s0a, s1, s2, s0a, s1a, s2a`.

Layout 5 of 12 (form #172):

```
 63   59 58  57 56    55   54 53     52  51    50      49     48
+-------+------+-----+-------+------+---+-----+-------+------+------+
| 10100 | pred | abs | flags | src2 | - | dst | flags | src1 | src2 |
+-------+------+-----+-------+------+---+-----+-------+------+------+
 47  46   44 43  42 41 38 37  36 35 34     33 32
+---+-------+------+-----+---+-----+------+-----+
| - | flags | type |  op | - |  rs | src0 | dst |
+---+-------+------+-----+---+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — as in layout 2.
- `dst` — as in layout 1.
- `abs` — as in layout 2.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 53 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2
  `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7
  `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`, 11
  `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`.
- `op` — bits 41:38. Values: 0 `u16, u16`. Other values do not decode.

Layout 6 of 12 (form #173):

```
 63   59 58  57 56    55   54 53     52     51    50      49     48
+-------+------+-----+-------+------+------+-----+-------+------+------+
| 10100 | pred | abs | flags | src2 | src1 | dst | flags | src1 | src2 |
+-------+------+-----+-------+------+------+-----+-------+------+------+
 47  46   44 43  42 41 38 37  36 35 34     33 32
+---+-------+------+-----+---+-----+------+-----+
| - | flags | type |  op | - |  rs | src0 | dst |
+---+-------+------+-----+---+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — as in layout 2.
- `dst` — as in layout 1.
- `abs` — as in layout 2.
- `src0` — as in layout 1.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 52 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0.low`…`i3.low`, 1
  `o0.low`…`o127.low`, 2 `pa0.low`…`pa127.low`, 3 `sa0.low`…`sa127.low`, 4 `r[il + #0].low`…`sa[ih +
  #15].low`, 5 `c0.low`…`g63.low`, 6 `#0.low`…`#127.low`, 7 `i0.low`…`i127.low`, 8
  `r0.high`…`i3.high`, 9 `o0.high`…`o127.high`, 10 `pa0.high`…`pa127.high`, 11
  `sa0.high`…`sa127.high`, 12 `r[il + #0].high`…`sa[ih + #15].high`, 13 `c0.high`…`g63.high`, 14
  `#0.high`…`#127.high`, 15 `i0.high`…`i127.high`.
- `src2` — as in layout 5.
- `op` — bits 41:38. Values: 1 `u8, u16`, 2 `s8, u16`, 3 `o8, u16`. Other values do not decode.

Layout 7 of 12 (form #174):

```
 63   59 58  57 56    55   54 53     52  51    50      49     48
+-------+------+-----+-------+------+---+-----+-------+------+------+
| 10100 | pred | abs | flags | src2 | - | dst | flags | src1 | src2 |
+-------+------+-----+-------+------+---+-----+-------+------+------+
 47  46   44 43  42 41 38 37     36 35 34     33 32
+---+-------+------+-----+------+-----+------+-----+
| - | flags | type |  op | src2 |  rs | src0 | dst |
+---+-------+------+-----+------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — as in layout 2.
- `dst` — as in layout 1.
- `abs` — as in layout 2.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 53 → v[10], 37 → v[11]. A register operand,
  v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0.low`…`i3.low`, 1
  `o0.low`…`o127.low`, 2 `pa0.low`…`pa127.low`, 3 `sa0.low`…`sa127.low`, 4 `r[il + #0].low`…`sa[ih +
  #15].low`, 5 `c0.low`…`g63.low`, 6 `#0.low`…`#127.low`, 7 `i0.low`…`i127.low`, 8
  `r0.neg.low`…`i3.neg.low`, 9 `o0.neg.low`…`o127.neg.low`, 10 `pa0.neg.low`…`pa127.neg.low`, 11
  `sa0.neg.low`…`sa127.neg.low`, 12 `r[il + #0].neg.low`…`sa[ih + #15].neg.low`, 13
  `c0.neg.low`…`g63.neg.low`, 14 `#0.neg.low`…`#127.neg.low`, 15 `i0.neg.low`…`i127.neg.low`, 16
  `r0.high`…`i3.high`, 17 `o0.high`…`o127.high`, 18 `pa0.high`…`pa127.high`, 19
  `sa0.high`…`sa127.high`, 20 `r[il + #0].high`…`sa[ih + #15].high`, 21 `c0.high`…`g63.high`, 22
  `#0.high`…`#127.high`, 23 `i0.high`…`i127.high`, 24 `r0.neg.high`…`i3.neg.high`, 25
  `o0.neg.high`…`o127.neg.high`, 26 `pa0.neg.high`…`pa127.neg.high`, 27
  `sa0.neg.high`…`sa127.neg.high`, 28 `r[il + #0].neg.high`…`sa[ih + #15].neg.high`, 29
  `c0.neg.high`…`g63.neg.high`, 30 `#0.neg.high`…`#127.neg.high`, 31 `i0.neg.high`…`i127.neg.high`.
- `op` — bits 41:38. Values: 4 `u16, u8`, 8 `u16, s8`, 12 `u16, o8`. Other values do not decode.

Layout 8 of 12 (form #175):

```
 63   59 58  57 56    55   54 53     52     51    50      49     48
+-------+------+-----+-------+------+------+-----+-------+------+------+
| 10100 | pred | abs | flags | src2 | src1 | dst | flags | src1 | src2 |
+-------+------+-----+-------+------+------+-----+-------+------+------+
 47  46   44 43  42 41 38 37     36 35 34     33 32
+---+-------+------+-----+------+-----+------+-----+
| - | flags | type |  op | src2 |  rs | src0 | dst |
+---+-------+------+-----+------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — as in layout 2.
- `dst` — as in layout 1.
- `abs` — as in layout 2.
- `src0` — as in layout 1.
- `src1` — as in layout 6.
- `src2` — as in layout 7.
- `op` — bits 41:38. Values: 5 `u8, u8`, 6 `s8, u8`, 7 `o8, u8`, 9 `u8, s8`, 10 `s8, s8`, 11 `o8,
  s8`, 13 `u8, o8`, 14 `s8, o8`, 15 `o8, o8`. Other values do not decode.

Layout 9 of 12 (form #176):

```
 63   59 58  57 56     55   54 53     52  51    50      49     48
+-------+------+------+-------+------+---+-----+-------+------+------+
| 10101 | pred | src0 | flags | src1 | - | dst | flags | src1 | src2 |
+-------+------+------+-------+------+---+-----+-------+------+------+
 47   46   44 43  42 41 38 37 35 34     33 32
+----+-------+------+-----+-----+------+-----+
| op | flags | type |  op |  rs | src0 | dst |
+----+-------+------+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — as in layout 1.
- `dst` — as in layout 1.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 56 → v[8]. A register operand, v = bank·128 + number (the
  bank bits may carry modifiers). By bank: 0 `r0.low`…`i3.low`, 1 `pa0.low`…`pa127.low`, 2
  `r0.high`…`i3.high`, 3 `pa0.high`…`pa127.high`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 53 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0.low`…`i3.low`, 1
  `o0.low`…`o127.low`, 2 `pa0.low`…`pa127.low`, 3 `sa0.low`…`sa127.low`, 4 `r[il + #0].low`…`sa[ih +
  #15].low`, 5 `c0.low`…`g63.low`, 6 `#0.low`…`#127.low`, 7 `i0.low`…`i127.low`, 8
  `r0.high`…`i3.high`, 9 `o0.high`…`o127.high`, 10 `pa0.high`…`pa127.high`, 11
  `sa0.high`…`sa127.high`, 12 `r[il + #0].high`…`sa[ih + #15].high`, 13 `c0.high`…`g63.high`, 14
  `#0.high`…`#127.high`, 15 `i0.high`…`i127.high`.
- `src2` — as in layout 1.
- `op` — bits 39:38 → v[1:0], 41:40 → v[3:2], 47 → v[4]. Values: 0 `z16`, 1 `u16`, 2 `u32`, 4 `z16,
  coe, i0`, 5 `u16, coe, i0`, 6 `u32, coe, i0`, 8 `z16, cie, i0`, 9 `u16, cie, i0`, 10 `u32, cie,
  i0`, 12 `z16, coe, cie, i0`, 13 `u16, coe, cie, i0`, 14 `u32, coe, cie, i0`, 16 `z16`, 17 `u16`,
  18 `u32`, 20 `z16, coe, i1`, 21 `u16, coe, i1`, 22 `u32, coe, i1`, 24 `z16, cie, i1`, 25 `u16,
  cie, i1`, 26 `u32, cie, i1`, 28 `z16, coe, cie, i1`, 29 `u16, coe, cie, i1`, 30 `u32, coe, cie,
  i1`. Other values do not decode.

Layout 10 of 12 (form #177):

```
 63   59 58  57 56     55   54 53     52     51    50      49     48
+-------+------+------+-------+------+------+-----+-------+------+------+
| 10101 | pred | src0 | flags | src1 | src2 | dst | flags | src1 | src2 |
+-------+------+------+-------+------+------+-----+-------+------+------+
 47   46   44 43  42 41 38 37 35 34     33 32
+----+-------+------+-----+-----+------+-----+
| op | flags | type |  op |  rs | src0 | dst |
+----+-------+------+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `type` — as in layout 1.
- `rs` — as in layout 1.
- `dst` — as in layout 1.
- `src0` — as in layout 9.
- `src1` — as in layout 9.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 52 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0.low`…`i3.low`, 1
  `o0.low`…`o127.low`, 2 `pa0.low`…`pa127.low`, 3 `sa0.low`…`sa127.low`, 4 `r[il + #0].low`…`sa[ih +
  #15].low`, 5 `c0.low`…`g63.low`, 6 `#0.low`…`#127.low`, 7 `i0.low`…`i127.low`, 8
  `r0.high`…`i3.high`, 9 `o0.high`…`o127.high`, 10 `pa0.high`…`pa127.high`, 11
  `sa0.high`…`sa127.high`, 12 `r[il + #0].high`…`sa[ih + #15].high`, 13 `c0.high`…`g63.high`, 14
  `#0.high`…`#127.high`, 15 `i0.high`…`i127.high`.
- `op` — as in layout 9.

Layout 11 of 12 (form #178):

```
 63   59 58  56 55   54 53 52 51    50      49     48
+-------+------+-------+-----+-----+-------+------+------+
| 11010 | pred | flags |  00 | dst | flags | src1 | src2 |
+-------+------+-------+-----+-----+-------+------+------+
 47     46   44 43    42 41     40     39     38    36 35  34     33 32
+------+-------+--------+------+------+------+--------+---+------+-----+
| src0 | flags | extra2 | type | src1 | src2 | extra1 | - | src0 | dst |
+------+-------+--------+------+------+------+--------+---+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 3.
- `flags` — as in layout 1.
- `type` — as in layout 3.
- `dst` — as in layout 1.
- `extra1` — bits 38 → v[0], 37:36 → v[2:1]. Flags, printed in this order. Each bit adds: v[0] `,
  i0`.
- `src0` — as in layout 3.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 40 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2
  `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7
  `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`, 11
  `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 39 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2
  `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7
  `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`, 11
  `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`.
- `extra2` — bits 43:42. Flags, printed in this order. Each bit adds: v[0] `, cie, i0`, v[1] `, cie,
  i1`.

Layout 12 of 12 (form #179):

```
 63   59 58  56 55   54 53 52 51  50      49     48     47  46   44 43 40 39    38 33 32
+-------+------+-------+-----+---+-------+------+------+---+-------+-----+-----+-----+-----+
| 11010 | pred | flags |  01 | - | flags | src1 | src2 | - | flags |  -  | dst |  -  | drc |
+-------+------+-------+-----+---+-------+------+------+---+-------+-----+-----+-----+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 3.
- `flags` — as in layout 1.
- `dst` — bits 27:21 → v[6:0], 39 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`r127`, 1 `pa0`…`pa127`.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `drc` — bit 32. A 1-bit number, printed in decimal.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 12 of 12 forms have semantics. The
probe vectors that test them: 223 match the hardware, 4 stop unsupported, 0 mismatch (`idiv` 7/0/0,
`ima16` 56/0/0, `ima16-8bit` 8/0/0, `ima16-imm-c` 28/0/0, `ima16-plain` 42/0/0, `ima16-reg-b`
28/0/0, `ima8` 8/0/0, `imae-modes` 14/0/0, `x-misc` 32/4/0); unsupported mostly for flag .repeatN
(1); imaN modifier .rsN (1); imaN.usat (1).

### 6.6 Colour operations: `sop2`, `sop3`, `lrp1`, `lrp2`, `sop2wm`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#164  class 16       mask F800000000000000  value 8000000000000000  +asop2 sop2
      <pred>sop2<flags> <dst>, <src1>, <src2>, <op>+asop2 <sop>
#165  class 17       mask F8200C0000000000  value 8800000000000000  +asop sop3
      <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
#166  class 17       mask F8200C0000000000  value 8800080000000000  +arsop sop3
      <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+arsop <sop>
#167  class 17       mask F820040000000000  value 8800040000000000  +alrp sop3
      <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
#168  class 17       mask F820040000000000  value 8820000000000000  +asop lrp1
      <pred>lrp1<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
#169  class 17       mask F820040000000000  value 8820040000000000  +alrp lrp2
      <pred>lrp2<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
#170  class 18       mask F800000000000000  value 9000000000000000  sop2wm
      <pred>sop2wm<flags> <dst><bytemask>, <src1>, <src2>, <op>
```

Mnemonics: `+alrp`, `+arsop`, `+asop`, `+asop2`, `lrp1`, `lrp2`, `sop2`, `sop2wm`, `sop3`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`.

Layout 1 of 7 (form #164):

```
 63   59 58  57 56   55   54 53 52 51    50      49     48     47   46   44 43 41 40 35 34    33 32
+-------+------+----+-------+-----+-----+-------+------+------+----+-------+-----+-----+-----+-----+
| 10000 | pred | op | flags | sop | dst | flags | src1 | src2 | op | flags | sop |  op | sop | dst |
+-------+------+----+-------+-----+-----+-------+------+------+----+-------+-----+-----+-----+-----+
 31  30 29  28 27 21 20 18 17 14 13   7 6    0
+------+------+-----+-----+-----+------+------+
| src1 | src2 | dst |  op | sop | src1 | src2 |
+------+------+-----+-----+-----+------+------+
```

- `pred` — bits 58:57. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2], 46:44 → v[5:3]. Flags, printed in this order. Each
  bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.end`, v[5:3] a count, `.repeat2`…`.repeat8`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `r[il + #0]`…`sa[ih + #7].c10`, 4 `sa0`…`sa63.c10`, 5 `c0`…`g63`, 6
  `i.`…`i.lh`, 7 `i0`…`i63.c10`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- `op` — bits 20 → v[0], 40:38 → v[3:1], 56 → v[4], 37:35 → v[7:5], 47 → v[8], 19:18 → v[10:9]. An
  operand list (see *Operand encodings*), for example 0 `zero, zero, add`, 1 `comp, zero, zero,
  add`, 2 `s1, zero, add`.
- `sop` — bits 15 → v[0], 53:52 → v[2:1], 43 → v[3], 42:41 → v[5:4], 34 → v[6], 17:16 → v[8:7], 14 →
  v[9]. An operand list (see *Operand encodings*), for example 0 `zero, zero, add`, 1 `comp, zero,
  zero, add`, 2 `s1a, zero, add`.

Layout 2 of 7 (form #165):

```
 63   59 58  57 56   55   54 53  52   51    50      49     48
+-------+------+----+-------+---+----+-----+-------+------+------+
| 10001 | pred | op | flags | 0 | op | dst | flags | src1 | src2 |
+-------+------+----+-------+---+----+-----+-------+------+------+
 47       46 44 43 42 41    40 38 37    35 34     33 32
+--------+-----+-----+-----+-----+--------+------+-----+
| op/sop | sop |  00 | sop |  op | op/sop | src0 | dst |
+--------+-----+-----+-----+-----+--------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2]. Flags, printed in this order. Each bit adds: v[0]
  `.skipinv`, v[1] `.nosched`, v[2] `.end`.
- `dst` — as in layout 1.
- `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `pa0`…`pa63.c10`.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op` — bits 40:38 → v[2:0], 56 → v[3], 37:35 → v[6:4], 47 → v[7], 52 → v[8]. An operand list (see
  *Operand encodings*), for example 0 `zero, zero, add`, 1 `asat, zero, add`, 2 `s1, zero, add`.
- `sop` — bits 45:44 → v[1:0], 46 → v[2], 37:35 → v[5:3], 47 → v[6], 41 → v[7]. An operand list (see
  *Operand encodings*), for example 0 `zero, zero, add`, 1 `s0a, zero, add`, 2 `s1a, zero, add`.

Layout 3 of 7 (form #166):

```
 63   59 58  57 56   55   54 53  52   51    50      49     48
+-------+------+----+-------+---+----+-----+-------+------+------+
| 10001 | pred | op | flags | 0 | op | dst | flags | src1 | src2 |
+-------+------+----+-------+---+----+-----+-------+------+------+
 47   46 44 43 42 41    40 35 34     33 32
+----+-----+-----+-----+-----+------+-----+
| op | sop |  10 | sop |  op | src0 | dst |
+----+-----+-----+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 2.
- `dst` — as in layout 1.
- `src0` — as in layout 2.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op` — as in layout 2.
- `sop` — bits 45:44 → v[1:0], 46 → v[2], 41 → v[3]. Values: 0 `zero, add`, 1-3 `s0a, add`…`s2a,
  add`, 4 `zero.comp, add`, 5-7 `s0a.comp, add`…`s2a.comp, add`, 8 `zero, sub`, 9-11 `s0a,
  sub`…`s2a, sub`, 12 `zero.comp, sub`, 13-15 `s0a.comp, sub`…`s2a.comp, sub`.

Layout 4 of 7 (form #167):

```
 63   59 58  57 56   55   54 53  52   51    50      49     48
+-------+------+----+-------+---+----+-----+-------+------+------+
| 10001 | pred | op | flags | 0 | op | dst | flags | src1 | src2 |
+-------+------+----+-------+---+----+-----+-------+------+------+
 47   46 44 43   42  41    40 35 34     33 32
+----+-----+----+---+-----+-----+------+-----+
| op | sop | op | 1 | sop |  op | src0 | dst |
+----+-----+----+---+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 2.
- `dst` — as in layout 1.
- `src0` — as in layout 2.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op` — bits 40:38 → v[2:0], 56 → v[3], 37:35 → v[6:4], 47 → v[7], 52 → v[8], 43 → v[9]. An operand
  list (see *Operand encodings*), for example 0 `zero, zero, add`, 1 `asat, zero, add`, 2 `s1, zero,
  add`.
- `sop` — bits 45 → v[0], 41 → v[1], 44 → v[2], 46 → v[3]. Values: 0 `zero, zero`, 1 `s1a, zero`, 2
  `zero.comp, zero`, 3 `s1a.comp, zero`, 4 `zero, s2a`, 5 `s1a, s2a`, 6 `zero.comp, s2a`, 7
  `s1a.comp, s2a`, 8 `zero, zero.comp`, 9 `s1a, zero.comp`, 10 `zero.comp, zero.comp`, 11 `s1a.comp,
  zero.comp`, 12 `zero, s2a.comp`, 13 `s1a, s2a.comp`, 14 `zero.comp, s2a.comp`, 15 `s1a.comp,
  s2a.comp`.

Layout 5 of 7 (form #168):

```
 63   59 58  57 56    55   54 53  52   51    50      49     48
+-------+------+-----+-------+---+----+-----+-------+------+------+
| 10001 | pred | sop | flags | 1 | op | dst | flags | src1 | src2 |
+-------+------+-----+-------+---+----+-----+-------+------+------+
 47    46 43 42  41 35 34     33 32
+-----+-----+---+-----+------+-----+
| sop |  op | 0 | sop | src0 | dst |
+-----+-----+---+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 2.
- `dst` — as in layout 1.
- `src0` — as in layout 2.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op` — bits 45 → v[0], 52 → v[1], 44 → v[2], 46 → v[3], 43 → v[4]. Values: 0 `zero, zero, s0`, 1
  `s1, zero, s0`, 2 `zero.comp, zero, s0`, 3 `s1.comp, zero, s0`, 4 `zero, s2, s0`, 5 `s1, s2, s0`,
  6 `zero.comp, s2, s0`, 7 `s1.comp, s2, s0`, 8 `zero, zero.comp, s0`, 9 `s1, zero.comp, s0`, 10
  `zero.comp, zero.comp, s0`, 11 `s1.comp, zero.comp, s0`, 12 `zero, s2.comp, s0`, 13 `s1, s2.comp,
  s0`, 14 `zero.comp, s2.comp, s0`, 15 `s1.comp, s2.comp, s0`, 16 `zero, zero, s0a`, 17 `s1, zero,
  s0a`, 18 `zero.comp, zero, s0a`, 19 `s1.comp, zero, s0a`, 20 `zero, s2, s0a`, 21 `s1, s2, s0a`, 22
  `zero.comp, s2, s0a`, 23 `s1.comp, s2, s0a`, 24 `zero, zero.comp, s0a`, 25 `s1, zero.comp, s0a`,
  26 `zero.comp, zero.comp, s0a`, 27 `s1.comp, zero.comp, s0a`, 28 `zero, s2.comp, s0a`, 29 `s1,
  s2.comp, s0a`, 30 `zero.comp, s2.comp, s0a`, 31 `s1.comp, s2.comp, s0a`.
- `sop` — bits 40:38 → v[2:0], 56 → v[3], 37:35 → v[6:4], 47 → v[7], 41 → v[8]. An operand list (see
  *Operand encodings*), for example 0 `zero, zero, add`, 1 `asat, zero, add`, 2 `s1, zero, add`.

Layout 6 of 7 (form #169):

```
 63   59 58  57 56   55   54 53  52   51    50      49     48
+-------+------+----+-------+---+----+-----+-------+------+------+
| 10001 | pred | op | flags | 1 | op | dst | flags | src1 | src2 |
+-------+------+----+-------+---+----+-----+-------+------+------+
 47   46 44 43   42  41    40 35 34     33 32
+----+-----+----+---+-----+-----+------+-----+
| op | sop | op | 1 | sop |  op | src0 | dst |
+----+-----+----+---+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 2.
- `dst` — as in layout 1.
- `src0` — as in layout 2.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op` — bits 40:38 → v[2:0], 56 → v[3], 37:35 → v[6:4], 47 → v[7], 43 → v[8], 52 → v[9]. An operand
  list (see *Operand encodings*), for example 0 `zero, zero, s1, s2`, 1 `asat, zero, s1, s2`, 2 `s1,
  zero, s1, s2`.
- `sop` — as in layout 4.

Layout 7 of 7 (form #170):

```
 63   59 58  57 56   55   54 53 52 51    50      49     48     47   46      43 42 35 34  33 32
+-------+------+----+-------+-----+-----+-------+------+------+----+----------+-----+---+-----+
| 10010 | pred | op | flags |  op | dst | flags | src1 | src2 | op | bytemask |  op | - | dst |
+-------+------+----+-------+-----+-----+-------+------+------+----+----------+-----+---+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 2.
- `dst` — as in layout 1.
- `bytemask` — bits 46:44 → v[2:0], 43 → v[3]. Values: 0 `.bytemask0000`, 1 `.bytemask0001`, 2
  `.bytemask0010`, 3 `.bytemask0011`, 4 `.bytemask0100`, 5 `.bytemask0101`, 6 `.bytemask0110`, 7
  `.bytemask0111`, 8 `.bytemask1000`, 9 `.bytemask1001`, 10 `.bytemask1010`, 11 `.bytemask1011`, 12
  `.bytemask1100`, 13 `.bytemask1101`, 14 `.bytemask1110`, 15 `.bytemask1111`.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op` — bits 40:38 → v[2:0], 56 → v[3], 37:35 → v[6:4], 47 → v[7], 53:52 → v[9:8], 42:41 →
  v[11:10]. An operand list (see *Operand encodings*), for example 0 `zero, zero, add, add`, 1
  `asat, zero, add, add`, 2 `s1, zero, add, add`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 7 forms have semantics. The
probe vectors that test them: 5372 match the hardware, 0 stop unsupported, 0 mismatch (`colour-c10`
176/0/0, `colour-c10-other` 9/0/0, `colour-dot-repeats` 30/0/0, `sop2-ops` 504/0/0, `sop2-random`
288/0/0, `sop2-selectors` 1056/0/0, `sop2wm` 440/0/0, `sop2wm-bytemask` 5/0/0, `sop3-lrp` 1672/0/0,
`sop3-random` 176/0/0, `x-arsop` 72/0/0, `x-c10` 24/0/0, `x-misc` 7/0/0, `x-rep` 8/0/0, `x-sop2`
174/0/0, `x-sop2r` 60/0/0, `x-sop2wm` 168/0/0, `x-sop2wmr` 32/0/0, `x-sop3` 471/0/0).

### 6.7 Integer dot products: `u8dot3`, `u8dot4`, `u16dot3`, `u16dot4`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#180  class 24       mask F900000000000000  value C100000000000000  u16dot4 u16dot4off u8dot4
      u8dot4off
      <pred><op1>dot4<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
#181  class 24       mask F900060000000000  value C000060000000000  u16dot3 u16dot3off u8dot3
      u8dot3off
      <pred><op1>dot3<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
#182  class 24       mask F930000000000000  value C030000000000000  u16dot3 u16dot3off
      <pred>u16dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2><mode>
#183  class 24       mask F900002000000000  value C000000000000000  +aadd +asub u8dot3 u8dot3off
      <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> Ax<sax>, <sop2>
#184  class 24       mask F900002000000000  value C000002000000000  +aintrp1 +aintrp2 u8dot3
      u8dot3off
      <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> <ssrc0>, <sop2>
```

Mnemonics: `+aadd`, `+aintrp1`, `+aintrp2`, `+asub`, `u16dot3`, `u16dot3off`, `u16dot4`,
`u16dot4off`, `u8dot3`, `u8dot3off`, `u8dot4`, `u8dot4off`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`.

Layout 1 of 5 (form #180):

```
 63   59 58  57 56  55   54 53 52 51    50      49     48
+-------+------+---+-------+-----+-----+-------+------+------+
| 11000 | pred | 1 | flags | op1 | dst | flags | src1 | src2 |
+-------+------+---+-------+-----+-----+-------+------+------+
 47     46   44 43     42 41 40 39 38    37 34 33 32
+------+-------+------+-----+-----+-----+-----+-----+
| src2 | flags | src1 |  -  |  cx | op2 |  -  | dst |
+------+-------+------+-----+-----+-----+-----+-----+
 31  30 29  28 27 21 20 18 17 16 15 14 13   7 6    0
+------+------+-----+-----+-----+-----+------+------+
| src1 | src2 | dst |  -  |  ax |  -  | src1 | src2 |
+------+------+-----+-----+-----+-----+------+------+
```

- `pred` — bits 58:57. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `op1` — bits 53:52. Values: 0 `u8`, 1 `u8`, 2 `u8`, 3 `u16`.
- `op2` — bit 38. Values: 0 (none), 1 `off`.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2], 46:44 → v[5:3]. Flags, printed in this order. Each
  bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.end`, v[5:3] a count, `.repeat2`…`.repeat8`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `r[il + #0]`…`sa[ih + #7].c10`, 4 `sa0`…`sa63.c10`, 5 `c0`…`g63`, 6
  `i.`…`i.lh`, 7 `i0`…`i63.c10`.
- `cx` — bits 40:39. Values: 0 `1`, 1 `2`, 2 `4`, 3 `8`.
- `ax` — bits 17:16. Values: 0 `1`, 1 `2`, 2 `4`, 3 `8`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 43 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`,
  2 `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`, 8 `r0.comp`…`i3.comp.c10`, 9 `o0.comp`…`o63.comp.c10`, 10
  `pa0.comp`…`pa63.comp.c10`, 11 `sa0.comp`…`sa63.comp.c10`, 12 `r[il + #0].comp`…`sa[ih +
  #7].comp.c10`, 13 `c0.comp`…`g63.comp`, 14 `#0.comp`…`#127.comp`, 15 `i0.comp`…`i63.comp.c10`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 47 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`,
  2 `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`, 8 `r0.comp`…`i3.comp.c10`, 9 `o0.comp`…`o63.comp.c10`, 10
  `pa0.comp`…`pa63.comp.c10`, 11 `sa0.comp`…`sa63.comp.c10`, 12 `r[il + #0].comp`…`sa[ih +
  #7].comp.c10`, 13 `c0.comp`…`g63.comp`, 14 `#0.comp`…`#127.comp`, 15 `i0.comp`…`i63.comp.c10`.

Layout 2 of 5 (form #181):

```
 63   59 58  57 56  55   54 53 52 51    50      49     48
+-------+------+---+-------+-----+-----+-------+------+------+
| 11000 | pred | 0 | flags | op1 | dst | flags | src1 | src2 |
+-------+------+---+-------+-----+-----+-------+------+------+
 47     46   44 43     42 41 40 39 38    37 34 33 32
+------+-------+------+-----+-----+-----+-----+-----+
| src2 | flags | src1 |  11 |  cx | op2 |  -  | dst |
+------+-------+------+-----+-----+-----+-----+-----+
 31  30 29  28 27 21 20 18 17 16 15 14 13   7 6    0
+------+------+-----+-----+-----+-----+------+------+
| src1 | src2 | dst |  -  |  ax |  -  | src1 | src2 |
+------+------+-----+-----+-----+-----+------+------+
```

- `pred` — as in layout 1.
- `op1` — as in layout 1.
- `op2` — as in layout 1.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `cx` — as in layout 1.
- `ax` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.

Layout 3 of 5 (form #182):

```
 63   59 58  57 56  55   54 53 52 51    50      49     48
+-------+------+---+-------+-----+-----+-------+------+------+
| 11000 | pred | 0 | flags |  11 | dst | flags | src1 | src2 |
+-------+------+---+-------+-----+-----+-------+------+------+
 47     46   44 43     42  41 40 39 38   37 34 33 32
+------+-------+------+------+-----+----+-----+-----+
| src2 | flags | src1 | mode |  cx | op |  -  | dst |
+------+-------+------+------+-----+----+-----+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 1.
- `op` — bit 38. Values: 0 (none), 1 `off`.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `cx` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `mode` — bits 42:41. Values: 0 (none), 1 (none), 2 (none). Other values do not decode.

Layout 4 of 5 (form #183):

```
 63   59 58  57 56  55   54 53  52 51    50      49     48
+-------+------+---+-------+------+-----+-------+------+------+
| 11000 | pred | 0 | flags | sop2 | dst | flags | src1 | src2 |
+-------+------+---+-------+------+-----+-------+------+------+
 47     46   44 43     42  41 40 39 38   37  36     35  34 33 32
+------+-------+------+------+-----+----+---+------+------+-----+
| src2 | flags | src1 | sop2 |  cx | op | 0 | sop1 | sop2 | dst |
+------+-------+------+------+-----+----+---+------+------+-----+
 31  30 29  28 27 21 20 18 17 16 15     14  13   7 6    0
+------+------+-----+-----+-----+------+---+------+------+
| src1 | src2 | dst |  -  | sax | sop2 | - | src1 | src2 |
+------+------+-----+-----+-----+------+---+------+------+
```

- `pred` — as in layout 1.
- `op` — as in layout 3.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `cx` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `sop1` — bit 36. Values: 0 `aadd`, 1 `asub`.
- `sax` — bits 17:16. Values: 0 `1`, 1 `2`, 2 `4`, 3 `8`.
- `sop2` — bits 53:52 → v[1:0], 34 → v[2], 15 → v[3], 42:41 → v[5:4], 35 → v[6]. An operand list
  (see *Operand encodings*), for example 0 `zero, s1a, zero, s2a`, 1 `s1a, s1a, zero, s2a`, 2 `s2a,
  s1a, zero, s2a`.

Layout 5 of 5 (form #184):

```
 63   59 58  57 56  55   54 53  52 51    50      49     48
+-------+------+---+-------+------+-----+-------+------+------+
| 11000 | pred | 0 | flags | sop2 | dst | flags | src1 | src2 |
+-------+------+---+-------+------+-----+-------+------+------+
 47     46   44 43     42  41 40 39 38   37  36     35     34      33 32
+------+-------+------+------+-----+----+---+------+------+-------+-----+
| src2 | flags | src1 | sop2 |  cx | op | 1 | sop1 | sop2 | ssrc0 | dst |
+------+-------+------+------+-----+----+---+------+------+-------+-----+
 31  30 29  28 27 21 20   14 13   7 6    0
+------+------+-----+-------+------+------+
| src1 | src2 | dst | ssrc0 | src1 | src2 |
+------+------+-----+-------+------+------+
```

- `pred` — as in layout 1.
- `op` — as in layout 3.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `cx` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `sop1` — bit 36. Values: 0 `aintrp1`, 1 `aintrp2`.
- `ssrc0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `pa0`…`pa63.c10`.
- `sop2` — bits 53:52 → v[1:0], 42:41 → v[3:2], 35 → v[4]. Values: 0 `zero, zero`, 1 `s1a, zero`, 2
  `s2a, zero`, 4 `zero, s1a`, 5 `s1a, s1a`, 6 `s2a, s1a`, 8 `zero, s2a`, 9 `s1a, s2a`, 10 `s2a,
  s2a`, 16 `zero, zero.comp`, 17 `s1a, zero.comp`, 18 `s2a, zero.comp`, 20 `zero, s1a.comp`, 21
  `s1a, s1a.comp`, 22 `s2a, s1a.comp`, 24 `zero, s2a.comp`, 25 `s1a, s2a.comp`, 26 `s2a, s2a.comp`.
  Other values do not decode.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 5 of 5 forms have semantics. The
probe vectors that test them: 1485 match the hardware, 0 stop unsupported, 0 mismatch
(`colour-dot-repeats` 45/0/0, `intdot-alpha` 352/0/0, `intdot-lanes` 792/0/0, `x-aintrp` 8/0/0,
`x-dotcomp` 4/0/0, `x-idot` 272/0/0, `x-rep` 12/0/0).

### 6.8 `rlp`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#185  class 14       mask F820000000000000  value 7000000000000000  rlp
      <pred>rlp<flags> <dst><wmask>, <src1>, p<p>
#186  class 14       mask F820000000000000  value 7020000000000000  rlp
      <pred>rlp<flags><repeat> <dst>, <src1>, p<p>
```

Mnemonics: `rlp`.

Modifiers: `.end`, `.nosched`, `.partial`, `.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 2 (form #185):

```
 63   59 58  56 55   54 53  52      51    50      49     48  47   44 43 35 34      33 32
+-------+------+-------+---+-------+-----+-------+------+---+-------+-----+-------+-----+
| 01110 | pred | flags | 0 | flags | dst | flags | src1 | - | wmask |  -  | flags | dst |
+-------+------+-------+---+-------+-----+-------+------+---+-------+-----+-------+-----+
 31  30 29 28 27 21 20 14 13   7 6 0
+------+-----+-----+-----+------+---+
| src1 |  -  | dst |  p  | src1 | - |
+------+-----+-----+-----+------+---+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `flags` — bits 55 → v[0], 52 → v[1], 54 → v[2], 50 → v[3], 34 → v[4]. Flags, printed in this
  order. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`, v[4]
  `.partial`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5 `.xz`, 6 `.yz`, 7
  `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15 `.xyzw`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `p` — bits 20:14. A 7-bit number, printed in decimal.

Layout 2 of 2 (form #186):

```
 63   59 58  56 55   54 53  52      51    50      49     48  47    44 43 35 34      33 32
+-------+------+-------+---+-------+-----+-------+------+---+--------+-----+-------+-----+
| 01110 | pred | flags | 1 | flags | dst | flags | src1 | - | repeat |  -  | flags | dst |
+-------+------+-------+---+-------+-----+-------+------+---+--------+-----+-------+-----+
 31  30 29 28 27 21 20 14 13   7 6 0
+------+-----+-----+-----+------+---+
| src1 |  -  | dst |  p  | src1 | - |
+------+-----+-----+-----+------+---+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `repeat` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `dst` — as in layout 1.
- `src1` — as in layout 1.
- `p` — as in layout 1.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 2 forms have semantics. The
probe vectors that test them: 287 match the hardware, 0 stop unsupported, 0 mismatch (`rlp` 160/0/0,
`rlp-high-predicates` 15/0/0, `x-rlp` 112/0/0).

### 6.9 Pack and unpack: `pck*`, `unpck*`, `statomic`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#160  class 8        mask F820000000000000  value 4000000000000000  pckc10f16 pckc10f32 pckc10s16
      pckc10u16 pckf16f16 pckf16f32 pckf16s16 pckf16u16 pcko8f16 pcko8f32 pcko8s16 pcko8u16
      pcks16f16 pcks16f32 pcks16s16 pcks16u16 pcks8f16 pcks8f32 pcks8s16 pcks8u16 pcku16f16
      pcku16f32 pcku16s16 pcku16u16 pcku8f16 pcku8f32 pcku8s16 pcku8u16 unpckc10c10 unpckf16c10
      unpckf16o8 unpckf16s8 unpckf16u8 unpckf32c10 unpckf32f16 unpckf32f32 unpckf32o8 unpckf32s16
      unpckf32s8 unpckf32u16 unpckf32u8 unpcks16c10 unpcks16o8 unpcks16s8 unpcks16u8 unpcku16c10
      unpcku16o8 unpcku16s8 unpcku16u8 unpcku8s8 unpcku8u8
      <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
#161  class 8        mask F820000000000000  value 4020000000000000  pckc10f16 pckc10f32 pckc10s16
      pckc10u16 pckf16f16 pckf16f32 pckf16s16 pckf16u16 pcko8f16 pcko8f32 pcko8s16 pcko8u16
      pcks16f16 pcks16f32 pcks16s16 pcks16u16 pcks8f16 pcks8f32 pcks8s16 pcks8u16 pcku16f16
      pcku16f32 pcku16s16 pcku16u16 pcku8f16 pcku8f32 pcku8s16 pcku8u16 unpckc10c10 unpckf16c10
      unpckf16o8 unpckf16s8 unpckf16u8 unpckf32c10 unpckf32f16 unpckf32f32 unpckf32o8 unpckf32s16
      unpckf32s8 unpckf32u16 unpckf32u8 unpcks16c10 unpcks16o8 unpcks16s8 unpcks16u8 unpcku16c10
      unpcku16o8 unpcku16s8 unpcku16u8 unpcku8s8 unpcku8u8
      <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
#162  class 8        mask F820000000000000  value 4000000000000000  statomic
      <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
#163  class 8        mask F820000000000000  value 4020000000000000  statomic
      <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
```

Mnemonics: `pckc10f16`, `pckc10f32`, `pckc10s16`, `pckc10u16`, `pckf16f16`, `pckf16f32`,
`pckf16s16`, `pckf16u16`, `pcko8f16`, `pcko8f32`, `pcko8s16`, `pcko8u16`, `pcks16f16`, `pcks16f32`,
`pcks16s16`, `pcks16u16`, `pcks8f16`, `pcks8f32`, `pcks8s16`, `pcks8u16`, `pcku16f16`, `pcku16f32`,
`pcku16s16`, `pcku16u16`, `pcku8f16`, `pcku8f32`, `pcku8s16`, `pcku8u16`, `statomic`, `unpckc10c10`,
`unpckf16c10`, `unpckf16o8`, `unpckf16s8`, `unpckf16u8`, `unpckf32c10`, `unpckf32f16`,
`unpckf32f32`, `unpckf32o8`, `unpckf32s16`, `unpckf32s8`, `unpckf32u16`, `unpckf32u8`,
`unpcks16c10`, `unpcks16o8`, `unpcks16s8`, `unpcks16u8`, `unpcku16c10`, `unpcku16o8`, `unpcku16s8`,
`unpcku16u8`, `unpcku8s8`, `unpcku8u8`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.scale`, `.skipinv`, `.syncs`.

Layout (forms #160, #161, #162, #163):

```
 63   59 58  56 55 54 53  52    51    50    49     48     47   44 43 38 37      34 33 32
+-------+------+-----+---+-----+-----+-----+------+------+-------+-----+----------+-----+
| 01000 | pred | op1 | ? | op1 | dst | op1 | src1 | src2 | wmask | op1 | bytemask | dst |
+-------+------+-----+---+-----+-----+-----+------+------+-------+-----+----------+-----+
 31  30 29  28 27 21 20 19 18    17  16 15  14 13   7 6    0
+------+------+-----+-----+-----+------+------+------+------+
| src1 | src2 | dst | op2 | op1 | src1 | src2 | src1 | src2 |
+------+------+-----+-----+-----+------+------+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- (forms #160, #161) `op1` — bits 43:38 → v[5:0], 55 → v[6], 52 → v[7], 54 → v[8], 50 → v[9], 18 →
  v[10]. 246 or more distinct texts, for example 0 `unpcku8u8`, 1 `unpcku8s8`, 3 `unpcku16u8`, 4
  `unpcks16u8`, 5 `unpckf16u8`, 6 `unpckf32u8`.
- (forms #162, #163) `op1` — bits 43:38 → v[5:0], 55 → v[6], 52 → v[7], 54 → v[8], 50 → v[9], 18 →
  v[10]. 4 or more distinct texts, for example 8 `statomic`, 72 `statomic.skipinv`, 136
  `statomic.syncs`, 200 `statomic.skipinv.syncs`.
- (forms #160, #162) `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5
  `.xz`, 6 `.yz`, 7 `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15
  `.xyzw`.
- (forms #161, #163) `wmask` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `bytemask` — bits 37:34. Values: 0 `.bytemask0000`, 1 `.bytemask0001`, 2 `.bytemask0010`, 3
  `.bytemask0011`, 4 `.bytemask0100`, 5 `.bytemask0101`, 6 `.bytemask0110`, 7 `.bytemask0111`, 8
  `.bytemask1000`, 9 `.bytemask1001`, 10 `.bytemask1010`, 11 `.bytemask1011`, 12 `.bytemask1100`, 13
  `.bytemask1101`, 14 `.bytemask1110`, 15 (none).
- (forms #160, #161) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 17:16 → v[11:10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0.0`…`i3.0`, 1 `o0.0`…`o127.0`, 2 `pa0.0`…`pa127.0`, 3 `sa0.0`…`sa127.0`, 4 `r[il +
  #0].0`…`sa[ih + #15].0`, 5 `c0.0`…`g63.0`, 6 `#0.0`…`#127.0`, 7 `i0.0`…`i127.0`, 8 `r0.1`…`i3.1`,
  9 `o0.1`…`o127.1`, 10 `pa0.1`…`pa127.1`, 11 `sa0.1`…`sa127.1`, 12 `r[il + #0].1`…`sa[ih + #15].1`,
  13 `c0.1`…`g63.1`, 14 `#0.1`…`#127.1`, 15 `i0.1`…`i127.1`, 16 `r0.2`…`i3.2`, 17 `o0.2`…`o127.2`,
  18 `pa0.2`…`pa127.2`, 19 `sa0.2`…`sa127.2`, 20 `r[il + #0].2`…`sa[ih + #15].2`, 21 `c0.2`…`g63.2`,
  22 `#0.2`…`#127.2`, 23 `i0.2`…`i127.2`, 24 `r0.3`…`i3.3`, 25 `o0.3`…`o127.3`, 26
  `pa0.3`…`pa127.3`, 27 `sa0.3`…`sa127.3`, 28 `r[il + #0].3`…`sa[ih + #15].3`, 29 `c0.3`…`g63.3`, 30
  `#0.3`…`#127.3`, 31 `i0.3`…`i127.3`.
- (forms #162, #163) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 17:16 → v[11:10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.1`…`i3.1`, 9 `o0.1`…`o127.1`, 10 `pa0.1`…`pa127.1`, 11
  `sa0.1`…`sa127.1`, 12 `r[il + #0].1`…`sa[ih + #15].1`, 13 `c0.1`…`g63.1`, 14 `#0.1`…`#127.1`, 15
  `i0.1`…`i127.1`, 16 `r0.2`…`i3.2`, 17 `o0.2`…`o127.2`, 18 `pa0.2`…`pa127.2`, 19 `sa0.2`…`sa127.2`,
  20 `r[il + #0].2`…`sa[ih + #15].2`, 21 `c0.2`…`g63.2`, 22 `#0.2`…`#127.2`, 23 `i0.2`…`i127.2`, 24
  `r0.3`…`i3.3`, 25 `o0.3`…`o127.3`, 26 `pa0.3`…`pa127.3`, 27 `sa0.3`…`sa127.3`, 28 `r[il +
  #0].3`…`sa[ih + #15].3`, 29 `c0.3`…`g63.3`, 30 `#0.3`…`#127.3`, 31 `i0.3`…`i127.3`.
- (forms #160, #161) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 15:14 → v[11:10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0.0`…`i3.0`, 1 `o0.0`…`o127.0`, 2 `pa0.0`…`pa127.0`, 3 `sa0.0`…`sa127.0`, 4 `r[il +
  #0].0`…`sa[ih + #15].0`, 5 `c0.0`…`g63.0`, 6 `#0.0`…`#127.0`, 7 `i0.0`…`i127.0`, 8 `r0.1`…`i3.1`,
  9 `o0.1`…`o127.1`, 10 `pa0.1`…`pa127.1`, 11 `sa0.1`…`sa127.1`, 12 `r[il + #0].1`…`sa[ih + #15].1`,
  13 `c0.1`…`g63.1`, 14 `#0.1`…`#127.1`, 15 `i0.1`…`i127.1`, 16 `r0.2`…`i3.2`, 17 `o0.2`…`o127.2`,
  18 `pa0.2`…`pa127.2`, 19 `sa0.2`…`sa127.2`, 20 `r[il + #0].2`…`sa[ih + #15].2`, 21 `c0.2`…`g63.2`,
  22 `#0.2`…`#127.2`, 23 `i0.2`…`i127.2`, 24 `r0.3`…`i3.3`, 25 `o0.3`…`o127.3`, 26
  `pa0.3`…`pa127.3`, 27 `sa0.3`…`sa127.3`, 28 `r[il + #0].3`…`sa[ih + #15].3`, 29 `c0.3`…`g63.3`, 30
  `#0.3`…`#127.3`, 31 `i0.3`…`i127.3`.
- (forms #162, #163) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 15:14 → v[11:10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.1`…`i3.1`, 9 `o0.1`…`o127.1`, 10 `pa0.1`…`pa127.1`, 11
  `sa0.1`…`sa127.1`, 12 `r[il + #0].1`…`sa[ih + #15].1`, 13 `c0.1`…`g63.1`, 14 `#0.1`…`#127.1`, 15
  `i0.1`…`i127.1`, 16 `r0.2`…`i3.2`, 17 `o0.2`…`o127.2`, 18 `pa0.2`…`pa127.2`, 19 `sa0.2`…`sa127.2`,
  20 `r[il + #0].2`…`sa[ih + #15].2`, 21 `c0.2`…`g63.2`, 22 `#0.2`…`#127.2`, 23 `i0.2`…`i127.2`, 24
  `r0.3`…`i3.3`, 25 `o0.3`…`o127.3`, 26 `pa0.3`…`pa127.3`, 27 `sa0.3`…`sa127.3`, 28 `r[il +
  #0].3`…`sa[ih + #15].3`, 29 `c0.3`…`g63.3`, 30 `#0.3`…`#127.3`, 31 `i0.3`…`i127.3`.
- `op2` — bits 20:19. Values: 0 `nearest`, 1 `down`, 2 `up`, 3 `zero`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 4 of 4 forms have semantics. The
probe vectors that test them: 4348 match the hardware, 0 stop unsupported, 0 mismatch (`pack-c10`
140/0/0, `pack-f16` 140/0/0, `pack-o8-int` 98/0/0, `pack-o8-scale` 90/0/0, `pack-round-down` 70/0/0,
`pack-round-nearest` 70/0/0, `pack-round-scale` 56/0/0, `pack-round-up` 70/0/0, `pack-round-zero`
70/0/0, `pack-s16-int` 98/0/0, `pack-s16-scale` 90/0/0, `pack-s8-int` 98/0/0, `pack-s8-scale`
90/0/0, `pack-u16-bytemask` 224/0/0, `pack-u16-int` 98/0/0, `pack-u16-scale` 90/0/0,
`pack-u8-bytemask` 224/0/0, `pack-u8-int` 98/0/0, `pack-u8-scale` 90/0/0, `pack-u8-scale-sweep`
84/0/0, `statomic` 264/0/0, `unpack-16-scale` 58/0/0, `unpack-c10` 14/0/0, `unpack-c10-16` 140/0/0,
`unpack-c10-int` 88/0/0, `unpack-f16` 14/0/0, `unpack-o8-scale` 448/0/0, `unpack-s16s8` 14/0/0,
`unpack-s16u8` 14/0/0, `unpack-s8-scale` 448/0/0, `unpack-select` 14/0/0, `unpack-u16s8` 14/0/0,
`unpack-u16u8` 14/0/0, `unpack-u8-scale` 448/0/0, `unpack-u8s8` 14/0/0, `wmask-other` 28/0/0,
`x-c10u` 192/0/0, `x-statomic` 18/0/0, `x-unpc10` 16/0/0).

### 6.10 Float multiply-add: `fmad`, `fadm`, `fmsa`, `fsubflr`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#39   class 0        mask F840000000000000  value 0000000000000000  fadm fmad fmsa
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
#40   class 0        mask F840060000000000  value 0000060000000000  fsubflr
      <pred>fsubflr<flags><repeat><incs> <dst>, <src1>, <src2>
#41   class 0        mask F840000000000000  value 0040000000000000  fadm fmad fmsa
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
#42   class 0        mask F840060000000000  value 0040060000000000  fsubflr
      <pred>fsubflr<flags><repeat><incs> <dst>, <src1>, <src2>
```

Mnemonics: `fadm`, `fmad`, `fmsa`, `fsubflr`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 2 (forms #39, #41):

```
 63   59 58  56 55      54  53     52      51    50      49     48
+-------+------+-------+---+------+-------+-----+-------+------+------+
| 00000 | pred | flags | ? | incs | flags | dst | flags | src1 | src2 |
+-------+------+-------+---+------+-------+-----+-------+------+------+
 47  46 45    44 43      42 41 40     39     38  37 36  35 34     33 32
+------+--------+-------+-----+------+------+------+------+------+-----+
| incs | repeat | flags |  op | incs | src0 | src1 | src2 | src0 | dst |
+------+--------+-------+-----+------+------+------+------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 42:41. Values: 0 `fmad`, 1 `fadm`, 2 `fmsa`. Other values do not decode.
- `flags` — bits 55 → v[0], 52 → v[1], 43 → v[2], 50 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`.
- `repeat` — bits 45:44. Values: 0 (none), 1-3 `.repeat2`…`.repeat4`.
- `incs` — bits 53 → v[0], 40 → v[1], 47:46 → v[3:2]. Flags, printed in this order. Value 0 prints
  `.incs000`. Each bit adds: v[0] drops `.incs000`, v[1] `.incs001`, v[2] `.incs010`, v[3]
  `.incs100`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- (form #39) `src0` — bits 20:14 → v[6:0], 34 → v[7], 39 → v[8]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`, 2
  `r0.abs`…`i3.abs`, 3 `pa0.abs`…`pa127.abs`.
- (form #41) `src0` — bits 20:14 → v[6:0], 34 → v[7], 39 → v[8]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1 `pa0`…`pa31.flt16.2`,
  2 `r0.abs`…`i1.abs.flt16.2`, 3 `pa0.abs`…`pa31.abs.flt16.2`.
- (form #39) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #41) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.
- (form #39) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #41) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.

Layout 2 of 2 (forms #40, #42):

```
 63   59 58  56 55      54  53     52      51    50      49     48
+-------+------+-------+---+------+-------+-----+-------+------+------+
| 00000 | pred | flags | ? | incs | flags | dst | flags | src1 | src2 |
+-------+------+-------+---+------+-------+-----+-------+------+------+
 47  46 45    44 43      42 41 40     39  38  37 36  35 34  33 32
+------+--------+-------+-----+------+---+------+------+---+-----+
| incs | repeat | flags |  11 | incs | - | src1 | src2 | - | dst |
+------+--------+-------+-----+------+---+------+------+---+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `repeat` — as in layout 1.
- `incs` — as in layout 1.
- `dst` — as in layout 1.
- (form #40) `src1` — as in layout 1.
- (form #42) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16 `r0.abs`…`i1.abs.flt16.2`, 17
  `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19 `sa0.abs`…`sa31.abs.flt16.2`, 20
  `r[il + #0].abs`…`sa[ih + #7].abs.flt16`, 21 `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23
  `i0.abs`…`i31.abs.flt16.2`, 24 `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25
  `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26 `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27
  `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il + #0].neg.abs`…`sa[ih + #7].neg.abs.flt16`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.
- (form #40) `src2` — as in layout 1.
- (form #42) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16 `r0.abs`…`i1.abs.flt16.2`, 17
  `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19 `sa0.abs`…`sa31.abs.flt16.2`, 20
  `r[il + #0].abs`…`sa[ih + #7].abs.flt16`, 21 `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23
  `i0.abs`…`i31.abs.flt16.2`, 24 `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25
  `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26 `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27
  `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il + #0].neg.abs`…`sa[ih + #7].neg.abs.flt16`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 4 forms have semantics (not
yet: #41, #42). The probe vectors that test them: 1403 match the hardware, 0 stop unsupported, 0
mismatch (`fadm-fmsa-fsubflr` 297/0/0, `fadm-fmsa-fsubflr-specials` 36/0/0, `fmad-denormal` 42/0/0,
`fmad-fused` 14/0/0, `fmad-move` 14/0/0, `fmad-round` 42/0/0, `fmad-zero-sign` 84/0/0, `incs-fmad`
126/0/0, `sweep-fmad` 672/0/0, `x-dual3` 4/0/0, `x-fadm` 48/0/0, `x-fspec` 6/0/0, `x-misc` 18/0/0).

### 6.11 Single-source float functions: `frcp`, `frsq`, `flog`, `fexp`, `fsqrt`, `fsin`, `fcos`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#43   class 1        mask F820000000000000  value 0800000000000000  fcos fexp flog frcp frsq fsin
      fsqrt
      <pred><op><flags1> <dst><wmask>, <src1><flags2>
#44   class 1        mask F820000000000000  value 0820000000000000  fcos fexp flog frcp frsq fsin
      fsqrt
      <pred><op><flags1><repeat> <dst>, <src1><flags2>
```

Mnemonics: `fcos`, `fexp`, `flog`, `frcp`, `frsq`, `fsin`, `fsqrt`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`, `.tpres`.

Layout 1 of 2 (form #43):

```
 63   59 58  56 55       54  53  52       51    50       49     48
+-------+------+--------+---+---+--------+-----+--------+------+----+
| 00001 | pred | flags1 | - | 0 | flags1 | dst | flags1 | src1 | op |
+-------+------+--------+---+---+--------+-----+--------+------+----+
 47   44 43       42 41 40    35 34       33 32
+-------+--------+-----+--------+--------+-----+
| wmask | flags1 |  op | flags2 | flags1 | dst |
+-------+--------+-----+--------+--------+-----+
 31  30 29 28 27 21 20 14 13   7 6 0
+------+-----+-----+-----+------+---+
| src1 |  -  | dst |  -  | src1 | - |
+------+-----+-----+-----+------+---+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 42:41 → v[1:0], 48 → v[2]. Values: 0 `frcp`, 1 `frsq`, 2 `flog`, 3 `fexp`, 4 `fsqrt`,
  5 `fsin`, 6 `fcos`. Other values do not decode.
- `flags1` — bits 55 → v[0], 52 → v[1], 43 → v[2], 50 → v[3], 34 → v[4]. Flags, printed in this
  order. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`, v[4] `.tpres`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `wmask` — bits 47:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`, 4 `.z`, 5 `.xz`, 6 `.yz`, 7
  `.xyz`, 8 `.w`, 9 `.xw`, 10 `.yw`, 11 `.xyw`, 12 `.zw`, 13 `.xzw`, 14 `.yzw`, 15 `.xyzw`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `flags2` — bits 38:37 → v[1:0], 40:39 → v[3:2], 36:35 → v[5:4]. Flags, printed in this order. Each
  bit adds: v[0] `.neg`, v[1] `.abs`, v[2] `.flt16.0`, v[3] `.c10.0`.

Layout 2 of 2 (form #44):

```
 63   59 58  56 55       54  53  52       51    50       49     48
+-------+------+--------+---+---+--------+-----+--------+------+----+
| 00001 | pred | flags1 | - | 1 | flags1 | dst | flags1 | src1 | op |
+-------+------+--------+---+---+--------+-----+--------+------+----+
 47    44 43       42 41 40    35 34       33 32
+--------+--------+-----+--------+--------+-----+
| repeat | flags1 |  op | flags2 | flags1 | dst |
+--------+--------+-----+--------+--------+-----+
 31  30 29 28 27 21 20 14 13   7 6 0
+------+-----+-----+-----+------+---+
| src1 |  -  | dst |  -  | src1 | - |
+------+-----+-----+-----+------+---+
```

- `pred` — as in layout 1.
- `op` — as in layout 1.
- `flags1` — as in layout 1.
- `repeat` — bits 47:44. Values: 0 (none), 1-15 `.repeat2`…`.repeat16`.
- `dst` — as in layout 1.
- `src1` — as in layout 1.
- `flags2` — as in layout 1.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 2 forms have semantics. The
probe vectors that test them: 3141 match the hardware (2717 with near values, each within the bound
the hardware was measured to), 6 stop unsupported, 0 mismatch (`flags-other` 0/3/0, `frcp` 84/0/0,
`frsq` 70/0/0, `fsingle-base` 48/0/0, `fsingle-specials` 48/0/0, `fsqrt` 70/0/0, `sweep-fcos`
132/0/0, `sweep-fexp` 132/0/0, `sweep-flog` 132/0/0, `sweep-frcp` 672/0/0, `sweep-frsq` 672/0/0,
`sweep-fsin` 132/0/0, `sweep-fsqrt` 672/0/0, `wmask-frcp` 224/0/0, `x-fcos` 13/0/0, `x-fexp` 13/0/0,
`x-flog` 13/0/0, `x-fsin` 13/0/0, `x-misc` 1/3/0); unsupported mostly for flag .tpres (6).

### 6.12 Float dot products: `fdp`, `fdpc`, `fddp`, `fddpc`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#45   class 2        mask F840060000004000  value 1000000000000000  fdp
      <pred>fdp<flags><repeat><incs> <dst>, <src1>, <src2>
#46   class 2        mask F840060000004000  value 1000000000004000  fdpc
      <pred>fdpc<flags><repeat><incs> <dst>, cp<cp>, <src1>, <src2>
#47   class 2        mask F840060000000000  value 1000020000000000  fddp
      <pred>fddp<flags><repeat><incs> <dst>, i<i>, <src0>, <src1>, <src2>
#48   class 2        mask F840060000000000  value 1000040000000000  fddpc
      <pred>fddpc<flags><repeat> <dst>, i<i>, cp<cp1>, cp<cp2>, <src0>, <src1>, <src2>
#49   class 2        mask F840060000004000  value 1040000000000000  fdp
      <pred>fdp<flags><repeat><incs> <dst>, <src1>, <src2>
#50   class 2        mask F840060000004000  value 1040000000004000  fdpc
      <pred>fdpc<flags><repeat><incs> <dst>, cp<cp>, <src1>, <src2>
#51   class 2        mask F840060000000000  value 1040020000000000  fddp
      <pred>fddp<flags><repeat><incs> <dst>, i<i>, <src0>, <src1>, <src2>
#52   class 2        mask F840060000000000  value 1040040000000000  fddpc
      <pred>fddpc<flags><repeat> <dst>, i<i>, cp<cp1>, cp<cp2>, <src0>, <src1>, <src2>
```

Mnemonics: `fddp`, `fddpc`, `fdp`, `fdpc`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 4 (forms #45, #49):

```
 63   59 58  56 55      54  53     52      51    50      49     48
+-------+------+-------+---+------+-------+-----+-------+------+------+
| 00010 | pred | flags | ? | incs | flags | dst | flags | src1 | src2 |
+-------+------+-------+---+------+-------+-----+-------+------+------+
 47  46 45    44 43      42 41 40     39  38  37 36  35 34  33 32
+------+--------+-------+-----+------+---+------+------+---+-----+
| incs | repeat | flags |  00 | incs | - | src1 | src2 | - | dst |
+------+--------+-------+-----+------+---+------+------+---+-----+
 31  30 29  28 27 21 20 15 14  13   7 6    0
+------+------+-----+-----+---+------+------+
| src1 | src2 | dst |  -  | 0 | src1 | src2 |
+------+------+-----+-----+---+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `flags` — bits 55 → v[0], 52 → v[1], 43 → v[2], 50 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`.
- `repeat` — bits 45:44. Values: 0 (none), 1-3 `.repeat2`…`.repeat4`.
- `incs` — bits 53 → v[0], 40 → v[1], 47:46 → v[3:2]. Flags, printed in this order. Value 0 prints
  `.incs000`. Each bit adds: v[0] drops `.incs000`, v[1] `.incs001`, v[2] `.incs010`, v[3]
  `.incs100`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- (form #45) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #49) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.
- (form #45) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #49) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.

Layout 2 of 4 (forms #46, #50):

```
 63   59 58  56 55      54  53     52      51    50      49     48
+-------+------+-------+---+------+-------+-----+-------+------+------+
| 00010 | pred | flags | ? | incs | flags | dst | flags | src1 | src2 |
+-------+------+-------+---+------+-------+-----+-------+------+------+
 47  46 45    44 43      42 41 40     39  38  37 36  35 34  33 32
+------+--------+-------+-----+------+---+------+------+---+-----+
| incs | repeat | flags |  00 | incs | - | src1 | src2 | - | dst |
+------+--------+-------+-----+------+---+------+------+---+-----+
 31  30 29  28 27 21 20 18 17 15 14  13   7 6    0
+------+------+-----+-----+-----+---+------+------+
| src1 | src2 | dst |  -  |  cp | 1 | src1 | src2 |
+------+------+-----+-----+-----+---+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `repeat` — as in layout 1.
- `incs` — as in layout 1.
- `dst` — as in layout 1.
- `cp` — bits 17:15. A 3-bit number, printed in decimal.
- (form #46) `src1` — as in layout 1.
- (form #50) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16 `r0.abs`…`i1.abs.flt16.2`, 17
  `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19 `sa0.abs`…`sa31.abs.flt16.2`, 20
  `r[il + #0].abs`…`sa[ih + #7].abs.flt16`, 21 `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23
  `i0.abs`…`i31.abs.flt16.2`, 24 `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25
  `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26 `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27
  `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il + #0].neg.abs`…`sa[ih + #7].neg.abs.flt16`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.
- (form #46) `src2` — as in layout 1.
- (form #50) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16 `r0.abs`…`i1.abs.flt16.2`, 17
  `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19 `sa0.abs`…`sa31.abs.flt16.2`, 20
  `r[il + #0].abs`…`sa[ih + #7].abs.flt16`, 21 `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23
  `i0.abs`…`i31.abs.flt16.2`, 24 `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25
  `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26 `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27
  `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il + #0].neg.abs`…`sa[ih + #7].neg.abs.flt16`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.

Layout 3 of 4 (forms #47, #51):

```
 63   59 58  56 55      54  53     52      51  50      49     48
+-------+------+-------+---+------+-------+---+-------+------+------+
| 00010 | pred | flags | ? | incs | flags | i | flags | src1 | src2 |
+-------+------+-------+---+------+-------+---+-------+------+------+
 47  46 45    44 43      42 41 40     39     38  37 36  35 34     33 32
+------+--------+-------+-----+------+------+------+------+------+-----+
| incs | repeat | flags |  01 | incs | src0 | src1 | src2 | src0 | dst |
+------+--------+-------+-----+------+------+------+------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `repeat` — as in layout 1.
- `incs` — as in layout 1.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7]. A register operand, v = bank·128 + number (the bank
  bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `r[il +
  #0]`…`sa[ih + #15]`.
- `i` — bit 51. A 1-bit number, printed in decimal.
- (form #47) `src0` — bits 20:14 → v[6:0], 34 → v[7], 39 → v[8]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`, 2
  `r0.abs`…`i3.abs`, 3 `pa0.abs`…`pa127.abs`.
- (form #51) `src0` — bits 20:14 → v[6:0], 34 → v[7], 39 → v[8]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1 `pa0`…`pa31.flt16.2`,
  2 `r0.abs`…`i1.abs.flt16.2`, 3 `pa0.abs`…`pa31.abs.flt16.2`.
- (form #47) `src1` — as in layout 1.
- (form #51) `src1` — as in layout 2.
- (form #47) `src2` — as in layout 1.
- (form #51) `src2` — as in layout 2.

Layout 4 of 4 (forms #48, #52):

```
 63   59 58  56 55      54  53  52      51  50      49     48
+-------+------+-------+---+---+-------+---+-------+------+------+
| 00010 | pred | flags | ? | - | flags | i | flags | src1 | src2 |
+-------+------+-------+---+---+-------+---+-------+------+------+
 47 46 45    44 43      42 41 40 38 37 35 34     33 32
+-----+--------+-------+-----+-----+-----+------+-----+
|  -  | repeat | flags |  10 | cp2 | cp1 | src0 | dst |
+-----+--------+-------+-----+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `repeat` — as in layout 1.
- `dst` — as in layout 3.
- `i` — as in layout 3.
- `cp1` — bits 37:35. A 3-bit number, printed in decimal.
- `cp2` — bits 40:38. A 3-bit number, printed in decimal.
- (form #48) `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the
  bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- (form #52) `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the
  bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1 `pa0`…`pa31.flt16.2`.
- (form #48) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the
  standard source 1 or 2 (*Register operands*).
- (form #52) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1
  `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`.
- (form #48) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard
  source 1 or 2 (*Register operands*).
- (form #52) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v = bank·128
  + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`,
  2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`,
  6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 4 of 8 forms have semantics (not
yet: #49, #50, #51, #52). The probe vectors that test them: 1280 match the hardware, 24 stop
unsupported, 0 mismatch (`fddp` 84/0/0, `fddpc` 42/0/0, `fdp-count` 56/0/0, `fdp-incs` 126/0/0,
`fdp-rounding` 672/0/0, `fdp-specials` 112/0/0, `fdpc` 84/0/0, `fdpc-clip` 80/0/0, `x-fdpc`
24/24/0); unsupported mostly for reads rN before the program writes it (24).

### 6.13 Float min/max: `fminmax`, `fmaxmin`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#53   class 3        mask F840000000000000  value 1800000000000000  fmaxmin fminmax
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
#54   class 3        mask F840000000000000  value 1840000000000000  fmaxmin fminmax
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
```

Mnemonics: `fmaxmin`, `fminmax`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout (forms #53, #54):

```
 63   59 58  56 55      54  53     52      51    50      49     48
+-------+------+-------+---+------+-------+-----+-------+------+------+
| 00011 | pred | flags | ? | incs | flags | dst | flags | src1 | src2 |
+-------+------+-------+---+------+-------+-----+-------+------+------+
 47  46 45    44 43      42 41 40     39     38  37 36  35 34     33 32
+------+--------+-------+-----+------+------+------+------+------+-----+
| incs | repeat | flags |  op | incs | src0 | src1 | src2 | src0 | dst |
+------+--------+-------+-----+------+------+------+------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 42:41. Values: 0 `fmaxmin`, 1 `fminmax`. Other values do not decode.
- `flags` — bits 55 → v[0], 52 → v[1], 43 → v[2], 50 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`.
- `repeat` — bits 45:44. Values: 0 (none), 1-3 `.repeat2`…`.repeat4`.
- `incs` — bits 53 → v[0], 40 → v[1], 47:46 → v[3:2]. Flags, printed in this order. Value 0 prints
  `.incs000`. Each bit adds: v[0] drops `.incs000`, v[1] `.incs001`, v[2] `.incs010`, v[3]
  `.incs100`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- (form #53) `src0` — bits 20:14 → v[6:0], 34 → v[7], 39 → v[8]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`, 2
  `r0.abs`…`i3.abs`, 3 `pa0.abs`…`pa127.abs`.
- (form #54) `src0` — bits 20:14 → v[6:0], 34 → v[7], 39 → v[8]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`, 1 `pa0`…`pa31.flt16.2`,
  2 `r0.abs`…`i1.abs.flt16.2`, 3 `pa0.abs`…`pa31.abs.flt16.2`.
- (form #53) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #54) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.
- (form #53) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #54) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 2 forms have semantics (not
yet: #54). The probe vectors that test them: 1203 match the hardware, 0 stop unsupported, 0 mismatch
(`fmaxmin-roles` 224/0/0, `fmaxmin-specials` 308/0/0, `fminmax-incs` 126/0/0, `fminmax-roles`
224/0/0, `fminmax-specials` 308/0/0, `minmax-allnan` 12/0/0, `x-allnan` 1/0/0).

### 6.14 Screen-space derivatives: `fdsx`, `fdsy`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#55   class 4        mask F840000000000000  value 2000000000000000  fdsx fdsy
      <pred><op><flags><repeat><incs> <dst>, <src1>, <src2>
#56   class 4        mask F840000000000000  value 2040000000000000  fdsx fdsy
      <pred><op><flags><repeat><incs> <dst>, <src1>, <src2>
```

Mnemonics: `fdsx`, `fdsy`.

Modifiers: `.end`, `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout (forms #55, #56):

```
 63   59 58  56 55      54  53     52      51    50      49     48
+-------+------+-------+---+------+-------+-----+-------+------+------+
| 00100 | pred | flags | ? | incs | flags | dst | flags | src1 | src2 |
+-------+------+-------+---+------+-------+-----+-------+------+------+
 47  46 45    44 43      42 41 40     39  38  37 36  35 34  33 32
+------+--------+-------+-----+------+---+------+------+---+-----+
| incs | repeat | flags |  op | incs | - | src1 | src2 | - | dst |
+------+--------+-------+-----+------+---+------+------+---+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 42:41. Values: 0 `fdsx`, 1 `fdsy`. Other values do not decode.
- `flags` — bits 55 → v[0], 52 → v[1], 43 → v[2], 50 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.end`.
- `repeat` — bits 45:44. Values: 0 (none), 1-3 `.repeat2`…`.repeat4`.
- `incs` — bits 53 → v[0], 40 → v[1], 47:46 → v[3:2]. Flags, printed in this order. Value 0 prints
  `.incs000`. Each bit adds: v[0] drops `.incs000`, v[1] `.incs001`, v[2] `.incs010`, v[3]
  `.incs100`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- (form #55) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #56) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 37 → v[10], 38 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.
- (form #55) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`,
  1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`, 16 `r0.abs`…`i3.abs`, 17 `o0.abs`…`o127.abs`, 18
  `pa0.abs`…`pa127.abs`, 19 `sa0.abs`…`sa127.abs`, 20 `r[il + #0].abs`…`sa[ih + #15].abs`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i127.abs`, 24 `r0.neg.abs`…`i3.neg.abs`,
  25 `o0.neg.abs`…`o127.neg.abs`, 26 `pa0.neg.abs`…`pa127.neg.abs`, 27
  `sa0.neg.abs`…`sa127.neg.abs`, 28 `r[il + #0].neg.abs`…`sa[ih + #15].neg.abs`, 29
  `c0.neg.abs`…`g63.neg.abs`, 30 `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i127.neg.abs`.
- (form #56) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10], 36 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13
  `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`, 16
  `r0.abs`…`i1.abs.flt16.2`, 17 `o0.abs`…`o31.abs.flt16.2`, 18 `pa0.abs`…`pa31.abs.flt16.2`, 19
  `sa0.abs`…`sa31.abs.flt16.2`, 20 `r[il + #0].abs`…`sa[ih + #7].abs.flt16.0`, 21
  `c0.abs`…`g63.abs`, 22 `#0.abs`…`#127.abs`, 23 `i0.abs`…`i31.abs.flt16.2`, 24
  `r0.neg.abs`…`i1.neg.abs.flt16.2`, 25 `o0.neg.abs`…`o31.neg.abs.flt16.2`, 26
  `pa0.neg.abs`…`pa31.neg.abs.flt16.2`, 27 `sa0.neg.abs`…`sa31.neg.abs.flt16.2`, 28 `r[il +
  #0].neg.abs`…`sa[ih + #7].neg.abs.flt16.0`, 29 `c0.neg.abs`…`g63.neg.abs`, 30
  `#0.neg.abs`…`#127.neg.abs`, 31 `i0.neg.abs`…`i31.neg.abs.flt16.2`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 2 forms have semantics (not
yet: #56). The probe vectors that test them: 42 match the hardware, 22 stop unsupported, 0 mismatch
(`fds-incs` 0/12/0, `fds-quad` 30/0/0, `x-fds` 12/0/0, `x-m2` 0/7/0, `x-misc` 0/3/0); unsupported
mostly for fds repeated (22).

### 6.15 Half-float multiply-add: `fmad16`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#57   class 6        mask F800000000000000  value 3000000000000000  fmad16
      <pred>fmad16<flags1><flags2> <dst>, <src0><flags3>, <src1><flags4>, <src2><flags5>
```

Mnemonics: `fmad16`.

Modifiers: `.incsXYZ`, `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout (form #57):

```
 63   59 58  56 55       54    53 52       51    50       49     48
+-------+------+--------+--------+--------+-----+--------+------+------+
| 00110 | pred | flags1 | flags5 | flags1 | dst | flags4 | src1 | src2 |
+-------+------+--------+--------+--------+-----+--------+------+------+
 47    46 45       44       43       42    41 40       39       38    37 36    35 34     33 32
+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+-----+
| flags2 | flags4 | flags2 | flags1 | flags3 | flags2 | flags3 | flags4 | flags5 | src0 | dst |
+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `flags1` — bits 55 → v[0], 52 → v[1], 43 → v[2]. Flags, printed in this order. Each bit adds: v[0]
  `.skipinv`, v[1] `.syncs`, v[2] `.nosched`.
- `flags2` — bits 44 → v[0], 40 → v[1], 47:46 → v[3:2]. Flags, printed in this order. Each bit adds:
  v[0] `.repeat2.incs000`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- `flags3` — bits 39 → v[0], 42:41 → v[2:1]. Flags, printed in this order. Each bit adds: v[0]
  `.abs`, v[1] `.swzll`, v[2] `.swzhh`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `flags4` — bits 38:37 → v[1:0], 45 → v[2], 50 → v[3]. Flags, printed in this order. Each bit adds:
  v[0] `.neg`, v[1] `.abs`, v[2] `.swzll`, v[3] `.swzhh`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `flags5` — bits 36:35 → v[1:0], 54:53 → v[3:2]. Flags, printed in this order. Each bit adds: v[0]
  `.neg`, v[1] `.abs`, v[2] `.swzll`, v[3] `.swzhh`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 832 match the hardware, 0 stop unsupported, 0 mismatch (`fmad16-incs`
96/0/0, `fmad16-lanes` 396/0/0, `fmad16-rounding` 220/0/0, `fmad16-specials` 36/0/0, `x-fmad16`
52/0/0, `x-fmad16c` 1/0/0, `x-fmad16r` 4/0/0, `x-fmad16r2` 24/0/0, `x-fmad16s` 1/0/0, `x-swzf32`
2/0/0).

### 6.16 Extended float operation: `efo`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#58   class 7        mask F800000000000000  value 3800000000000000  efo
      <pred>efo<flags> <dst>= <reg>, <f4>, <op1>, <op2>, <src0>, <src1>, <src2>
```

Mnemonics: `efo`.

Modifiers: `.gpiext`, `.nosched`, `.repeatN`, `.skipinv`.

Layout (form #58):

```
 63   59 58    57  56 55      54    53 52 51 50 49 48
+-------+-----+------+-------+-----+-----+-----+-----+
| 00111 | op1 | pred | flags | op1 | reg |  f4 | op1 |
+-------+-----+------+-------+-----+-----+-----+-----+
 47 46 45   43 42 41 40      39     38  37 36     35     34     33 32
+-----+-------+-----+-------+------+------+------+------+------+-----+
| op2 | flags |  f4 | flags | src2 | src0 | src1 | src2 | src0 | dst |
+-----+-------+-----+-------+------+------+------+------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 57:56. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `flags` — bits 55 → v[0], 43 → v[1], 40 → v[2], 45:44 → v[4:3]. Flags, printed in this order. Each
  bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.gpiext`, v[4:3] a count, `.repeat2`…`.repeat4`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7]. A register operand, v = bank·128 + number (the bank
  bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `r[il +
  #0]`…`sa[ih + #15]`.
- `reg` — bits 53:52. Values: 0 `i0`, 1 `i1`, 2 `a0`, 3 `a1`.
- `f4` — bits 51:50 → v[1:0], 42 → v[2], 41 → v[3]. Values: 0 `!i0 = a0, !i1 = a1`, 1 `!i0 = a1, !i1
  = a0`, 2 `!i0 = m0, !i1 = m1`, 3 `!i0 = a0, !i1 = m1`, 4 `i0 = a0, !i1 = a1`, 5 `i0 = a1, !i1 =
  a0`, 6 `i0 = m0, !i1 = m1`, 7 `i0 = a0, !i1 = m1`, 8 `!i0 = a0, i1 = a1`, 9 `!i0 = a1, i1 = a0`,
  10 `!i0 = m0, i1 = m1`, 11 `!i0 = a0, i1 = m1`, 12 `i0 = a0, i1 = a1`, 13 `i0 = a1, i1 = a0`, 14
  `i0 = m0, i1 = m1`, 15 `i0 = a0, i1 = m1`.
- `op1` — bits 49:48 → v[1:0], 54 → v[2], 58 → v[3]. Values: 0 `a0=m0+m1, a1=i1+i0`, 1 `a0=m0+src0,
  a1=i1+i0`, 2 `a0=m0+i0, a1=i1+m1`, 3 `a0=src0+src2 a1=src1+src2`, 4 `a0=m0+m1, a1=-i1+i0`, 5
  `a0=m0+src0, a1=-i1+i0`, 6 `a0=m0+i0, a1=-i1+m1`, 7 `a0=src0+src2 a1=-src1+src2`, 8 `a0=m0-m1,
  a1=i1+i0`, 9 `a0=m0-src0, a1=i1+i0`, 10 `a0=m0-i0, a1=i1+m1`, 11 `a0=src0-src2 a1=src1+src2`, 12
  `a0=m0-m1, a1=-i1+i0`, 13 `a0=m0-src0, a1=-i1+i0`, 14 `a0=m0-i0, a1=-i1+m1`, 15 `a0=src0-src2
  a1=-src1+src2`.
- `op2` — bits 47:46. Values: 0 `m0=src0*src2, m1=src1*src2`, 1 `m0=src2*src1, m1=src2*src2`, 2
  `m0=src1*src0, m1=src2*src2`, 3 `m0=src1*i0, m1=src2*i1`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 38 → v[8], 37 → v[9]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3/i1.flt16.2`, 1
  `pa0`…`pa127/pa31.flt16.2`, 2 `o0`…`o127/o31.flt16.2`, 3 `sa0`…`sa127/sa31.flt16.2`, 4
  `r0.neg`…`i3.neg/i1.neg.flt16.2`, 5 `pa0.neg`…`pa127.neg/pa31.neg.flt16.2`, 6
  `o0.neg`…`o127.neg/o31.neg.flt16.2`, 7 `sa0.neg`…`sa127.neg/sa31.neg.flt16.2`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 36 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3/i1.flt16.2`, 1 `o0`…`o127/o31.flt16.2`, 2
  `pa0`…`pa127/pa31.flt16.2`, 3 `sa0`…`sa127/sa31.flt16.2`, 4 `r0.neg`…`i3.neg/i1.neg.flt16.2`, 5
  `o0.neg`…`o127.neg/o31.neg.flt16.2`, 6 `pa0.neg`…`pa127.neg/pa31.neg.flt16.2`, 7
  `sa0.neg`…`sa127.neg/sa31.neg.flt16.2`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 39 → v[9], 35 → v[10]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3/i1.flt16.2`, 1
  `o0`…`o127/o31.flt16.2`, 2 `pa0`…`pa127/pa31.flt16.2`, 3 `sa0`…`sa127/sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #15]/sa[ih + #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i127/i31.flt16.2`, 8
  `r0.neg`…`i3.neg/i1.neg.flt16.2`, 9 `o0.neg`…`o127.neg/o31.neg.flt16.2`, 10
  `pa0.neg`…`pa127.neg/pa31.neg.flt16.2`, 11 `sa0.neg`…`sa127.neg/sa31.neg.flt16.2`, 12 `r[il +
  #0].neg`…`sa[ih + #15].neg/sa[ih + #7].neg.flt16.0`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg/i31.neg.flt16.2`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 1023 match the hardware, 73 stop unsupported, 0 mismatch
(`efo-dataflow` 451/0/0, `efo-denormals` 0/4/0, `efo-gpiext` 0/38/0, `efo-gpiext-quad` 45/0/0,
`efo-gpiext-race` 0/10/0, `efo-rounding` 264/0/0, `x-efo` 148/2/0, `x-efor` 48/0/0, `x-gpiext`
1/4/0, `x-gpiext-map` 18/0/0, `x-gpiext-map2` 24/0/0, `x-gpiext-map3` 2/5/0, `x-gpiext-map5` 22/6/0,
`x-gpiext-xtask` 0/2/0, `x-misc` 0/2/0); unsupported mostly for efo.gpiext reaches the other
instances of the quad (43); the top-right iN read races the iN writes (20); reads iN before the
program writes it (8).

### 6.17 Fixed-point multiply-add: `fpma`

Class 25 **[RE]**: destination and three sources in the c10 reading, then colour selections
(`s0`/`s1`/`s0a`/`s1a`, `s1`/`s1a`, `s2`/`s2a`) and alpha selections, each with a `.comp`
complement; the flags carry a three-bit repeat and `.sat`.

- **Semantics [HW].** A colour multiply-add, `c0 + c1 × c2` per channel with the first
  selection the addend, on 8-bit channels or, with `.c10`, signed 10-bit ones (*`fpma`*);
  `.sat` changes nothing we measured, also on `c10` results out of 0..1.0, and a repeat
  advances every operand **[HW]**.
- **In the firmware:** not used.

Forms (generated from the decoder's table; `<name>` is a field):

```
#59   class 25       mask F800000000000000  value C800000000000000  fpma
      <pred>fpma<flags> <dst>, <src0>, <src1>, <src2>, <op1>, <op2>
```

Mnemonics: `fpma`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.sat`, `.skipinv`.

Layout (form #59):

```
 63   59 58  57 56    55   54 53 52 51    50      49     48
+-------+------+-----+-------+-----+-----+-------+------+------+
| 11001 | pred | op1 | flags | op1 | dst | flags | src1 | src2 |
+-------+------+-----+-------+-----+-----+-------+------+------+
 47    46   43 42    41 38 37 36 35  34 33 32
+-----+-------+-----+-----+-----+------+-----+
| op1 | flags | op1 | op2 | op1 | src0 | dst |
+-----+-------+-----+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 58:57. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2], 46:44 → v[5:3], 43 → v[6]. Flags, printed in this
  order. Each bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.end`, v[5:3] a count,
  `.repeat2`…`.repeat8`, v[6] `.sat`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `r[il + #0]`…`sa[ih + #7].c10`, 4 `sa0`…`sa63.c10`, 5 `c0`…`g63`, 6
  `i.`…`i.lh`, 7 `i0`…`i63.c10`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 35 → v[8]. A register operand, v = bank·128 + number (the
  bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `pa0`…`pa63.c10`, 2
  `r0.neg`…`i3.neg.c10`, 3 `pa0.neg`…`pa63.neg.c10`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand, v = bank·128 + number
  (the bank bits may carry modifiers). By bank: 0 `r0`…`i3.c10`, 1 `o0`…`o63.c10`, 2
  `pa0`…`pa63.c10`, 3 `sa0`…`sa63.c10`, 4 `r[il + #0]`…`sa[ih + #7].c10`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i63.c10`.
- `op1` — bits 53:52 → v[1:0], 42 → v[2], 37 → v[3], 56 → v[4], 36 → v[5], 47 → v[6]. An operand
  list (see *Operand encodings*), for example 0 `s0, s1, s2`, 1 `s1, s1, s2`, 2 `s0a, s1, s2`.
- `op2` — bits 41 → v[0], 40:38 → v[3:1]. Values: 0 `s0a, s1a, s2a`, 1 `s1a, s1a, s2a`, 2 `s0a.comp,
  s1a, s2a`, 3 `s1a.comp, s1a, s2a`, 4 `s0a, s1a.comp, s2a`, 5 `s1a, s1a.comp, s2a`, 6 `s0a.comp,
  s1a.comp, s2a`, 7 `s1a.comp, s1a.comp, s2a`, 8 `s0a, s1a, s2a.comp`, 9 `s1a, s1a, s2a.comp`, 10
  `s0a.comp, s1a, s2a.comp`, 11 `s1a.comp, s1a, s2a.comp`, 12 `s0a, s1a.comp, s2a.comp`, 13 `s1a,
  s1a.comp, s2a.comp`, 14 `s0a.comp, s1a.comp, s2a.comp`, 15 `s1a.comp, s1a.comp, s2a.comp`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 543 match the hardware, 0 stop unsupported, 0 mismatch
(`colour-dot-repeats` 15/0/0, `fpma` 396/0/0, `fpma-sat` 60/0/0, `x-fpma` 48/0/0, `x-rep` 24/0/0).

### 6.18 Dual issue: two operations in one word

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#60   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 fadm fmad fminmax fmsa ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#61   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr fadm
      fmad fminmax fmsa ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#62   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fadm fmad fminmax fmsa ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#63   class 27       mask F840C00000000000  value D800000000000000  +mov fadm fmad fminmax fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#64   class 27       mask F840C00000000000  value D800400000000000  +mov fadm fmad fminmax fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#65   class 27       mask F840C00000000000  value D800800000000000  +mov fadm fmad fminmax fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#66   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 mov
      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#67   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
#68   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#69   class 27       mask F840C00000000000  value D800000000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc0>
#70   class 27       mask F840C00000000000  value D800400000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#71   class 27       mask F840C00000000000  value D800800000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc2>
#72   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#73   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#74   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#75   class 27       mask F840C00000000000  value D800000000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#76   class 27       mask F840C00000000000  value D800400000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#77   class 27       mask F840C00000000000  value D800800000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#78   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#79   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#80   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#81   class 27       mask F840C00000000000  value D800000000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#82   class 27       mask F840C00000000000  value D800400000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#83   class 27       mask F840C00000000000  value D800800000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#84   class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#85   class 27       mask F840000000000000  value D840000000000000  +ima32 fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#86   class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr fadm
      fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#87   class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#88   class 27       mask F840C00000000000  value D840000000000000  +mov fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#89   class 27       mask F840C00000000000  value D840400000000000  +mov fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#90   class 27       mask F840C00000000000  value D840800000000000  +mov fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#91   class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#92   class 27       mask F840000000000000  value D840000000000000  +ima32 ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#93   class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#94   class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#95   class 27       mask F840C00000000000  value D840000000000000  +mov ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#96   class 27       mask F840C00000000000  value D840400000000000  +mov ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#97   class 27       mask F840C00000000000  value D840800000000000  +mov ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#98   class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa mov
      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#99   class 27       mask F840000000000000  value D840000000000000  +ima32 mov
      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#100  class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
#101  class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#102  class 27       mask F840C00000000000  value D840000000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc0>
#103  class 27       mask F840C00000000000  value D840400000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#104  class 27       mask F840C00000000000  value D840800000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc2>
#105  class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#106  class 27       mask F840000000000000  value D840000000000000  +ima32 fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#107  class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#108  class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#109  class 27       mask F840C00000000000  value D840000000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#110  class 27       mask F840C00000000000  value D840400000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#111  class 27       mask F840C00000000000  value D840800000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#112  class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#113  class 27       mask F840000000000000  value D840000000000000  +ima32 fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#114  class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#115  class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#116  class 27       mask F840C00000000000  value D840000000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#117  class 27       mask F840C00000000000  value D840400000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#118  class 27       mask F840C00000000000  value D840800000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
```

Mnemonics: `+fadd`, `+fcos`, `+fexp`, `+flog`, `+fmad`, `+fmad16`, `+fmaxmin`, `+fmsa`, `+fmul`,
`+frcp`, `+frsq`, `+fsin`, `+fsub`, `+fsubflr`, `+ima32`, `+mov`, `fadm`, `fddp`, `fmad`, `fminmax`,
`fmsa`, `fssq`, `ima32`, `mov`.

Modifiers: `.nosched`, `.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 18 (forms #60, #72, #84, #85, #91, #92, #105, #106):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+------------+
| 11011 | op | pred | op | ? | ssrc2 |  op | ssrc1/src1 | ssrc2/src2 |
+-------+----+------+----+---+-------+-----+------------+------------+
 47   46 45      44 43 42      41 38 37      36     35     34           33 32
+-------+-------+-----+-------+-----+-------+------+------+------------+-----+
| ssrc1 | ssrc0 |  op | ssrc2 | sop | ssrc0 | src1 | src2 | ssrc0/src0 | dst |
+-------+-------+-----+-------+-----+-------+------+------+------------+-----+
 31        30 29        28 27 21 20        14 13         7 6          0
+------------+------------+-----+------------+------------+------------+
| ssrc1/src1 | ssrc2/src2 | dst | ssrc0/src0 | ssrc1/src1 | ssrc2/src2 |
+------------+------------+-----+------------+------------+------------+
```

- (forms #60, #72) `sop` — bits 41:38. Values: 0 `fmad`, 1 `fmad16`, 2 `fmsa`, 4 `fmaxmin`, 14
  `ima32`. Other values do not decode.
- (forms #84, #91, #105) `sop` — bits 41:38. Values: 0 `fmad`, 1 `fmad16`, 2 `fmsa`, 4 `fmaxmin`.
  Other values do not decode.
- (forms #85, #92, #106) `sop` — bits 41:38. Values: 14 `ima32`. Other values do not decode.
- `ssrc0` — bits 20:14 → v[6:0], 34 → v[7], 37 → v[8], 45 → v[9]. An operand list (see *Operand
  encodings*), for example 0 `i0, i0`, 1 `i0, i0`, 2 `i0, i0`.
- (forms #60, #72) `ssrc1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 47:46 → v[11:10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i0`,
  8-15 `i1`, 16-23 `i2`, 24 `r0`…`i3`, 25 `o0`…`o127`, 26 `pa0`…`pa127`, 27 `sa0`…`sa127`, 28 `r[il
  + #0]`…`sa[ih + #15]`, 29 `c0`…`g63`, 30 `#0`…`#127`, 31 `i0`…`i127`.
- (forms #84, #91, #105) `ssrc1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 47:46 → v[11:10].
  A register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i0`,
  8-15 `i1`, 16-23 `i2`, 24 `r0`…`i1.flt16.2`, 25 `o0`…`o31.flt16.2`, 26 `pa0`…`pa31.flt16.2`, 27
  `sa0`…`sa31.flt16.2`, 28 `r[il + #0]`…`sa[ih + #7].flt16.0`, 29 `c0`…`g63`, 30 `#0`…`#127`, 31
  `i0`…`i31.flt16.2`.
- (forms #85, #92, #106) `ssrc1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 47:46 → v[11:10].
  A register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i0`,
  8-15 `i1`, 16-23 `i2`, 24 `r0`…`i1.flt16.2`, 25 `o0`…`o31.flt16.2`, 26 `pa0`…`pa31.flt16.2`, 27
  `sa0`…`sa31.flt16.2`, 28 `r[il + #0]`…`sa[ih + #7].flt16`, 29 `c0`…`g63`, 30 `#0`…`#127`, 31
  `i0`…`i31.flt16.2`.
- (forms #60, #72) `ssrc2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 42 → v[10], 53 → v[11]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i0`,
  8-15 `i1`, 16-23 `i2`, 24 `r0`…`i3`, 25 `o0`…`o127`, 26 `pa0`…`pa127`, 27 `sa0`…`sa127`, 28 `r[il
  + #0]`…`sa[ih + #15]`, 29 `c0`…`g63`, 30 `#0`…`#127`, 31 `i0`…`i127`.
- (forms #84, #91, #105) `ssrc2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 42 → v[10], 53 →
  v[11]. A register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7
  `i0`, 8-15 `i1`, 16-23 `i2`, 24 `r0`…`i1.flt16.2`, 25 `o0`…`o31.flt16.2`, 26 `pa0`…`pa31.flt16.2`,
  27 `sa0`…`sa31.flt16.2`, 28 `r[il + #0]`…`sa[ih + #7].flt16.0`, 29 `c0`…`g63`, 30 `#0`…`#127`, 31
  `i0`…`i31.flt16.2`.
- (forms #85, #92, #106) `ssrc2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 42 → v[10], 53 →
  v[11]. A register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7
  `i0`, 8-15 `i1`, 16-23 `i2`, 24 `r0`…`i1.flt16.2`, 25 `o0`…`o31.flt16.2`, 26 `pa0`…`pa31.flt16.2`,
  27 `sa0`…`sa31.flt16.2`, 28 `r[il + #0]`…`sa[ih + #7].flt16`, 29 `c0`…`g63`, 30 `#0`…`#127`, 31
  `i0`…`i31.flt16.2`.
- `pred` — bits 57:56. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- (form #60) `op` — bits 51:50 → v[1:0], 58 → v[2], 55 → v[3], 52 → v[4], 43 → v[5], 44 → v[6]. 80
  distinct texts, for example 0 `fmad`, 2 `fadm`, 3 `fmsa`, 5 `fminmax`, 6 `ima32`, 8
  `fmad.skipinv`.
- (forms #72, #105, #106) `op` — bits 51:50 → v[1:0], 58 → v[2], 55 → v[3], 52 → v[4], 43 → v[5], 44
  → v[6]. Values: 4 `fddp`, 12 `fddp.skipinv`, 20 `fddp.syncs`, 28 `fddp.skipinv.syncs`, 36
  `fddp.nosched`, 44 `fddp.skipinv.nosched`, 52 `fddp.syncs.nosched`, 60
  `fddp.skipinv.syncs.nosched`, 68 `fddp.repeat2`, 76 `fddp.skipinv.repeat2`, 84
  `fddp.syncs.repeat2`, 92 `fddp.skipinv.syncs.repeat2`, 100 `fddp.nosched.repeat2`, 108
  `fddp.skipinv.nosched.repeat2`, 116 `fddp.syncs.nosched.repeat2`, 124
  `fddp.skipinv.syncs.nosched.repeat2`. Other values do not decode.
- (forms #84, #85) `op` — bits 51:50 → v[1:0], 58 → v[2], 55 → v[3], 52 → v[4], 43 → v[5], 44 →
  v[6]. 64 distinct texts, for example 0 `fmad`, 2 `fadm`, 3 `fmsa`, 5 `fminmax`, 8 `fmad.skipinv`,
  10 `fadm.skipinv`.
- (forms #91, #92) `op` — bits 51:50 → v[1:0], 58 → v[2], 55 → v[3], 52 → v[4], 43 → v[5], 44 →
  v[6]. Values: 6 `ima32`, 14 `ima32.skipinv`, 22 `ima32.syncs`, 30 `ima32.skipinv.syncs`, 38
  `ima32.nosched`, 46 `ima32.skipinv.nosched`, 54 `ima32.syncs.nosched`, 62
  `ima32.skipinv.syncs.nosched`, 70 `ima32.repeat2`, 78 `ima32.skipinv.repeat2`, 86
  `ima32.syncs.repeat2`, 94 `ima32.skipinv.syncs.repeat2`, 102 `ima32.nosched.repeat2`, 110
  `ima32.skipinv.nosched.repeat2`, 118 `ima32.syncs.nosched.repeat2`, 126
  `ima32.skipinv.syncs.nosched.repeat2`. Other values do not decode.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7]. A register operand, v = bank·128 + number (the bank
  bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `r[il +
  #0]`…`sa[ih + #15]`.
- (forms #60, #72) `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 +
  number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- (forms #84, #85, #91, #92, #105, #106) `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`,
  1 `pa0`…`pa31.flt16.2`.
- (forms #60, #72) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 36 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1
  `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`.
- (forms #84, #85) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 36 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`,
  1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8 `r0.neg`…`i1.neg.flt16.2`, 9
  `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11 `sa0.neg`…`sa31.neg.flt16.2`, 12
  `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13 `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15
  `i0.neg`…`i31.neg.flt16.2`.
- (forms #91, #92, #105, #106) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 36 → v[10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`.
- (forms #60, #72) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1
  `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0`…`#127`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10 `pa0.neg`…`pa127.neg`,
  11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13 `c0.neg`…`g63.neg`, 14
  `#0.neg`…`#127.neg`, 15 `i0.neg`…`i127.neg`.
- (forms #84, #85) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`,
  1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16.0`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8 `r0.neg`…`i1.neg.flt16.2`, 9
  `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11 `sa0.neg`…`sa31.neg.flt16.2`, 12
  `r[il + #0].neg`…`sa[ih + #7].neg.flt16.0`, 13 `c0.neg`…`g63.neg`, 14 `#0.neg`…`#127.neg`, 15
  `i0.neg`…`i31.neg.flt16.2`.
- (forms #91, #92, #105, #106) `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 35 → v[10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0
  `r0`…`i1.flt16.2`, 1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il +
  #0]`…`sa[ih + #7].flt16`, 5 `c0`…`g63`, 6 `#0`…`#127`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0.neg`…`#127.neg`, 15 `i0.neg`…`i31.neg.flt16.2`.

Layout 2 of 18 (forms #61, #73, #86, #93, #107):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+------------+
| 11011 | op | pred | op | ? | ssrc2 |  op | ssrc1/src1 | ssrc2/src2 |
+-------+----+------+----+---+-------+-----+------------+------------+
 47   46 45     44 43 42      41 38 37     36     35     34     33 32
+-------+------+-----+-------+-----+------+------+------+------+-----+
| ssrc1 | sreg |  op | ssrc2 | sop | sreg | src1 | src2 | src0 | dst |
+-------+------+-----+-------+-----+------+------+------+------+-----+
 31        30 29        28 27 21 20  14 13         7 6          0
+------------+------------+-----+------+------------+------------+
| ssrc1/src1 | ssrc2/src2 | dst | src0 | ssrc1/src1 | ssrc2/src2 |
+------------+------------+-----+------+------------+------------+
```

- `sop` — bits 41:38. Values: 3 `fsubflr`, 11 `fadd`, 12 `fsub`, 13 `fmul`. Other values do not
  decode.
- `sreg` — bits 37 → v[0], 45 → v[1]. Values: 0-2 `i0`…`i2`, 3 `i0`.
- (forms #61, #73) `ssrc1` — as in layout 1.
- (forms #86, #93, #107) `ssrc1` — as in layout 1.
- (forms #61, #73) `ssrc2` — as in layout 1.
- (forms #86, #93, #107) `ssrc2` — as in layout 1.
- `pred` — as in layout 1.
- (form #61) `op` — as in layout 1.
- (forms #73, #107) `op` — as in layout 1.
- (form #86) `op` — as in layout 1.
- (form #93) `op` — as in layout 1.
- `dst` — as in layout 1.
- (forms #61, #73) `src0` — as in layout 1.
- (forms #86, #93, #107) `src0` — as in layout 1.
- (forms #61, #73) `src1` — as in layout 1.
- (form #86) `src1` — as in layout 1.
- (forms #93, #107) `src1` — as in layout 1.
- (forms #61, #73) `src2` — as in layout 1.
- (form #86) `src2` — as in layout 1.
- (forms #93, #107) `src2` — as in layout 1.

Layout 3 of 18 (forms #62, #74, #87, #94, #108):

```
 63   59 58   57  56 55   54  53  52 50 49           48
+-------+----+------+----+---+---+-----+------------+------+
| 11011 | op | pred | op | ? | - |  op | ssrc1/src1 | src2 |
+-------+----+------+----+---+---+-----+------------+------+
 47   46 45     44 43 42  41 38 37     36     35     34     33 32
+-------+------+-----+---+-----+------+------+------+------+-----+
| ssrc1 | sreg |  op | - | sop | sreg | src1 | src2 | src0 | dst |
+-------+------+-----+---+-----+------+------+------+------+-----+
 31        30 29  28 27 21 20  14 13         7 6    0
+------------+------+-----+------+------------+------+
| ssrc1/src1 | src2 | dst | src0 | ssrc1/src1 | src2 |
+------------+------+-----+------+------------+------+
```

- `sop` — bits 41:38. Values: 5 `frcp`, 6 `frsq`, 7 `fexp`, 8 `flog`, 9 `fsin`, 10 `fcos`. Other
  values do not decode.
- `sreg` — as in layout 2.
- (forms #62, #74) `ssrc1` — as in layout 1.
- (forms #87, #94, #108) `ssrc1` — as in layout 1.
- `pred` — as in layout 1.
- (form #62) `op` — as in layout 1.
- (forms #74, #108) `op` — as in layout 1.
- (form #87) `op` — as in layout 1.
- (form #94) `op` — as in layout 1.
- `dst` — as in layout 1.
- (forms #62, #74) `src0` — as in layout 1.
- (forms #87, #94, #108) `src0` — as in layout 1.
- (forms #62, #74) `src1` — as in layout 1.
- (form #87) `src1` — as in layout 1.
- (forms #94, #108) `src1` — as in layout 1.
- (forms #62, #74) `src2` — as in layout 1.
- (form #87) `src2` — as in layout 1.
- (forms #94, #108) `src2` — as in layout 1.

Layout 4 of 18 (forms #63, #75, #88, #95, #109):

```
 63   59 58   57  56 55   54  53      52 50 49     48
+-------+----+------+----+---+-------+-----+------+------+
| 11011 | op | pred | op | ? | ssrc0 |  op | src1 | src2 |
+-------+----+------+----+---+-------+-----+------+------+
 47 46 45     44 43 42  41 38 37     36     35     34           33 32
+-----+------+-----+---+-----+------+------+------+------------+-----+
|  00 | sreg |  op | - | sop | sreg | src1 | src2 | ssrc0/src0 | dst |
+-----+------+-----+---+-----+------+------+------+------------+-----+
 31  30 29  28 27 21 20        14 13   7 6    0
+------+------+-----+------------+------+------+
| src1 | src2 | dst | ssrc0/src0 | src1 | src2 |
+------+------+-----+------------+------+------+
```

- `sop` — bits 41:38. Values: 15 `mov`. Other values do not decode.
- `sreg` — as in layout 2.
- (forms #63, #75) `ssrc0` — bits 20:14 → v[6:0], 34 → v[7], 53 → v[8]. A register operand, v =
  bank·128 + number (the bank bits may carry modifiers). By bank: 0-1 `i0`, 2 `r0`…`i3`, 3
  `pa0`…`pa127`.
- (forms #88, #95, #109) `ssrc0` — bits 20:14 → v[6:0], 34 → v[7], 53 → v[8]. A register operand, v
  = bank·128 + number (the bank bits may carry modifiers). By bank: 0-1 `i0`, 2 `r0`…`i1.flt16.2`, 3
  `pa0`…`pa31.flt16.2`.
- `pred` — as in layout 1.
- (form #63) `op` — as in layout 1.
- (forms #75, #109) `op` — as in layout 1.
- (form #88) `op` — as in layout 1.
- (form #95) `op` — as in layout 1.
- `dst` — as in layout 1.
- (forms #63, #75) `src0` — as in layout 1.
- (forms #88, #95, #109) `src0` — as in layout 1.
- (forms #63, #75) `src1` — as in layout 1.
- (form #88) `src1` — as in layout 1.
- (forms #95, #109) `src1` — as in layout 1.
- (forms #63, #75) `src2` — as in layout 1.
- (form #88) `src2` — as in layout 1.
- (forms #95, #109) `src2` — as in layout 1.

Layout 5 of 18 (forms #64, #76, #89, #96, #110):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+------+
| 11011 | op | pred | op | ? | ssrc1 |  op | ssrc1/src1 | src2 |
+-------+----+------+----+---+-------+-----+------------+------+
 47 46 45     44 43 42  41 38 37     36     35     34     33 32
+-----+------+-----+---+-----+------+------+------+------+-----+
|  01 | sreg |  op | - | sop | sreg | src1 | src2 | src0 | dst |
+-----+------+-----+---+-----+------+------+------+------+-----+
 31        30 29  28 27 21 20  14 13         7 6    0
+------------+------+-----+------+------------+------+
| ssrc1/src1 | src2 | dst | src0 | ssrc1/src1 | src2 |
+------------+------+-----+------+------------+------+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (forms #64, #76) `ssrc1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 53 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i1`, 8
  `r0`…`i3`, 9 `o0`…`o127`, 10 `pa0`…`pa127`, 11 `sa0`…`sa127`, 12 `r[il + #0]`…`sa[ih + #15]`, 13
  `c0`…`g63`, 14 `#0x00000000`…`#0x0000007F`, 15 `i0`…`i127`.
- (forms #89, #96, #110) `ssrc1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 53 → v[10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i1`, 8
  `r0`…`i1.flt16.2`, 9 `o0`…`o31.flt16.2`, 10 `pa0`…`pa31.flt16.2`, 11 `sa0`…`sa31.flt16.2`, 12
  `r[il + #0]`…`sa[ih + #7].flt16`, 13 `c0`…`g63`, 14 `#0x00000000`…`#0x0000007F`, 15
  `i0`…`i31.flt16.2`.
- `pred` — as in layout 1.
- (form #64) `op` — as in layout 1.
- (forms #76, #110) `op` — as in layout 1.
- (form #89) `op` — as in layout 1.
- (form #96) `op` — as in layout 1.
- `dst` — as in layout 1.
- (forms #64, #76) `src0` — as in layout 1.
- (forms #89, #96, #110) `src0` — as in layout 1.
- (forms #64, #76) `src1` — as in layout 1.
- (form #89) `src1` — as in layout 1.
- (forms #96, #110) `src1` — as in layout 1.
- (forms #64, #76) `src2` — as in layout 1.
- (form #89) `src2` — as in layout 1.
- (forms #96, #110) `src2` — as in layout 1.

Layout 6 of 18 (forms #65, #77, #90, #97, #111):

```
 63   59 58   57  56 55   54  53      52 50 49     48
+-------+----+------+----+---+-------+-----+------+------------+
| 11011 | op | pred | op | ? | ssrc2 |  op | src1 | ssrc2/src2 |
+-------+----+------+----+---+-------+-----+------+------------+
 47 46 45     44 43 42  41 38 37     36     35     34     33 32
+-----+------+-----+---+-----+------+------+------+------+-----+
|  10 | sreg |  op | - | sop | sreg | src1 | src2 | src0 | dst |
+-----+------+-----+---+-----+------+------+------+------+-----+
 31  30 29        28 27 21 20  14 13   7 6          0
+------+------------+-----+------+------+------------+
| src1 | ssrc2/src2 | dst | src0 | src1 | ssrc2/src2 |
+------+------------+-----+------+------+------------+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (forms #65, #77) `ssrc2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 53 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i2`, 8
  `r0`…`i3`, 9 `o0`…`o127`, 10 `pa0`…`pa127`, 11 `sa0`…`sa127`, 12 `r[il + #0]`…`sa[ih + #15]`, 13
  `c0`…`g63`, 14 `#0x00000000`…`#0x0000007F`, 15 `i0`…`i127`.
- (forms #90, #97, #111) `ssrc2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 53 → v[10]. A
  register operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0-7 `i2`, 8
  `r0`…`i1.flt16.2`, 9 `o0`…`o31.flt16.2`, 10 `pa0`…`pa31.flt16.2`, 11 `sa0`…`sa31.flt16.2`, 12
  `r[il + #0]`…`sa[ih + #7].flt16`, 13 `c0`…`g63`, 14 `#0x00000000`…`#0x0000007F`, 15
  `i0`…`i31.flt16.2`.
- `pred` — as in layout 1.
- (form #65) `op` — as in layout 1.
- (forms #77, #111) `op` — as in layout 1.
- (form #90) `op` — as in layout 1.
- (form #97) `op` — as in layout 1.
- `dst` — as in layout 1.
- (forms #65, #77) `src0` — as in layout 1.
- (forms #90, #97, #111) `src0` — as in layout 1.
- (forms #65, #77) `src1` — as in layout 1.
- (form #90) `src1` — as in layout 1.
- (forms #97, #111) `src1` — as in layout 1.
- (forms #65, #77) `src2` — as in layout 1.
- (form #90) `src2` — as in layout 1.
- (forms #97, #111) `src2` — as in layout 1.

Layout 7 of 18 (forms #66, #98, #99):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+-------+
| 11011 | op | pred | op | ? | ssrc2 |  op | ssrc1/src1 | ssrc2 |
+-------+----+------+----+---+-------+-----+------------+-------+
 47   46 45      44 43 42      41 38 37      36     35  34      33 32
+-------+-------+-----+-------+-----+-------+------+---+-------+-----+
| ssrc1 | ssrc0 |  op | ssrc2 | sop | ssrc0 | src1 | - | ssrc0 | dst |
+-------+-------+-----+-------+-----+-------+------+---+-------+-----+
 31        30 29   28 27 21 20   14 13         7 6     0
+------------+-------+-----+-------+------------+-------+
| ssrc1/src1 | ssrc2 | dst | ssrc0 | ssrc1/src1 | ssrc2 |
+------------+-------+-----+-------+------------+-------+
```

- (form #66) `sop` — as in layout 1.
- (form #98) `sop` — as in layout 1.
- (form #99) `sop` — as in layout 1.
- `ssrc0` — as in layout 1.
- (form #66) `ssrc1` — as in layout 1.
- (form #98) `ssrc1` — as in layout 1.
- (form #99) `ssrc1` — as in layout 1.
- (form #66) `ssrc2` — as in layout 1.
- (form #98) `ssrc2` — as in layout 1.
- (form #99) `ssrc2` — as in layout 1.
- `pred` — as in layout 1.
- `op` — bits 51:50 → v[1:0], 58 → v[2], 55 → v[3], 52 → v[4], 43 → v[5], 44 → v[6]. Values: 1
  `mov`, 9 `mov.skipinv`, 17 `mov.syncs`, 25 `mov.skipinv.syncs`, 33 `mov.nosched`, 41
  `mov.skipinv.nosched`, 49 `mov.syncs.nosched`, 57 `mov.skipinv.syncs.nosched`, 65 `mov.repeat2`,
  73 `mov.skipinv.repeat2`, 81 `mov.syncs.repeat2`, 89 `mov.skipinv.syncs.repeat2`, 97
  `mov.nosched.repeat2`, 105 `mov.skipinv.nosched.repeat2`, 113 `mov.syncs.nosched.repeat2`, 121
  `mov.skipinv.syncs.nosched.repeat2`. Other values do not decode.
- `dst` — as in layout 1.
- (form #66) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 36 → v[10]. A register operand,
  v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2
  `pa0`…`pa127`, 3 `sa0`…`sa127`, 4 `r[il + #0]`…`sa[ih + #15]`, 5 `c0`…`g63`, 6
  `#0x00000000`…`#0x0000007F`, 7 `i0`…`i127`, 8 `r0.neg`…`i3.neg`, 9 `o0.neg`…`o127.neg`, 10
  `pa0.neg`…`pa127.neg`, 11 `sa0.neg`…`sa127.neg`, 12 `r[il + #0].neg`…`sa[ih + #15].neg`, 13
  `c0.neg`…`g63.neg`, 14 `#0x00000000.neg`…`#0x0000007F.neg`, 15 `i0.neg`…`i127.neg`.
- (forms #98, #99) `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9], 36 → v[10]. A register
  operand, v = bank·128 + number (the bank bits may carry modifiers). By bank: 0 `r0`…`i1.flt16.2`,
  1 `o0`…`o31.flt16.2`, 2 `pa0`…`pa31.flt16.2`, 3 `sa0`…`sa31.flt16.2`, 4 `r[il + #0]`…`sa[ih +
  #7].flt16`, 5 `c0`…`g63`, 6 `#0x00000000`…`#0x0000007F`, 7 `i0`…`i31.flt16.2`, 8
  `r0.neg`…`i1.neg.flt16.2`, 9 `o0.neg`…`o31.neg.flt16.2`, 10 `pa0.neg`…`pa31.neg.flt16.2`, 11
  `sa0.neg`…`sa31.neg.flt16.2`, 12 `r[il + #0].neg`…`sa[ih + #7].neg.flt16`, 13 `c0.neg`…`g63.neg`,
  14 `#0x00000000.neg`…`#0x0000007F.neg`, 15 `i0.neg`…`i31.neg.flt16.2`.

Layout 8 of 18 (forms #67, #100):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+-------+
| 11011 | op | pred | op | ? | ssrc2 |  op | ssrc1/src1 | ssrc2 |
+-------+----+------+----+---+-------+-----+------------+-------+
 47   46 45     44 43 42      41 38 37     36     35 34 33 32
+-------+------+-----+-------+-----+------+------+-----+-----+
| ssrc1 | sreg |  op | ssrc2 | sop | sreg | src1 |  -  | dst |
+-------+------+-----+-------+-----+------+------+-----+-----+
 31        30 29   28 27 21 20 14 13         7 6     0
+------------+-------+-----+-----+------------+-------+
| ssrc1/src1 | ssrc2 | dst |  -  | ssrc1/src1 | ssrc2 |
+------------+-------+-----+-----+------------+-------+
```

- `sop` — as in layout 2.
- `sreg` — as in layout 2.
- (form #67) `ssrc1` — as in layout 1.
- (form #100) `ssrc1` — as in layout 1.
- (form #67) `ssrc2` — as in layout 1.
- (form #100) `ssrc2` — as in layout 1.
- `pred` — as in layout 1.
- `op` — as in layout 7.
- `dst` — as in layout 1.
- (form #67) `src1` — as in layout 7.
- (form #100) `src1` — as in layout 7.

Layout 9 of 18 (forms #68, #101):

```
 63   59 58   57  56 55   54  53  52 50 49           48
+-------+----+------+----+---+---+-----+------------+---+
| 11011 | op | pred | op | ? | - |  op | ssrc1/src1 | - |
+-------+----+------+----+---+---+-----+------------+---+
 47   46 45     44 43 42  41 38 37     36     35 34 33 32
+-------+------+-----+---+-----+------+------+-----+-----+
| ssrc1 | sreg |  op | - | sop | sreg | src1 |  -  | dst |
+-------+------+-----+---+-----+------+------+-----+-----+
 31        30 29 28 27 21 20 14 13         7 6 0
+------------+-----+-----+-----+------------+---+
| ssrc1/src1 |  -  | dst |  -  | ssrc1/src1 | - |
+------------+-----+-----+-----+------------+---+
```

- `sop` — as in layout 3.
- `sreg` — as in layout 2.
- (form #68) `ssrc1` — as in layout 1.
- (form #101) `ssrc1` — as in layout 1.
- `pred` — as in layout 1.
- `op` — as in layout 7.
- `dst` — as in layout 1.
- (form #68) `src1` — as in layout 7.
- (form #101) `src1` — as in layout 7.

Layout 10 of 18 (forms #69, #102):

```
 63   59 58   57  56 55   54  53      52 50 49     48
+-------+----+------+----+---+-------+-----+------+---+
| 11011 | op | pred | op | ? | ssrc0 |  op | src1 | - |
+-------+----+------+----+---+-------+-----+------+---+
 47 46 45     44 43 42  41 38 37     36     35  34      33 32
+-----+------+-----+---+-----+------+------+---+-------+-----+
|  00 | sreg |  op | - | sop | sreg | src1 | - | ssrc0 | dst |
+-----+------+-----+---+-----+------+------+---+-------+-----+
 31  30 29 28 27 21 20   14 13   7 6 0
+------+-----+-----+-------+------+---+
| src1 |  -  | dst | ssrc0 | src1 | - |
+------+-----+-----+-------+------+---+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (form #69) `ssrc0` — as in layout 4.
- (form #102) `ssrc0` — as in layout 4.
- `pred` — as in layout 1.
- `op` — as in layout 7.
- `dst` — as in layout 1.
- (form #69) `src1` — as in layout 7.
- (form #102) `src1` — as in layout 7.

Layout 11 of 18 (forms #70, #103):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+---+
| 11011 | op | pred | op | ? | ssrc1 |  op | ssrc1/src1 | - |
+-------+----+------+----+---+-------+-----+------------+---+
 47 46 45     44 43 42  41 38 37     36     35 34 33 32
+-----+------+-----+---+-----+------+------+-----+-----+
|  01 | sreg |  op | - | sop | sreg | src1 |  -  | dst |
+-----+------+-----+---+-----+------+------+-----+-----+
 31        30 29 28 27 21 20 14 13         7 6 0
+------------+-----+-----+-----+------------+---+
| ssrc1/src1 |  -  | dst |  -  | ssrc1/src1 | - |
+------------+-----+-----+-----+------------+---+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (form #70) `ssrc1` — as in layout 5.
- (form #103) `ssrc1` — as in layout 5.
- `pred` — as in layout 1.
- `op` — as in layout 7.
- `dst` — as in layout 1.
- (form #70) `src1` — as in layout 7.
- (form #103) `src1` — as in layout 7.

Layout 12 of 18 (forms #71, #104):

```
 63   59 58   57  56 55   54  53      52 50 49     48
+-------+----+------+----+---+-------+-----+------+-------+
| 11011 | op | pred | op | ? | ssrc2 |  op | src1 | ssrc2 |
+-------+----+------+----+---+-------+-----+------+-------+
 47 46 45     44 43 42  41 38 37     36     35 34 33 32
+-----+------+-----+---+-----+------+------+-----+-----+
|  10 | sreg |  op | - | sop | sreg | src1 |  -  | dst |
+-----+------+-----+---+-----+------+------+-----+-----+
 31  30 29   28 27 21 20 14 13   7 6     0
+------+-------+-----+-----+------+-------+
| src1 | ssrc2 | dst |  -  | src1 | ssrc2 |
+------+-------+-----+-----+------+-------+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (form #71) `ssrc2` — as in layout 6.
- (form #104) `ssrc2` — as in layout 6.
- `pred` — as in layout 1.
- `op` — as in layout 7.
- `dst` — as in layout 1.
- (form #71) `src1` — as in layout 7.
- (form #104) `src1` — as in layout 7.

Layout 13 of 18 (forms #78, #112, #113):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+------------+
| 11011 | op | pred | op | ? | ssrc2 |  op | ssrc1/src1 | ssrc2/src2 |
+-------+----+------+----+---+-------+-----+------------+------------+
 47   46 45      44 43 42      41 38 37      36     35     34      33 32
+-------+-------+-----+-------+-----+-------+------+------+-------+-----+
| ssrc1 | ssrc0 |  op | ssrc2 | sop | ssrc0 | src1 | src2 | ssrc0 | dst |
+-------+-------+-----+-------+-----+-------+------+------+-------+-----+
 31        30 29        28 27 21 20   14 13         7 6          0
+------------+------------+-----+-------+------------+------------+
| ssrc1/src1 | ssrc2/src2 | dst | ssrc0 | ssrc1/src1 | ssrc2/src2 |
+------------+------------+-----+-------+------------+------------+
```

- (form #78) `sop` — as in layout 1.
- (form #112) `sop` — as in layout 1.
- (form #113) `sop` — as in layout 1.
- `ssrc0` — as in layout 1.
- (form #78) `ssrc1` — as in layout 1.
- (form #112) `ssrc1` — as in layout 1.
- (form #113) `ssrc1` — as in layout 1.
- (form #78) `ssrc2` — as in layout 1.
- (form #112) `ssrc2` — as in layout 1.
- (form #113) `ssrc2` — as in layout 1.
- `pred` — as in layout 1.
- `op` — bits 51:50 → v[1:0], 58 → v[2], 55 → v[3], 52 → v[4], 43 → v[5], 44 → v[6]. Values: 7
  `fssq`, 15 `fssq.skipinv`, 23 `fssq.syncs`, 31 `fssq.skipinv.syncs`, 39 `fssq.nosched`, 47
  `fssq.skipinv.nosched`, 55 `fssq.syncs.nosched`, 63 `fssq.skipinv.syncs.nosched`, 71
  `fssq.repeat2`, 79 `fssq.skipinv.repeat2`, 87 `fssq.syncs.repeat2`, 95
  `fssq.skipinv.syncs.repeat2`, 103 `fssq.nosched.repeat2`, 111 `fssq.skipinv.nosched.repeat2`, 119
  `fssq.syncs.nosched.repeat2`, 127 `fssq.skipinv.syncs.nosched.repeat2`. Other values do not
  decode.
- `dst` — as in layout 1.
- (form #78) `src1` — as in layout 1.
- (forms #112, #113) `src1` — as in layout 1.
- (form #78) `src2` — as in layout 1.
- (forms #112, #113) `src2` — as in layout 1.

Layout 14 of 18 (forms #79, #114):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+------------+
| 11011 | op | pred | op | ? | ssrc2 |  op | ssrc1/src1 | ssrc2/src2 |
+-------+----+------+----+---+-------+-----+------------+------------+
 47   46 45     44 43 42      41 38 37     36     35     34  33 32
+-------+------+-----+-------+-----+------+------+------+---+-----+
| ssrc1 | sreg |  op | ssrc2 | sop | sreg | src1 | src2 | - | dst |
+-------+------+-----+-------+-----+------+------+------+---+-----+
 31        30 29        28 27 21 20 14 13         7 6          0
+------------+------------+-----+-----+------------+------------+
| ssrc1/src1 | ssrc2/src2 | dst |  -  | ssrc1/src1 | ssrc2/src2 |
+------------+------------+-----+-----+------------+------------+
```

- `sop` — as in layout 2.
- `sreg` — as in layout 2.
- (form #79) `ssrc1` — as in layout 1.
- (form #114) `ssrc1` — as in layout 1.
- (form #79) `ssrc2` — as in layout 1.
- (form #114) `ssrc2` — as in layout 1.
- `pred` — as in layout 1.
- `op` — as in layout 13.
- `dst` — as in layout 1.
- (form #79) `src1` — as in layout 1.
- (form #114) `src1` — as in layout 1.
- (form #79) `src2` — as in layout 1.
- (form #114) `src2` — as in layout 1.

Layout 15 of 18 (forms #80, #115):

```
 63   59 58   57  56 55   54  53  52 50 49           48
+-------+----+------+----+---+---+-----+------------+------+
| 11011 | op | pred | op | ? | - |  op | ssrc1/src1 | src2 |
+-------+----+------+----+---+---+-----+------------+------+
 47   46 45     44 43 42  41 38 37     36     35     34  33 32
+-------+------+-----+---+-----+------+------+------+---+-----+
| ssrc1 | sreg |  op | - | sop | sreg | src1 | src2 | - | dst |
+-------+------+-----+---+-----+------+------+------+---+-----+
 31        30 29  28 27 21 20 14 13         7 6    0
+------------+------+-----+-----+------------+------+
| ssrc1/src1 | src2 | dst |  -  | ssrc1/src1 | src2 |
+------------+------+-----+-----+------------+------+
```

- `sop` — as in layout 3.
- `sreg` — as in layout 2.
- (form #80) `ssrc1` — as in layout 1.
- (form #115) `ssrc1` — as in layout 1.
- `pred` — as in layout 1.
- `op` — as in layout 13.
- `dst` — as in layout 1.
- (form #80) `src1` — as in layout 1.
- (form #115) `src1` — as in layout 1.
- (form #80) `src2` — as in layout 1.
- (form #115) `src2` — as in layout 1.

Layout 16 of 18 (forms #81, #116):

```
 63   59 58   57  56 55   54  53      52 50 49     48
+-------+----+------+----+---+-------+-----+------+------+
| 11011 | op | pred | op | ? | ssrc0 |  op | src1 | src2 |
+-------+----+------+----+---+-------+-----+------+------+
 47 46 45     44 43 42  41 38 37     36     35     34      33 32
+-----+------+-----+---+-----+------+------+------+-------+-----+
|  00 | sreg |  op | - | sop | sreg | src1 | src2 | ssrc0 | dst |
+-----+------+-----+---+-----+------+------+------+-------+-----+
 31  30 29  28 27 21 20   14 13   7 6    0
+------+------+-----+-------+------+------+
| src1 | src2 | dst | ssrc0 | src1 | src2 |
+------+------+-----+-------+------+------+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (form #81) `ssrc0` — as in layout 4.
- (form #116) `ssrc0` — as in layout 4.
- `pred` — as in layout 1.
- `op` — as in layout 13.
- `dst` — as in layout 1.
- (form #81) `src1` — as in layout 1.
- (form #116) `src1` — as in layout 1.
- (form #81) `src2` — as in layout 1.
- (form #116) `src2` — as in layout 1.

Layout 17 of 18 (forms #82, #117):

```
 63   59 58   57  56 55   54  53      52 50 49           48
+-------+----+------+----+---+-------+-----+------------+------+
| 11011 | op | pred | op | ? | ssrc1 |  op | ssrc1/src1 | src2 |
+-------+----+------+----+---+-------+-----+------------+------+
 47 46 45     44 43 42  41 38 37     36     35     34  33 32
+-----+------+-----+---+-----+------+------+------+---+-----+
|  01 | sreg |  op | - | sop | sreg | src1 | src2 | - | dst |
+-----+------+-----+---+-----+------+------+------+---+-----+
 31        30 29  28 27 21 20 14 13         7 6    0
+------------+------+-----+-----+------------+------+
| ssrc1/src1 | src2 | dst |  -  | ssrc1/src1 | src2 |
+------------+------+-----+-----+------------+------+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (form #82) `ssrc1` — as in layout 5.
- (form #117) `ssrc1` — as in layout 5.
- `pred` — as in layout 1.
- `op` — as in layout 13.
- `dst` — as in layout 1.
- (form #82) `src1` — as in layout 1.
- (form #117) `src1` — as in layout 1.
- (form #82) `src2` — as in layout 1.
- (form #117) `src2` — as in layout 1.

Layout 18 of 18 (forms #83, #118):

```
 63   59 58   57  56 55   54  53      52 50 49     48
+-------+----+------+----+---+-------+-----+------+------------+
| 11011 | op | pred | op | ? | ssrc2 |  op | src1 | ssrc2/src2 |
+-------+----+------+----+---+-------+-----+------+------------+
 47 46 45     44 43 42  41 38 37     36     35     34  33 32
+-----+------+-----+---+-----+------+------+------+---+-----+
|  10 | sreg |  op | - | sop | sreg | src1 | src2 | - | dst |
+-----+------+-----+---+-----+------+------+------+---+-----+
 31  30 29        28 27 21 20 14 13   7 6          0
+------+------------+-----+-----+------+------------+
| src1 | ssrc2/src2 | dst |  -  | src1 | ssrc2/src2 |
+------+------------+-----+-----+------+------------+
```

- `sop` — as in layout 4.
- `sreg` — as in layout 2.
- (form #83) `ssrc2` — as in layout 6.
- (form #118) `ssrc2` — as in layout 6.
- `pred` — as in layout 1.
- `op` — as in layout 13.
- `dst` — as in layout 1.
- (form #83) `src1` — as in layout 1.
- (form #118) `src1` — as in layout 1.
- (form #83) `src2` — as in layout 1.
- (form #118) `src2` — as in layout 1.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 24 of 59 forms have semantics (not
yet: #84, #85, #86, #87, #88, #89, #90, #91, #92, #93, #94, #95, #96, #97, #98, #99, #100, #101,
#102, #103, #104, #105, #106, #107, #108, #109, #110, #111, #112, #113, #114, #115, #116, #117,
#118). The probe vectors that test them: 472 match the hardware (32 with near values, each within
the bound the hardware was measured to), 50 stop unsupported, 0 mismatch (`dual-issue` 115/0/0,
`dual-issue-conflicts` 50/10/0, `dual-issue-ima32` 34/0/0, `dual-issue-predicates` 60/0/0,
`dual-issue-shared` 14/0/0, `x-dual` 167/40/0, `x-dual2` 16/0/0, `x-dual3` 16/0/0); unsupported
mostly for dual issue fddp + fadd (13); dual issue fminmax + imaN (7); dual issue fadm + fmadN (1).

### 6.19 Loads and stores: `ld*`, `st*`, `eldd`, `eldq`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#29   class 29       mask F840000000000000  value E800000000000000  ldab ldad ldaw ldlb ldld ldlw
      ldtb ldtd ldtw
      <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
#30   class 29       mask F840030000000000  value E800010000000000  ldab ldad ldaw ldlb ldld ldlw
      ldtb ldtd ldtw
      <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
#31   class 29       mask F840030000000000  value E800020000000000  ldab ldad ldaw ldlb ldld ldlw
      ldtb ldtd ldtw
      <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
#32   class 30       mask F800000000000000  value F000000000000000  stab stad staw stlb stld stlw
      sttb sttd sttw
      <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
#33   class 30       mask F800030000000000  value F000010000000000  stab stad staw stlb stld stlw
      sttb sttd sttw
      <pred>st<op><flags> [<base>,<step><src1>], <src2>
#34   class 30       mask F800030000000000  value F000020000000000  stab stad staw stlb stld stlw
      sttb sttd sttw
      <pred>st<op><flags> [<base>,<src1><step>], <src2>
#123  class 29       mask F840000000000000  value E840000000000000  eldd eldq
      <pred>eld<op><flags> <dst>, [<base>, <src0>, #<imm>], <src2>, drc<drc>
```

Mnemonics: `eldd`, `eldq`, `ldab`, `ldad`, `ldaw`, `ldlb`, `ldld`, `ldlw`, `ldtb`, `ldtd`, `ldtw`,
`stab`, `stad`, `staw`, `stlb`, `stld`, `stlw`, `sttb`, `sttd`, `sttw`.

Modifiers: `.bpcache`, `.bypassl1`, `.fcfill`, `.fetchN`, `.noread`, `.nosched`, `.rangeenable`,
`.repeatN`, `.skipinv`, `.syncs`.

Layout 1 of 5 (form #29):

```
 63   59 58  56 55      54  53   51 50     49     48
+-------+------+-------+---+-------+------+------+-------+
| 11101 | pred | flags | 0 | flags | base | src1 | extra |
+-------+------+-------+---+-------+------+------+-------+
 47   44 43 42 41  40 39    38      37 36 35     34     33      32
+-------+-----+------+-----+-------+-----+------+------+-------+-----+
| flags |  op | mode | dst | extra |  op | sign | base | flags | drc |
+-------+-----+------+-----+-------+-----+------+------+-------+-----+
 31  30 29   28 27 21 20  14 13   7 6     0
+------+-------+-----+------+------+-------+
| src1 | extra | dst | base | src1 | extra |
+------+-------+-----+------+------+-------+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 37:36 → v[1:0], 43:42 → v[3:2]. Values: 0 `ad`, 1 `aw`, 2 `ab`, 4 `ld`, 5 `lw`, 6
  `lb`, 8 `td`, 9 `tw`, 10 `tb`. Other values do not decode.
- `flags` — bits 55 → v[0], 52 → v[1], 33 → v[2], 51 → v[3], 53 → v[4], 47:44 → v[8:5]. Flags,
  printed in this order. Value 0 prints `.fetch1`. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`,
  v[2] `.bpcache`, v[3] `.fcfill`, v[4] drops `.fetch1`, v[5] `.fetch2`, v[6] `.fetch3`, v[7]
  `.fetch5`, v[8] `.fetch9`.
- `dst` — bits 27:21 → v[6:0], 39 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`r127`, 1 `pa0`…`pa127`.
- `base` — bits 20:14 → v[6:0], 34 → v[7], 50 → v[8]. A register operand: the standard source 0
  (*Register operands*).
- `mode` — bits 41:40. Values: 0 (none), 3 (none). Other values do not decode.
- `sign` — bit 35. Values: 0 `+`, 1 `-`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `extra` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9], 38 → v[10]. An optional trailing operand:
  some values print nothing, the others a comma and the operand, for example 1024 `, r0`, 1025 `,
  r1`, 1151 `, i3`.
- `drc` — bit 32. A 1-bit number, printed in decimal.

Layout 2 of 5 (forms #30, #31):

```
 63   59 58  56 55      54  53   51 50     49     48
+-------+------+-------+---+-------+------+------+-------+
| 11101 | pred | flags | 0 | flags | base | src1 | extra |
+-------+------+-------+---+-------+------+------+-------+
 47   44 43 42 41 40 39    38      37 36 35     34     33      32
+-------+-----+-----+-----+-------+-----+------+------+-------+-----+
| flags |  op |  ?? | dst | extra |  op | step | base | flags | drc |
+-------+-----+-----+-----+-------+-----+------+------+-------+-----+
 31  30 29   28 27 21 20  14 13   7 6     0
+------+-------+-----+------+------+-------+
| src1 | extra | dst | base | src1 | extra |
+------+-------+-----+------+------+-------+
```

- `pred` — as in layout 1.
- `op` — as in layout 1.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `base` — as in layout 1.
- `step` — bit 35. Values: 0 `++`, 1 `--`.
- `src1` — as in layout 1.
- `extra` — as in layout 1.
- `drc` — as in layout 1.

Layout 3 of 5 (form #32):

```
 63   59 58  56 55   51 50     49     48
+-------+------+-------+------+------+------+
| 11110 | pred | flags | base | src1 | src2 |
+-------+------+-------+------+------+------+
 47   44 43 42 41  40 39  38      37 36 35     34     33   32
+-------+-----+------+---+-------+-----+------+------+-------+
| flags |  op | mode | - | flags |  op | sign | base | flags |
+-------+-----+------+---+-------+-----+------+------+-------+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 |  -  | base | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `op` — as in layout 1.
- `flags` — bits 55 → v[0], 52 → v[1], 54 → v[2], 33 → v[3], 51 → v[4], 32 → v[5], 38 → v[6], 53 →
  v[7], 47:44 → v[11:8]. Flags, printed in this order. Value 0 prints `.fetch1`. Each bit adds: v[0]
  `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.bpcache`, v[4] `.fcfill`, v[5] `.noread`, v[6]
  `.rangeenable`, v[7] drops `.fetch1`, v[8] `.fetch2`, v[9] `.fetch3`, v[10] `.fetch5`, v[11]
  `.fetch9`.
- `base` — as in layout 1.
- `mode` — as in layout 1.
- `sign` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 4 of 5 (forms #33, #34):

```
 63   59 58  56 55   51 50     49     48
+-------+------+-------+------+------+------+
| 11110 | pred | flags | base | src1 | src2 |
+-------+------+-------+------+------+------+
 47   44 43 42 41 40 39  38      37 36 35     34     33   32
+-------+-----+-----+---+-------+-----+------+------+-------+
| flags |  op |  ?? | - | flags |  op | step | base | flags |
+-------+-----+-----+---+-------+-----+------+------+-------+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 |  -  | base | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `op` — as in layout 1.
- `flags` — as in layout 3.
- `base` — as in layout 1.
- `step` — as in layout 2.
- `src1` — as in layout 1.
- `src2` — as in layout 3.

Layout 5 of 5 (form #123):

```
 63   59 58  56 55      54  53   52 51 48 47   44 43 37 36    35   34     33      32
+-------+------+-------+---+-------+-----+-------+-----+-----+----+------+-------+-----+
| 11101 | pred | flags | 1 | flags | imm | flags | imm | dst | op | src0 | flags | drc |
+-------+------+-------+---+-------+-----+-------+-----+-----+----+------+-------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| base | src2 | dst | src0 | base | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `op` — bit 35. Values: 0 `d`, 1 `q`.
- `flags` — bits 55 → v[0], 52 → v[1], 33 → v[2], 53 → v[3], 47:44 → v[7:4]. Flags, printed in this
  order. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.bpcache`, v[3] `.fcfill`, v[7:4] a
  count, `.fetch2`…`.fetch16`.
- `dst` — bits 27:21 → v[6:0], 36 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`r127`, 1 `pa0`…`pa127`.
- `base` — bits 13:7 → v[6:0], 31:30 → v[8:7]. A register operand, v = bank·128 + number (the bank
  bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- `imm` — bits 43:37 → v[6:0], 51:48 → v[10:7]. An 11-bit number, printed in decimal.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7]. A register operand, v = bank·128 + number (the bank
  bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `sa0`…`sa127`.
- `drc` — as in layout 1.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 6 of 7 forms have semantics (not
yet: #123). The probe vectors that test them: 555 match the hardware, 28 stop unsupported, 0
mismatch (`cache-control` 100/0/0, `ldad-reg-off` 224/0/0, `ldad-reg-off-fetch` 0/28/0,
`ldad-reg-off-neg` 56/0/0, `stad-reg-off` 90/0/0, `stad-reg-off-unaligned` 45/0/0, `x-cache2`
40/0/0); unsupported mostly for flag .fetchN with the MOE state unknown (28).

### 6.20 Texture sampling: `smp1d`, `smp2d`, `smp3d`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#119  class 28       mask F820000000000000  value E000000000000000  smp1d smp2d smp3d
      <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, drc<drc>
#120  class 28       mask F820000000000000  value E020000000000000  smp1d smp2d smp3d
      <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, drc<drc>
#121  class 28       mask F820000000000000  value E000000000000000  smp1dbias smp1dgrad smp1dreplace
      smp2dbias smp2dgrad smp2dreplace smp3dbias smp3dgrad smp3dreplace
      <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
#122  class 28       mask F820000000000000  value E020000000000000  smp1dbias smp1dgrad smp1dreplace
      smp2dbias smp2dgrad smp2dreplace smp3dbias smp3dgrad smp3dreplace
      <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
```

Mnemonics: `smp1d`, `smp1dbias`, `smp1dgrad`, `smp1dreplace`, `smp2d`, `smp2dbias`, `smp2dgrad`,
`smp2dreplace`, `smp3d`, `smp3dbias`, `smp3dgrad`, `smp3dreplace`.

Modifiers: `.nosched`, `.pcf`, `.pcff16`, `.repeatN`, `.rsd`, `.sinf`, `.skipinv`, `.syncs`.

Layout 1 of 2 (forms #119, #120):

```
 63   59 58  56 55    54 53  52       51  50     49     48
+-------+------+--------+---+--------+---+------+------+---+
| 11100 | pred | flags1 | ? | flags1 | - | src0 | src1 | - |
+-------+------+--------+---+--------+---+------+------+---+
 47    46 45   44 43 40 39    38    37 36    35 34     33 32
+--------+-------+-----+-----+--------+--------+------+-----+
| flags1 | wmask |  op | dst | flags1 | flags2 | src0 | drc |
+--------+-------+-----+-----+--------+--------+------+-----+
 31  30 29 28 27 21 20  14 13   7 6 0
+------+-----+-----+------+------+---+
| src1 |  -  | dst | src0 | src1 | - |
+------+-----+-----+------+------+---+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `op` — bits 43:42 → v[1:0], 41:40 → v[3:2]. Values: 0-2 `1d`…`3d`. Other values do not decode.
- `flags1` — bits 55 → v[0], 52 → v[1], 54 → v[2], 38:37 → v[4:3], 47:46 → v[6:5]. Flags, printed in
  this order. Each bit adds: v[0] `.skipinv`, v[1] `.syncs`, v[2] `.nosched`, v[3] `.pcf`, v[4]
  `.pcff16`, v[5] `.sinf`, v[6] `.rsd`.
- (form #119) `wmask` — bits 45:44. Values: 0 `.`, 1 (none), 2 `.y`, 3 `.xy`.
- (form #120) `wmask` — bits 45:44. Values: 0 (none), 1-3 `.repeat2`…`.repeat4`.
- `dst` — bits 27:21 → v[6:0], 39 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`r127`, 1 `pa0`…`pa127`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 50 → v[8]. A register operand: the standard source 0
  (*Register operands*).
- `flags2` — bits 36:35. Flags, printed in this order. Each bit adds: v[0] `.flt16`, v[1] `.c10`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `drc` — bits 33:32. A 2-bit number, printed in decimal.

Layout 2 of 2 (forms #121, #122):

```
 63   59 58  56 55    54 53  52       51  50     49     48
+-------+------+--------+---+--------+---+------+------+------+
| 11100 | pred | flags1 | ? | flags1 | - | src0 | src1 | src2 |
+-------+------+--------+---+--------+---+------+------+------+
 47    46 45   44 43 40 39    38    37 36    35 34     33 32
+--------+-------+-----+-----+--------+--------+------+-----+
| flags1 | wmask |  op | dst | flags1 | flags2 | src0 | drc |
+--------+-------+-----+-----+--------+--------+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `op` — bits 43:42 → v[1:0], 41:40 → v[3:2]. Values: 4-6 `1dbias`…`3dbias`, 8-10
  `1dreplace`…`3dreplace`, 12-14 `1dgrad`…`3dgrad`. Other values do not decode.
- `flags1` — as in layout 1.
- (form #121) `wmask` — as in layout 1.
- (form #122) `wmask` — as in layout 1.
- `dst` — as in layout 1.
- `src0` — as in layout 1.
- `flags2` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `drc` — as in layout 1.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 4 of 4 forms have semantics. The
probe vectors that test them: 574 match the hardware, 152 stop unsupported, 0 mismatch
(`smp-address` 40/0/0, `smp-address-border` 0/4/0, `smp-bilinear` 21/0/0, `smp-convert` 48/0/0,
`smp-filter-modes` 8/0/0, `smp-formats` 57/0/0, `smp-formats-other` 0/40/0, `smp-lod` 3/8/0,
`smp-pcf` 28/0/0, `smp-point` 65/0/0, `smp-twiddled` 16/0/0, `x-smp` 89/19/0, `x-smp2` 75/57/0,
`x-smp3` 124/24/0); unsupported mostly for texture format N (64); TN bits NxN (21); smpNdreplace
with a LOD value of NxN (12).

### 6.21 Register file access: `ldr`, `str`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#35   class 31       mask FF79000030000000  value FE21000020000000  ldr
      ldr<flags> <dst>, #<imm>, drc<drc>
#36   class 31       mask FF78000000000000  value FE20000000000000  ldr
      ldr<flags> <dst>, <src2>, drc<drc>
#37   class 31       mask FF79000030000000  value FE29000020000000  str
      str<flags> #<imm>, <src1>
#38   class 31       mask FF78000000000000  value FE28000000000000  str
      str<flags> <src2>, <src1>
#144  class 31       mask FF39000030000000  value FE21000020000000  ldr
      ldr<flags> <dst>, #<imm>, drc<drc>
#145  class 31       mask FF38000000000000  value FE20000000000000  ldr
      ldr<flags> <dst>, <src2>, drc<drc>
#146  class 31       mask FF39000030000000  value FE29000020000000  str
      str<flags> #<imm>, <src1>
#147  class 31       mask FF38000000000000  value FE28000000000000  str
      str<flags> <src2>, <src1>
```

Mnemonics: `ldr`, `str`.

Modifiers: `.nosched`, `.skipinv`.

Layout 1 of 8 (form #35):

```
 63      56 55      54  51 50 49 48  47 44 43      42 40 39    38 34 33 32
+----------+-------+------+-----+---+-----+-------+-----+-----+-----+-----+
| 11111110 | flags | 0100 |  -  | 1 |  -  | flags |  -  | dst |  -  | drc |
+----------+-------+------+-----+---+-----+-------+-----+-----+-----+-----+
 31 30 29 28 27 21 20 14 13 7 6   0
+-----+-----+-----+-----+----+-----+
|  -  |  10 | dst | imm | -  | imm |
+-----+-----+-----+-----+----+-----+
```

- `flags` — bits 55 → v[0], 43 → v[1]. Flags, printed in this order. Each bit adds: v[0] `.skipinv`,
  v[1] `.nosched`.
- `dst` — bits 27:21 → v[6:0], 39 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`r127`, 1 `pa0`…`pa127`.
- `imm` — bits 6:0 → v[6:0], 20:14 → v[13:7]. A 14-bit number, printed in decimal.
- `drc` — bits 33:32. A 2-bit number, printed in decimal.

Layout 2 of 8 (form #36):

```
 63      56 55      54  51 50 49 48     47 44 43      42 40 39    38 34 33 32
+----------+-------+------+-----+------+-----+-------+-----+-----+-----+-----+
| 11111110 | flags | 0100 |  -  | src2 |  -  | flags |  -  | dst |  -  | drc |
+----------+-------+------+-----+------+-----+-------+-----+-----+-----+-----+
 31 30 29  28 27 21 20 7 6    0
+-----+------+-----+----+------+
|  -  | src2 | dst | -  | src2 |
+-----+------+-----+----+------+
```

- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `drc` — as in layout 1.

Layout 3 of 8 (form #37):

```
 63      56 55      54  51 50  49     48  47 44 43      42 32
+----------+-------+------+---+------+---+-----+-------+-----+
| 11111110 | flags | 0101 | - | src1 | 1 |  -  | flags |  -  |
+----------+-------+------+---+------+---+-----+-------+-----+
 31  30 29 28 27 21 20 14 13   7 6   0
+------+-----+-----+-----+------+-----+
| src1 |  10 |  -  | imm | src1 | imm |
+------+-----+-----+-----+------+-----+
```

- `flags` — as in layout 1.
- `imm` — as in layout 1.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 4 of 8 (form #38):

```
 63      56 55      54  51 50  49     48     47 44 43      42 32
+----------+-------+------+---+------+------+-----+-------+-----+
| 11111110 | flags | 0101 | - | src1 | src2 |  -  | flags |  -  |
+----------+-------+------+---+------+------+-----+-------+-----+
 31  30 29  28 27 14 13   7 6    0
+------+------+-----+------+------+
| src1 | src2 |  -  | src1 | src2 |
+------+------+-----+------+------+
```

- `flags` — as in layout 1.
- `src2` — as in layout 2.
- `src1` — as in layout 3.

Layout 5 of 8 (form #144):

```
 63      56 55      54  53 51 50 49 48  47 44 43      42 40 39    38 34 33 32
+----------+-------+---+-----+-----+---+-----+-------+-----+-----+-----+-----+
| 11111110 | flags | - | 100 |  -  | 1 |  -  | flags |  -  | dst |  -  | drc |
+----------+-------+---+-----+-----+---+-----+-------+-----+-----+-----+-----+
 31 30 29 28 27 21 20 14 13 7 6   0
+-----+-----+-----+-----+----+-----+
|  -  |  10 | dst | imm | -  | imm |
+-----+-----+-----+-----+----+-----+
```

- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `imm` — as in layout 1.
- `drc` — as in layout 1.

Layout 6 of 8 (form #145):

```
 63      56 55      54  53 51 50 49 48     47 44 43      42 40 39    38 34 33 32
+----------+-------+---+-----+-----+------+-----+-------+-----+-----+-----+-----+
| 11111110 | flags | - | 100 |  -  | src2 |  -  | flags |  -  | dst |  -  | drc |
+----------+-------+---+-----+-----+------+-----+-------+-----+-----+-----+-----+
 31 30 29  28 27 21 20 7 6    0
+-----+------+-----+----+------+
|  -  | src2 | dst | -  | src2 |
+-----+------+-----+----+------+
```

- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `src2` — as in layout 2.
- `drc` — as in layout 1.

Layout 7 of 8 (form #146):

```
 63      56 55      54  53 51 50  49     48  47 44 43      42 32
+----------+-------+---+-----+---+------+---+-----+-------+-----+
| 11111110 | flags | - | 101 | - | src1 | 1 |  -  | flags |  -  |
+----------+-------+---+-----+---+------+---+-----+-------+-----+
 31  30 29 28 27 21 20 14 13   7 6   0
+------+-----+-----+-----+------+-----+
| src1 |  10 |  -  | imm | src1 | imm |
+------+-----+-----+-----+------+-----+
```

- `flags` — as in layout 1.
- `imm` — as in layout 1.
- `src1` — as in layout 3.

Layout 8 of 8 (form #147):

```
 63      56 55      54  53 51 50  49     48     47 44 43      42 32
+----------+-------+---+-----+---+------+------+-----+-------+-----+
| 11111110 | flags | - | 101 | - | src1 | src2 |  -  | flags |  -  |
+----------+-------+---+-----+---+------+------+-----+-------+-----+
 31  30 29  28 27 14 13   7 6    0
+------+------+-----+------+------+
| src1 | src2 |  -  | src1 | src2 |
+------+------+-----+------+------+
```

- `flags` — as in layout 1.
- `src2` — as in layout 2.
- `src1` — as in layout 3.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 8 of 8 forms have semantics. The
probe vectors that test them: 0 match the hardware, 70 stop unsupported, 0 mismatch
(`ldr-index-mask` 0/70/0); unsupported mostly for no SGX registers on this bus (70).

### 6.22 Flow control: `ba`, `br`, `lapc`, `bexception`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#12   class 31       mask F87801C000000000  value F800000000000000  ba
      <pred>ba<flags> #<imm>
#13   class 31       mask F87801C000000000  value F800004000000000  br
      <pred>br<flags> <imm>
#14   class 31       mask F878018000000000  value F808000000000000  bexception
      bexception<nosched>
#124  class 31       mask F87001C000000000  value F800008000000000  lapc
      <pred>lapc<flags>
```

Mnemonics: `ba`, `bexception`, `br`, `lapc`.

Modifiers: `.mon`, `.nosched`, `.savelink`, `.synce`, `.syncent`.

Layout 1 of 3 (forms #12, #13):

```
 63   59 58  56 55      54  51 50 45 44   41 40 38 37 32
+-------+------+-------+------+-----+-------+-----+-----+
| 11111 | pred | flags | 0000 |  -  | flags | 00? |  -  |
+-------+------+-------+------+-----+-------+-----+-----+
 31 18 17  0
+-----+-----+
|  -  | imm |
+-----+-----+
```

- `pred` — bits 58:56. Values: 0 (none), 1-4 `p0`…`p3`, 5 `!p0`, 6 `!p1`, 7 `Pn`.
- `flags` — bits 43 → v[0], 55 → v[1], 44 → v[2], 41 → v[3], 42 → v[4]. Flags, printed in this
  order. Each bit adds: v[0] `.nosched`, v[1] `.synce`, v[3] `.savelink`, v[4] `.mon`.
- (form #12) `imm` — bits 17:0. An 18-bit number, printed in hex.
- (form #13) `imm` — bits 17:0. An 18-bit value printed as a number by the field's own printer (the
  group's text says how), for example 0 `#0x00000000`, 1 `#0x00000001`, 131072 `-#0x00020000`,
  262143 `-#0x00000001`.

Layout 2 of 3 (form #14):

```
 63   59 58 55 54  51 50 44 43        42 41 40 39 38 32
+-------+-----+------+-----+---------+-----+-----+-----+
| 11111 |  -  | 0001 |  -  | nosched |  -  |  00 |  -  |
+-------+-----+------+-----+---------+-----+-----+-----+
 31 0
+----+
| -  |
+----+
```

- `nosched` — bit 43. Values: 0 (none), 1 `.nosched`.

Layout 3 of 3 (form #124):

```
 63   59 58  56 55      54 52 51 45 44   43 42 41 40 38 37 32
+-------+------+-------+-----+-----+-------+-----+-----+-----+
| 11111 | pred | flags | 000 |  -  | flags |  -  | 010 |  -  |
+-------+------+-------+-----+-----+-------+-----+-----+-----+
 31 0
+----+
| -  |
+----+
```

- `pred` — as in layout 1.
- `flags` — bits 43 → v[0], 55 → v[1], 44 → v[2]. Flags, printed in this order. Each bit adds: v[0]
  `.nosched`, v[1] `.synce`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 3 of 4 forms have semantics (not
yet: #14). The probe vectors that test them: 14 match the hardware, 0 stop unsupported, 0 mismatch
(`br-offset` 5/0/0, `x-m2` 5/0/0, `x-misc` 4/0/0).

### 6.23 Synchronisation and misc: `nop`, `wdf`, `idf`, `setm`, `wop`, `lock`, `release`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#5    class 31       mask FF7BF7C000000000  value F800014000000000  nop
      nop<syncs><nosched><end><tog><trigger>
#6    class 31       mask FFFFFFFEFFFFFFFF  value F820000000000000  idf
      idf drc<drc>, bif
#7    class 31       mask FFFFFFFEFFFFFFFF  value F920000000000000  wdf
      wdf drc<drc>
#127  class 31       mask F87001C000000000  value F800014000000000  nop
      nop<syncs><nosched><end><tog><trigger>
#139  class 31       mask FF30000000000000  value F820000000000000  idf
      idf<nosched> drc<drc>, <op>
#140  class 31       mask FF30000000000000  value F920000000000000  wdf
      wdf<nosched> drc<drc>
#141  class 31       mask FF30000000000000  value FA20000000000000  setm
      setm<nosched> #<imm>
#142  class 31       mask FF30000000000000  value FF20000000000000  wop
      wop<nosched><twopart> #<imm>
#143  class 31       mask FF30000000000080  value FD20000000000000  lock release
      <op><nosched> #<imm>
```

Mnemonics: `idf`, `lock`, `nop`, `release`, `setm`, `wdf`, `wop`.

Modifiers: `.end`, `.nosched`, `.syncs`, `.tog`, `.trigger`, `.twopart`.

Layout 1 of 8 (form #5):

```
 63      56 55      54  51 50    49    44 43        42   38 37 32
+----------+-------+------+-----+--------+---------+-------+-----+
| 11111000 | syncs | 0000 | end | 000000 | nosched | 00101 |  -  |
+----------+-------+------+-----+--------+---------+-------+-----+
 31 2 1         0
+----+---------+-----+
| -  | trigger | tog |
+----+---------+-----+
```

- `syncs` — bit 55. Values: 0 (none), 1 `.syncs`.
- `nosched` — bit 43. Values: 0 (none), 1 `.nosched`.
- `end` — bit 50. Values: 0 (none), 1 `.end`.
- `tog` — bit 0. Values: 0 (none), 1 `.tog`.
- `trigger` — bit 1. Values: 0 (none), 1 `.trigger`.

Layout 2 of 8 (forms #6, #7):

```
 63                             33 32
+---------------------------------+-----+
| 1111100?00100000000000000000000 | drc |
+---------------------------------+-----+
 31                               0
+----------------------------------+
| 00000000000000000000000000000000 |
+----------------------------------+
```

- `drc` — bit 32. A 1-bit number, printed in decimal.

Layout 3 of 8 (form #127):

```
 63   59 58 56 55      54 52 51  50    49 44 43        42 41 40 38 37 32
+-------+-----+-------+-----+---+-----+-----+---------+-----+-----+-----+
| 11111 |  -  | syncs | 000 | - | end |  -  | nosched |  -  | 101 |  -  |
+-------+-----+-------+-----+---+-----+-----+---------+-----+-----+-----+
 31 2 1         0
+----+---------+-----+
| -  | trigger | tog |
+----+---------+-----+
```

- `syncs` — as in layout 1.
- `nosched` — as in layout 1.
- `end` — as in layout 1.
- `tog` — as in layout 1.
- `trigger` — as in layout 1.

Layout 4 of 8 (form #139):

```
 63      56 55 54 53 52 51 48 47 46 45 44 43        42 34 33 32
+----------+-----+-----+-----+-----+-----+---------+-----+-----+
| 11111000 |  -  |  10 |  -  |  op |  -  | nosched |  -  | drc |
+----------+-----+-----+-----+-----+-----+---------+-----+-----+
 31 0
+----+
| -  |
+----+
```

- `nosched` — as in layout 1.
- `drc` — bits 33:32. A 2-bit number, printed in decimal.
- `op` — bits 47:46. Values: 0 `bif`, 1 `pixelbe`, 2 `reserved(=2)`, 3 `reserved(=3)`.

Layout 5 of 8 (form #140):

```
 63      56 55 54 53 52 51 44 43        42 34 33 32
+----------+-----+-----+-----+---------+-----+-----+
| 11111001 |  -  |  10 |  -  | nosched |  -  | drc |
+----------+-----+-----+-----+---------+-----+-----+
 31 0
+----+
| -  |
+----+
```

- `nosched` — as in layout 1.
- `drc` — as in layout 4.

Layout 6 of 8 (form #141):

```
 63      56 55 54 53 52 51 44 43        42 32
+----------+-----+-----+-----+---------+-----+
| 11111010 |  -  |  10 |  -  | nosched |  -  |
+----------+-----+-----+-----+---------+-----+
 31 4 3   0
+----+-----+
| -  | imm |
+----+-----+
```

- `nosched` — as in layout 1.
- `imm` — bits 3:0. A 4-bit number, printed in decimal.

Layout 7 of 8 (form #142):

```
 63      56 55 54 53 52 51 44 43        42 32
+----------+-----+-----+-----+---------+-----+
| 11111111 |  -  |  10 |  -  | nosched |  -  |
+----------+-----+-----+-----+---------+-----+
 31 4 3         2   0
+----+---------+-----+
| -  | twopart | imm |
+----+---------+-----+
```

- `nosched` — as in layout 1.
- `twopart` — bit 3. Values: 0 (none), 1 `.twopart`.
- `imm` — bits 2:0. A 3-bit number, printed in decimal.

Layout 8 of 8 (form #143):

```
 63      56 55 54 53 52 51 44 43        42 32
+----------+-----+-----+-----+---------+-----+
| 11111101 |  -  |  10 |  -  | nosched |  -  |
+----------+-----+-----+-----+---------+-----+
 31 8 7   6   4 3 1 0
+----+---+-----+---+----+
| -  | 0 | imm | - | op |
+----+---+-----+---+----+
```

- `op` — bit 0. Values: 0 `lock`, 1 `release`.
- `nosched` — as in layout 1.
- `imm` — bits 6:4. A 3-bit number, printed in decimal.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 9 forms have semantics (not
yet: #141, #142). The probe vectors that test them: 0 match the hardware, 56 stop unsupported, 0
mismatch (`sa-values` 0/56/0); unsupported mostly for reads saN before the program writes it (56).

### 6.24 MOE control: `smoa`, `smr`, `smlsi`, `smbo`, `imo`, `setfc`, `moest`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#128  class 31       mask FF38000000000000  value F818000000000000  smoa
      smoa<nosched> <src1>, <src2>
#129  class 31       mask FF38000000000000  value F918000000000000  smr
      smr<nosched> <src1>, <src2>
#130  class 31       mask FF38000000000000  value FA18000000000000  smlsi
      smlsi<nosched> <src1>, <src2>
#131  class 31       mask FF38000000000000  value FB18000000000000  smbo
      smbo<nosched> <src1>, <src2>
#132  class 31       mask FF38000000000000  value F810000000000000  smoa
      smoa<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>, <op1>, <op2>, <op3>, <op4>
#133  class 31       mask FF38000000000000  value F910000000000000  smr
      smr<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
#134  class 31       mask FF38000000000000  value FB10000000000000  smbo
      smbo<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
#135  class 31       mask FF38000000000000  value FA10000000000000  smlsi
      smlsi<nosched> <f1>, <f2>, <f3>, <f4><extra>, #<imm1>, #<imm2>, #<imm3>
#136  class 31       mask FF30000000000000  value FC10000000000000  imo
      imo<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
#137  class 31       mask FF30000000000000  value FD10000000000000  setfc
      setfc<nosched> #<imm1>, #<imm2>
#138  class 31       mask FF30000000000000  value FE10000000000000  moest
      moest<skipinv><nosched> <dst>, #<imm>
```

Mnemonics: `imo`, `moest`, `setfc`, `smbo`, `smlsi`, `smoa`, `smr`.

Modifiers: `.nosched`, `.skipinv`.

Layout 1 of 7 (forms #128, #129, #130, #131):

```
 63      56 55 54 53 51 50        49     48     47 32
+----------+-----+-----+---------+------+------+-----+
| 111110?? |  -  | 011 | nosched | src1 | src2 |  -  |
+----------+-----+-----+---------+------+------+-----+
 31  30 29  28 27 14 13   7 6    0
+------+------+-----+------+------+
| src1 | src2 |  -  | src1 | src2 |
+------+------+-----+------+------+
```

- `nosched` — bit 50. Values: 0 (none), 1 `.nosched`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 7 (form #132):

```
 63      56 55 54 53 51 50        49 48 47 46 45 44 43 42 41 40 39  32
+----------+-----+-----+---------+-----+-----+-----+-----+-----+------+
| 11111000 |  -  | 010 | nosched |  -  | op1 | op2 | op3 | op4 | imm1 |
+----------+-----+-----+---------+-----+-----+-----+-----+-----+------+
 31  30 29  20 19  10 9    0
+------+------+------+------+
| imm1 | imm2 | imm3 | imm4 |
+------+------+------+------+
```

- `nosched` — as in layout 1.
- `imm1` — bits 39:30. A 10-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 512 `-512`, 1023 `-1`.
- `imm2` — bits 29:20. A 10-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 512 `-512`, 1023 `-1`.
- `imm3` — bits 19:10. A 10-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 512 `-512`, 1023 `-1`.
- `imm4` — bits 9:0. A 10-bit value printed as a number by the field's own printer (the group's text
  says how), for example 0 `0`, 1 `1`, 512 `-512`, 1023 `-1`.
- `op1` — bits 47:46. Values: 0 `none`, 1 `repeat`, 2 `clamp`, 3 `mirror`.
- `op2` — bits 45:44. Values: 0 `none`, 1 `repeat`, 2 `clamp`, 3 `mirror`.
- `op3` — bits 43:42. Values: 0 `none`, 1 `repeat`, 2 `clamp`, 3 `mirror`.
- `op4` — bits 41:40. Values: 0 `none`, 1 `repeat`, 2 `clamp`, 3 `mirror`.

Layout 3 of 7 (forms #133, #134):

```
 63      56 55 54 53 51 50        49 48 47  36 35  32
+----------+-----+-----+---------+-----+------+------+
| 111110?1 |  -  | 010 | nosched |  -  | imm1 | imm2 |
+----------+-----+-----+---------+-----+------+------+
 31  24 23  12 11   0
+------+------+------+
| imm2 | imm3 | imm4 |
+------+------+------+
```

- `nosched` — as in layout 1.
- `imm1` — bits 47:36. A 12-bit number, printed in decimal.
- `imm2` — bits 35:24. A 12-bit number, printed in decimal.
- `imm3` — bits 23:12. A 12-bit number, printed in decimal.
- `imm4` — bits 11:0. A 12-bit number, printed in decimal.

Layout 4 of 7 (form #135):

```
 63      56 55 54 53 51 50        49 48
+----------+-----+-----+---------+-----+
| 11111010 |  -  | 010 | nosched |  -  |
+----------+-----+-----+---------+-----+
 47  44 43  40 39  36 35         34         33         32
+------+------+------+----------+----------+----------+----------+
| imm1 | imm2 | imm3 | f1/extra | f2/extra | f3/extra | f4/extra |
+------+------+------+----------+----------+----------+----------+
 31 24 23 16 15 8 7  0
+-----+-----+----+----+
|  f1 |  f2 | f3 | f4 |
+-----+-----+----+----+
```

- `nosched` — as in layout 1.
- `f1` — bits 31:24 → v[7:0], 35 → v[8]. 262 or more distinct texts, for example 0 `#0`, 1 `#1`, 2
  `#2`, 3 `#3`, 4 `#4`, 5 `#5`.
- `f2` — bits 23:16 → v[7:0], 34 → v[8]. 262 or more distinct texts, for example 0 `#0`, 1 `#1`, 2
  `#2`, 3 `#3`, 4 `#4`, 5 `#5`.
- `f3` — bits 15:8 → v[7:0], 33 → v[8]. 262 or more distinct texts, for example 0 `#0`, 1 `#1`, 2
  `#2`, 3 `#3`, 4 `#4`, 5 `#5`.
- `f4` — bits 7:0 → v[7:0], 32 → v[8]. 262 or more distinct texts, for example 0 `#0`, 1 `#1`, 2
  `#2`, 3 `#3`, 4 `#4`, 5 `#5`.
- `extra` — bits 35 → v[0], 34 → v[1], 33 → v[2], 32 → v[3]. Flags, printed in this order. Value 0
  prints `, incrementmode, incrementmode, incrementmode, incrementmode`. Each bit adds: v[0] `,
  swizzlemode`, v[1] `, swizzlemode`, v[2] `, swizzlemode`, v[3] `, swizzlemode`.
- `imm1` — bits 47:44. A 4-bit number, printed in decimal, times 4.
- `imm2` — bits 43:40. A 4-bit number, printed in decimal, times 4.
- `imm3` — bits 39:36. A 4-bit number, printed in decimal, times 4.

Layout 5 of 7 (form #136):

```
 63      56 55 54 53 52 51  50        49 48 47  36 35  32
+----------+-----+-----+---+---------+-----+------+------+
| 11111100 |  -  |  01 | - | nosched |  -  | imm1 | imm2 |
+----------+-----+-----+---+---------+-----+------+------+
 31  24 23  12 11   0
+------+------+------+
| imm2 | imm3 | imm4 |
+------+------+------+
```

- `nosched` — as in layout 1.
- `imm1` — bits 47:36. A 12-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 2048 `2048`, 4095 `-1`.
- `imm2` — bits 35:24. A 12-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 2048 `2048`, 4095 `-1`.
- `imm3` — bits 23:12. A 12-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 2048 `2048`, 4095 `-1`.
- `imm4` — bits 11:0. A 12-bit value printed as a number by the field's own printer (the group's
  text says how), for example 0 `0`, 1 `1`, 2048 `2048`, 4095 `-1`.

Layout 6 of 7 (form #137):

```
 63      56 55 54 53 52 51  50        49 32
+----------+-----+-----+---+---------+-----+
| 11111101 |  -  |  01 | - | nosched |  -  |
+----------+-----+-----+---+---------+-----+
 31 9 8      7 1 0
+----+------+---+------+
| -  | imm2 | - | imm1 |
+----+------+---+------+
```

- `nosched` — as in layout 1.
- `imm1` — bit 0. A 1-bit number, printed in decimal.
- `imm2` — bit 8. A 1-bit number, printed in decimal.

Layout 7 of 7 (form #138):

```
 63      56 55        54  53 52 51    50        49 47 46 43 42 34 33 32
+----------+---------+---+-----+-----+---------+-----+-----+-----+-----+
| 11111110 | skipinv | - |  01 | dst | nosched |  -  | imm |  -  | dst |
+----------+---------+---+-----+-----+---------+-----+-----+-----+-----+
 31 28 27 21 20 0
+-----+-----+----+
|  -  | dst | -  |
+-----+-----+----+
```

- `skipinv` — bit 55. Values: 0 (none), 1 `.skipinv`.
- `nosched` — as in layout 1.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `imm` — bits 46:43. Values: 0 `0 /* SMOA low data */`, 1 `1 /* SMOA high data */`, 2 `2 /* SMLSI
  low data */`, 3 `3 /* SMLSI high data */`, 4 `4 /* SMBO low data */`, 5 `5 /* SMBO high data */`,
  6 `6 /* SMR low data */`, 7 `7 /* SMR high data */`, 8 `8 /* Index data */`, 9-15 `9 /* Reserved
  */`…`15 /* Reserved */`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 11 forms have semantics (not
yet: #128, #129, #130, #131, #132, #133, #134, #136, #137, #138). The probe vectors that test them:
6 match the hardware, 0 stop unsupported, 0 mismatch (`x-misc` 6/0/0).

### 6.25 Phases: `phas`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#8    class 31       mask FFF4000000000000  value FA44000000000000  phas
      phas<end> #<imm1>, #<imm2>, <op1>, <op2>, <op3>
#9    class 31       mask FFF4000000000000  value FA40000000000000  phas
      phas<nosched><end> <src1>, <src2>
#158  class 31       mask FF74000000000000  value FA44000000000000  phas
      phas<end> #<imm1>, #<imm2>, <op1>, <op2>, <op3>
#159  class 31       mask FF74000000000000  value FA40000000000000  phas
      phas<nosched><end> <src1>, <src2>
```

Mnemonics: `phas`.

Modifiers: `.end`, `.nosched`.

Layout 1 of 4 (form #8):

```
 63          52 51    50  49 46 45    44 43 42 40 39  32
+--------------+-----+---+-----+-----+-----+-----+------+
| 111110100100 | end | 1 |  -  | op3 | op1 | op2 | imm2 |
+--------------+-----+---+-----+-----+-----+-----+------+
 31 18 17   0
+-----+------+
|  -  | imm1 |
+-----+------+
```

- `end` — bit 51. Values: 0 (none), 1 `.end`.
- `imm1` — bits 17:0. An 18-bit number, printed in decimal.
- `imm2` — bits 39:32. An 8-bit number, printed in decimal, times 4.
- `op1` — bits 44:43. Values: 0 `pixel`, 1 `selective`, 2 `sample`. Other values do not decode.
- `op2` — bits 42:40. Values: 0 `none`, 1 `pt`, 2 `vcull`, 7 `end`. Other values do not decode.
- `op3` — bit 45. Values: 0 `parallel`, 1 `perinstance`.

Layout 2 of 4 (form #9):

```
 63          52 51    50  49     48     47 44 43        42 32
+--------------+-----+---+------+------+-----+---------+-----+
| 111110100100 | end | 0 | src1 | src2 |  -  | nosched |  -  |
+--------------+-----+---+------+------+-----+---------+-----+
 31  30 29  28 27 14 13   7 6    0
+------+------+-----+------+------+
| src1 | src2 |  -  | src1 | src2 |
+------+------+-----+------+------+
```

- `nosched` — bit 43. Values: 0 (none), 1 `.nosched`.
- `end` — as in layout 1.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 3 of 4 (form #158):

```
 63      56 55  54 52 51    50  49 46 45    44 43 42 40 39  32
+----------+---+-----+-----+---+-----+-----+-----+-----+------+
| 11111010 | - | 100 | end | 1 |  -  | op3 | op1 | op2 | imm2 |
+----------+---+-----+-----+---+-----+-----+-----+-----+------+
 31 18 17   0
+-----+------+
|  -  | imm1 |
+-----+------+
```

- `end` — as in layout 1.
- `imm1` — as in layout 1.
- `imm2` — as in layout 1.
- `op1` — as in layout 1.
- `op2` — as in layout 1.
- `op3` — as in layout 1.

Layout 4 of 4 (form #159):

```
 63      56 55  54 52 51    50  49     48     47 44 43        42 32
+----------+---+-----+-----+---+------+------+-----+---------+-----+
| 11111010 | - | 100 | end | 0 | src1 | src2 |  -  | nosched |  -  |
+----------+---+-----+-----+---+------+------+-----+---------+-----+
 31  30 29  28 27 14 13   7 6    0
+------+------+-----+------+------+
| src1 | src2 |  -  | src1 | src2 |
+------+------+-----+------+------+
```

- `nosched` — as in layout 2.
- `end` — as in layout 1.
- `src1` — as in layout 2.
- `src2` — as in layout 2.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 4 forms have semantics (not
yet: #158, #159).

### 6.26 Emits: `emitpix1`, `emitpix2`, `emitvcb*`, `emitmte*`, `emitpds`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#10   class 31       mask FF30C10000000000  value FB20010000000000  emitpix1
      emitpix1<nosched><end><freep> #<imm> /* incp */, <src0>, <src1>
#11   class 31       mask FF30C10000000000  value FB20000000000000  emitpix2
      emitpix2<nosched><end><freep> #<imm1> /* incp */, <src0>, <src1>, <src2>, #<imm2>
#148  class 31       mask FF30C00000000000  value FB20800000000000  emitpds
      emitpds<nosched><end><tasks><taske><freep> #<imm1> /* incp */, <src0>, <src1>, <src2>, #<imm2>
#149  class 31       mask FF30D00000000000  value FB20400000000000  emitmtest
      emitmtest<nosched><end><threepart><freep> #<imm> /* incp */, <src1>, <src2>
#150  class 31       mask FF30D00000000000  value FB20500000000000  emitmtevtx
      emitmtevtx<nosched><end><twopart><cut><freep> #<imm> /* incp */, <src1>, <src2>
#151  class 31       mask FF30D00000000000  value FB20C00000000000  emitvcbst
      emitvcbst<nosched><end><threepart><freep> #<imm> /* incp */, <src2>
#152  class 31       mask FF30D00000000000  value FB20D00000000000  emitvcbvtx
      emitvcbvtx<nosched><end><twopart><freep> #<imm> /* incp */, <src2>
```

Mnemonics: `emitmtest`, `emitmtevtx`, `emitpds`, `emitpix1`, `emitpix2`, `emitvcbst`, `emitvcbvtx`.

Modifiers: `.cut`, `.end`, `.freep`, `.nosched`, `.taske`, `.tasks`, `.threepart`, `.twopart`.

Layout 1 of 6 (form #10):

```
 63      56 55 54 53 52 51     50    49     48
+----------+-----+-----+------+-----+------+---+
| 11111011 |  -  |  10 | src0 | end | src1 | - |
+----------+-----+-----+------+-----+------+---+
 47 46 45 44 43        42  41    40  39 35 34     33 32
+-----+-----+---------+---+-----+---+-----+------+-----+
|  00 |  -  | nosched | - | imm | 1 |  -  | src0 | imm |
+-----+-----+---------+---+-----+---+-----+------+-----+
 31  30 29 22 21      20  14 13   7 6 0
+------+-----+-------+------+------+---+
| src1 |  -  | freep | src0 | src1 | - |
+------+-----+-------+------+------+---+
```

- `nosched` — bit 43. Values: 0 (none), 1 `.nosched`.
- `end` — bit 50. Values: 0 (none), 1 `.end`.
- `freep` — bit 21. Values: 0 (none), 1 `.freep`.
- `imm` — bits 33:32 → v[1:0], 41 → v[2]. A 3-bit number, printed in decimal.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 51 → v[8]. A register operand: the standard source 0
  (*Register operands*).
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 6 (form #11):

```
 63      56 55  54 53 52 51     50    49     48
+----------+------+-----+------+-----+------+------+
| 11111011 | imm2 |  10 | src0 | end | src1 | src2 |
+----------+------+-----+------+-----+------+------+
 47 46 45 44 43        42  41     40  35 34     33  32
+-----+-----+---------+---+------+------+------+------+
|  00 |  -  | nosched | - | imm1 | imm2 | src0 | imm1 |
+-----+-----+---------+---+------+------+------+------+
 31  30 29  28 27  22 21      20  14 13   7 6    0
+------+------+------+-------+------+------+------+
| src1 | src2 | imm2 | freep | src0 | src1 | src2 |
+------+------+------+-------+------+------+------+
```

- `nosched` — as in layout 1.
- `end` — as in layout 1.
- `freep` — as in layout 1.
- `imm1` — bits 33:32 → v[1:0], 41 → v[2]. A 3-bit number, printed in decimal.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `imm2` — bits 27:22 → v[5:0], 40:35 → v[11:6], 55:54 → v[13:12]. A 14-bit number, printed in hex.

Layout 3 of 6 (form #148):

```
 63      56 55  54 53 52 51     50    49     48
+----------+------+-----+------+-----+------+------+
| 11111011 | imm2 |  10 | src0 | end | src1 | src2 |
+----------+------+-----+------+-----+------+------+
 47 46 45      44      43        42  41     40  35 34     33  32
+-----+-------+-------+---------+---+------+------+------+------+
|  10 | tasks | taske | nosched | - | imm1 | imm2 | src0 | imm1 |
+-----+-------+-------+---------+---+------+------+------+------+
 31  30 29  28 27  22 21      20  14 13   7 6    0
+------+------+------+-------+------+------+------+
| src1 | src2 | imm2 | freep | src0 | src1 | src2 |
+------+------+------+-------+------+------+------+
```

- `nosched` — as in layout 1.
- `end` — as in layout 1.
- `tasks` — bit 45. Values: 0 (none), 1 `.tasks`.
- `taske` — bit 44. Values: 0 (none), 1 `.taske`.
- `freep` — as in layout 1.
- `imm1` — as in layout 2.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 2.
- `imm2` — as in layout 2.

Layout 4 of 6 (form #149):

```
 63      56 55 54 53 52 51  50    49     48
+----------+-----+-----+---+-----+------+------+
| 11111011 |  -  |  10 | - | end | src1 | src2 |
+----------+-----+-----+---+-----+------+------+
 47 46 45  44  43        42          41    40 34 33 32
+-----+---+---+---------+-----------+-----+-----+-----+
|  01 | - | 0 | nosched | threepart | imm |  -  | imm |
+-----+---+---+---------+-----------+-----+-----+-----+
 31  30 29  28 27 22 21      20 14 13   7 6    0
+------+------+-----+-------+-----+------+------+
| src1 | src2 |  -  | freep |  -  | src1 | src2 |
+------+------+-----+-------+-----+------+------+
```

- `nosched` — as in layout 1.
- `end` — as in layout 1.
- `threepart` — bit 42. Values: 0 (none), 1 `.threepart`.
- `freep` — as in layout 1.
- `imm` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 2.

Layout 5 of 6 (form #150):

```
 63      56 55 54 53 52 51  50    49     48
+----------+-----+-----+---+-----+------+------+
| 11111011 |  -  |  10 | - | end | src1 | src2 |
+----------+-----+-----+---+-----+------+------+
 47 46 45    44  43        42        41    40 34 33 32
+-----+-----+---+---------+---------+-----+-----+-----+
|  01 | cut | 1 | nosched | twopart | imm |  -  | imm |
+-----+-----+---+---------+---------+-----+-----+-----+
 31  30 29  28 27 22 21      20 14 13   7 6    0
+------+------+-----+-------+-----+------+------+
| src1 | src2 |  -  | freep |  -  | src1 | src2 |
+------+------+-----+-------+-----+------+------+
```

- `nosched` — as in layout 1.
- `end` — as in layout 1.
- `twopart` — bit 42. Values: 0 (none), 1 `.twopart`.
- `cut` — bit 45. Values: 0 (none), 1 `.cut`.
- `freep` — as in layout 1.
- `imm` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 2.

Layout 6 of 6 (forms #151, #152):

```
 63      56 55 54 53 52 51  50    49  48     47 46 45  44  43        42          41    40 34 33 32
+----------+-----+-----+---+-----+---+------+-----+---+---+---------+-----------+-----+-----+-----+
| 11111011 |  -  |  10 | - | end | - | src2 |  11 | - | ? | nosched | threepart | imm |  -  | imm |
+----------+-----+-----+---+-----+---+------+-----+---+---+---------+-----------+-----+-----+-----+
 31 30 29  28 27 22 21      20 7 6    0
+-----+------+-----+-------+----+------+
|  -  | src2 |  -  | freep | -  | src2 |
+-----+------+-----+-------+----+------+
```

- `nosched` — as in layout 1.
- `end` — as in layout 1.
- (form #151) `threepart` — as in layout 4.
- (form #152) `threepart` — bit 42. Values: 0 (none), 1 `.twopart`.
- `freep` — as in layout 1.
- `imm` — as in layout 1.
- `src2` — as in layout 2.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 7 of 7 forms have semantics.

### 6.27 Visibility tests: `pcoeff`, `ptoff`, `atst8`, `depthf`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#153  class 31       mask FF30800000000000  value F830000000000000  pcoeff
      pcoeff<nosched><end> <src0>, <src1>, <src2>
#154  class 31       mask FF30800000000000  value F830800000000000  ptoff
      ptoff<nosched><end>
#155  class 31       mask FF30000000000000  value F930000000000000  atst8
      <pred>atst8<flags> <dst>, <w>p<p>, <src0>, <src1>, <src2><extra><op>
#156  class 31       mask FF30000000000000  value FB30000000000000  depthf
      <pred>depthf<flags> <src0>, <src1>, <src2><extra><op>
```

Mnemonics: `atst8`, `depthf`, `pcoeff`, `ptoff`.

Modifiers: `.c10`, `.end`, `.nosched`, `.syncs`.

Layout 1 of 4 (form #153):

```
 63      56 55 54 53 52 51     50    49     48     47  46 44 43        42 35 34     33 32
+----------+-----+-----+------+-----+------+------+---+-----+---------+-----+------+-----+
| 11111000 |  -  |  11 | src0 | end | src1 | src2 | 0 |  -  | nosched |  -  | src0 |  -  |
+----------+-----+-----+------+-----+------+------+---+-----+---------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 |  -  | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `nosched` — bit 43. Values: 0 (none), 1 `.nosched`.
- `end` — bit 50. Values: 0 (none), 1 `.end`.
- `src0` — bits 20:14 → v[6:0], 34 → v[7], 51 → v[8]. A register operand: the standard source 0
  (*Register operands*).
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 4 (form #154):

```
 63      56 55 54 53 52 51  50    49 48 47  46 44 43        42 32
+----------+-----+-----+---+-----+-----+---+-----+---------+-----+
| 11111000 |  -  |  11 | - | end |  -  | 1 |  -  | nosched |  -  |
+----------+-----+-----+---+-----+-----+---+-----+---------+-----+
 31 0
+----+
| -  |
+----+
```

- `nosched` — as in layout 1.
- `end` — as in layout 1.

Layout 3 of 4 (form #155):

```
 63      56 55   54 53 52 51     50      49     48
+----------+-------+-----+------+-------+------+------+
| 11111001 | flags |  11 | src0 | flags | src1 | src2 |
+----------+-------+-----+------+-------+------+------+
 47 44 43      42  41 40 39 38  37      36 35 34     33 32
+-----+-------+------+-----+---+-------+-----+------+-----+
|  -  | flags | pred |  p  | w | extra |  op | src0 | dst |
+-----+-------+------+-----+---+-------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — bits 42:41. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `flags` — bits 55 → v[0], 43 → v[1], 50 → v[2], 54 → v[3]. Flags, printed in this order. Each bit
  adds: v[0] `.syncs`, v[1] `.nosched`, v[2] `.end`, v[3] `.c10`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7]. A register operand, v = bank·128 + number (the bank
  bits may carry modifiers). By bank: 0 `r0`…`i3`, 1 `o0`…`o127`, 2 `pa0`…`pa127`, 3 `r[il +
  #0]`…`sa[ih + #15]`.
- `w` — bit 38. Values: 0 `!`, 1 (none).
- `p` — bits 40:39. A 2-bit number, printed in decimal.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `extra` — bit 37. Values: 0 `,`, 1 `, twosided,`.
- `op` — bits 36:35. Values: 0 `fb`, 1 `nfb`, 2 `optdwd`, 3 `optdwd`.

Layout 4 of 4 (form #156):

```
 63      56 55      54  53 52 51     50      49     48
+----------+-------+---+-----+------+-------+------+------+
| 11111011 | flags | - |  11 | src0 | flags | src1 | src2 |
+----------+-------+---+-----+------+-------+------+------+
 47 44 43      42  41 40 38 37      36 35 34     33 32
+-----+-------+------+-----+-------+-----+------+-----+
|  -  | flags | pred |  -  | extra |  op | src0 |  -  |
+-----+-------+------+-----+-------+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 |  -  | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 3.
- `flags` — bits 55 → v[0], 43 → v[1], 50 → v[2]. Flags, printed in this order. Each bit adds: v[0]
  `.syncs`, v[1] `.nosched`, v[2] `.end`.
- `src0` — as in layout 1.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `extra` — as in layout 3.
- `op` — as in layout 3.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 4 forms have semantics (not
yet: #153, #155, #156).

### 6.28 Cache control: `ci`, `cf`, `cfi`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#157  class 31       mask FF70000000000000  value F840000000000000  cf cfi ci
      <op><flags> <src1>, <src2>, #<imm>
```

Mnemonics: `cf`, `cfi`, `ci`.

Modifiers: `.dm_nomatch`, `.global`, `.nosched`.

Layout (form #157):

```
 63      56 55  54 52 51 50 49     48     47   46 45 44 43      42  41 40 39 32
+----------+---+-----+-----+------+------+-------+-----+-------+---+-----+-----+
| 11111000 | - | 100 |  -  | src1 | src2 | flags |  op | flags | - | imm |  -  |
+----------+---+-----+-----+------+------+-------+-----+-------+---+-----+-----+
 31  30 29  28 27 14 13   7 6    0
+------+------+-----+------+------+
| src1 | src2 |  -  | src1 | src2 |
+------+------+-----+------+------+
```

- `op` — bits 45:44. Values: 1 `ci`, 2 `cf`, 3 `cfi`. Other values do not decode.
- `flags` — bits 43 → v[0], 47 → v[1], 46 → v[2]. Flags, printed in this order. Each bit adds: v[0]
  `.nosched`, v[1] `.global`, v[2] `.dm_nomatch`.
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `imm` — bits 41:40. Values: 0 `0 /* Reserved */`, 1 `1 /* L0 and L1 */`, 2 `2 /* L2 */`, 3 `3 /*
  L0, L1 and L2 */`.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 1 of 1 forms have semantics. The
probe vectors that test them: 68 match the hardware, 0 stop unsupported, 0 mismatch (`cache-control`
50/0/0, `x-cache2` 18/0/0).

### 6.29 Video: `adif`, `adifsum`, `bilin`, `firv`, `firh`

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

Forms (generated from the decoder's table; `<name>` is a field):

```
#213  class 22       mask F830004000000000  value B000000000000000  adif
      <pred>adif<flags> <dst>, <src1>, <src2>
#214  class 22       mask F830004000000000  value B000004000000000  adifsum
      <pred>adifsum<flags> <dst>, <src0>.<half>, <src1>, <src2>
#215  class 22       mask F830000000000000  value B020000000000000  bilin
      <pred>bilin<flags> <dst>, <src0>, <src1>, <src2>, <op1>, <op2>, <op3><extra>
#216  class 22       mask F830000000000000  value B030000000000000  firv
      <pred>firv<flags> <w><dst>, <src0>, <src2>, <op><extra>
#217  class 23       mask F800000000000000  value B800000000000000  firh
      <pred>firh<flags> <dst>, <src0>, <src2>, <op1>, <op2>, #<imm1>, #<imm2>
```

Mnemonics: `adif`, `adifsum`, `bilin`, `firh`, `firv`.

Modifiers: `.end`, `.nosched`, `.repeatN`, `.skipinv`.

Layout 1 of 5 (form #213):

```
 63   59 58  57 56  55   54 53 52 51    50      49     48     47  46   44 43 39 38  37 34 33 32
+-------+------+---+-------+-----+-----+-------+------+------+---+-------+-----+---+-----+-----+
| 10110 | pred | - | flags |  00 | dst | flags | src1 | src2 | - | flags |  -  | 0 |  -  | dst |
+-------+------+---+-------+-----+-----+-------+------+------+---+-------+-----+---+-----+-----+
 31  30 29  28 27 21 20 14 13   7 6    0
+------+------+-----+-----+------+------+
| src1 | src2 | dst |  -  | src1 | src2 |
+------+------+-----+-----+------+------+
```

- `pred` — bits 58:57. Values: 0 (none), 1 `p0`, 2 `p1`, 3 `!p0`.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2], 46:44 → v[5:3]. Flags, printed in this order. Each
  bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.end`, v[5:3] a count, `.repeat2`…`.repeat8`.
- `dst` — bits 27:21 → v[6:0], 33:32 → v[8:7], 51 → v[9]. A register operand: the standard
  destination (*Register operands*).
- `src1` — bits 13:7 → v[6:0], 31:30 → v[8:7], 49 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).
- `src2` — bits 6:0 → v[6:0], 29:28 → v[8:7], 48 → v[9]. A register operand: the standard source 1
  or 2 (*Register operands*).

Layout 2 of 5 (form #214):

```
 63   59 58  57 56  55   54 53 52 51    50      49     48
+-------+------+---+-------+-----+-----+-------+------+------+
| 10110 | pred | - | flags |  00 | dst | flags | src1 | src2 |
+-------+------+---+-------+-----+-----+-------+------+------+
 47     46   44 43 39 38  37 35 34     33 32
+------+-------+-----+---+-----+------+-----+
| half | flags |  -  | 1 |  -  | src0 | dst |
+------+-------+-----+---+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `src0` — bits 20:14 → v[6:0], 34 → v[7]. A register operand, v = bank·128 + number (the bank bits
  may carry modifiers). By bank: 0 `r0`…`i3`, 1 `pa0`…`pa127`.
- `half` — bit 47. Values: 0 `low`, 1 `high`.
- `src1` — as in layout 1.
- `src2` — as in layout 1.

Layout 3 of 5 (form #215):

```
 63   59 58  57 56    55   54 53 52 51    50      49     48
+-------+------+-----+-------+-----+-----+-------+------+------+
| 10110 | pred | op2 | flags |  10 | dst | flags | src1 | src2 |
+-------+------+-----+-------+-----+-----+-------+------+------+
 47      46   44 43 42 41 40 39 35 34     33 32
+-------+-------+-----+-----+-----+------+-----+
| extra | flags | op3 | op1 |  -  | src0 | dst |
+-------+-------+-----+-----+-----+------+-----+
 31  30 29  28 27 21 20  14 13   7 6    0
+------+------+-----+------+------+------+
| src1 | src2 | dst | src0 | src1 | src2 |
+------+------+-----+------+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `dst` — as in layout 1.
- `src0` — as in layout 2.
- `src1` — as in layout 1.
- `src2` — as in layout 1.
- `op1` — bits 41:40. Values: 0 `u8`, 1 `s8`, 2 `o8`. Other values do not decode.
- `op2` — bit 56. Values: 0 `planar`, 1 `interleaved`.
- `op3` — bits 43 → v[0], 42 → v[1]. Values: 0 `src01, dst01`, 1 `src23, dst01`, 2 `src01, dst23`, 3
  `src23, dst23`.
- `extra` — bit 47. Values: 0 (none), 1 `, rnd`.

Layout 4 of 5 (form #216):

```
 63   59 58  57 56      55   54 53 52 51    50      49     48
+-------+------+-------+-------+-----+-----+-------+------+------+
| 10110 | pred | extra | flags |  11 | dst | flags | src0 | src2 |
+-------+------+-------+-------+-----+-----+-------+------+------+
 47  46   44 43  42      41 40 39      38 34 33 32
+---+-------+---+-------+-----+-------+-----+-----+
| w | flags | - | extra |  op | extra |  -  | dst |
+---+-------+---+-------+-----+-------+-----+-----+
 31  30 29  28 27 21 20   7 6    0
+------+------+-----+------+------+
| src0 | src2 | dst | src0 | src2 |
+------+------+-----+------+------+
```

- `pred` — as in layout 1.
- `flags` — as in layout 1.
- `w` — bit 47. Values: 0 `!`, 1 (none).
- `dst` — as in layout 1.
- `src0` — bits 20:14 → v[6:0], 13:7 → v[13:7], 31:30 → v[15:14], 49 → v[16]. An operand list (see
  *Operand encodings*), for example 0 `r0, r0`, 1 `r1, r0`, 2 `r2, r0`.
- `src2` — as in layout 1.
- `op` — bits 41:40. Values: 0 `u8`, 1 `s8`, 2 `o8`. Other values do not decode.
- `extra` — bits 56 → v[0], 39 → v[1], 42 → v[2]. Flags, printed in this order. Each bit adds: v[0]
  `, +src2.2`, v[1] `, iadd`, v[2] `, scale`.

Layout 5 of 5 (form #217):

```
 63   59 58  57 56     55   54 53  52 51    50      49     48
+-------+------+------+-------+------+-----+-------+------+------+
| 10111 | pred | imm1 | flags | imm2 | dst | flags | src0 | src2 |
+-------+------+------+-------+------+-----+-------+------+------+
 47  46 45   44 43  42 41 40 39 35 34  33 32
+------+-------+------+-----+-----+---+-----+
| imm1 | flags | imm1 | op1 | op2 | - | dst |
+------+-------+------+-----+-----+---+-----+
 31  30 29  28 27 21 20   7 6    0
+------+------+-----+------+------+
| src0 | src2 | dst | src0 | src2 |
+------+------+-----+------+------+
```

- `pred` — as in layout 1.
- `flags` — bits 55 → v[0], 54 → v[1], 50 → v[2], 45:44 → v[4:3]. Flags, printed in this order. Each
  bit adds: v[0] `.skipinv`, v[1] `.nosched`, v[2] `.end`, v[4:3] a count, `.repeat2`…`.repeat4`.
- `dst` — as in layout 1.
- `src0` — as in layout 4.
- `src2` — as in layout 1.
- `op1` — as in layout 3.
- `op2` — bits 39:38 → v[1:0], 37:35 → v[4:2]. Values: 0 `rep, fcs0`, 1 `msc, fcs0`, 2 `mdc, fcs0`,
  3 `reserved, fcs0`, 4 `rep, fcs1`, 5 `msc, fcs1`, 6 `mdc, fcs1`, 7 `reserved, fcs1`, 8 `rep,
  fcs2`, 9 `msc, fcs2`, 10 `mdc, fcs2`, 11 `reserved, fcs2`, 12 `rep, fcs3`, 13 `msc, fcs3`, 14
  `mdc, fcs3`, 15 `reserved, fcs3`, 16 `rep, fcs4`, 17 `msc, fcs4`, 18 `mdc, fcs4`, 19 `reserved,
  fcs4`, 20 `rep, fcs5`, 21 `msc, fcs5`, 22 `mdc, fcs5`, 23 `reserved, fcs5`, 24 `rep, fcs6`, 25
  `msc, fcs6`, 26 `mdc, fcs6`, 27 `reserved, fcs6`, 28 `rep, fcs7`, 29 `msc, fcs7`, 30 `mdc, fcs7`,
  31 `reserved, fcs7`.
- `imm1` — bits 43:42 → v[1:0], 47:46 → v[3:2], 56 → v[4]. Values: 0-15 `0`…`15`, 16 `-16`, 17
  `-15`, 18 `-14`, 19 `-13`, 20 `-12`, 21 `-11`, 22 `-10`, 23 `-9`, 24 `-8`, 25 `-7`, 26 `-6`, 27
  `-5`, 28 `-4`, 29 `-3`, 30 `-2`, 31 `-1`.
- `imm2` — bits 53:52. A 2-bit number, printed in decimal.

Simulator (`crates/sgx-sim`, from `doc/isa/sim-coverage.json`): 2 of 5 forms have semantics (not
yet: #215, #216, #217). The probe vectors that test them: 132 match the hardware, 135 stop
unsupported, 0 mismatch (`video-adif` 120/0/0, `video-filters` 0/105/0, `x-video` 12/30/0);
unsupported mostly for no semantics yet: bilin (75); no semantics yet: firv (39); no semantics yet:
firh (21).

## 7. Hardware probe results

What the instruction text leaves open was asked of the GPU on 2026-10-22: our own pixel
programs ran each instruction under test on preset inputs and wrote the result to the pixel
(`tools/usse-asm/probe.py`, the probe table in `tools/usse-asm/progs/probes/`). The results are
summarised in `design/sgx545_hw_spec.md` §14 and given with their evidence in
`tools/usse-dis/r2/README.md` ("Hardware probes"), whose radare2 ESIL models them. Each fact is
one snippet in `doc/isa/content/usse/probe/`, marked *Measured on the hardware (2026-10-22)*,
with what stays undecided and what would settle it, or *Not probed yet*:

- §7.1 The test sign `p` **[HW]**
- §7.2 `t` in the `a`/`o` joins of a test **[HW]**
- §7.3 The 32-bit adds `iadd32` and `iaddu32` **[HW]**
- §7.4 Test channels **[HW]**
- §7.5 `isub16`/`isubu16` lanes **[HW]**
- §7.6 `ima16` lanes and source formats **[HW]**
- §7.7 Integer multiply-add modes **[HW]**
- §7.8 Shift amounts of 32 and over **[HW]**
- §7.9 Index registers **[HW]**
- §7.10 Register offsets of loads and stores **[HW]**
- §7.11 The 14-bit `ldr`/`str` index **[HW]**
- §7.12 Repeat order and overlapping ranges **[HW]**
- §7.13 Predicates on repeated instructions **[HW]**
- §7.14 `br` offsets and the `Pn` predicate **[HW]**
- §7.15 `smlsi` increments **[HW]**
- §7.16 Float rounding and NaN **[HW]**
- §7.17 `frcp`, `frsq` and `fsqrt` precision **[HW]**
- §7.18 Denormals **[HW]**
- §7.19 Write masks **[HW]**
- §7.20 `.incsXYZ` **[HW]**
- §7.21 Pack and unpack lanes **[HW]**
- §7.22 Pack and unpack conversions **[HW]**
- §7.23 Float dot products **[HW]**
- §7.24 Internal registers **[HW]**
- §7.25 Float min/max **[HW]**
- §7.26 Conditional and masked moves **[HW]**
- §7.27 Colour operations **[HW]**
- §7.28 Integer dot products **[HW]**
- §7.29 `fpma` **[HW]**
- §7.30 Half-float multiply-add **[HW]**
- §7.31 `efo` **[HW]**
- §7.32 `fadm`, `fmsa`, `fsubflr` **[HW]**
- §7.33 `flog`, `fexp`, `fsin`, `fcos` **[HW]**
- §7.34 Dual issue **[HW]**
- §7.35 Derivatives `fdsx`, `fdsy` **[HW]**
- §7.36 Texture sampling `smp` **[HW]**
- §7.37 Sampler state **[HW]**
- §6.8 `rlp` **[HW]**
- §7.39 Cache control `ci`, `cf`, `cfi` **[HW]**
- §7.40 Video `adif`, `adifsum` **[HW]**
- §7.41 `.tpres`, `.partial`, `.nosched`, `.syncs` **[HW]**

### 7.1 The test sign `p`

*Measured on the hardware (2026-10-22).* The sign part of a test (`.test<S>...`) is `t`, `n` or `p`.
On a 32-bit result `n` is bit 31 and **`p` is its complement, `>= 0`**, so a zero result passes `p`
**[HW]**: `shl.testpat` and `and.testpat` set the predicate for 0, 1 and `0x7fffffff` and not for
`0x80000000` (probes `sign-p-shl`, `sign-p-and`; `design/sgx545_hw_spec.md` §14). The earlier open
reading `> 0` is wrong. The firmware's `testpat` on `shl` and `isub16` results therefore asks "bit
clear" (after the shift) and "not negative". On the 16-bit tests of `chan0`, `p` is `>= 0` on the
low lane (*`isub16`/`isubu16` lanes*); `iaddu32`, unsigned, passes `p` always (*The 32-bit adds*).

### 7.2 `t` in the `a`/`o` joins of a test

*Measured on the hardware (2026-10-22).* A test joins a sign test and a zero test with `a` (and) or
`o` (or), and either can be `t`. **`t` means true** **[HW]**: `and.testnot`, `.testtoz` and
`.testtonz` set the predicate for every input (probes `join-t-not`, `join-t-toz`, `join-t-tonz`;
`design/sgx545_hw_spec.md` §14). So an `o` join with a `t` side is always true, `testtaz` is "zero",
`testtanz` "not zero", `testnat` "negative" and `testpat` "not negative"; the "not tested" reading
is ruled out.

### 7.3 The 32-bit adds `iadd32` and `iaddu32`

*Measured on the hardware (2026-10-22).* **`iadd32` and `iaddu32` are not full 32-bit adds**
**[HW]**: they add **source 1's low 16 bits**, sign-extended (`iadd32`) or zero-extended
(`iaddu32`), to the 32-bit source 2, modulo 2^32. `0x12345678 + 0x11111111` gives `0x11116789` and
`iadd32 0xffff + 1` gives 0, while `#1 + r` is the plain increment the firmware uses
(`iaddu32 r3, p0, #1, r3`). The test reads the 32-bit sum: `z` and `n` as usual for `iadd32`;
`iaddu32` is unsigned, its `n` is never and its `p` always true, also for `0x80000000` (probes
`iadd32-value`, `iaddu32-value`, `iaddu32-imm`, `iadd32-src1-imm`, `iadd32-z`, `iadd32-n`,
`iadd32-p`, `iaddu32-tests`, `iaddu32-np`; `design/sgx545_hw_spec.md` §14).

### 7.4 Test channels

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

### 7.5 `isub16`/`isubu16` lanes

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

### 7.6 `ima16` lanes and source formats

*Measured on the hardware (2026-10-22).* **`ima16 ..., u16, u16` computes `a × b + c`** (`a × b - c`
with `.neg` on source 2) **in each 16-bit lane, wrapping** **[HW]**: `0x00020005 × #3 - 0x00010007`
is `0x00050008` and `0x0000ffff × #3` is `0x0000fffd`; an immediate `b` or `c` is in both lanes, a
register one is taken lane by lane (probes `ima16`, `ima16-plain`, `ima16-imm-c`, `ima16-reg-b`;
`design/sgx545_hw_spec.md` §14). This is the firmware's `u16, u16` use.

**Undecided:** the 8-bit source formats `u8`, `s8`, `o8` (`enc[39:38]`, `enc[41:40]`) with their
`.low`/`.high` select, saturation, `.rsN` and `.abs` on the destination; the same probe template
with those formats and lane values that tell the bytes apart would settle them.

### 7.7 Integer multiply-add modes

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

### 7.8 Shift amounts of 32 and over

*Measured on the hardware (2026-10-22).* By a **register** amount of 32 and over, `shl` and `shr`
give 0, `asr` gives the sign (0 or `0xffffffff`) and `rol` takes the amount modulo 32 **[HW]**:
amounts 32, 33, 64, `0x100` and `0xffffffff` behave so, and `shl.testnat` tests the same result
(probes `shl-reg`, `shr-reg`, `asr-reg`, `rol-reg`, `shl-test-reg`; `design/sgx545_hw_spec.md` §14).

*Round 4 (2026-10-22).* **Immediate amounts of 32 and over act as register ones** (`#32`, `#33`,
`#40`, `#127`), and **`~` complements the 32-bit amount or operand before use**: `shl d, a, ~r2`
with `r2 = 0xfffffffc` shifts by 3, `~#3` is `0xfffffffc` (so `shl`/`shr` give 0 and `rol` turns
by 28), and `and`/`or`/`xor` with `~#3` take the complement as their operand **[HW]** (probe
`shift-immediates`).

### 7.9 Index registers

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

### 7.10 Register offsets of loads and stores

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

### 7.11 The 14-bit `ldr`/`str` index

*Measured on the hardware (2026-10-22).* The immediate register index of `ldr`/`str` has 14 bits (a
reach of 64 KiB of register space) **[RE]**, and **`ldr rD, rI` masks a register index to 14 bits**
**[HW]**: `0x4007`, `0x8007`, `0x10007` and `0x40007` read `CORE_ID` like 7 (probe `ldr-index-mask`;
`design/sgx545_hw_spec.md` §14). A validator can reduce an index modulo `0x4000`.

**Undecided:** `str` with a register index (a client `str` was not seen to land; a probe from the
EDM's own task, on a register that is safe to write, would settle it).

### 7.12 Repeat order and overlapping ranges

*Measured on the hardware (2026-10-22).* With the default MOE state a `.repeatN` steps every operand
by one register per iteration, and **the iterations run one after the other, in ascending order**
**[HW]**: `mov.repeat2 r2, r1` leaves `r3 = r1` (overlapping ranges propagate), and
`mov.repeat3 r1, r2` gives `r1 = r2`, `r2 = r3`, `r3 = r4` (probe `repeat-order`;
`design/sgx545_hw_spec.md` §14). A repeat behaves like N instructions in sequence.

### 7.13 Predicates on repeated instructions

*Measured on the hardware (2026-10-22).* **One predicate covers the whole instruction** **[HW]**:
`p0 mov.repeat2` with `p0 = 1, p1 = 0` writes both registers and with `p0 = 0, p1 = 1` neither
(probe `repeat-pred`; `design/sgx545_hw_spec.md` §14).

**Undecided:** what a repeated test writes into its predicate, and `Pn`; a probe of
`and.testtaz.repeat2` with inputs that differ per iteration would settle the first.

### 7.14 `br` offsets and the `Pn` predicate

*Measured on the hardware (2026-10-22).* **`br #k` jumps `k` instructions on from the `br`
itself**: `br #1` runs the next instruction, `br #2` skips one (probe `br-offset`; `ba` agrees
with the absolute index). **An instruction predicated `Pn` (value 7) never runs**, whatever
`p0`-`p3` hold (probe `predicate-pn`) **[HW]** (`design/sgx545_hw_spec.md` §14).

### 7.15 `smlsi` increments

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

### 7.16 Float rounding and NaN

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

### 7.17 `frcp`, `frsq` and `fsqrt` precision

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

### 7.18 Denormals

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

### 7.19 Write masks

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

### 7.20 `.incsXYZ`

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

### 7.21 Pack and unpack lanes

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

### 7.22 Pack and unpack conversions

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

### 7.23 Float dot products

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

### 7.24 Internal registers

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

### 7.25 Float min/max

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

### 7.26 Conditional and masked moves

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

### 7.27 Colour operations

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

### 7.28 Integer dot products

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

### 7.29 `fpma`

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

### 7.30 Half-float multiply-add

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

### 7.31 `efo`

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

### 7.32 `fadm`, `fmsa`, `fsubflr`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fadm-fmsa-fsubflr`,
`fadm-fmsa-fsubflr-specials`; `design/sgx545_hw_spec.md` §14):

- **`fadm d, a, b, c` = `a·c + b·c`**, each product and then the sum rounded to binary32 (not
  `(a + b) × c`: 24 random cases fit only this order), like a two-term `fdp`;
- **`fmsa d, a, b, c` = `a·a + b·c`**, rounded the same way;
- **`fsubflr d, a, b` = `a - floor(b)`**, one rounding.

Denormal inputs are flushed; an invalid result (`inf × 0`, `inf - inf`, a NaN input) is the
NaN `0x7fc00000`, as for the dot products.

**Undecided:** the half-float source forms (`enc[54]`), as for `fmad`.

### 7.33 `flog`, `fexp`, `fsin`, `fcos`

*Measured on the hardware (2026-10-22).* **[HW]** (probes `fsingle-base`, `fsingle-specials`,
`sweep-flog`, `sweep-fexp`, `sweep-fsin`, `sweep-fcos`; `design/sgx545_hw_spec.md` §14):

- **`flog` is `log2`**: exact on powers of two; within one ulp elsewhere, or within 2^-22
  near 1 (`log2(1 + 2^-23)` gives 2^-23); 0, `-0` and denormals give `-inf`, `+inf` gives
  `+inf`, negatives and NaN the NaN `0x7fffffff`.
- **`fexp` is `2^x`**: exact on integers, within two ulps elsewhere (78 of 95 exploration
  cases within one); results below 2^-126 are 0, from 128 up `+inf`; `-inf` gives 0, a NaN
  `0x7fffffff`.
- **`fsin`, `fcos` take the argument in quarter turns**: `sin(πx/2)`, `cos(πx/2)`, period 4,
  exact on integers; the result is a fixed-point value with 15 fraction bits (absolute error up
  to about 2^-15.5, so tiny results are 0 or 2^-15); an infinite or NaN argument gives
  `0x7fffffff`. Odd multiples of 1/2 are not exact: `fcos(0.5)` is 23171/2^15, the nearest
  being 23170/2^15 (probe `x-fcos`).
- **The zeros at integers have a sign** (probes `x-fsin`, `x-fcos`, `fsingle-specials`):
  `fsin` is odd and `fcos` even, and in quarter turns `r = |x| mod 4` `fsin` gives `+0` at
  `r = 0` and `-0` at `r = 2`, `fcos` `-0` at `r = 1` and `+0` at `r = 3`: `fsin(-0)` is `-0`,
  `fsin(2)` `-0`, `fsin(4)`, `fsin(100)` and `fsin(10^6)` `+0`, `fcos(1)` and `fcos(-1)` `-0`,
  `fcos(3)` `+0`. A denormal argument is flushed (`fcos` of one is 1).

*Simulator check, 2026-10-23.* The simulator (`crates/sgx-sim`, `func.rs`)
gives these functions and `frcp`, `frsq`, `fsqrt` exactly where the hardware is exact and
otherwise, as a *near* value, the nearest one with the bound above; the vector tests accept a
near value within its bound and stop on anything computed from one. All the recorded vectors
of the seven functions fit, write masks included (as *Write masks*). The `isa2/f1` runs of the
four sweeps read back other inputs than the rebuilt table's (the inputs they store differ).

**Undecided:** `.flt16` and `.c10` sources, repeats on these, `.tpres`, and the sign of a tiny
result rounded to zero.

### 7.34 Dual issue

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

### 7.35 Derivatives `fdsx`, `fdsy`

*Measured on the hardware (2026-10-22).* **`fdsx d, a, b` = `a`(top-right) − `b`(top-left) and
`fdsy d, a, b` = `a`(bottom-left) − `b`(top-left), the same value for all four pixels of the 2x2
quad** **[HW]** (coarse derivatives; probe `fds-quad`, on per-pixel values from the pixel
coordinates; `design/sgx545_hw_spec.md` §14). The difference is rounded once to binary32;
`.neg` and `.abs` act on the source before. The quads are the aligned 2x2 blocks of the
render target (even x and y at the top left).

**Undecided:** partly covered quads (the probes fill whole quads), and denormal inputs.

### 7.36 Texture sampling `smp`

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

### 7.37 Sampler state

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
  65535 for `u16`). Data size field (T3 bits 26:23) changes nothing seen. **Without alpha
  (565) c10 and f32 leave its register unwritten** (c10 one register, f32 three) and f16 gives
  1.0 even unnormalized. **The other formats convert their channels** (cases 24-47): format 0
  is one u8 channel, 7 two (the low byte first), 9 one u16; c10 puts channel k in the k-th
  10-bit field of one register (a u16 as its top byte: `0xd000` gives `0xd0`), f16 packs the
  channels in pairs (low half first: format 7's `0x0451` gives `h(81) | h(4) << 16`), f32 gives
  one register per channel.
- **PCF (`smp-pcf`).** `smp2d.pcf` compares **texel OP reference**, the reference being the
  f32 in the register after the coordinates (`.pcff16`: an f16, low half), by T3 bits 31:29
  (0 never, 1 `<`, 2 `==`, 3 `<=`, 4 `>`, 5 `>=`, 6 `!=`, 7 always), and returns 1.0 for an f32
  texture (format 18) or `0xffff` for `u16` (format 9, compared as texel / 65535) when it
  holds, else 0; with bilinear filtering the four results are averaged (`0x8000` for one of two
  texels at half weight, the only split measured). A colour format ignores `.pcf`.
- **Other state bits (exploration `x-smp2`).** Setting T0 bits 27, 30 or 31, or T3 bits outside
  the fields above (0-7, 10, 14, 18, 19, 27, 28), reads other texels or values: they act, how
  is not known.

*Simulator check, 2026-10-23.* The simulator's texture unit
(`crates/sgx-sim`, `tex.rs`) follows this page and *Texture sampling*; it reproduces every
recorded sampling vector it models and stops on the rest: the LOD values, the bits above, the
recorded formats and modes, tiled textures, and filtered `.pcf` at other weights.

**Undecided:** mipmaps and the LOD values (`smp-lod`: neither `replace`, `bias` nor `grad` left
level 0 with T0 bits 20:17 at 0, 12 or 15), the border modes, the recorded formats, `.sinf`/
`.rsd`, `smp3d`, and state from `sa` or `c`.

### 7.38 `rlp`

*Measured on the hardware (2026-10-22).* **`rlp d, s, pN` rotates left through the predicate**:
`d = s << 1 | pN`, then `pN` = bit 31 of `s` **[HW]** (probe `rlp`, 32 cases over every
predicate). **Predicate numbers from 4 up alias `p0`-`p3`** (number mod 4: `rlp ..., p4` reads
and writes `p0`) **[HW]** (probe `rlp-high-predicates`).

### 7.39 Cache control `ci`, `cf`, `cfi`

*Measured on the hardware (2026-10-22).* **A load after `ci`, `cf` or `cfi`, on any of the
three level selections, with `.global` or `.dm_nomatch`, returns the pixel's own store**
**[HW]**: none of them drops a store or makes a later load read stale memory, and none
writes a register (probe `cache-control`, every pixel on its own dword).

**Undecided:** what they do to other units' view of memory (the texture cache, the host),
which our probes cannot see.

### 7.40 Video `adif`, `adifsum`

*Measured on the hardware (2026-10-22).* **`adif d, a, b` = `|a − b|` per byte; `adifsum d,
s0.low|high, a, b` = the sum of the four byte differences plus that 16-bit half of `s0`**, as a
32-bit value; a repeat advances the destination and the sources **[HW]** (probe `video-adif`).

**Undecided:** `bilin`, `firv` and `firh` (probe `video-filters`, recorded): no single or
two-weight interpolation of the source bytes fits `bilin`, and the FIR filters read
coefficient sets (`fcsN`) we do not set.

### 7.41 `.tpres`, `.partial`, `.nosched`, `.syncs`

*Measured on the hardware (2026-10-22).* In a fill, **`.tpres` (`frcp`, `frsq`, `fsqrt`),
`.partial` (`and`, `or`) and `.nosched` change nothing a pixel sees, while `.syncs` (on `mov`
and `fmad`) hangs the task**, which the driver recovers from with `-ETIMEDOUT` **[HW]** (probe
`flags-other`; `design/sgx545_hw_spec.md` §14). `.syncs` presumably waits for something a
fill never provides; what is open.

## 8. Open questions

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

The simulator (`crates/sgx-sim`) reproduces 29951 probe vectors (2749 of them with near values:
float functions within their measured bound) and 364 pixel vectors; it stops on 724 rather than
guess, most often because (vectors, from `doc/isa/sim-coverage.json`):

- no semantics yet: bilin (75)
- no SGX registers on this bus (70)
- texture format N (64)
- reads iN before the program writes it (63)
- reads saN before the program writes it (56)
- efo.gpiext reaches the other instances of the quad (43)
- no semantics yet: firv (39)
- flag .fetchN with the MOE state unknown (28)
- reads rN before the program writes it (24)
- iN reads another instance of the quad (23)
- fds repeated (22)
- TN bits NxN (21)

## 9. Appendix: every instruction form

Every form of the decoder's table, in table order within each opcode class: its index, the
classes it can occupy, its identifying mask and value, the mnemonics it prints and its
pattern with named fields. A word decodes with the first form, in table order, whose mask and
value match and whose fields all print; the classes here group the forms for reading only.

**Class 31**

```
#0    class 31       mask FF30000000000000  value FC20000000000000  mov
      <pred>mov<skipinv><nosched><end> <dst>, #<imm>
#3    class 31       mask FFFFFFFFFFFFC07F  value F80000C000000000  mov
      mov pclink, <src1>
#4    class 31       mask FFFFFFFFF01FFFFF  value F800010000000000  mov
      mov <dst>, pclink
#5    class 31       mask FF7BF7C000000000  value F800014000000000  nop
      nop<syncs><nosched><end><tog><trigger>
#6    class 31       mask FFFFFFFEFFFFFFFF  value F820000000000000  idf
      idf drc<drc>, bif
#7    class 31       mask FFFFFFFEFFFFFFFF  value F920000000000000  wdf
      wdf drc<drc>
#8    class 31       mask FFF4000000000000  value FA44000000000000  phas
      phas<end> #<imm1>, #<imm2>, <op1>, <op2>, <op3>
#9    class 31       mask FFF4000000000000  value FA40000000000000  phas
      phas<nosched><end> <src1>, <src2>
#10   class 31       mask FF30C10000000000  value FB20010000000000  emitpix1
      emitpix1<nosched><end><freep> #<imm> /* incp */, <src0>, <src1>
#11   class 31       mask FF30C10000000000  value FB20000000000000  emitpix2
      emitpix2<nosched><end><freep> #<imm1> /* incp */, <src0>, <src1>, <src2>, #<imm2>
#12   class 31       mask F87801C000000000  value F800000000000000  ba
      <pred>ba<flags> #<imm>
#13   class 31       mask F87801C000000000  value F800004000000000  br
      <pred>br<flags> <imm>
#14   class 31       mask F878018000000000  value F808000000000000  bexception
      bexception<nosched>
#35   class 31       mask FF79000030000000  value FE21000020000000  ldr
      ldr<flags> <dst>, #<imm>, drc<drc>
#36   class 31       mask FF78000000000000  value FE20000000000000  ldr
      ldr<flags> <dst>, <src2>, drc<drc>
#37   class 31       mask FF79000030000000  value FE29000020000000  str
      str<flags> #<imm>, <src1>
#38   class 31       mask FF78000000000000  value FE28000000000000  str
      str<flags> <src2>, <src1>
#124  class 31       mask F87001C000000000  value F800008000000000  lapc
      <pred>lapc<flags>
#125  class 31       mask F87001C000000000  value F80000C000000000  mov
      <pred>mov<nosched> pclink, <src1>
#126  class 31       mask F87001C000000000  value F800010000000000  mov
      <pred>mov<nosched> <dst>, pclink
#127  class 31       mask F87001C000000000  value F800014000000000  nop
      nop<syncs><nosched><end><tog><trigger>
#128  class 31       mask FF38000000000000  value F818000000000000  smoa
      smoa<nosched> <src1>, <src2>
#129  class 31       mask FF38000000000000  value F918000000000000  smr
      smr<nosched> <src1>, <src2>
#130  class 31       mask FF38000000000000  value FA18000000000000  smlsi
      smlsi<nosched> <src1>, <src2>
#131  class 31       mask FF38000000000000  value FB18000000000000  smbo
      smbo<nosched> <src1>, <src2>
#132  class 31       mask FF38000000000000  value F810000000000000  smoa
      smoa<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>, <op1>, <op2>, <op3>, <op4>
#133  class 31       mask FF38000000000000  value F910000000000000  smr
      smr<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
#134  class 31       mask FF38000000000000  value FB10000000000000  smbo
      smbo<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
#135  class 31       mask FF38000000000000  value FA10000000000000  smlsi
      smlsi<nosched> <f1>, <f2>, <f3>, <f4><extra>, #<imm1>, #<imm2>, #<imm3>
#136  class 31       mask FF30000000000000  value FC10000000000000  imo
      imo<nosched> #<imm1>, #<imm2>, #<imm3>, #<imm4>
#137  class 31       mask FF30000000000000  value FD10000000000000  setfc
      setfc<nosched> #<imm1>, #<imm2>
#138  class 31       mask FF30000000000000  value FE10000000000000  moest
      moest<skipinv><nosched> <dst>, #<imm>
#139  class 31       mask FF30000000000000  value F820000000000000  idf
      idf<nosched> drc<drc>, <op>
#140  class 31       mask FF30000000000000  value F920000000000000  wdf
      wdf<nosched> drc<drc>
#141  class 31       mask FF30000000000000  value FA20000000000000  setm
      setm<nosched> #<imm>
#142  class 31       mask FF30000000000000  value FF20000000000000  wop
      wop<nosched><twopart> #<imm>
#143  class 31       mask FF30000000000080  value FD20000000000000  lock release
      <op><nosched> #<imm>
#144  class 31       mask FF39000030000000  value FE21000020000000  ldr
      ldr<flags> <dst>, #<imm>, drc<drc>
#145  class 31       mask FF38000000000000  value FE20000000000000  ldr
      ldr<flags> <dst>, <src2>, drc<drc>
#146  class 31       mask FF39000030000000  value FE29000020000000  str
      str<flags> #<imm>, <src1>
#147  class 31       mask FF38000000000000  value FE28000000000000  str
      str<flags> <src2>, <src1>
#148  class 31       mask FF30C00000000000  value FB20800000000000  emitpds
      emitpds<nosched><end><tasks><taske><freep> #<imm1> /* incp */, <src0>, <src1>, <src2>, #<imm2>
#149  class 31       mask FF30D00000000000  value FB20400000000000  emitmtest
      emitmtest<nosched><end><threepart><freep> #<imm> /* incp */, <src1>, <src2>
#150  class 31       mask FF30D00000000000  value FB20500000000000  emitmtevtx
      emitmtevtx<nosched><end><twopart><cut><freep> #<imm> /* incp */, <src1>, <src2>
#151  class 31       mask FF30D00000000000  value FB20C00000000000  emitvcbst
      emitvcbst<nosched><end><threepart><freep> #<imm> /* incp */, <src2>
#152  class 31       mask FF30D00000000000  value FB20D00000000000  emitvcbvtx
      emitvcbvtx<nosched><end><twopart><freep> #<imm> /* incp */, <src2>
#153  class 31       mask FF30800000000000  value F830000000000000  pcoeff
      pcoeff<nosched><end> <src0>, <src1>, <src2>
#154  class 31       mask FF30800000000000  value F830800000000000  ptoff
      ptoff<nosched><end>
#155  class 31       mask FF30000000000000  value F930000000000000  atst8
      <pred>atst8<flags> <dst>, <w>p<p>, <src0>, <src1>, <src2><extra><op>
#156  class 31       mask FF30000000000000  value FB30000000000000  depthf
      <pred>depthf<flags> <src0>, <src1>, <src2><extra><op>
#157  class 31       mask FF70000000000000  value F840000000000000  cf cfi ci
      <op><flags> <src1>, <src2>, #<imm>
#158  class 31       mask FF74000000000000  value FA44000000000000  phas
      phas<end> #<imm1>, #<imm2>, <op1>, <op2>, <op3>
#159  class 31       mask FF74000000000000  value FA40000000000000  phas
      phas<nosched><end> <src1>, <src2>
```

**Class 5**

```
#1    class 5        mask F820070000000000  value 2800000000000000  mov
      <pred>mov<skipinv><syncs><nosched><end> <dst><wmask>, <src1>
#2    class 5        mask F820070000000000  value 2820000000000000  mov
      <pred>mov<skipinv><syncs><nosched><end><repeat> <dst>, <src1>
#187  class 5        mask F820000000000000  value 2800000000000000  movc
      <pred>movc<flags1><flags2><flags3> <dst><wmask>, <src0>, <src1>, <src2>
#188  class 5        mask F820000000000000  value 2820000000000000  movc
      <pred>movc<flags1><flags2><repeat><flags3> <dst>, <src0>, <src1>, <src2>
#189  class 5        mask F820070000000000  value 2820010000000000  movc
      <pred>movc<flags1><repeat>.i8<flags2> <dst>, <src0>, <src1>, <src2>
#190  class 5        mask F820070000000000  value 2800060000000000  movmsk
      <pred>movmsk<flags1><flags2> <dst><wmask>, <src1>, <src2><flags3>
#191  class 5        mask F820070000000000  value 2820060000000000  movmsk
      <pred>movmsk<flags1><flags2><repeat> <dst>, <src1>, <src2><flags3>
#192  class 5        mask F820070000000000  value 2800070000000000  movmsk
      <pred>movmsk<flags1><flags2>.smsk <dst><wmask>, <src0>, <src1>, <src2><flags3>
#193  class 5        mask F820070000000000  value 2820070000000000  movmsk
      <pred>movmsk<flags1><flags2>.smsk<repeat> <dst>, <src0>, <src1>, <src2><flags3>
#194  class 5        mask F860078000000000  value 2800068000000000  movmsk
      <pred>movmsk<flags1>.u8 <dst><wmask>, <src1>, <src2><flags2>
#195  class 5        mask F860078000000000  value 2820068000000000  movmsk
      <pred>movmsk<flags1><repeat>.u8 <dst>, <src1>, <src2><flags2>
#196  class 5        mask F860078000000000  value 2800078000000000  movmsk
      <pred>movmsk<flags1>.smsk.u8 <dst><wmask>, <src0>, <src1>, <src2><flags2>
#197  class 5        mask F860078000000000  value 2820078000000000  movmsk
      <pred>movmsk<flags1>.smsk<repeat>.u8 <dst>, <src0>, <src1>, <src2><flags2>
```

**Class 10-11**

```
#15   classs 10-11   mask F021000030000000  value 5001000020000000  and or xor
      <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
#16   classs 10-11   mask F021000030000000  value 5021000020000000  and or xor
      <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
#17   classs 10-11   mask F020000000000000  value 5000000000000000  and or xor
      <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
#18   classs 10-11   mask F020000000000000  value 5020000000000000  and or xor
      <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
```

**Class 12-13**

```
#19   classs 12-13   mask F021000030000000  value 6001000020000000  asr rol shl shr
      <pred><op><flags> <dst><wmask>, <src1>, <inv>#<imm>
#20   classs 12-13   mask F021000030000000  value 6021000020000000  asr rol shl shr
      <pred><op><flags><repeat> <dst>, <src1>, <inv>#<imm>
#21   classs 12-13   mask F020000000000000  value 6000000000000000  asr rol shl shr
      <pred><op><flags> <dst><wmask>, <src1>, <inv><src2>
#22   classs 12-13   mask F020000000000000  value 6020000000000000  asr rol shl shr
      <pred><op><flags><repeat> <dst>, <src1>, <inv><src2>
```

**Class 9**

```
#23   class 9        mask F800000000000000  value 4800000000000000  and asr or rol shl shr xor
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#24   class 9        mask F804000000000000  value 4800000000000000  fadd fdp fdsx fdsy fmax fmin
      fmul fpadd8 fpmul8 fpsub8 fsub fsubflr iadd16 iadd32 iadd8 iaddu16 iaddu32 iaddu8 imul16 imul8
      imulu16 imulu8 isub16 isub8 isubu16 isubu8
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#25   class 9        mask F804000000000000  value 4800000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
#198  class 9        mask F804000000000000  value 4804000000000000  iadd16 iadd32 iaddu16 iaddu32
      imul16 imulu16 isub16 isubu16
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#199  class 9        mask F804000000000000  value 4804000000000000  iadd8 iaddu8 imul8 imulu8 isub8
      isubu8
      <pred><op><flags>.sat.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#200  class 9        mask F804000000000000  value 4804000000000000  fadd fmul fsub fsubflr
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#201  class 9        mask F804000000000000  value 4804000000000000  fdp fdsx fdsy fmax fmin
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
#202  class 9        mask F804000000000000  value 4804000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>
#203  class 9        mask F804000000000000  value 4804000000000000  fpadd8 fpmul8 fpsub8
      <pred><op><flags>.<test> <w><dst><wmask>, p<p>, <src1>, <src2>
```

**Class 21**

```
#26   class 21       mask F80083C000000000  value A800008000000000  imae
      <pred>imae<flags>.<type><rs> <dst>, <src0>.<half1>, <src1>.<half2>, <src2>, u32
#176  class 21       mask F800008000000000  value A800008000000000  imae
      <pred>imae<flags>.<type><rs> <dst>, <src0>, <src1>, <src2>, <op>
#177  class 21       mask F800008000000000  value A800000000000000  imae
      <pred>imae<flags>.<type><rs> <dst>, <src0>, <src1>, <src2>, <op>
```

**Class 20**

```
#27   class 20       mask F80083E000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2><neg>, u16, u16
#172  class 20       mask F80003C000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#173  class 20       mask F800030000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#174  class 20       mask F80000C000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
#175  class 20       mask F800000000000000  value A000000000000000  ima16
      <pred>ima16<flags>.<type><rs> <dst><abs>, <src0>, <src1>, <src2>, <op>
```

**Class 26**

```
#28   class 26       mask F8300C4000000000  value D000000000000000  ima32
      <pred>ima32<flags>.<type> <dst>, <src0>, <src1><neg1>, <src2><neg2>
#178  class 26       mask F830000000000000  value D000000000000000  ima32
      <pred>ima32<flags>.<type> <dst><extra1>, <src0>, <src1>, <src2><extra2>
#179  class 26       mask F830000000000000  value D010000000000000  idiv
      <pred>idiv<flags> <dst>, <src1>, <src2>, drc<drc>
```

**Class 29**

```
#29   class 29       mask F840000000000000  value E800000000000000  ldab ldad ldaw ldlb ldld ldlw
      ldtb ldtd ldtw
      <pred>ld<op><flags> <dst>, [<base>,<mode><sign><src1>]<extra>, drc<drc>
#30   class 29       mask F840030000000000  value E800010000000000  ldab ldad ldaw ldlb ldld ldlw
      ldtb ldtd ldtw
      <pred>ld<op><flags> <dst>, [<base>,<step><src1>]<extra>, drc<drc>
#31   class 29       mask F840030000000000  value E800020000000000  ldab ldad ldaw ldlb ldld ldlw
      ldtb ldtd ldtw
      <pred>ld<op><flags> <dst>, [<base>,<src1><step>]<extra>, drc<drc>
#123  class 29       mask F840000000000000  value E840000000000000  eldd eldq
      <pred>eld<op><flags> <dst>, [<base>, <src0>, #<imm>], <src2>, drc<drc>
```

**Class 30**

```
#32   class 30       mask F800000000000000  value F000000000000000  stab stad staw stlb stld stlw
      sttb sttd sttw
      <pred>st<op><flags> [<base>,<mode><sign><src1>], <src2>
#33   class 30       mask F800030000000000  value F000010000000000  stab stad staw stlb stld stlw
      sttb sttd sttw
      <pred>st<op><flags> [<base>,<step><src1>], <src2>
#34   class 30       mask F800030000000000  value F000020000000000  stab stad staw stlb stld stlw
      sttb sttd sttw
      <pred>st<op><flags> [<base>,<src1><step>], <src2>
```

**Class 0**

```
#39   class 0        mask F840000000000000  value 0000000000000000  fadm fmad fmsa
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
#40   class 0        mask F840060000000000  value 0000060000000000  fsubflr
      <pred>fsubflr<flags><repeat><incs> <dst>, <src1>, <src2>
#41   class 0        mask F840000000000000  value 0040000000000000  fadm fmad fmsa
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
#42   class 0        mask F840060000000000  value 0040060000000000  fsubflr
      <pred>fsubflr<flags><repeat><incs> <dst>, <src1>, <src2>
```

**Class 1**

```
#43   class 1        mask F820000000000000  value 0800000000000000  fcos fexp flog frcp frsq fsin
      fsqrt
      <pred><op><flags1> <dst><wmask>, <src1><flags2>
#44   class 1        mask F820000000000000  value 0820000000000000  fcos fexp flog frcp frsq fsin
      fsqrt
      <pred><op><flags1><repeat> <dst>, <src1><flags2>
```

**Class 2**

```
#45   class 2        mask F840060000004000  value 1000000000000000  fdp
      <pred>fdp<flags><repeat><incs> <dst>, <src1>, <src2>
#46   class 2        mask F840060000004000  value 1000000000004000  fdpc
      <pred>fdpc<flags><repeat><incs> <dst>, cp<cp>, <src1>, <src2>
#47   class 2        mask F840060000000000  value 1000020000000000  fddp
      <pred>fddp<flags><repeat><incs> <dst>, i<i>, <src0>, <src1>, <src2>
#48   class 2        mask F840060000000000  value 1000040000000000  fddpc
      <pred>fddpc<flags><repeat> <dst>, i<i>, cp<cp1>, cp<cp2>, <src0>, <src1>, <src2>
#49   class 2        mask F840060000004000  value 1040000000000000  fdp
      <pred>fdp<flags><repeat><incs> <dst>, <src1>, <src2>
#50   class 2        mask F840060000004000  value 1040000000004000  fdpc
      <pred>fdpc<flags><repeat><incs> <dst>, cp<cp>, <src1>, <src2>
#51   class 2        mask F840060000000000  value 1040020000000000  fddp
      <pred>fddp<flags><repeat><incs> <dst>, i<i>, <src0>, <src1>, <src2>
#52   class 2        mask F840060000000000  value 1040040000000000  fddpc
      <pred>fddpc<flags><repeat> <dst>, i<i>, cp<cp1>, cp<cp2>, <src0>, <src1>, <src2>
```

**Class 3**

```
#53   class 3        mask F840000000000000  value 1800000000000000  fmaxmin fminmax
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
#54   class 3        mask F840000000000000  value 1840000000000000  fmaxmin fminmax
      <pred><op><flags><repeat><incs> <dst>, <src0>, <src1>, <src2>
```

**Class 4**

```
#55   class 4        mask F840000000000000  value 2000000000000000  fdsx fdsy
      <pred><op><flags><repeat><incs> <dst>, <src1>, <src2>
#56   class 4        mask F840000000000000  value 2040000000000000  fdsx fdsy
      <pred><op><flags><repeat><incs> <dst>, <src1>, <src2>
```

**Class 6**

```
#57   class 6        mask F800000000000000  value 3000000000000000  fmad16
      <pred>fmad16<flags1><flags2> <dst>, <src0><flags3>, <src1><flags4>, <src2><flags5>
```

**Class 7**

```
#58   class 7        mask F800000000000000  value 3800000000000000  efo
      <pred>efo<flags> <dst>= <reg>, <f4>, <op1>, <op2>, <src0>, <src1>, <src2>
```

**Class 25**

```
#59   class 25       mask F800000000000000  value C800000000000000  fpma
      <pred>fpma<flags> <dst>, <src0>, <src1>, <src2>, <op1>, <op2>
```

**Class 27**

```
#60   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 fadm fmad fminmax fmsa ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#61   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr fadm
      fmad fminmax fmsa ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#62   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fadm fmad fminmax fmsa ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#63   class 27       mask F840C00000000000  value D800000000000000  +mov fadm fmad fminmax fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#64   class 27       mask F840C00000000000  value D800400000000000  +mov fadm fmad fminmax fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#65   class 27       mask F840C00000000000  value D800800000000000  +mov fadm fmad fminmax fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#66   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 mov
      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#67   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
#68   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#69   class 27       mask F840C00000000000  value D800000000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc0>
#70   class 27       mask F840C00000000000  value D800400000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#71   class 27       mask F840C00000000000  value D800800000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc2>
#72   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#73   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#74   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#75   class 27       mask F840C00000000000  value D800000000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#76   class 27       mask F840C00000000000  value D800400000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#77   class 27       mask F840C00000000000  value D800800000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#78   class 27       mask F840000000000000  value D800000000000000  +fmad +fmad16 +fmaxmin +fmsa
      +ima32 fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#79   class 27       mask F840000000000000  value D800000000000000  +fadd +fmul +fsub +fsubflr fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#80   class 27       mask F840000000000000  value D800000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#81   class 27       mask F840C00000000000  value D800000000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#82   class 27       mask F840C00000000000  value D800400000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#83   class 27       mask F840C00000000000  value D800800000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#84   class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#85   class 27       mask F840000000000000  value D840000000000000  +ima32 fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#86   class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr fadm
      fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#87   class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#88   class 27       mask F840C00000000000  value D840000000000000  +mov fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#89   class 27       mask F840C00000000000  value D840400000000000  +mov fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#90   class 27       mask F840C00000000000  value D840800000000000  +mov fadm fmad fminmax fmsa
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#91   class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#92   class 27       mask F840000000000000  value D840000000000000  +ima32 ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#93   class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#94   class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#95   class 27       mask F840C00000000000  value D840000000000000  +mov ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#96   class 27       mask F840C00000000000  value D840400000000000  +mov ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#97   class 27       mask F840C00000000000  value D840800000000000  +mov ima32
      <pred><op> <dst>, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#98   class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa mov
      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#99   class 27       mask F840000000000000  value D840000000000000  +ima32 mov
      <pred><op> <dst>, <src1>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#100  class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>, <ssrc2>
#101  class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#102  class 27       mask F840C00000000000  value D840000000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc0>
#103  class 27       mask F840C00000000000  value D840400000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc1>
#104  class 27       mask F840C00000000000  value D840800000000000  +mov mov
      <pred><op> <dst>, <src1>+<sop> <sreg>, <ssrc2>
#105  class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#106  class 27       mask F840000000000000  value D840000000000000  +ima32 fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#107  class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#108  class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#109  class 27       mask F840C00000000000  value D840000000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#110  class 27       mask F840C00000000000  value D840400000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#111  class 27       mask F840C00000000000  value D840800000000000  +mov fddp
      <pred><op> <dst>, i0, <src0>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
#112  class 27       mask F840000000000000  value D840000000000000  +fmad +fmad16 +fmaxmin +fmsa
      fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#113  class 27       mask F840000000000000  value D840000000000000  +ima32 fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <ssrc0>, <ssrc1>, <ssrc2>
#114  class 27       mask F840000000000000  value D840000000000000  +fadd +fmul +fsub +fsubflr fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>, <ssrc2>
#115  class 27       mask F840000000000000  value D840000000000000  +fcos +fexp +flog +frcp +frsq
      +fsin fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#116  class 27       mask F840C00000000000  value D840000000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc0>
#117  class 27       mask F840C00000000000  value D840400000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc1>
#118  class 27       mask F840C00000000000  value D840800000000000  +mov fssq
      <pred><op> <dst>, <src1>, <src2>+<sop> <sreg>, <ssrc2>
```

**Class 28**

```
#119  class 28       mask F820000000000000  value E000000000000000  smp1d smp2d smp3d
      <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, drc<drc>
#120  class 28       mask F820000000000000  value E020000000000000  smp1d smp2d smp3d
      <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, drc<drc>
#121  class 28       mask F820000000000000  value E000000000000000  smp1dbias smp1dgrad smp1dreplace
      smp2dbias smp2dgrad smp2dreplace smp3dbias smp3dgrad smp3dreplace
      <pred>smp<op><flags1> <dst><wmask>, <src0><flags2>, <src1>, <src2>, drc<drc>
#122  class 28       mask F820000000000000  value E020000000000000  smp1dbias smp1dgrad smp1dreplace
      smp2dbias smp2dgrad smp2dreplace smp3dbias smp3dgrad smp3dreplace
      <pred>smp<op><flags1><repeat> <dst>, <src0><flags2>, <src1>, <src2>, drc<drc>
```

**Class 8**

```
#160  class 8        mask F820000000000000  value 4000000000000000  pckc10f16 pckc10f32 pckc10s16
      pckc10u16 pckf16f16 pckf16f32 pckf16s16 pckf16u16 pcko8f16 pcko8f32 pcko8s16 pcko8u16
      pcks16f16 pcks16f32 pcks16s16 pcks16u16 pcks8f16 pcks8f32 pcks8s16 pcks8u16 pcku16f16
      pcku16f32 pcku16s16 pcku16u16 pcku8f16 pcku8f32 pcku8s16 pcku8u16 unpckc10c10 unpckf16c10
      unpckf16o8 unpckf16s8 unpckf16u8 unpckf32c10 unpckf32f16 unpckf32f32 unpckf32o8 unpckf32s16
      unpckf32s8 unpckf32u16 unpckf32u8 unpcks16c10 unpcks16o8 unpcks16s8 unpcks16u8 unpcku16c10
      unpcku16o8 unpcku16s8 unpcku16u8 unpcku8s8 unpcku8u8
      <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
#161  class 8        mask F820000000000000  value 4020000000000000  pckc10f16 pckc10f32 pckc10s16
      pckc10u16 pckf16f16 pckf16f32 pckf16s16 pckf16u16 pcko8f16 pcko8f32 pcko8s16 pcko8u16
      pcks16f16 pcks16f32 pcks16s16 pcks16u16 pcks8f16 pcks8f32 pcks8s16 pcks8u16 pcku16f16
      pcku16f32 pcku16s16 pcku16u16 pcku8f16 pcku8f32 pcku8s16 pcku8u16 unpckc10c10 unpckf16c10
      unpckf16o8 unpckf16s8 unpckf16u8 unpckf32c10 unpckf32f16 unpckf32f32 unpckf32o8 unpckf32s16
      unpckf32s8 unpckf32u16 unpckf32u8 unpcks16c10 unpcks16o8 unpcks16s8 unpcks16u8 unpcku16c10
      unpcku16o8 unpcku16s8 unpcku16u8 unpcku8s8 unpcku8u8
      <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
#162  class 8        mask F820000000000000  value 4000000000000000  statomic
      <pred><op1> <dst><bytemask><wmask>, <src1>, <src2>, <op2>
#163  class 8        mask F820000000000000  value 4020000000000000  statomic
      <pred><op1><repeat> <dst><bytemask>, <src1>, <src2>, <op2>
```

**Class 16**

```
#164  class 16       mask F800000000000000  value 8000000000000000  +asop2 sop2
      <pred>sop2<flags> <dst>, <src1>, <src2>, <op>+asop2 <sop>
```

**Class 17**

```
#165  class 17       mask F8200C0000000000  value 8800000000000000  +asop sop3
      <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
#166  class 17       mask F8200C0000000000  value 8800080000000000  +arsop sop3
      <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+arsop <sop>
#167  class 17       mask F820040000000000  value 8800040000000000  +alrp sop3
      <pred>sop3<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
#168  class 17       mask F820040000000000  value 8820000000000000  +asop lrp1
      <pred>lrp1<flags> <dst>, <src0>, <src1>, <src2>, <op>+asop <sop>
#169  class 17       mask F820040000000000  value 8820040000000000  +alrp lrp2
      <pred>lrp2<flags> <dst>, <src0>, <src1>, <src2>, <op>+alrp <sop>
```

**Class 18**

```
#170  class 18       mask F800000000000000  value 9000000000000000  sop2wm
      <pred>sop2wm<flags> <dst><bytemask>, <src1>, <src2>, <op>
```

**Class 19**

```
#171  class 19       mask F800000000000000  value 9800000000000000  ima8
      <pred>ima8<flags><sat> <dst>, <src0>, <src1>, <src2>, <op>
```

**Class 24**

```
#180  class 24       mask F900000000000000  value C100000000000000  u16dot4 u16dot4off u8dot4
      u8dot4off
      <pred><op1>dot4<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
#181  class 24       mask F900060000000000  value C000060000000000  u16dot3 u16dot3off u8dot3
      u8dot3off
      <pred><op1>dot3<op2><flags> <dst>, Cx<cx>, Ax<ax>, <src1>, <src2>
#182  class 24       mask F930000000000000  value C030000000000000  u16dot3 u16dot3off
      <pred>u16dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2><mode>
#183  class 24       mask F900002000000000  value C000000000000000  +aadd +asub u8dot3 u8dot3off
      <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> Ax<sax>, <sop2>
#184  class 24       mask F900002000000000  value C000002000000000  +aintrp1 +aintrp2 u8dot3
      u8dot3off
      <pred>u8dot3<op><flags> <dst>, Cx<cx>, <src1>, <src2>+<sop1> <ssrc0>, <sop2>
```

**Class 14**

```
#185  class 14       mask F820000000000000  value 7000000000000000  rlp
      <pred>rlp<flags> <dst><wmask>, <src1>, p<p>
#186  class 14       mask F820000000000000  value 7020000000000000  rlp
      <pred>rlp<flags><repeat> <dst>, <src1>, p<p>
```

**Class 15**

```
#204  class 15       mask F800000000000000  value 7800000000000000  and asr or rol shl shr xor
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#205  class 15       mask F804000000000000  value 7800000000000000  fadd fdp fdsx fdsy fmax fmin
      fmul fpadd8 fpmul8 fpsub8 fsub fsubflr iadd16 iadd32 iadd8 iaddu16 iaddu32 iaddu8 imul16 imul8
      imulu16 imulu8 isub16 isub8 isubu16 isubu8
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#206  class 15       mask F804000000000000  value 7800000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>
#207  class 15       mask F804000000000000  value 7804000000000000  iadd16 iadd32 iaddu16 iaddu32
      imul16 imulu16 isub16 isubu16
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#208  class 15       mask F804000000000000  value 7804000000000000  iadd8 iaddu8 imul8 imulu8 isub8
      isubu8
      <pred><op><flags>.sat.<test> <w><dst><wmask>, <src1>, <src2>
#209  class 15       mask F804000000000000  value 7804000000000000  fadd fmul fsub fsubflr
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#210  class 15       mask F804000000000000  value 7804000000000000  fdp fdsx fdsy fmax fmin
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
#211  class 15       mask F804000000000000  value 7804000000000000  fexp flog frcp frsq
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>
#212  class 15       mask F804000000000000  value 7804000000000000  fpadd8 fpmul8 fpsub8
      <pred><op><flags>.<test> <w><dst><wmask>, <src1>, <src2>
```

**Class 22**

```
#213  class 22       mask F830004000000000  value B000000000000000  adif
      <pred>adif<flags> <dst>, <src1>, <src2>
#214  class 22       mask F830004000000000  value B000004000000000  adifsum
      <pred>adifsum<flags> <dst>, <src0>.<half>, <src1>, <src2>
#215  class 22       mask F830000000000000  value B020000000000000  bilin
      <pred>bilin<flags> <dst>, <src0>, <src1>, <src2>, <op1>, <op2>, <op3><extra>
#216  class 22       mask F830000000000000  value B030000000000000  firv
      <pred>firv<flags> <w><dst>, <src0>, <src2>, <op><extra>
```

**Class 23**

```
#217  class 23       mask F800000000000000  value B800000000000000  firh
      <pred>firh<flags> <dst>, <src0>, <src2>, <op1>, <op2>, #<imm1>, #<imm2>
```
