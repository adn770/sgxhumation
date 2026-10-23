; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; eot_patched.s -- use_eot.s as the driver patches it for one transfer, with made-up PBE
; words (doc/isa tutorial, "Leaving a tile"): the movs load r0-r3, the emits send them.

        phas #0, #0, pixel, end, parallel
        mov.skipinv r0, #0x00000000             ; word 0: XMIN 0, YMIN 0
        mov.skipinv r1, #0x001f003f             ; word 1: XMAX 63, YMAX 31
        mov.skipinv r2, #0x01001000             ; word 2: COUNT, LINESTRIDE
        mov.skipinv r3, #0x12340000             ; word 3: FBADDR
        emitpix1.freep #0 /* incp */, r0, r1
        emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000200
