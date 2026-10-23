; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; pds_mte.s -- the PDS program of a VDM state block (design/sgx545_3d.md S.2).
;
; A VDM state block names this program. It DMAs an MTE state block (a presence word
; and the words it announces) from memory into the USSE primary attributes and starts
; a USSE task that copies them to the outputs and emits them to the MTE
; (use_mtest18.s, use_mtest2.s).
;
; Data segment, 8 dwords (banks interleave by pairs of dwords, hardware spec 13.8),
; all patched by the driver:
;   dword 0 = ds0[0]: the state block's address
;   dword 1 = ds0[1]: the doutd word: burst - 1 (3:0), lines - 1 (7:4), destination
;                     attribute 0 (19:8), stride - 1 (29:22) (S.2)
;   dword 4 = ds0[2]: DOUTU U0 of the copy task (code base 10, the vertex heap)
;   dword 5 = ds0[3]: DOUTU U1 = 0; dword 2 = ds1[0]: DOUTU U2 = 0

        .word 0          ; dword 0 = ds0[0]: state block address (patched)
        .word 0          ; dword 1 = ds0[1]: doutd word (patched)
        .word 0          ; dword 2 = ds1[0]: DOUTU U2 = 0
        .word 0          ; dword 3 = ds1[1]: unused
        .word 0          ; dword 4 = ds0[2]: DOUTU U0 (patched)
        .word 0          ; dword 5 = ds0[3]: DOUTU U1 = 0
        .word 0          ; dword 6 = ds1[2]: unused
        .word 0          ; dword 7 = ds1[3]: unused

        movs doutd, ds0[0], ds0[1], ds0[0], ds0[1]      ; DMA the block (two sources used)
        movs doutu, ds0[2], ds0[3], ds1[0], ds1[0]      ; start the copy task
        halt
