<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Test channels `chan1`-`chan3`

*Measured on the hardware (2026-10-22).* `and.testtaz.chan1`, `.chan2`, `.chan3` and
`isub16.testtaz.chan1` **clear the predicate** (`p0`, leaving `p1`-`p3` alone) whatever the input,
even when the tested value is zero, on 32-bit and 16-bit operations alike **[HW]** (probes
`chan-and`, `chan-isub16`, `chan-preds`; `design/sgx545_hw_spec.md` §14). `chan0` tests the 32-bit
result, or the low lane of a 16-bit one.

**Undecided:** what `chan1`-`chan3` test, and `andall`, `orall`, `and02`, `or02`. A probe on an
8-bit or a repeated test, where there are more lanes to tell apart, would settle it.
