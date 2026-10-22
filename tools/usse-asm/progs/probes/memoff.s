; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Loads and stores with a register offset: r1 = the scratch buffer + 0x8000 (dword 0x2000;
; the host fills dword i with 0xD0000000 | i), r2 = the offset O, r3 = 0x11111111, then OP
; and register OUT as the pixel.
phas #0, #0, pixel, end, parallel
mov.skipinv r1, #0x5CAF8000
mov.skipinv r2, #{O}
mov.skipinv r3, #0x11111111
mov.skipinv r30, #0xEEEEEEEE
{OP}
wdf drc0
mov.skipinv.end o0, {OUT}
