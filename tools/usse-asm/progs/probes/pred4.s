; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Probe template: every predicate p0-p3 preset (PV = 0: set, otherwise clear), inputs A-C
; in r1-r3, the instruction under test OP, then the predicates as the pixel's low bits:
; o0 = p0 | p1 << 1 | p2 << 2 | p3 << 3.
phas #0, #0, pixel, end, parallel
mov.skipinv r10, #{PV}
and.testtaz.chan0 !c0, p0, r10, r10
and.testtaz.chan0 !c0, p1, r10, r10
and.testtaz.chan0 !c0, p2, r10, r10
and.testtaz.chan0 !c0, p3, r10, r10
mov.skipinv r1, #{A}
mov.skipinv r2, #{B}
mov.skipinv r3, #{C}
{OP}
mov.skipinv r30, #0
p0 or.skipinv r30, r30, #1
p1 or.skipinv r30, r30, #2
p2 or.skipinv r30, r30, #4
p3 or.skipinv r30, r30, #8
mov.skipinv.end o0, r30
