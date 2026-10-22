# usse-dis — SGX Series-5 USSE disassembler

A from-scratch disassembler for the PowerVR SGX **USSE** instruction set (Series-5 / SGX5xx),
built to read the extracted SGX545 microkernel (`harness/ukernel-extract/extracted/`) and,
longer term, to ship as a **radare2 architecture plugin** (no PowerVR/USSE arch exists in r2
today — this is intended to be upstreamable).

The instruction sets it decodes are documented in `design/sgx545_usse_isa.md` and
`design/sgx545_pds_isa.md`, whose bit diagrams, field lists and form index are generated from
this decoder's form tables (`doc/isa/`): a change to a form shows up there as a stale document
until `python3 doc/isa/build.py` is run.

## Design

- **C decoder core** (`usse.c` / `usse.h`): pure `decode(enc) -> {text}`, no radare2 dependency,
  so it can be regression-tested standalone and later wrapped as an r2 `RArch` plugin from the
  *same* source (one source of truth).
- **The form table** (`usse_form.h`, engine in `usse_form.c`): every instruction form is a text
  pattern, the bits that identify it (`(word & mask) == val`) and up to ten fields, each a set
  of bit ranges plus a *kind* that prints it (an enumeration, a number, a register operand, a
  rotated immediate...). `usse_decode()` walks the table in order and prints the first form
  whose fields all print. The assembler (`tools/usse-asm`) inverts the same table, so the two
  directions cannot drift apart; a form's `dflt` records how the closed toolchain fills the
  bits an instruction ignores, so its code reassembles bit for bit.
- **CLI** (`main.c` → `usse-dis`): decodes a raw little-endian USSE blob, one 64-bit word per
  line, in a format that lines up with the SGX540 oracle for diffing. `--forms` lists the
  table (index, mask, value, bits read, pattern), `--which` appends the index of the form that
  decoded each word, `--core 540` follows the SGX540 where it differs (the load/store cache
  bits are swapped there).
- **Scope**: grown group by group. The opcode space (major class = `enc[63:59]`) is fully
  enumerated from the oracle; see the table in `usse.c`.

## The oracle (ground truth)

Two tiers, in the order we found them:

1. **Bootstrap** — the SGX540 RE project (`reference/sgx540-reversing/`) shipped ukernel/TA/3D/SPM
   binaries + disassembly text. We bootstrapped the decoder by bit-correlating field layouts
   against it (`solve.py`). It has gaps, though: it duplicates each dump and **truncates branch
   targets** (every branch printed ≤ 7), which made branches look undecidable.
2. **Authoritative** — `refdisasm/` builds **Imagination's own `ref_usse_disasm`** (from the PVR_PSP2
   source, SGX545 config) into a reference disassembler. It fully disassembles our extracted 545
   microkernel (6637/6637, 0 failures) with *correct* branch targets. This is now the
   regression oracle.

**`oracle_check.py`** disassembles each binary with both `refdisasm` (truth) and our `usse-dis`,
and compares per instruction. **Invariant: a line we claim to decode must equal refdisasm —
`WRONG` must stay 0.** We'd rather emit `; TODO` than a plausible lie.

