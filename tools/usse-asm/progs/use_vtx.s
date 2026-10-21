; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; use_vtx.s -- the vertex shader of the first triangle (design/sgx545_3d.md S.1, 9.2).
;
; pds_vtx.s DMAs one vertex (x, y, z, w, already in clip space) into pa0-pa3 and starts
; this task. It copies the position to the outputs o0-o3 and hands the vertex first to
; the vertex cache (emitvcbvtx) and then, once the vertex culling stage has seen it, to
; the MTE (emitmtevtx): two phases, the second waiting for "vcull" [CAP: the shape of
; every vertex program the closed compiler emits].
;
; The phas operand is the instruction index of the next phase, counted from the code
; base the task runs from (USE_CODE_BASE_10). This source writes it relative to the
; program's first instruction; the driver adds the index at which it places the
; program (sgx_tri.c).

        ; Phase 0: wait for the attribute DMA, copy the position, emit to the VCB.
        phas #7, #0, pixel, vcull, parallel
        nop.nosched
        ; A load that never runs (p0 is false: 0 & 0 is zero) followed by a wait on the
        ; data fence: the task does not read pa0-pa3 before the PDS DMA has landed.
        and.skipinv.testtanz.chan0 !r0, p0, #0x00000000, #0
        p0 ldad.skipinv.fetch4 pa0, [pa0,+#0], drc0
        wdf drc0
        mov.skipinv.repeat4 o0, pa0                     ; o0-o3 = x, y, z, w
        emitvcbvtx.end.twopart #0, #0
        ; Phase 1 (instruction 7): emit the vertex to the MTE and end.
        phas #0, #0, pixel, end, parallel
        emitmtevtx.end.twopart.freep #0, #0, #0
