# usse-asm — a clean-room SGX545 USSE and PDS assembler

`usse-asm` and `pds-asm` assemble text written the way our disassemblers print it
(`tools/usse-dis`: `usse-dis`, `pds-dis`) into raw little-endian words, so that

```
disasm(asm(text)) == text        asm(disasm(words)) decodes as words
```

This is milestone 1 of the open userspace path (`design/sgx545_render_uapi.md` §9.1a,
decision 2): the GPU programs of the 2D transfers no longer need to come from the closed
`libsrv_um.so`. `progs/` holds the nine transfer templates as our own assembly source, and
assembling them reproduces the templates `tools/fwcutter` cuts from the library, bit for bit.

## Usage

```sh
make                                      # builds usse-asm and pds-asm (needs ../usse-dis)
./usse-asm -o fill.bin progs/use_fill.s   # 64-bit USSE words
./pds-asm -l -o prim.bin progs/pds_prim.s # 32-bit PDS words; -l prints a listing
../usse-dis/usse-dis prog.bin | ./usse-asm -o again.bin -    # listings assemble too
../../.venv/bin/pytest -q
```

Options: `-o <file>` writes the binary, `-l` prints a listing in the disassembler's format,
`--core 540` follows the SGX540 reading of the load/store cache bits (default 545). `-` reads
standard input. Errors name the file and line; the exit status is 1.

## Source syntax

One statement per line:

| statement | meaning |
|-----------|---------|
| `mov.skipinv.end o0, #0xFF336699` | an instruction, as the disassembler prints it |
| `.long 0x...` / `.word 0x...` | raw words (USSE 64-bit / PDS 32-bit), comma-separated |
| `name:` | a label; may share the line with a statement |
| `; ...`, `/* ... */` | comments |
| `12: 0xFA44070000000000\tphas ...` | a disassembler listing line: the index and hex columns are ignored, except that a PDS `data` line takes its value from the hex column |

Instruction text is matched leniently around the exact disassembler spelling, letters in
either case (radare2 lowercases what it assembles; `test_case.py`): blanks may
vary (around commas too), numbers may be decimal or `0x` hex in any case (`#0` for
`#0x00000000`), and the `/* incp */`-style comments the disassembler prints are optional.
Mnemonics, flags and register names are exact.

**PDS**: the raw words before the first instruction are the data segment (the program's
constants, in the `ds0`/`ds1` banks by pairs of dwords, hardware spec §13.8). `bra`/`call`
take a label or a number; a label's value is its instruction index counted from the first
instruction after the data segment, as the branch field counts. **USSE**: `ba #label` takes
the label as an absolute instruction index; `br` takes only a number (its offset base is not
established).

## Design

The USSE decoder is a table of instruction forms (`../usse-dis/usse_form.h`): a text pattern,
the bits that identify the form, and fields printed by *kinds*. The assembler is the same
table read backwards, by the engine in `usse_form.c`:

1. Try the forms in table order. Match the pattern against the text: literal text exactly
   (blanks flexible); for each field, either parse it (numbers, the rotated immediate, branch
   offsets, movs operand lists) or search the field's values for one that prints the text.
   The search backtracks, so ambiguous prefixes (`r1` vs `r12`) resolve.
2. Build the word from the form's fixed bits, its preferred values for the bits it ignores
   (`dflt`), and the fields.
3. **Accept the word only if the decoder prints it with the form's text.** An earlier form
   may claim the word, or a value may not be printable; then the search goes on.

Step 3 makes the assembler unable to emit a word the disassembler would read differently.
For PDS, whose decoder (`pds.c`) is a compact field decoder, `pds_forms.c` describes the same
layouts as forms, and step 3 checks every word against `pds_decode()` itself, so a layout
error could only make an instruction fail to assemble, never assemble wrong.

**Canonical encodings.** Many words print the same text: bits an instruction ignores, an
unused operand, a mask that equals a repeat count of one. The assembler picks the first
form in table order and, within it, the form's preferred values: 0, or what the closed
toolchain's own code shows. Two conventions are recorded (`UNUSED_SRC2` in `usse.c`): an
unused source 2 of a class-5 `mov` or of a load without a range operand is written as
immediate 0. With them, every decoded word of the SGX545 microkernel (6637) and of the SGX540
corpus (10742, with `--core 540`), the five EDM PDS programs and the nine transfer templates
reassemble bit for bit from their listings.

