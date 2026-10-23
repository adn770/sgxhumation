<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
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
