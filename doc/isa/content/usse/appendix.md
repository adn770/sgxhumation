<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Appendix: every instruction form

Every form of the decoder's table, in table order within each opcode class: its index, the
classes it can occupy, its identifying mask and value, the mnemonics it prints and its
pattern with named fields. A word decodes with the first form, in table order, whose mask and
value match and whose fields all print; the classes here group the forms for reading only.
