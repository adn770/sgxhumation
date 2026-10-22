<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `t` in the `a`/`o` joins of a test

*Measured on the hardware (2026-10-22).* A test joins a sign test and a zero test with `a` (and) or
`o` (or), and either can be `t`. **`t` means true** **[HW]**: `and.testnot`, `.testtoz` and
`.testtonz` set the predicate for every input (probes `join-t-not`, `join-t-toz`, `join-t-tonz`;
`design/sgx545_hw_spec.md` §14). So an `o` join with a `t` side is always true, `testtaz` is "zero",
`testtanz` "not zero", `testnat` "negative" and `testpat` "not negative"; the "not tested" reading
is ruled out.
