<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Phases: `phas`

Class 31 **[RE]**. The immediate form is described in *Tasks and phases*: the next phase's
instruction index (18 bits), its temporaries (eight bits, times four), the rate, the wait
condition and the instance mode, plus `.end`. A register form takes two source operands
instead (presumably the same information computed at run time **[infer]**). Both exist twice
in the decoder's table, as for `ldr`/`str`.

<!-- @prog use_vtx.s match='phas' -->

<!-- @example usse phas #7, #0, pixel, vcull, parallel -->

<!-- @group usse class=31 pat='^phas' -->
