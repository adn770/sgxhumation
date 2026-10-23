<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Predicates on repeated instructions

*Measured on the hardware (2026-10-22).* **One predicate covers the whole instruction** **[HW]** on
`mov` (other repeated forms are taken to behave alike **[infer]**): `p0 mov.repeat2` with
`p0 = 1, p1 = 0` writes both registers and with `p0 = 0, p1 = 1` neither (probe `repeat-pred`;
`design/sgx545_hw_spec.md` §14).

**Undecided:** what a repeated test writes into its predicate, and `Pn`; a probe of
`and.testtaz.repeat2` with inputs that differ per iteration would settle the first.