**Provenance of the oracle.** PVR_PSP2 is Imagination-confidential source ("All rights
reserved", no licence); it is not open source. We built the reference disassemblers in
`refdisasm/` from it and used them as black-box oracles: the decoder here was derived from
their output, never from their source. Early in the work, PVR_PSP2's headers were also
consulted for field and encoding names. No code or text from PVR_PSP2 is in this repository,
and `usse-dis` does not need it to build or run; to build the oracles, users must obtain
PVR_PSP2 themselves (see `../../reference/CATALOG.md` §7).

## Build & test

```sh
refdisasm/build.sh   # build the authoritative reference disassembler (needs reference/PVR_PSP2)
make                 # builds ./usse-dis
make check           # oracle_check.py: usse-dis vs refdisasm (545 ukernel + 540 corpus)
python3 random_oracle_check.py   # every form vs refdisasm545 on random words
../../.venv/bin/pytest -q
./usse-dis ../../harness/ukernel-extract/extracted/sgx545_ukernel_template.bin | less
```

## Status

**The whole SGX545 USSE instruction set decodes.** On uniformly random words, every encoding
the oracle considers valid (it prints the rest as `invalid_*`) is decoded with exactly the
oracle's text: 153414 of 153414 on a 200k-word sample (`coverage.py`), in all 32 opcode
classes. The forms live in `usse.c` (the original groups) and in one file per instruction
family under `forms/` (see `forms/README.md` for the classes each owns):

- **moves**: `limm`, class-5 `mov`, `movc`, `movmsk`, `pclink` moves;
- **integer**: the bitwise group (`and`/`or`/`xor`/shifts/rotates), the test group in both its
  predicate (class 9) and mask (class 15) forms with every operation and format,
  `imae`/`ima8`/`ima16`/`ima32`, `idiv`, `sop2`/`sop3`/`sop2wm`/`lrp1`/`lrp2`, `rlp`,
  `u8dot`/`u16dot`;
- **float**: `fmad`/`fadm`/`fmsa`/`fsubflr`, `frcp`/`frsq`/`fsqrt`/`fexp`/`flog`/`fsin`/`fcos`,
  `fdp`/`fddp` and their clamped forms, `fminmax`, `fdsx`/`fdsy`, `fmad16`, `efo`, `fpma`, the
  class-27 dual-issue group;
- **formats**: every `pck`/`unpck` conversion, `statomic`;
- **memory**: `ld`/`st` in every addressing and cache mode, `eldd`/`eldq`, texture sampling
  `smp1d`/`smp2d`/`smp3d` with bias, replace and gradients;
- **control and special**: `ba`/`br`/`lapc`, `phas`, `nop`, `ldr`/`str`, `idf`/`wdf`,
  `lock`/`release`, `setm`/`setfc`/`wop`, `smoa`/`smr`/`smlsi`/`smbo`/`imo`/`moest`,
  `emitpix`/`emitpds`/`emitmt*`/`emitvcb*`, `pcoeff`/`ptoff`/`atst8`/`depthf`, `ci`/`cf`/`cfi`;
- **video**: `firh`/`firv`/`bilin`/`adif`/`adifsum`.

Validated vs authoritative refdisasm, **0 wrong**:
- **545 microkernel: 6637/6637 decoded.** 540 corpus (ukernel+TA+3D+SPM): 10742/10752; the
  SGX540 reads the float classes differently, so those forms refuse to print under
  `--core 540` and its float words stay undecoded rather than wrong.
- `random_oracle_check.py`: per form and uniform random words, 1390000 words with 1144639
  decoded, 0 wrong.

The family forms were derived by black-box probing of `refdisasm545` (single-bit flips and
field sweeps; never its source) and held to the oracle by the same checks. Every decoded word
reassembles (`tools/usse-asm`, `test_roundtrip.py`), in either letter case.

## radare2 plugin

`r2/` wraps the core (`usse.c` and `usse_form.c`) as a radare2 `RArch` plugin (`arch usse`,
`bits 32`): `make -C r2 install`, then `radare2 -a usse -b 32 <blob>`. Verified against
refdisasm in r2 (6631/6637 of the ukernel; op types + branch targets wired so jump arrows /
function graphs work). See `r2/README.md` for the upstream path to radareorg/radare2.

## Next

1. Open the radare2 PR (plugin and assembler are structured for it; `r2/README.md`).
2. Semantics beyond text: the radare2 ESIL layer (the fork's plugin) models moves (`mov.repeatN`
   and the index register too), the bitwise group, the test group (32-bit and the 16-bit lane
   ops), `imae`/`ima32`/`ima16`, `fmad`/`frcp`/`frsq`/`fsqrt`, branches, `lapc`, loads/stores
   (register offsets too) and the SGX register file (`ldr`/`str`, in a window above 4 GiB):
   84.2% of the microkernel's words, every one with a register effect but three. Where the text
   left the meaning open, our own programs measured it on the GPU (`../usse-asm/probe.py`;
   `r2/README.md`, "Hardware probes"). Other repeats, write masks, the 8-bit and float tests,
   pack/unpack and sampling decode but are not emulated: their semantics are not pinned yet.

## Load/store addressing — what the immediate means

usse-dis prints the encoded offset field of `ldad`/`ldaw`/`stad`/`staw`, exactly as refdisasm545
does (`test_ldst.py` pins this; `oracle_check.py` finds 0 differences on every fetch form). The
text is right. What it means depends on the form. Offsets count elements of the data size (4
bytes for `d`, 2 for `w`):

| form | elements accessed | base writeback |
|------|-------------------|----------------|
| `[b,+#k]` / `[b,-#k]` | `b+k` / `b-k` | none |
| `[b,++#k]` (pre) | `b+k` | `b += k` |
| `[b,#k++]` (post) | `b` | `b += k` |
| `.fetchN [b,+#k]` | **`b+k+1` … `b+k+N`** | none |
| `.fetchN [b,#k++]` | `b+k` … `b+k+N-1` | `b += N` |

So a fetch with a base+offset address starts **one element after** the printed offset:
`ldad.fetch4 r3, [r2,+#1]` loads dwords 2–5 into `r3`–`r6`. Stores behave the same way.
`.fetchN` is printed when the encoding's MOE-expand bit is clear, `.fetch1` included. Plain
forms (no `.fetch`) never add the extra element.

Evidence:
- **DDK, the base+offset fetch.** The PSP2 shader compilers compensate for the extra element.
  `usc2/finalise.c`: "An LDAD instruction with the FETCH flag adds an extra 4 byte onto the
  memory address", so it lowers the static offset by one data size. `ffgen/reg.c` emits fetch
  loads and stores with offset − 1 dword "to compensate for preincrement" and calls the
  non-incrementing form a "pre increment without the writeback".
- **Hardware, the plain forms and the post-increment fetch.** The CCB fetch at `0x4c0` is
  `ldad r4, [r0,#1++]` then `ldad.fetch7 r5, [r0,#0++]`. The handlers get `ui32Data[1]` in
  `r7`, so `r5`–`r11` are dwords 1–7 (no extra element). GETMISCINFO then stores the reply
  with plain `stad [r2,+#1]` … `[r2,+#7]` into dwords 1–7. Both were checked on the laptop
  (spec §13.10a).
- **ukernel, the post-increment writeback.** Loops such as `0x3f28` (`ldad.fetch2 [r3,#0++]`
  until `r3` reaches `start + 8 × count`) need `b += N`. `stad.fetch2 [r1,#82++]` in a loop
  after `stad [r1,+#81]` (the count) fills pairs from dword 82, so `k` is a displacement
  there, not part of the increment. The SGX540 TA has the same pattern at `#64++`.
- **ukernel, the base+offset fetch.** With the extra element the TRANSFER handler's sync
  blocks land on the documented `SGXMKIF_TRANSFERCMD` fields (`design/sgx545_transfer.md` §3).

Not seen in any corpus and left unmodelled: `.fetchN` with pre-increment or a negative offset,
and register offsets (`[b,+rN]`). The radare2 plugin follows this table. Its ESIL loads and
stores every fetched register and updates the base. `asm.pseudo` names the elements actually
accessed (`r3..r6 = mem[r2 + 2..5]`, `r5..r11 = mem[r0 + 0..6]; r0 += 7`).

## regtouch — the firmware's register map

`regtouch.py` lists every SGX register a USSE program touches directly: the EDM uses
`ldr rD, #idx` / `str #idx, src` with a dword register index (offset = idx × 4). It runs on
usse-dis (or refdisasm) output and can emit radare2 comments for each access site:

```sh
./usse-dis ukernel.bin > uk.txt
../../.venv/bin/python regtouch.py uk.txt --r2 regs.r2     # 191 registers on the SGX545 ukernel
../../.venv/bin/pytest -q test_regtouch.py
```

`--r2-flags out.r2` also writes a flag per register (`sgx.EUR_CR_CORE_ID`, ...) at the address
where the fork's ESIL emulates it, `0x100000000 + offset` (`r2/README.md`, "Emulation").

## pds-dis — the PDS (data sequencer) decoder

The EDM is entered through small **PDS programs** (spec §13.8). `pds.c` / `pds.h` decode the
32-bit PDS instruction word: opcode `w[31:27]`, predicate `w[26:24]` (`p0`–`p2`, `if0`/`if1`,
`aluz`/`alun`, 7 = always), and a 24-bit operand body per group (`movs`/`movsa` to the
`dout*` issue ports, `mov16`–`mov128`, `load`/`store`, integer ALU, `tstz`/`tstn`, `bra`/`call`,
fences). `pds-dis <program.bin> <data_dwords>` prints the data segment as `data` and decodes
the rest, in `refpds545`'s format.

The layouts are clean-room: they were derived **black-box**, by probing `refpds545` with
single-bit flips and value sweeps over each field. `pds_oracle_check.py` then diffs the result
against the oracle over the five captured EDM programs and a fixed-seed random corpus of
128k words. **0 wrong, and nothing missed**: `.word` appears only where the oracle itself
reports a reserved encoding.

```sh
make pds-dis
./pds-dis ../../harness/ukernel-extract/extracted/sgx545_pds13_runtime.bin 24
python3 pds_oracle_check.py              # needs refdisasm/refpds545
../../.venv/bin/pytest -q test_pds.py
```