## Coverage

Whatever `usse-dis` and `pds-dis` decode assembles. `usse-dis` decodes the whole SGX545 USSE
instruction set (`../usse-dis/README.md` lists the groups: moves, integer, float, formats,
memory and texture sampling, control and special, video), and `pds-dis` every PDS instruction
group (`movs`/`movsa` to the `dout*` ports, `mov16`–`mov128`, `load`/`store`, the integer
ALU, `tstz`/`tstn`, `bra`/`call`/`rtn`/`halt`, fences, `alum`, shifts). The assembler reads
the decoder's form table, so a form added there assembles too.

## Tests

`../../.venv/bin/pytest -q` (50 tests, about 20 s):

- `test_usse_asm.py`, `test_pds_asm.py`: the transfer instructions and every PDS group,
  lenient spelling, listings, labels, data segments, errors and refused values.
- `test_templates.py`: the nine programs in `progs/` disassemble to the instructions documented
  in `design/sgx545_transfer.md` §4.2.4/§4.4.3 and `design/sgx545_fw_format.md` §9.2; and,
  with the user's `libsrv_um.so` (found as `tools/fwcutter` finds it, else skipped), the
  assembled bytes equal the cut templates. No closed bytes are stored in this tree.
- `test_roundtrip.py`: random words, drawn per form of the decoder's table (1500 each, half
  with flipped identifying bits) plus 50k uniform USSE words, and 4000 random PDS words per
  opcode. Every decoded word must reassemble to a word that decodes the same, and, where
  `refdisasm545`/`refpds545` are built, that the oracle prints the same for both. Then the
  corpora above, bit for bit. A run prints the statistics:

```
usse-asm: 377000 words, 312849 decoded, 0 failed, 94714 bit-identical, 218135 canonicalised (69.7%)
  oracle refdisasm545: 312849/312849 identical
pds-asm: 128000 words, 84642 decoded, 0 failed, 8357 bit-identical, 76285 canonicalised (90.1%)
  oracle refpds545: 84642/84642 identical
```

Random words are mostly not canonical (their ignored bits are random), so most come back
canonicalised; the oracle agreeing on every one of them shows the canonical words mean the
same. The corpora, written by the closed toolchain, come back bit-identical.

## Programs

| file | template | what it does |
|------|----------|--------------|
| `use_fill.s` | `USE_FILL` (0) | writes the fill colour (patched immediate) to `o0` |
| `use_sec.s` | `USE_SEC` (1) | the empty USE secondary program |
| `use_eot.s` | `USE_EOT` (2) | end of tile: the PBE state words to the pixel back end |
| `use_eor.s` | `USE_EOR` (3) | end of render |
| `pds_prim.s` | `PDS_PRIM` (4) | kicks the fill shader (`DOUTU`) |
| `pds_sec.s` | `PDS_SEC` (5) | kicks the USE secondary program |
| `pds_pixevent.s` | `PDS_PIXEVENT` (6) | dispatches pixel events: PTOFF, end of tile, end of render |
| `use_copy.s` | `USE_COPY` (7) | moves the fetched texel `pa0` to `o0` |
| `pds_prim_1src.s` | `PDS_PRIM_1SRC` (8) | kicks the copy shader, the iterator and the texture fetch |

The comments explain each instruction from the documented semantics. The programs keep the
values the driver patches (`sgx545_fw_format.md` §9.4, §10.4) at 0, as the templates do, so
that the existing patch records apply unchanged; `use_eot.s` keeps its four `mov`s on `r0`
for the same reason (the patch adds the register number).

## Users

- `tools/fwcutter/ours.py` assembles `progs/` for `fwcutter.py cut <libsrv_init.so>
  --tq-from-source`, which writes them into the firmware file as origin `OURS` (format v4,
  `design/sgx545_fw_format.md` §11), so 2D needs no `libsrv_um.so`.
- `userspace/libsgxfive` generates `src/prog_asm_gen.h` from them (`make gen`): its default
  program source. After editing a program, run `make gen` there; its tests fail until then.

## Next

- Establish the `br` offset base, so that `br` can take a label as `ba` does.
