; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Index registers on the secondary attributes: sa0-sa7 are set to 0xB0 + n (every
; instance writes the same values), i.l = K (through r20), then OP (a move through
; sa[il + #n], into r30 or from r21 = 0x5555AAAA) and register OUT as the pixel.
phas #0, #0, pixel, end, parallel
mov.skipinv r0, #0xB0
mov.skipinv r1, #0xB1
mov.skipinv r2, #0xB2
mov.skipinv r3, #0xB3
mov.skipinv r4, #0xB4
mov.skipinv r5, #0xB5
mov.skipinv r6, #0xB6
mov.skipinv r7, #0xB7
mov.skipinv sa0, r0
mov.skipinv sa1, r1
mov.skipinv sa2, r2
mov.skipinv sa3, r3
mov.skipinv sa4, r4
mov.skipinv sa5, r5
mov.skipinv sa6, r6
mov.skipinv sa7, r7
mov.skipinv r21, #0x5555AAAA
mov.skipinv r30, #0xEEEEEEEE
mov.skipinv r20, #{K}
mov i.l, r20
{OP}
mov.skipinv.end o0, {OUT}
