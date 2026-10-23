# SGX545 PDS instruction set reference

The **PDS** (programmable data sequencer) of the SGX545 runs small programs that prepare and
start USSE tasks: they DMA vertex data and state into the USSE attribute registers, issue
iterator and texture work, and start a task on a code address with `doutu`. Every USSE task
starts this way, the microkernel's own included (`design/sgx545_hw_spec.md` §13.7-§13.8). The
USSE instruction set has its own reference, `design/sgx545_usse_isa.md`.

This reference covers the PDS instruction set as our disassembler `tools/usse-dis/pds-dis`
decodes it and our assembler `tools/usse-asm/pds-asm` writes it. The decoder was derived
black-box from the reference disassembler and matches it on every word (0 wrong, nothing
missed, `tools/usse-dis/README.md`); the assembler describes the same layouts as forms in
`tools/usse-asm/pds_forms.c` and checks each word it builds against the decoder. The
generated parts of this document read those forms.

No simulator models the PDS and no hardware probe has been run on it: **[HW]** here means that
one of our own programs depends on the behaviour and works, as the transfer and fill paths do
(`design/sgx545_transfer.md`).

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

Two tags joined by a slash (**[CAP/HW]**, **[RE/CAP]**) mean both kinds of evidence agree, the
first being the primary one. The design documents also use **[patent]**, **[PSP2]** and
**[decide]** (HACKING.md).

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

## 2. Programs and the instruction word

A PDS program is a **data segment** followed by **instructions**, both 32-bit little-endian
words **[CAP/HW]**. The data segment holds the program's constants (code addresses, DMA control
words, masks); the hardware is told its size separately, in rows of four dwords (16 bytes):
in a `doutu`-started program's resource record, in the VDM state words, or in the
`EVENT_*_PDS_DATA` registers (`design/sgx545_hw_spec.md` §13.8, `design/sgx545_3d.md` §9.1).
Branch targets count instructions from the first instruction after the data segment
**[RE/CAP]**.

An instruction word has the **opcode** in `w[31:27]` and, in most groups, the **predicate** in
`w[26:24]` (7 = always); the remaining 24 bits are the operands, laid out per group **[RE]**.
The opcodes and the mnemonics each one holds (the integer ALU group takes its operation from
the opcode field itself, so its single form spans several opcodes):

- Opcode 0 (`0x00`): `movs`
- Opcode 1 (`0x01`): `mov16`
- Opcode 2 (`0x02`): `mov32`
- Opcode 3 (`0x03`): `mov64`
- Opcode 4 (`0x04`): `mov128`
- Opcode 5 (`0x05`): `movsa`
- Opcode 6 (`0x06`): `load`
- Opcode 7 (`0x07`): `store`
- Opcode 8 (`0x08`): (none: no form decodes it)
- Opcode 9 (`0x09`): (none: no form decodes it)
- Opcode 10 (`0x0a`): (none: no form decodes it)
- Opcode 11 (`0x0b`): (none: no form decodes it)
- Opcode 12 (`0x0c`): `mul`
- Opcode 13 (`0x0d`): `abs`
- Opcode 14 (`0x0e`): (none: no form decodes it)
- Opcode 15 (`0x0f`): `rfence`, `rwfence`
- Opcode 16 (`0x10`): `tstz`
- Opcode 17 (`0x11`): `tstn`
- Opcode 18 (`0x12`): `bra`
- Opcode 19 (`0x13`): `call`
- Opcode 20 (`0x14`): `rtn`
- Opcode 21 (`0x15`): `halt`
- Opcode 22 (`0x16`): `nop`
- Opcode 23 (`0x17`): `alum`
- Opcode 24 (`0x18`): (none: no form decodes it)
- Opcode 25 (`0x19`): (none: no form decodes it)
- Opcode 26 (`0x1a`): (none: no form decodes it)
- Opcode 27 (`0x1b`): `not`
- Opcode 28 (`0x1c`): (none: no form decodes it)
- Opcode 29 (`0x1d`): (none: no form decodes it)
- Opcode 30 (`0x1e`): `shl`
- Opcode 31 (`0x1f`): `shr`
- With the opcode in a field: `adc`, `add`, `and`, `nand`, `nor`, `or`, `sbc`, `sub`, `xor`

## 3. Machine model

### 3.1 Data registers

The data segment is loaded into two banks, `ds0` and `ds1`, of 64 dwords each **[RE]**. It is
stored **in rows of four dwords, two per bank**: row `r` holds `ds0[2r]`, `ds0[2r+1]`,
`ds1[2r]`, `ds1[2r+1]` **[CAP/HW]**. So data dwords 0-3 are `ds0[0]`, `ds0[1]`, `ds1[0]`,
`ds1[1]`, dwords 4-7 are `ds0[2]`, `ds0[3]`, `ds1[2]`, `ds1[3]`, and so on; every one of our
PDS programs documents its data segment this way, and the hardware reads them so.

