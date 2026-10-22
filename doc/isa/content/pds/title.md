<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
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
