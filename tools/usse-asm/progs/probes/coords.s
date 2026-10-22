; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Probe template for --coords runs (pa0 = (y << 16) | x): the pixel number p = 64y + x, and
; per pixel the binary32 values (2048 + p) * 2^(12k) in r3 (k = 0), r4 (1), r5 (2), r6 (3),
; i.e. 0x45000000 + k * 0x06000000 | p << 12, which say both the pixel and which of them a
; result came from; r7 = 0, r8 = 1.0. Then the case's setup PRE, the instruction(s) under
; test OP, and register OUT as the pixel.
phas #0, #0, pixel, end, parallel
shr.skipinv r1, pa0, #10
and.skipinv r2, pa0, #0x3f
or.skipinv r1, r1, r2
shl.skipinv r1, r1, #12
or.skipinv r3, r1, #0x45000000
or.skipinv r4, r1, #0x4B000000
or.skipinv r5, r1, #0x51000000
or.skipinv r6, r1, #0x57000000
mov.skipinv r7, #0
mov.skipinv r8, #0x3F800000
{PRE}
{OP}
mov.skipinv.end o0, {OUT}
