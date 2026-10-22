<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Tests: `tstz`, `tstn`

Opcodes 16 and 17 **[RE]**: `tstz pN, src` and `tstn pN, src` set predicate `p0`-`p2`
(`w[2:0]`; other values print an empty predicate) from a source that is `ds1[n]` (`w[8]` set)
or `ds0`, `ir0`, `ir1`. The test itself can be predicated.

- **Semantics.** `tstz` sets `pN` when the source is zero **[CAP/HW]** (*Predicates*); `tstn`
  when it is negative **[infer]**.
- **In the microkernel's programs [CAP].** The event program runs ten `tstz` (some of them
  predicated, some on `ir0` directly) and branches past the cases that do not apply, as our
  pixel-event program does; the deferred-work program picks its task with two. Neither uses
  `tstn`.

<!-- @prog pds_pixevent.s match='tstz' -->

<!-- @group pds pat='tst[zn]' -->
