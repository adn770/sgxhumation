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

- **The tests [HW].** On 32-bit results tested on `chan0`: `z` and `nz` test the result for
  zero, `n` tests bit 31 and `p` is its complement (`>= 0`, so zero passes), and `t` is true
  in either join: `testtaz`/`testtanz` are "zero" and "not zero", an `o` join with a `t` side
  is always true (*The test sign `p`*, *`t` in the `a`/`o` joins of a test*). The firmware
  tests flags this way hundreds of times, and sign bits by shifting the flag to bit 31 first
  (`shl.testnat`); our state and vertex programs rely on `and.testtanz` of 0 and 0 leaving
  the predicate false (*Predicates*).
- **The 32-bit adds [HW]** are not full 32-bit adds: `iadd32`/`iaddu32` add source 1's low 16
  bits, sign- or zero-extended, to the 32-bit source 2, and `iaddu32`'s `n` is never true
  (*The 32-bit adds `iadd32` and `iaddu32`*).
- **The 16-bit operations [HW]:** `isub16`/`isubu16` subtract both lanes, wrapping, and `chan0`
  tests the low lane (*`isub16`/`isubu16` lanes*). `iadd16`, `iaddu16`, `imul16`, `imulu16`,
  `iadd8`, `isub8`, `imul8`, `fadd`, `fmul` and `fsub` test their lanes as the channels say, a
  16-bit lane on its full result before it is cut **[HW]** (*Test channels*).
- **Channels [HW]:** a test has one result per channel: bytes 0-3 for the 8-bit operations,
  the low and high halves as channels 0 and 2 for the 16-bit ones, the value as channel 0
  otherwise; a channel without a lane tests as a positive non-zero value. `chan0`-`chan3`
  pick one, `andall`/`orall` combine all four, `and02`/`or02` channels 0 and 2 (*Test
  channels*). The masks of class 15, `.partial` and `.onceonly` on tests are **[open]**.

**In the firmware [RE].** 671 tests, every one on `chan0` and into a predicate: `and.testtaz`
246, `and.testtanz` 163, `xor.testtaz` 108, `xor.testtanz` 67, `shl.testnat` 16,
`shl.testpat` 12, `isub16` 40 (with `testtanz`, `testpat`, `testnat`, `testtaz`), `iaddu32`
8, `or` 9, `isubu16` 1. Most discard the result (`!c0`): test, then branch on the predicate.

<!-- @prog use_mtest18.s match='\.test' -->

<!-- @example usse xor.testtaz.chan0 r1, p2, r1, r3 -->

<!-- @example usse isub16.testpat.chan0 r1, p2, r3, #4 -->

<!-- @group usse class=9,15 -->
