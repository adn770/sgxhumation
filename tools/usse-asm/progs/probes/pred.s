; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Probe template (tools/usse-asm/probe.py): inputs A-C in r1-r3, the instruction under
; test OP, then predicate P as the pixel: o0 = 1 when set, 0 when clear.
phas #0, #0, pixel, end, parallel
mov.skipinv r1, #{A}
mov.skipinv r2, #{B}
mov.skipinv r3, #{C}
{OP}
mov.skipinv r30, #0
{P} mov.skipinv r30, #1
mov.skipinv.end o0, r30
