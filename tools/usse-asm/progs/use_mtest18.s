; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; use_mtest18.s -- emit an 18-word MTE state block (design/sgx545_3d.md S.2).
;
; pds_mte.s has DMAed the block into pa0-pa17. The task waits for the DMA, copies the
; block to the outputs o0-o17, writes it to the vertex cache as state (emitvcbst),
; waits for the outputs to drain (wop) and emits the state to the MTE (emitmtest),
; ending the task [CAP: the closed driver's state copies have this shape].

        phas #0, #0, pixel, end, parallel               ; a single phase
        nop.nosched
        ; A load that never runs, then a wait on the data fence: the attributes the
        ; PDS DMAed are in place before they are read.
        and.skipinv.testtanz.chan0 !r0, p0, #0x00000000, #0
        p0 ldad.skipinv.fetch4 pa0, [pa0,+#0], drc0
        wdf drc0
        mov.skipinv.repeat16 o0, pa0                    ; words 0-15
        mov.skipinv.repeat2 o16, pa16                   ; words 16-17
        emitvcbst #0, #0
        wop #0
        emitmtest.end.freep #0, #0, #0