The registers past the data segment are scratch: our programs compute in `ds0[48]`, `ds1[48]`
and `ds1[50]` **[HW]**, and `load`/`store` move memory to and from `ds0[48]`/`ds1[48]` and up
**[RE]**. Whether the banks are cleared between programs is **[open]**.

### 3.2 Input and special registers

- `ir0`, input word 0: the vertex index of a vertex program **[HW]**, the tile position of
  the pixel-event program **[infer]**, the request mask the microkernel passes to its
  PDS #14 **[RE]**.
- `ir1`, input word 1: the event bits of the pixel-event program **[CAP/infer]**.
- `ir2`, input word 2 **[open]**.
- `tim`: a timer? **[open]** (readable, and a `movs` destination).
- `pc`: the program counter, readable by `mov16`/`mov32` **[RE]**.
- `mr01`, `mr23`: 64-bit sources of `mov64`; `mr` is also a `movs` destination **[open]**.

Some operand fields reach only a prefix of the specials (`ir0`, `ir1`; or `ir0`, `ir1`, `tim`),
and `mov16`/`mov32` number them with `pc` inserted **[RE]**: the field descriptions below say
which.

### 3.3 Predicates

The predicate field `w[26:24]` reads `p0`, `p1`, `p2`, `if0`, `if1`, `aluz`, `alun` or nothing
(7) **[RE]**:

- `p0`-`p2` are written by `tstz` (`pN = (src == 0)`) and `tstn` (`pN = (src < 0)`
  **[infer]**). `tstz` setting its predicate on zero is **[CAP/HW]**: our pixel-event program,
  bit-identical to the closed driver's, skips each case with `tstz p0, x; (p0) bra` and the
  right task runs for each event.
- `if0` and `if1` are **[open]**. Our vertex program starts its shader under `(if0)`, as the
  closed driver's vertex programs do **[CAP]**; a reading such as "first instance of a
  batch" fits, nothing more.
- `aluz` and `alun` presumably test the zero and negative flags of the last ALU result
  **[infer]**.

`load`, `store`, the fences and `nop` use `w[26:24]` for other purposes or not at all, and
carry no predicate.

### 3.4 The output ports

A `movs` (or `movsa`) writes four source words to a **port**; the `dout*` ports issue work to
other units **[RE]**. What each port does with its words, where we know it:

| port | words | known meaning |
|------|-------|---------------|
| `doutu` | U0, U1, U2 | start a USSE task **[HW]** |
| `doutd` | address, control | DMA memory into the USSE attributes **[HW]** |
| `douta` | value, control | write one attribute **[CAP/HW]** |
| `douti` | two words | issue iterator work **[HW]** |
| `doutt` | T0-T3 | the state of a texture fetch **[HW]** |
| `doutc`, `douts`, `slc`, `mr`, `tim`, `ir0` | | **[open]** |

