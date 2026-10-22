<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Instruction groups

One section per opcode group. Each starts with what the group does and what is known of it,
how the firmware uses it and examples, then the generated part: the group's forms, its
mnemonics and modifiers, and a bit diagram with the field list for every distinct word layout.
Forms that differ only in fixed bits share a diagram. Every form of the decoder appears in
exactly one group (a test checks it) and again in the appendix.
