<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `br` offsets and the `Pn` predicate

*Measured on the hardware (2026-10-22).* **`br #k` jumps `k` instructions on from the `br`
itself**: `br #1` runs the next instruction, `br #2` skips one (probe `br-offset`; `ba` agrees
with the absolute index). **An instruction predicated `Pn` (value 7) never runs**, whatever
`p0`-`p3` hold (probe `predicate-pn`) **[HW]** (`design/sgx545_hw_spec.md` §14).