- **`doutu`** **[HW]**. U0 holds the code address: `CBASE` (`USE_CODE_BASE_n`) in bits 3-0 and
  the instruction index above it (`EXE`, bits 19-4 on this build's layout), plus the
  temporaries in fours (`TRC`) in bits 31-24. U1 holds flags: bit 2 `SDSOFT` for the pixel
  and secondary programs, bit 23 `PBENABLE` for the end-of-tile task, bit 22 `ENDOFRENDER`,
  bit 25 `TEXTUREDEPENDENCY` (wait for the texture result); U2 is 0 in all our programs
  (`design/sgx545_transfer.md` §4.2.4, §4.4.3). A `movs` always names four sources; `doutu`
  uses three.
- **`doutd`** **[CAP/HW]**. The first word is the source address; the second is
  burst size − 1 (bits 3-0), lines − 1 (7-4), the destination attribute (19-8) and
  stride − 1 (29-22) (`design/sgx545_3d.md` S.2). With `movsa` the third source is added to
  the address: our vertex program fetches vertex `index × stride` this way.
- **`douta`** writes its first word to the attribute named by bits 19-8 of the second
  **[CAP]**; our pixel-event program uses it before starting the end-of-tile task.
- **`douti`** and **`doutt`** **[HW]**: our copy's primary program issues one iterator word pair
  (one texture fetch from the first layer, no USE issue) and the four texture state words
  T0-T3, and the texel arrives in `pa0` of the task `doutu` started
  (`design/sgx545_transfer.md` §4.4.3).

## 4. Operand encodings

The PDS has no single operand encoding: each group lays out its own fields **[RE]**. The
recurring ones:

- **Seven-bit destination** (`w[6:0]`): `ds0[0]`-`ds0[63]`, then `ds1[0]`-`ds1[63]`.
- **Seven-bit first source** (`w[23:17]`): `ds0[0]`-`ds0[63]`, then the first two or three of
  `ir0`, `ir1`, `tim`; the second source of the ALU group is always `ds1[n]` (`w[15:10]`).
- **Halves** (`.l`, `.h`): `mov16` and `mul` read 16-bit halves of a register; the `movs`
  sources read halves of an input register.
- **The `movs` source list** (`w[23:5]`): a `ds1` pair (`w[17:13]` × 2) and either a `ds0`
  pair (`w[22:18]` × 2) or, with `w[23]` set, the input register numbered by `w[22:18]`;
  then four two-bit selectors, one per slot (`w[12:5]`), each picking the `ds1` pair or the
  other one, and its first or second dword (the low or high half of an input register). So
  the four sources of one `movs` come from at most two `ds0` and two `ds1` dwords.

`movsa doutd, ds0[0], ds0[1], ds1[50], ds1[50]` assembles with form #1: the word is `0x2F032343`; in
memory (little endian) `43 23 03 2F`.

| field | value | prints |
|---|---|---|
| `pred` | 0x7 | (none) |
| `dst` | 0x3 | `doutd` |
| `srcs` | 0x191a | `ds0[0], ds0[1], ds1[50], ds1[50]` |

## 5. Assembler syntax

`pds-asm` reads the syntax `pds-dis` prints, with the rules of the USSE assembler (comments,
labels, lenient blanks and number bases, listings; `tools/usse-asm/README.md`) and two of its
own:

- **The data segment** is the raw words (`.word 0x...`) before the first instruction, in file
  order: dword 0 first, so `ds0[0]`, `ds0[1]`, `ds1[0]`, `ds1[1]`, `ds0[2]`, ... by the row
  layout above. In a listing, a `data` line takes its value from the hex column.
- **Labels** count instructions from the first one after the data segment, as `bra` and `call`
  do, so `(p0) bra not_eot` works without counting data words.

A predicate is written in parentheses before the mnemonic: `(p0) bra done`, `(if0) movs ...`.

From `tools/usse-asm/progs/pds_prim.s`:

```
; pds_prim.s -- the PDS primary program of a fill, KICKONLY (template
; PDS_PRIM, id 4).
;
; The background object names this program; the PDS runs it per pixel batch
; and it only starts the fill shader (use_fill.s) as a USE task: a DOUTU
; issue of the three words U0-U2 (design/sgx545_transfer.md 4.2.4).
;
; Data segment (the first 4 dwords; banks interleave by pairs of dwords,
; hardware spec 13.8). The driver patches dwords 0-2 (sgx545_fw_format.md
; 9.4): U0 = the DOUTU word of USE_FILL (EXE [19:4], CBASE [3:0], TRC
; [31:24]), U1 = 4 (SDSOFT), U2 = 0.

        .word 0         ; dword 0 = ds0[0]: U0 (patched)
        .word 0         ; dword 1 = ds0[1]: U1 (patched)
        .word 0         ; dword 2 = ds1[0]: U2 (patched)
        .word 0         ; dword 3 = ds1[1]: unused

        ; Issue U0, U1, U2 to the USE through the DOUTU port. A movs always
        ; names four sources; DOUTU takes three, the fourth repeats ds0[0].
        movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]
        halt
```

## 6. Instruction groups

One section per group: what it does and what is known of it, how the microkernel's five PDS
programs and ours use it, examples, then the generated forms, mnemonics, diagrams and field
lists. The microkernel's programs (event dispatch, boot, deferred work) are described in
`design/sgx545_hw_spec.md` §13.8; they are counted here, not quoted.

### 6.1 Port writes: `movs`, `movsa`

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

From `tools/usse-asm/progs/pds_vtx.s`:

```
        movsa doutd, ds0[0], ds0[1], ds1[50], ds1[50]
        (if0) movs doutu, ds0[2], ds0[3], ds1[1], ds1[1]
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#0    opcode 0       mask F8000000  value 00000000  movs
      <pred>movs <dst>, <srcs>
#1    opcode 5       mask F8000000  value 28000000  movsa
      <pred>movsa <dst>, <srcs>
```

Mnemonics: `movs`, `movsa`.

Layout (forms #0, #1):

```
 31   27 26  24 23   5 4   3   0
+-------+------+------+---+-----+
| 00?0? | pred | srcs | - | dst |
+-------+------+------+---+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `dst` — bits 3:0. Values: 0 `mr`, 1 `slc`, 2 `douti`, 3 `doutd`, 4 `doutt`, 5 `doutu`, 6 `douta`,
  7 `tim`, 8 `doutc`, 9 `douts`, 15 `ir0`. Other values do not decode.
- `srcs` — bits 23:5. An operand list (see *Operand encodings*), for example 0 `ds0[0], ds0[0],
  ds0[0], ds0[0]`, 1 `ds0[0], ds0[0], ds0[0], ds0[1]`, 2 `ds0[0], ds0[0], ds0[0], ds1[0]`.

### 6.2 Data moves: `mov16`, `mov32`, `mov64`, `mov128`

Opcodes 1-4 **[RE]**: `mov16 dsB[n], src.h|l` moves a 16-bit half (sources: `ds0`, `ds1`, then
`ir0`, `ir1`, `pc`, `tim`, `ir2`, each `.l` or `.h`); `mov32 dsB[n], src` a dword (the same
sources, whole); `mov64 dsB[2k], src` an aligned pair (from a `ds0`/`ds1` pair, `mr01` or
`mr23`); `mov128 ds[2k], ds[2j]` four dwords, optionally `, swap`.

- **Semantics [infer].** Copies of the named width. Which half of the destination `mov16`
  writes, how `mov128` addresses two banks with one index, and what `swap` swaps are
  **[open]**.
- **In the microkernel's programs:** not used; nor in ours.

`mov16 ds0[3], ds1[2].h` assembles with form #2: the word is `0x0F008503`; in memory (little endian)
`03 85 00 0F`.

| field | value | prints |
|---|---|---|
| `pred` | 0x7 | (none) |
| `ds` | 0x0 | `0` |
| `idx` | 0x3 | `3` |
| `src` | 0x85 | `ds1[2].h` |

Forms (generated from the decoder's table; `<name>` is a field):

```
#2    opcode 1       mask F8000000  value 08000000  mov16
      <pred>mov16 ds<ds>[<idx>], <src>
#3    opcode 2       mask F8000000  value 10000000  mov32
      <pred>mov32 ds<ds>[<idx>], <src>
#4    opcode 3       mask F8000000  value 18000000  mov64
      <pred>mov64 ds<ds>[<idx>], <src>
#5    opcode 4       mask F8000000  value 20000000  mov128
      <pred>mov128 ds[<idx1>], ds[<idx2>]<extra>
```

Mnemonics: `mov128`, `mov16`, `mov32`, `mov64`.

Layout 1 of 4 (form #2):

```
 31   27 26  24 23 17 16  8 7    6   0
+-------+------+-----+-----+----+-----+
| 00001 | pred |  -  | src | ds | idx |
+-------+------+-----+-----+----+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `ds` — bit 7. A 1-bit number, printed in decimal.
- `idx` — bits 6:0. A 7-bit number, printed in decimal.
- `src` — bits 16:8. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0].l`…`ds0[63].h`, 1 `ds1[0].l`…`ds1[63].h`, 2 `ir0.l`.

Layout 2 of 4 (form #3):

```
 31   27 26  24 23 17 16  9 8   7    6   1 0
+-------+------+-----+-----+---+----+-----+---+
| 00010 | pred |  -  | src | - | ds | idx | - |
+-------+------+-----+-----+---+----+-----+---+
```

- `pred` — as in layout 1.
- `ds` — as in layout 1.
- `idx` — bits 6:1. A 6-bit number, printed in decimal.
- `src` — bits 16:9. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`…`ds1[63]`, 1 `ir0`.

Layout 3 of 4 (form #4):

```
 31   27 26  24 23 17 16 10 9 8 7    6   2 1 0
+-------+------+-----+-----+---+----+-----+---+
| 00011 | pred |  -  | src | - | ds | idx | - |
+-------+------+-----+-----+---+----+-----+---+
```

- `pred` — as in layout 1.
- `ds` — as in layout 1.
- `idx` — bits 6:2. A 5-bit number, printed in decimal, times 2.
- `src` — bits 16:10. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`.

Layout 4 of 4 (form #5):

```
 31   27 26  24 23 15 14  10 9   8       7   6    2 1 0
+-------+------+-----+------+---+-------+---+------+---+
| 00100 | pred |  -  | idx2 | - | extra | - | idx1 | - |
+-------+------+-----+------+---+-------+---+------+---+
```

- `pred` — as in layout 1.
- `idx1` — bits 6:2. A 5-bit number, printed in decimal, times 2.
- `idx2` — bits 14:10. Values: 0 `0`, 1 `2`, 2 `4`, 3 `6`, 4 `8`, 5 `10`, 6 `12`, 7 `14`, 8 `16`, 9
  `18`, 10 `20`, 11 `22`, 12 `24`, 13 `26`, 14 `28`, 15 `30`. Other values do not decode.
- `extra` — bit 8. Values: 0 (none), 1 `, swap`.

### 6.3 Memory: `load`, `store`

Opcodes 6 and 7 **[RE]**: `load addr, dsB[48+i ... ]` and `store addr, dsB[48+i ...]` move one to
eight dwords between memory and the registers from `ds0[48]` or `ds1[48]` up. The address is an
18-bit dword index (printed as a byte address, `× 4`); the count reuses the predicate bits
`w[26:24]` (1-8, 0 meaning 8), so these instructions cannot be predicated.

- **Semantics [infer].** The address is relative to a base that is **[open]** (18 bits of
  dwords reach 1 MiB); whether the transfer completes before the next instruction is
  **[open]** (the fences may be for that).
- **In the microkernel's programs:** not used; nor in ours.

`load 0x00001000, ds0[48-51]` assembles with form #6: the word is `0x34000400`; in memory (little
endian) `00 04 00 34`.

| field | value | prints |
|---|---|---|
| `imm` | 0x400 | `0x00001000` |
| `ds` | 0x0 | `0` |
| `idx` | 0x40 | `48-51]` |

Forms (generated from the decoder's table; `<name>` is a field):

```
#6    opcode 6       mask F8000000  value 30000000  load
      load <imm>, ds<ds>[<idx>
#7    opcode 7       mask F8000000  value 38000000  store
      store <imm>, ds<ds>[<idx>
```

Mnemonics: `load`, `store`.

Layout (forms #6, #7):

```
 31   27 26 24 23  22   21 18 17  0
+-------+-----+---+----+-----+-----+
| 0011? | idx | - | ds | idx | imm |
+-------+-----+---+----+-----+-----+
```

- `imm` — bits 17:0. An 18-bit number, printed in hex, times 4.
- `ds` — bit 22. A 1-bit number, printed in decimal.
- `idx` — bits 21:18 → v[3:0], 26:24 → v[6:4]. 128 distinct texts, for example 0 `48-55]`, 1
  `49-56]`, 2 `50-57]`, 3 `51-58]`, 4 `52-59]`, 5 `53-60]`.

### 6.4 Integer ALU: `add`, `sub`, `adc`, `sbc`, `and`, `or`, `xor`, `nor`, `nand`

Opcodes 8-11 (`add`, `sub`, `adc`, `sbc`) and 24-26, 28-29 (`or`, `and`, `xor`, `nor`, `nand`)
share one layout: `op dst, src, ds1[n]` with a seven-bit destination, a first source from
`ds0` or `ir0`/`ir1`/`tim`, and `ds1[n]` **[RE]**. Opcode 27 is `not` (*`abs`, `not`*) and
opcode 14 does not decode.

- **Semantics.** `dst = src op ds1[n]` on 32 bits: `and` and `or` **[HW]** (our pixel-event
  program builds the end-of-tile task's `U1` from the tile position with `and`/`or` masks,
  and the tiles land where they should); `xor`, `nor`, `nand`, `add`, `sub` by name
  **[infer]**; `adc`/`sbc` add and subtract with a carry or borrow whose source is **[open]**.
  Whether `alum` makes them signed, and how they set `aluz`/`alun`, are **[open]**.
- **In the microkernel's programs [CAP].** The event program masks the event bits of `ir1`
  with `and` and compares groups of them with `xor` before its tests; the deferred-work
  program masks the request bits of `ir0` with `and`.

From `tools/usse-asm/progs/pds_pixevent.s`:

```
        and ds1[48], ir1, ds1[2]
        and ds1[48], ir1, ds1[3]
        and ds1[48], ds0[4], ds1[48]                    ; ~ir1 & 0x100: zero if bit 8 set
        and ds1[48], ds0[5], ds1[48]                    ; tile_x
        or ds0[48], ds0[6], ds1[48]                     ; base | tile_x
        and ds1[48], ds0[7], ds1[48]                    ; tile_y
        or ds0[48], ds0[48], ds1[48]
        and ds1[48], ds0[8], ds1[48]                    ; ENDOFRENDER
        or ds0[48], ds0[48], ds1[48]
        and ds1[48], ir1, ds1[4]
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#8    any opcode     mask 00000000  value 00000000  adc add and nand nor or sbc sub xor
      <pred><op> <dst>, <src>, ds1[<idx>]
