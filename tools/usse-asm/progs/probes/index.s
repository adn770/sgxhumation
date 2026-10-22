; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Index registers: r0-r19 hold 0xA0 + n, i.l = K (through r20), then OP (a move through
; r[il + #n], into r30 or from r21 = 0x5555AAAA) and register OUT as the pixel.
phas #0, #0, pixel, end, parallel
mov.skipinv r0, #0xA0
mov.skipinv r1, #0xA1
mov.skipinv r2, #0xA2
mov.skipinv r3, #0xA3
mov.skipinv r4, #0xA4
mov.skipinv r5, #0xA5
mov.skipinv r6, #0xA6
mov.skipinv r7, #0xA7
mov.skipinv r8, #0xA8
mov.skipinv r9, #0xA9
mov.skipinv r10, #0xAA
mov.skipinv r11, #0xAB
mov.skipinv r12, #0xAC
mov.skipinv r13, #0xAD
mov.skipinv r14, #0xAE
mov.skipinv r15, #0xAF
mov.skipinv r16, #0xB0
mov.skipinv r17, #0xB1
mov.skipinv r18, #0xB2
mov.skipinv r19, #0xB3
mov.skipinv r21, #0x5555AAAA
mov.skipinv r30, #0xEEEEEEEE
mov.skipinv r20, #{K}
mov i.l, r20
{OP}
mov.skipinv.end o0, {OUT}
