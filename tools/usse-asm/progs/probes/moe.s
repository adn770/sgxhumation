; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Repeat increments: r10-r15 hold 0x10 + n, r20-r25 0x20 + n; smlsi with increments
; I0-I3, then mov.repeat2 r20, r10; the pixel packs the low bytes of r20-r23.
phas #0, #0, pixel, end, parallel
mov.skipinv r10, #0x10
mov.skipinv r11, #0x11
mov.skipinv r12, #0x12
mov.skipinv r13, #0x13
mov.skipinv r14, #0x14
mov.skipinv r15, #0x15
mov.skipinv r20, #0x20
mov.skipinv r21, #0x21
mov.skipinv r22, #0x22
mov.skipinv r23, #0x23
mov.skipinv r24, #0x24
mov.skipinv r25, #0x25
smlsi #{I0}, #{I1}, #{I2}, #{I3}, incrementmode, incrementmode, incrementmode, incrementmode, #0, #0, #0
mov.repeat2 r20, r10
shl r30, r23, #24
shl r29, r22, #16
or r30, r30, r29
shl r29, r21, #8
or r30, r30, r29
or r30, r30, r20
mov.skipinv.end o0, r30
