<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Reading the encodings

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
