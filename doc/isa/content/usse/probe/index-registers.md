<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Index registers and bit 18

*Pending hardware probe (2026-10-22).* `mov i.l, src` loads an index register and
`bank[il + #n]` names a register through it **[RE]**. Open: the width of `i.l`/`i.h`, whether
the index counts registers of the bank, how the offset adds, what happens past the end of a
bank, and what the higher bits mean. The microkernel's page-table builder ORs **bit 18** into
an index register value (`design/sgx545_3d.md` §12.4), which suggests that the upper bits
carry a mode or a bank of their own.
