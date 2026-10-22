<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The test group: ALU operations with a test

Classes 9 and 15 run an ALU operation (`enc[19:14]`) and **test its result**: class 9 into a
predicate `p0`-`p3` (`enc[35:34]`), class 15 into a byte, word or dword mask (`.bmsk`,
`.wmsk`, `.dmsk`). With `enc[20]` clear the result itself is not written (`!` before the
destination) **[RE]**. The operations: the bitwise group and shifts, integer adds, subtracts
and multiplies on 8, 16 and 32 bits (`iadd8` ... `iaddu32`, the 8-bit ones with `.sat`), and
float adds, multiplies, min/max, derivatives, dot product and reciprocal-type operations,
with the operand formats of their own groups when `enc[50]` is set.

The test is `.test<S><J><Z>.<chan>`, from `enc[43:36]`:

| part | bits | values |
|---|---|---|
| sign test `S` | `enc[43:42]` | `t`, `n` (negative), `p` (positive) |
| join `J` | `enc[39]` | `o` (or), `a` (and) |
| zero test `Z` | `enc[41:40]` | `t`, `z` (zero), `nz` (not zero) |
| channel | `enc[38:36]` | `chan0`-`chan3`, `andall`, `orall`, `and02`, `or02` |

- **What is known [RE].** On 32-bit results tested on `chan0`: `z` and `nz` test the result
  for zero, `n` tests bit 31, and `testtaz`/`testtanz` behave as "zero" and "not zero": the
  firmware tests flags this way hundreds of times and its control flow only makes sense so,
  and our state and vertex programs rely on `and.testtanz` of 0 and 0 leaving the predicate
  false (*Predicates*). The firmware also tests sign bits by shifting the flag to bit 31
  first (`shl.testnat`).
- **Pending hardware probes:** whether `p` means `> 0` or `>= 0`; what `t` means in an `a`/`o`
  join (always true, or "not tested"), which decides `testpat`, `testnat` and the rest; the
  lanes and saturation of `isub16`/`isubu16`; the 8-bit, 16-bit and float tests in general.
- **Channels** other than `chan0`, the masks of class 15, `.partial` and `.onceonly` are
  **[open]**.

**In the firmware [RE].** 671 tests, every one on `chan0` and into a predicate: `and.testtaz`
246, `and.testtanz` 163, `xor.testtaz` 108, `xor.testtanz` 67, `shl.testnat` 16,
`shl.testpat` 12, `isub16` 40 (with `testtanz`, `testpat`, `testnat`, `testtaz`), `iaddu32`
8, `or` 9, `isubu16` 1. Most discard the result (`!c0`): test, then branch on the predicate.

<!-- @prog use_mtest18.s match='\.test' -->

<!-- @example usse xor.testtaz.chan0 r1, p2, r1, r3 -->

<!-- @example usse isub16.testpat.chan0 r1, p2, r3, #4 -->

<!-- @group usse class=9,15 -->
