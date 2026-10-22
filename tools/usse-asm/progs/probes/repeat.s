; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Repeats: r1-r7 hold 0x11 * n, r10 and r11 set p0 and p1 (zero: set), then OP; the
; pixel packs the low bytes of r1-r4 as r1 | r2 << 8 | r3 << 16 | r4 << 24.
phas #0, #0, pixel, end, parallel
mov.skipinv r1, #0x11
mov.skipinv r2, #0x22
mov.skipinv r3, #0x33
mov.skipinv r4, #0x44
mov.skipinv r5, #0x55
mov.skipinv r6, #0x66
mov.skipinv r7, #0x77
mov.skipinv r10, #{Z0}
mov.skipinv r11, #{Z1}
and.testtaz.chan0 !c0, p0, r10, r10
and.testtaz.chan0 !c0, p1, r11, r11
{OP}
shl r30, r4, #24
shl r29, r3, #16
or r30, r30, r29
shl r29, r2, #8
or r30, r30, r29
or r30, r30, r1
mov.skipinv.end o0, r30
