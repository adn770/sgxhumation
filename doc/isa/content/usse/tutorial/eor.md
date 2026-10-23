<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Ending a render, and the secondary program

Two more programs complete a transfer. After the last tile of a render, the end-of-render
program sends one more `emitpix2`, with zero words and the sideband `0x200`, which the closed
driver issues to finish the render; the secondary program, which the PDS kicks once per
transfer, has nothing to do and ends (`design/sgx545_transfer.md` §4.2.4):

<!-- @prog use_eor.s -->
<!-- @prog use_sec.s -->

`nop.end` is the shortest program there is: one phase marker, then an instruction that only
ends. Programs always end on an instruction with `.end`; one that runs past its last word
would execute whatever follows it in memory.
