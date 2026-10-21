; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; use_mtest2.s -- emit a two-word MTE state block (design/sgx545_3d.md S.2, S.4).
;
; As use_mtest18.s for the end-of-scene block (presence 0x2000 and its word).

        phas #0, #0, pixel, end, parallel
        nop.nosched
        and.skipinv.testtanz.chan0 !r0, p0, #0x00000000, #0
        p0 ldad.skipinv.fetch4 pa0, [pa0,+#0], drc0
        wdf drc0
        mov.skipinv.repeat2 o0, pa0                     ; words 0-1
        emitvcbst #0, #0
        wop #0
        emitmtest.end.freep #0, #0, #0
