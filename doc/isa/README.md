<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
# ISA documents: the snippet library

The USSE and PDS instruction set documents are built from this directory: handwritten
Markdown snippets, one per section, plus parts generated from the form tables of our
disassemblers and assemblers, assembled into books by `build.py`. The outputs are committed.

| book | manifest | output |
|------|----------|--------|
| A, USSE reference | `books/usse-ref.book` | `design/sgx545_usse_isa.md` |
| A, PDS reference | `books/pds-ref.book` | `design/sgx545_pds_isa.md` |
| B, USSE programmer's guide | `books/usse-guide.book` | `design/sgx545_usse_guide.md` |
| C, writing USSE by hand | `books/usse-tutorial.book` | `design/sgx545_usse_tutorial.md` |

B is A's text without its encodings: the manifest option `forms brief` renders every `@group`
as its mnemonics, modifiers and simulator coverage, without the layouts, and B has no appendix.

C walks through our own programs (`tools/usse-asm/progs/`, the tutorial's own cases in
`progs/tutorial/`): `@prog` quotes them and `@sim` runs them on `sgx-sim`, so the code and the
runs it shows are generated (the build runs `cargo build -p sgx-tools` once for `@sim`).

## Layout

```
doc/isa/
├── README.md           this file
├── build.py            assembles the books; build.py --check fails on a stale output
├── formdump.c          dumps the USSE and PDS form tables as JSON, encodes and decodes words
├── test_build.py       the tests (pytest)
├── books/*.book        one manifest per output
└── content/
    ├── shared/         snippets both instruction sets use (conventions, how to read encodings)
    ├── usse/           the USSE snippets; groups/ one per opcode group, probe/ one per fact
    │                   asked of the hardware
    └── pds/            the PDS snippets; groups/ one per group
```

## Rebuild

```sh
python3 doc/isa/build.py            # every book (needs a C compiler for formdump)
python3 doc/isa/build.py usse-ref   # one book
python3 doc/isa/build.py --check    # exit 1 if a committed output is stale
.venv/bin/pytest doc/isa            # the tests, freshness included
```

`build.py` compiles `formdump.c` against `tools/usse-dis` and `tools/usse-asm` into a temporary
directory, so a change to the form tables shows up as a stale document; rebuild and commit the
outputs with the change.

## Editing

- **Prose** lives in the snippets. Keep one section per file, lines at most 100 columns, and
  tag every statement about behaviour as `design/sgx545_hw_spec.md` does (`[HW]`, `[CAP]`,
  `[RE]`, `[DDK]`, `[infer]`, `[open]`; `content/shared/conventions.md` defines them).
- **Headings**: a snippet starts with `##` or `###`; books with `number yes` number them.
- **Generated parts** are directives on a line of their own, `<!-- @name args -->` in a
  snippet or `@name args` in a manifest:

| directive | output |
|-----------|--------|
| `@group ISA [class=A-B,C] [pat=RE] [not=RE]` | a group's forms, mnemonics, diagrams, fields |
| `@appendix ISA` | every form of the table |
| `@classmap ISA` | the opcode classes and their mnemonics |
| `@example ISA TEXT` | assemble `TEXT`, show its word and field values |
| `@prog FILE [match=RE]` | lines of one of our programs in `tools/usse-asm/progs/` |
| `@banks ISA pat=RE field=N` | the banks of a register operand |
| `@values ISA pat=RE field=N` | every value of a field |
| `@probes` | the book's probe snippets, with section numbers and `[HW]` or `[open]` |
| `@simopen [N]` | the simulator's N most frequent stops on the probe vectors |
| `@sim [OPTS] prog=FILE` | run one of our programs on `sgx-sim` and show its output |
| `@sim [OPTS] -- LINE \| LINE` | the same for a few lines of source |

  `class` selects forms whose opcode class is within the set, `pat` and `not` match the
  form's pattern (quote with `'...'`). In a book with `groups complete`, every form must fall
  in exactly one `@group`, and a test checks it. A USSE `@group` ends with what the simulator
  models of its forms and the probe vectors that check them (match, unsupported, mismatch),
  read from `sim-coverage.json`: `cargo run --release -p sgx-sim --example coverage` writes it
  from the offline corpus, and `cargo test` fails while it is stale.
- **A hardware probe result** is one snippet in `content/usse/probe/`, marked *Measured on the
  hardware (2026-10-22)* or *Not probed yet* (`build.py` `PROBE_MARKS`; the index of the
  section tags them `[HW]` or `[open]`). It states what was measured with the probe names and a
  pointer to `design/sgx545_hw_spec.md` §14, and what stays undecided with what would settle
  it. A new probe run rewrites the snippet (and the sections that refer to it); a new mark
  date goes in `PROBE_MARKS`.
- **Examples** come from our own programs or are assembled fresh by `@example`; never quote
  words of the closed firmware, and describe how the firmware uses an instruction in words
  and counts (`HACKING.md`, clean-room rules).
- A new book is a new manifest in `books/`; snippets can be shared between books freely.
