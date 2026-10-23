<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The test sign `p`

*Measured on the hardware (2026-10-22).* The sign part of a test (`.test<S>...`) is `t`, `n` or `p`.
On a 32-bit result `n` is bit 31 and **`p` is its complement, `>= 0`**, so a zero result passes `p`
**[HW]**: `shl.testpat` and `and.testpat` set the predicate for 0, 1 and `0x7fffffff` and not for
`0x80000000` (probes `sign-p-shl`, `sign-p-and`; `design/sgx545_hw_spec.md` §14). The earlier open
reading `> 0` is wrong. The firmware's `testpat` on `shl` and `isub16` results therefore asks "bit
clear" (after the shift) and "not negative" **[infer]**. On the 16-bit tests of `chan0`, `p` is
`>= 0` on the low lane (*`isub16`/`isubu16` lanes*); `iaddu32`, unsigned, passes `p` always (*The
32-bit adds*).
