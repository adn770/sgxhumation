<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Assembler syntax

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

Our programs in `asm/` are written in this syntax; `make -C tools/usse-asm`
builds the assembler, `tools/usse-asm/usse-asm -l prog.s` prints a listing.
