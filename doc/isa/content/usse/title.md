<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
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
