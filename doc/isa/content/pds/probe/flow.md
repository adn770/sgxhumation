<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Loops, `call` and `rtn`

*Not probed yet.* `pds-loop` runs a counted loop with a backward `bra` and a predicated forward
one (the label is the instruction index, as **[HW]** for forward branches). `pds-call` nests
calls 1 to 7 deep, marking each level on entry and after its call returns (readings: a return
stack at least 7 deep; one return register, so a nested return loops and the job fails).
