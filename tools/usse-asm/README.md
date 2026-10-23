# usse-asm — a clean-room SGX545 USSE and PDS assembler

`usse-asm` and `pds-asm` assemble text written the way our disassemblers print it
(`tools/usse-dis`: `usse-dis`, `pds-dis`) into raw little-endian words, so that

```
disasm(asm(text)) == text        asm(disasm(words)) decodes as words
```

This is milestone 1 of the open userspace path (`design/sgx545_render_uapi.md` §9.1a,
decision 2): the GPU programs of the 2D transfers no longer need to come from the closed
`libsrv_um.so`. `asm/transfer/` holds the nine transfer templates as our own assembly source, and
assembling them reproduces the templates `tools/fwcutter` cuts from the library, bit for bit.

The instruction sets, their syntax and what each instruction is known to do are documented in
`design/sgx545_usse_isa.md` and `design/sgx545_pds_isa.md` (generated from `doc/isa/`, with
examples from `asm/` and instructions assembled through this tool's form tables).

## Usage

```sh
make                                      # builds usse-asm and pds-asm (needs ../usse-dis)
./usse-asm -o fill.bin ../../asm/transfer/use_fill.s   # 64-bit USSE words
./pds-asm -l -o prim.bin ../../asm/transfer/pds_prim.s # 32-bit PDS words; -l lists
../usse-dis/usse-dis prog.bin | ./usse-asm -o again.bin -    # listings assemble too
../../.venv/bin/pytest -q
```

`crates/sgx-tools` builds Rust twins of both tools, with the same options, output and messages
(`crates/README.md`), and one option of their own: `--explain` follows each "cannot assemble"
with the forms that matched the most of the line and what each expected next, or says that no
form reads its mnemonic.

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

### Rust-only extensions

The Rust `usse-asm` and `pds-asm` also read these directives; the C tools do not (a source
without them assembles the same in both):

| statement | meaning |
|-----------|---------|
| `.include "file.s"` | read another source here; the name is relative to the including file (to the working directory from stdin) |
| `.equ NAME, expr` | a constant; defined once, before its use |
| `.macro name a, b=0` ... `.endm` | a macro; `\a` in the body is the argument, `\@` the call's number (for unique labels) |
| `name x, y` | a macro call; a missing argument takes its default |

Expressions use C operators and precedence (`- ~ + * / % << >> & ^ |`, parentheses) on
decimal, `0x` and `0b` numbers and `.equ` names. In an instruction, a `.equ` name and a
parenthesised expression (`#(BASE + 4)`, `ds0[(U0 + 1)]`) become their decimal value;
parentheses that are not an expression, such as the predicate in `(p0) bra`, stay. `.long` and
`.word` take expressions too, negative values in two's complement. Errors inside an included
file name that file; errors inside a macro name the line that called it.

### sgx-asm: several programs in one source

`sgx-asm` (Rust, `crates/sgx-tools`) reads the same syntax with sections and patch records,
for firmware and templates made of several USSE and PDS programs:

```sh
cargo run -p sgx-tools --bin sgx-asm -- -l -d out/ asm/transfer/transfer.s
```

`-d out/` writes each section's image as `out/<name>.bin` and `out/map.json`: the sections
(name, ISA, id, size, PDS data words), every label (section, value as branches count it,
byte offset) and the patch records. `-l` lists each section; `--core` and `--explain` as above.

| statement | meaning |
|-----------|---------|
| `.section name, usse\|pds\|data, id` | start a section; `id` is the buffer (or template) id its patches name; `data` holds `.word`s only |
| `kind(args)` in a `.word` | a `WORD32` patch on that word, which assembles as 0 |
| `kind(args)` in a USSE operand | a `USSE_LIMM` patch on that instruction (`mov rN, #kind(...)`), which assembles as 0 |
| `.patch encoding, kind(args)` | a patch on the next word or instruction, any encoding |
| `.patch encoding, kind(args), offset` | a patch at a byte offset in the section |

Encodings and kinds are the firmware format's (`design/sgx545_fw_format.md` §4, §9.4):
`word32`, `usse_limm`, `code_base`, `emitpix_sb`; `const(value)`, `addr(target[, addend])`,
`addr_shr4`, `heap(heap id[, addend])`, `pd_phys([addend])`, `code_word(usse label[, addend])`,
`pds_ctl`, `pds_rows`, `param(n[, addend])`, `doutu`. A target is a section name (its id) or a
number. Records keep source order, a `.patch` before the inline patch of the same statement,
since the driver applies them in order. Labels are per section: a branch reaches only its own
section; other sections name a label only through `code_word`.

`tools/fwcutter/asmmap.py` reads the output as firmware-format records. `asm/transfer/transfer.s`
writes the nine transfer-queue templates as one source; `tools/fwcutter/test_asmmap.py`
checks that it gives `ours.py`'s templates and `facts.TQ_PATCHES` exactly.

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
PDS works the same way: `pds_forms.c` inverts the PDS form table, which `pds_decode()` walks
(both generated from `isa/pds/`).

**Canonical encodings.** Many words print the same text: bits an instruction ignores, an
unused operand, a mask that equals a repeat count of one. The assembler picks the first
form in table order and, within it, the form's preferred values: 0, or what the closed
toolchain's own code shows. Two conventions are recorded (`UNUSED_SRC2` in `usse.c`): an
unused source 2 of a class-5 `mov` or of a load without a range operand is written as
immediate 0. The vertex-side emits follow the closed compiler's programs in the 3D captures
(`EMIT_UNUSED_SRC1`, `EMIT_UNUSED_SRC0` in `forms/memctl.inc`): `emitvcbst` and `emitvcbvtx`
write their unprinted source 1 as immediate 0, `emitmtest` sets the extension bit of its
unprinted source 0. With them, every decoded word of the SGX545 microkernel (6637) and of the SGX540
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

`../../.venv/bin/pytest -q` (94 tests, about 40 s):

- `test_usse_asm.py`, `test_pds_asm.py`: the transfer instructions and every PDS group,
  lenient spelling, listings, labels, data segments, errors and refused values.
- `test_templates.py`: the nine programs in `asm/transfer/` disassemble to the instructions documented
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

- `test_case.py`: every decoded instruction also assembles from its lowercase spelling, as
  radare2 hands it over.
- `test_probe.py`: the probe tables, their programs, the parser of `sgxfive-test probe` output
  and the verdicts (an end-to-end run goes through `sgxfive-test --mock`).
- `test_vectors.py`: `vectors.py`, the converter of probe readbacks into vector files.
- `test_vec_tables.py`: the vector tables in `progs/vectors/` are fresh and every case assembles.

## Programs

Our own GPU programs (the transfer templates, the first triangle, the tutorial's cases) are in
`asm/` at the repository root; `asm/README.md` lists them. This tool's `progs/` keeps its own
test inputs: the hardware probes (`progs/probes/`) and the vector tables (`progs/vectors/`).

## Users

- `tools/fwcutter/ours.py` assembles `asm/transfer/` for `fwcutter.py cut <libsrv_init.so>
  --tq-from-source`, which writes them into the firmware file as origin `OURS` (format v4,
  `design/sgx545_fw_format.md` §11), so 2D needs no `libsrv_um.so`.
- `userspace/libsgxfive` generates `src/prog_asm_gen.h` from them (`make gen`): its default
  program source. After editing a program, run `make gen` there; its tests fail until then.
- `kernel/gen_tri_progs.py` assembles the first triangle's programs into the kernel's
  `sgx_tri_progs.h`; `kernel/test_gen_tri_progs.py` fails while that header is stale.

## Hardware probes

`probe.py` runs our own short programs on the GPU to pin down what the instruction set does where
the text alone does not say (the questions are listed in `../usse-dis/r2/README.md`). Every
`progs/probes/*.toml` is a table (`--table FILE` picks one): per probe a template in
`progs/probes/`, its parameters, the input cases and, per hypothesis, the readback every case should
give. The exploration tables of the 2026-10-22 rounds are in `progs/probes/explore/` (their README),
not loaded by default. Each case is assembled into its own program, which `sgxfive-test probe`
(`userspace/libsgxfive`) runs as the shader of a client fill on the render node; the host reads back
`o0` and a 64 KiB scratch buffer. A probe is settled when exactly one hypothesis predicts every
case.

```sh
../../.venv/bin/python probe.py list                   # the probes and their questions
../../.venv/bin/python probe.py build ../../debug/b 'isub16-*'  # assemble the cases only
../../.venv/bin/python probe.py run --out ../../debug/probes/run 'isub16-*'
../../.venv/bin/python probe.py eval ../../debug/probes/run/raw.txt  # re-read a saved run
../../.venv/bin/python probe.py local --mock           # the mock: only checks the plumbing
../../.venv/bin/pytest -q test_probe.py
```

`run` needs the laptop booted with the render node on (`gma500_gfx.sgx_render=1`), its address
in `SGX_TARGET_HOST` (or `kernel/scripts/local.env`) and an i686 `sgxfive-test` in its home
directory (`SGX_PROBE_TOOL` overrides the path): build it with `make CC='cc -m32'` in
`userspace/libsgxfive` on a Linux host, or on the laptop itself, and copy
`build/sgxfive-test` there. `local` needs `make -C userspace/libsgxfive` first. A run's
readbacks become vector files with `vectors.py convert` (`design/sgx545_vectors.md`); publishing
them is described in `data/README.md`.
Every case runs in its own process, because a faulting probe gets its file banned after three
faults; a fault shows as `failed`. A new probe is a table entry, plus a template when none of
`pred.s` (a predicate as the pixel), `value.s` (a register), `dump.s` (sixteen registers to
the scratch buffer), `index.s`, `index_sa.s`, `index_sa2.s`, `memoff.s`, `repeat.s`, `moe.s`,
`pred4.s` or `coords.s` fits.

A table entry with `coords = true` runs as the shader of a copy whose source texel is the
pixel's coordinates (`sgxfive-test probe --coords`), so that every pixel computes its own
inputs (`coords.s` derives a value per pixel and register), and with `pixels = true` every
pixel is read back. A hypothesis then expects `{pixels = [...]}`, all 2048 in raster order, or
`{quad = [[k, n], ...]}`: per 2x2 quad position (top-left, top-right, bottom-left,
bottom-right), the `coords.s` value `k` of the pixel at position `n` of the same quad.

## Next

- Let `br` take a label as `ba` does: its offset base is measured (relative to the branch
  itself, probe `br-offset`).
