<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## The instruction word

A USSE instruction is one **64-bit word**, stored little endian: the low dword `enc[31:0]`
first, then the high dword `enc[63:32]` **[RE]**. The decoder, its comments and this document
number the bits of the whole word, `enc[63:0]`; the diagrams draw the high dword above the low
one. Instruction addresses everywhere (branch targets, the link register, the code-address
words of the PDS) count **instructions**, not bytes, from the start of a code base
**[RE/HW]**: the microkernel only boots when it sits exactly at its code base, because its
branch targets and the kernel CCB's service addresses are absolute instruction indices
(`design/sgx545_hw_spec.md` §13.10a).

The top five bits `enc[63:59]` are the **major opcode class**. Most classes hold one family;
class 31 is the control and special group, subdivided by `enc[54:52]` and further bits. A few
forms leave part of the class to a field: the bitwise group fixes only `enc[63:60]` and reads
`enc[59]` as part of its operation.

Some bits recur with one meaning across most classes **[RE]**:

- `enc[58:56]`, the execution predicate (two bits, `enc[58:57]`, in the multiply-add and
  colour groups);
- `enc[55]` `.skipinv`, `enc[52]` `.syncs`, `enc[50]` `.end` and either `enc[54]` or `enc[43]`
  `.nosched` (the scheduling flags);
- `enc[27:21]` the destination number, `enc[20:14]` source 0, `enc[13:7]` source 1 and
  `enc[6:0]` source 2, their bank bits in `enc[34:28]` and the bank extension bits `enc[51]`,
  `enc[49]` and `enc[48]`;
- `enc[53]`, which turns a write mask in `enc[47:44]` into a repeat count.

The classes and the mnemonics each one holds:

<!-- @classmap usse -->
