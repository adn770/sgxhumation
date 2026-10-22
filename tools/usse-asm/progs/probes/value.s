; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Probe template: inputs A-D in r1-r4, the instruction under test OP, then register
; OUT as the pixel (o0).
phas #0, #0, pixel, end, parallel
mov.skipinv r1, #{A}
mov.skipinv r2, #{B}
mov.skipinv r3, #{C}
mov.skipinv r4, #{D}
{OP}
mov.skipinv.end o0, {OUT}