```

Mnemonics: `adc`, `add`, `and`, `nand`, `nor`, `or`, `sbc`, `sub`, `xor`.

Layout (form #8):

```
 31 27 26  24 23 17 16  15 10 9 7 6   0
+-----+------+-----+---+-----+---+-----+
|  op | pred | src | - | idx | - | dst |
+-----+------+-----+---+-----+---+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `op` — bits 31:27. Values: 8 `add`, 9 `sub`, 10 `adc`, 11 `sbc`, 24 `or`, 25 `and`, 26 `xor`, 28
  `nor`, 29 `nand`. Other values do not decode.
- `dst` — bits 6:0. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`…`ds1[63]`.
- `src` — bits 23:17. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`.
- `idx` — bits 15:10. A 6-bit number, printed in decimal.

### 6.5 Multiply: `mul`

Opcode 12 **[RE]**: `mul dst, a.l|h, ds1[n].l|h` multiplies two 16-bit halves (the first from
`ds0` or `ir0`/`ir1`/`tim`) into a 32-bit destination.

- **Semantics [HW].** Our vertex program computes the vertex offset `ir0.l × stride` with it,
  and the vertices are fetched from the right place. Signedness (unsigned by default, `alum`?)
  is **[open]**.
- **In the microkernel's programs:** not used.

