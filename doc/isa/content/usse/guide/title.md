<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
# SGX545 USSE programmer's guide

The **USSE** (universal scalable shader engine) is the programmable core of the PowerVR SGX
Series 5: on the SGX545 of Intel's Cedar Trail it runs every vertex, pixel and state program of
a scene, and the firmware. This guide is for writing and reading those programs: the machine a
program runs on, its operands, the assembler syntax, what every group of instructions does, and
what the hardware was measured to do.

It is the reference's text without its encodings. The instruction set reference
(`design/sgx545_usse_isa.md`) is built from the same snippets and adds, for every group, the bit
layouts, field lists and an appendix of every form; read it to decode or encode a word by hand.
Both state behaviour with the conventions below, and both end every group with what the
simulator (`crates/sgx-sim`) models of it, checked against the hardware vectors: a rule the
simulator holds is one a program can rely on, and one it stops on is still open.
