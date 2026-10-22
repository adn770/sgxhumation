<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `t` in the `a`/`o` joins of a test

*Pending hardware probe (2026-10-22).* A test joins a sign test and a zero test with `a` (and)
or `o` (or), and either test can be `t`. If `t` means "true", `testtaz` is "zero" and
`testtoz` is always true; if it means "not tested", `a` and `o` give the same result whenever
one side is `t`. The firmware's `testtaz`/`testtanz` (zero, not zero) fit both readings; the
`o` joins and `testnat`/`testpat` decide between them.
