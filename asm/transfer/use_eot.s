; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; use_eot.s -- the end-of-tile program (template USE_EOT, id 2).
;
; Runs when a tile is finished and hands the tile's output registers to the
; pixel back end (PBE) with the five PBE state words of the destination
; (design/sgx545_transfer.md 4.2.5): emitpix1 sends words 0 and 1, emitpix2
; words 2 and 3 plus the sideband word 4 as an immediate. The task gets four
; temporaries (TQ_META temps 4) for words 0-3.
;
; Per transfer the driver patches (sgx545_fw_format.md 9.4):
;   - the destination register of the movs at bytes 16, 24 and 32 to r1, r2
;     and r3 (CONST 1 << 21, 2 << 21, 3 << 21 added to the word): the closed
;     generator adds the register number in a loop, so the template's four
;     movs all name r0 and so do ours, to reproduce it bit for bit;
;   - their immediates to PBE words 0-3 (PARAM 1-4, USSE_LIMM patches);
;   - the sideband of the emitpix2 (PARAM 5, EMITPIX_SB: value bits [5:0]
;     to e[27:22], [11:6] to e[40:35], [13:12] to e[55:54]).

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s

        ; Load words 0-3 into r0-r3 (the last three r0 until patched).
        mov.skipinv r0, #0x00000000             ; r0 = word 0: XMIN [12:0], YMIN [28:16]
        mov.skipinv r0, #0x00000000             ; -> r1 = word 1: XMAX, YMAX (inclusive)
        mov.skipinv r0, #0x00000000             ; -> r2 = word 2: COUNT [15:8], LINESTRIDE [27:16]
        mov.skipinv r0, #0x00000000             ; -> r3 = word 3: FBADDR [31:2]

        ; Emit to the PBE: incp #0, sources r0 and r1 (words 0 and 1).
        ; [infer] .freep frees the task's resources once the emit is done.
        emitpix1.freep #0 /* incp */, r0, r1

        ; Words 2 and 3 from r2 and r3, the sideband word as an immediate
        ; (PACKMODE [5:0], TILERELATIVE [9]: 0x200 for ARGB8888), and end.
        ; The third source (r0) is not used for this emit.
        emitpix2.end.freep #0 /* incp */, r2, r3, r0, #0x00000000
