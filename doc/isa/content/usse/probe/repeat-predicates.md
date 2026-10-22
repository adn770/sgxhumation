<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Predicates on repeated instructions

*Pending hardware probe (2026-10-22).* Whether a predicated `.repeatN` instruction tests its
predicate once for all iterations or a different predicate per iteration (which `Pn` might
name), and what a repeated test writes into its predicate, are not pinned.