From `tools/usse-asm/progs/pds_vtx.s`:

```
        mul ds1[50], ir0.l, ds1[0].l
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#9    opcode 12      mask F8000000  value 60000000  mul
      <pred>mul <dst>, <src1>, <src2>
```

Mnemonics: `mul`.

Layout (form #9):

```
 31   27 26  24 23  16 15   9 8 7 6   0
+-------+------+------+------+---+-----+
| 01100 | pred | src1 | src2 | - | dst |
+-------+------+------+------+---+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `dst` — bits 6:0. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`…`ds1[63]`.
- `src1` — bits 23:16. A register operand, v = bank·128 + number (the bank bits may carry
  modifiers). By bank: 0 `ds0[0].l`…`ds0[63].h`, 1 `ir0.l`.
- `src2` — bits 15:9. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds1[0].l`…`ds1[63].h`.

### 6.6 `abs`, `not`

Opcodes 13 and 27 **[RE]**: one source, either `ds1[n]` (`w[8]` set) or a first source (`ds0`,
then `ir0`, `ir1`, and for `not` also `tim`).

- **Semantics.** `not` is the bitwise complement **[infer]**, measured on one bit **[HW]**: our
  pixel-event program tests "bit 8 clear" as `not` then `and` then `tstz`, and runs the right
  task. `abs` is the
  absolute value by name **[infer]**.

From `tools/usse-asm/progs/pds_pixevent.s`:

```
        not ds1[48], ir1
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#10   opcode 13      mask F8000100  value 68000100  abs
      <pred>abs <dst>, ds1[<idx>]
#11   opcode 13      mask F8000100  value 68000000  abs
      <pred>abs <dst>, <src>
#12   opcode 27      mask F8000100  value D8000100  not
      <pred>not <dst>, ds1[<idx>]
#13   opcode 27      mask F8000100  value D8000000  not
      <pred>not <dst>, <src>
```

Mnemonics: `abs`, `not`.

Layout 1 of 3 (form #10):

```
 31   27 26  24 23 15 14 10 9   8   7   6   0
+-------+------+-----+-----+---+---+---+-----+
| 01101 | pred |  -  | idx | - | 1 | - | dst |
+-------+------+-----+-----+---+---+---+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `dst` — bits 6:0. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`…`ds1[63]`.
- `idx` — bits 14:10. A 5-bit number, printed in decimal.

Layout 2 of 3 (forms #11, #13):

```
 31   27 26  24 23 17 16 9 8   7   6   0
+-------+------+-----+----+---+---+-----+
| ?1??1 | pred | src | -  | 0 | - | dst |
+-------+------+-----+----+---+---+-----+
```

- `pred` — as in layout 1.
- `dst` — as in layout 1.
- `src` — bits 23:17. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`.

Layout 3 of 3 (form #12):

```
 31   27 26  24 23 16 15 10 9   8   7   6   0
+-------+------+-----+-----+---+---+---+-----+
| 11011 | pred |  -  | idx | - | 1 | - | dst |
+-------+------+-----+-----+---+---+---+-----+
```

- `pred` — as in layout 1.
- `dst` — as in layout 1.
- `idx` — bits 15:10. A 6-bit number, printed in decimal.

### 6.7 Shifts: `shl`, `shr`

Opcodes 30 and 31 **[RE]**: `shl dst, src, n` and `shr dst, src, n` with a source from `ds0`,
`ds1`, `ir0` or `ir1` and a signed seven-bit amount, which the decoder prints as an unsigned
32-bit number (so `-3` prints `4294967293`).

- **Semantics.** `shl` shifts left **[HW]**: our pixel-event program places the tile position with
  `shl` by 3 and the end-of-render bit with `shl` by 20. `shr` shifts right **[infer]** (by its
  name); what a negative amount does (the other direction?) and whether `shr` is logical are
  **[open]**.

From `tools/usse-asm/progs/pds_pixevent.s`:

```
        shl ds1[48], ir0, 3
        shl ds1[48], ir0, 3
        shl ds1[48], ir1, 20
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#25   opcode 30      mask F8000000  value F0000000  shl
      <pred>shl <dst>, <src>, <n>
#26   opcode 31      mask F8000000  value F8000000  shr
      <pred>shr <dst>, <src>, <n>
```

Mnemonics: `shl`, `shr`.

Layout (forms #25, #26):

```
 31   27 26  24 23 16 15  14 8 7   6   0
+-------+------+-----+---+----+---+-----+
| 1111? | pred | src | - | n  | - | dst |
+-------+------+-----+---+----+---+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `dst` — bits 6:0. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`…`ds1[63]`.
- `src` — bits 23:16. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`…`ds1[63]`, 1 `ir0`.
- `n` — bits 14:8. Values: 0-63 `0`…`63`, 64-127 `4294967232`…`4294967295`.

### 6.8 Tests: `tstz`, `tstn`

Opcodes 16 and 17 **[RE]**: `tstz pN, src` and `tstn pN, src` set predicate `p0`-`p2`
(`w[2:0]`; other values print an empty predicate) from a source that is `ds1[n]` (`w[8]` set)
or `ds0`, `ir0`, `ir1`. The test itself can be predicated.

- **Semantics.** `tstz` sets `pN` when the source is zero **[CAP/HW]** (*Predicates*); `tstn`
  when it is negative **[infer]**.
- **In the microkernel's programs [CAP].** The event program runs ten `tstz` (some of them
  predicated, some on `ir0` directly) and branches past the cases that do not apply, as our
  pixel-event program does; the deferred-work program picks its task with two. Neither uses
  `tstn`.

From `tools/usse-asm/progs/pds_pixevent.s`:

```
        tstz p0, ds1[48]
        tstz p0, ds1[48]
        tstz p0, ds1[48]
        tstz p0, ds1[48]
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#15   opcode 16      mask F8000100  value 80000100  tstz
      <pred>tstz <pdst>, ds1[<idx>]
#16   opcode 16      mask F8000100  value 80000000  tstz
      <pred>tstz <pdst>, <src>
#17   opcode 17      mask F8000100  value 88000100  tstn
      <pred>tstn <pdst>, ds1[<idx>]
#18   opcode 17      mask F8000100  value 88000000  tstn
      <pred>tstn <pdst>, <src>
```

Mnemonics: `tstn`, `tstz`.

Layout 1 of 2 (forms #15, #17):

```
 31   27 26  24 23 16 15 10 9   8   7 3 2    0
+-------+------+-----+-----+---+---+---+------+
| 1000? | pred |  -  | idx | - | 1 | - | pdst |
+-------+------+-----+-----+---+---+---+------+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `pdst` — bits 2:0. Values: 0-2 `p0`…`p2`, 3 (none), 4 (none), 5 (none), 6 (none), 7 (none).
- `idx` — bits 15:10. A 6-bit number, printed in decimal.

Layout 2 of 2 (forms #16, #18):

```
 31   27 26  24 23 17 16 9 8   7 3 2    0
+-------+------+-----+----+---+---+------+
| 1000? | pred | src | -  | 0 | - | pdst |
+-------+------+-----+----+---+---+------+
```

- `pred` — as in layout 1.
- `pdst` — as in layout 1.
- `src` — bits 23:17. A register operand, v = bank·128 + number (the bank bits may carry modifiers).
  By bank: 0 `ds0[0]`.

### 6.9 Flow control: `bra`, `call`, `rtn`, `halt`, `nop`

Opcodes 18-22 **[RE]**: `bra` and `call` take a 19-bit target, `rtn` and `halt` none, `nop`
has no predicate.

- **`bra`** jumps to an instruction index counted from the first instruction after the data
  segment **[RE/CAP]**: the captured programs' control flow only makes sense that way, and our
  pixel-event program, assembled with labels on that basis, works **[HW]**.
- **`halt`** ends the program **[HW]**: every program of ours ends with it.
- **`call`** and **`rtn`**: a call and a return by name **[infer]**; neither is used.
- **`nop`** does nothing **[infer]**.

From `tools/usse-asm/progs/pds_pixevent.s`:

```
        (p0) bra not_ptoff
        halt
        (p0) bra not_eot
        (p0) bra eor
        halt
        (p0) bra done
        halt
```

Forms (generated from the decoder's table; `<name>` is a field):

```
#19   opcode 18      mask F8000000  value 90000000  bra
      <pred>bra <imm>
#20   opcode 19      mask F8000000  value 98000000  call
      <pred>call <imm>
#21   opcode 20      mask F8000000  value A0000000  rtn
      <pred>rtn
#22   opcode 21      mask F8000000  value A8000000  halt
      <pred>halt
#23   opcode 22      mask F8000000  value B0000000  nop
      nop
```

Mnemonics: `bra`, `call`, `halt`, `nop`, `rtn`.

Layout 1 of 3 (forms #19, #20):

```
 31   27 26  24 23 19 18  0
+-------+------+-----+-----+
| 1001? | pred |  -  | imm |
+-------+------+-----+-----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `imm` — bits 18:0. A 19-bit number, printed in hex.

Layout 2 of 3 (forms #21, #22):

```
 31   27 26  24 23 0
+-------+------+----+
| 1010? | pred | -  |
+-------+------+----+
```

- `pred` — as in layout 1.

Layout 3 of 3 (form #23):

```
 31   27 26 0
+-------+----+
| 10110 | -  |
+-------+----+
```

### 6.10 Fences and ALU mode: `rfence`, `rwfence`, `alum`

Opcode 15 is a fence, `rfence` or `rwfence` by `w[0]`, without a predicate; opcode 23 is
`alum signed` or `alum unsigned` (`w[0]`) **[RE]**.

- **Semantics [infer].** By the names, fences that order reads (and writes) of the PDS's
  memory accesses, and a mode switch for the ALU's signedness. Neither the microkernel's
  programs nor ours use them; **[open]**.

Forms (generated from the decoder's table; `<name>` is a field):

```
#14   opcode 15      mask F8000000  value 78000000  rfence rwfence
      <op>
#24   opcode 23      mask F8000000  value B8000000  alum
      <pred>alum <op>
```

Mnemonics: `alum`, `rfence`, `rwfence`.

Layout 1 of 2 (form #14):

```
 31   27 26 1 0
+-------+----+----+
| 01111 | -  | op |
+-------+----+----+
```

- `op` — bit 0. Values: 0 `rfence`, 1 `rwfence`.

Layout 2 of 2 (form #24):

```
 31   27 26  24 23 1 0
+-------+------+----+----+
| 10111 | pred | -  | op |
+-------+------+----+----+
```

- `pred` — bits 26:24. Values: 0-2 `(p0)`…`(p2)`, 3 `(if0)`, 4 `(if1)`, 5 `(aluz)`, 6 `(alun)`, 7
  (none).
- `op` — bit 0. Values: 0 `unsigned`, 1 `signed`.

## 7. Open questions

1. **The ports** `doutc`, `douts`, `slc`, `mr`, `tim` and `ir0` as destinations, and
   `movsa` on ports other than `doutd`.
2. **`if0`/`if1`**, and how `aluz`/`alun` are set.
3. **`load`/`store`**: the address base, the timing, and their relation to the fences.
4. **`mov128` and `swap`**, `mr01`/`mr23`, `tim`, `ir2`.
5. **ALU details**: `adc`/`sbc` carries, `alum`, negative shift amounts, `mul` signedness.
6. **`call`/`rtn`**: a return stack, and its depth.
7. **The `doutu` word** on this build: whether `EXE` really stops at bit 19 (so that `CBASE`
   exists) or the 18 PC bits reach bit 21; our programs keep code low enough for both
   readings to agree (`design/sgx545_transfer.md` §4.2.4).

8. **Semantics stated only by name**: `xor`, `nor`, `nand`, `add`, `sub`, `tstn`, `abs`,
   `call`/`rtn` and the fences, beyond what our programs use.

## 8. Appendix: every instruction form

Every form of the assembler's PDS table (`tools/usse-asm/pds_forms.c`), which describes the
decoder's layouts, in table order within each opcode: its index, its identifying mask and
value, its mnemonics and its pattern with named fields.

**Opcode 0**

```
#0    opcode 0       mask F8000000  value 00000000  movs
      <pred>movs <dst>, <srcs>
```

**Opcode 5**

```
#1    opcode 5       mask F8000000  value 28000000  movsa
      <pred>movsa <dst>, <srcs>
```

**Opcode 1**

```
#2    opcode 1       mask F8000000  value 08000000  mov16
      <pred>mov16 ds<ds>[<idx>], <src>
```

**Opcode 2**

```
#3    opcode 2       mask F8000000  value 10000000  mov32
      <pred>mov32 ds<ds>[<idx>], <src>
```

**Opcode 3**

```
#4    opcode 3       mask F8000000  value 18000000  mov64
      <pred>mov64 ds<ds>[<idx>], <src>
```

**Opcode 4**

```
#5    opcode 4       mask F8000000  value 20000000  mov128
      <pred>mov128 ds[<idx1>], ds[<idx2>]<extra>
```

**Opcode 6**

```
#6    opcode 6       mask F8000000  value 30000000  load
      load <imm>, ds<ds>[<idx>
```

**Opcode 7**

```
#7    opcode 7       mask F8000000  value 38000000  store
      store <imm>, ds<ds>[<idx>
```

**Any opcode (in a field)**

```
#8    any opcode     mask 00000000  value 00000000  adc add and nand nor or sbc sub xor
      <pred><op> <dst>, <src>, ds1[<idx>]
```

**Opcode 12**

```
#9    opcode 12      mask F8000000  value 60000000  mul
      <pred>mul <dst>, <src1>, <src2>
```

**Opcode 13**

```
#10   opcode 13      mask F8000100  value 68000100  abs
      <pred>abs <dst>, ds1[<idx>]
#11   opcode 13      mask F8000100  value 68000000  abs
      <pred>abs <dst>, <src>
```

**Opcode 27**

```
#12   opcode 27      mask F8000100  value D8000100  not
      <pred>not <dst>, ds1[<idx>]
#13   opcode 27      mask F8000100  value D8000000  not
      <pred>not <dst>, <src>
```

**Opcode 15**

```
#14   opcode 15      mask F8000000  value 78000000  rfence rwfence
      <op>
```

**Opcode 16**

```
#15   opcode 16      mask F8000100  value 80000100  tstz
      <pred>tstz <pdst>, ds1[<idx>]
#16   opcode 16      mask F8000100  value 80000000  tstz
      <pred>tstz <pdst>, <src>
```

**Opcode 17**

```
#17   opcode 17      mask F8000100  value 88000100  tstn
      <pred>tstn <pdst>, ds1[<idx>]
#18   opcode 17      mask F8000100  value 88000000  tstn
      <pred>tstn <pdst>, <src>
```

**Opcode 18**

```
#19   opcode 18      mask F8000000  value 90000000  bra
      <pred>bra <imm>
```

**Opcode 19**

```
#20   opcode 19      mask F8000000  value 98000000  call
      <pred>call <imm>
```

**Opcode 20**

```
#21   opcode 20      mask F8000000  value A0000000  rtn
      <pred>rtn
```

**Opcode 21**

```
#22   opcode 21      mask F8000000  value A8000000  halt
      <pred>halt
```

**Opcode 22**

```
#23   opcode 22      mask F8000000  value B0000000  nop
      nop
```

**Opcode 23**

```
#24   opcode 23      mask F8000000  value B8000000  alum
      <pred>alum <op>
```

**Opcode 30**

```
#25   opcode 30      mask F8000000  value F0000000  shl
      <pred>shl <dst>, <src>, <n>
```

**Opcode 31**

```
#26   opcode 31      mask F8000000  value F8000000  shr
      <pred>shr <dst>, <src>, <n>
```
