; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; pds_pixevent.s -- the pixel-event PDS program (template PDS_PIXEVENT, id 6).
;
; The EDM runs it on pixel events (EDMPixelPDSExec/Data/Info): it reads the
; event bits in ir1 and the tile position in ir0 and starts the USE task the
; event asks for (design/sgx545_transfer.md 4.2.4):
;   ir1 bit 3  the PTOFF task;
;   ir1 bit 1  the end-of-tile task (use_eot.s), after a DOUTA write, with its
;              U1 built as base | tile_x | tile_y | ENDOFRENDER; but with bit 8
;              set, the end-of-render task instead;
;   ir1 bit 2  the end-of-render task (use_eor.s).
; [infer] ir0 and ir1 are the tile position and the event bits; the closed
; driver uses this program verbatim.
;
; tstz p0, x sets p0 when x is zero, so "(p0) bra" skips a case whose bit is
; clear. Branch targets count instructions from the first one after the data
; segment.
;
; Data segment, 20 dwords (banks interleave by pairs of dwords, hardware spec
; 13.8). The driver patches dwords 0-5 and 12 per transfer
; (sgx545_fw_format.md 9.4); the rest are constants.

        .word 0          ; dword 0  = ds0[0]: PTOFF task U0 (patched: the EOT task's)
        .word 0          ; dword 1  = ds0[1]: PTOFF task U1 (patched: 0x00800000)
        .word 0          ; dword 2  = ds1[0]: the DOUTA source (patched: 0)
        .word 0          ; dword 3  = ds1[1]: EOT task U0 (patched)
        .word 0          ; dword 4  = ds0[2]: EOR task U0 (patched)
        .word 0          ; dword 5  = ds0[3]: EOR task U1 (patched: 0x00c00000)
        .word 0x00000008 ; dword 6  = ds1[2]: ir1 bit 3, PTOFF
        .word 0x00000002 ; dword 7  = ds1[3]: ir1 bit 1, end of tile
        .word 0x00000100 ; dword 8  = ds0[4]: ir1 bit 8, end of render instead
        .word 0x00000FF8 ; dword 9  = ds0[5]: tile_x mask, U1 [11:3]
        .word 0x00000004 ; dword 10 = ds1[4]: ir1 bit 2, end of render
        .word 0          ; dword 11 = ds1[5]: unused
        .word 0          ; dword 12 = ds0[6]: EOT task U1 base (patched: 0x00800000, PBENABLE)
        .word 0x003FF000 ; dword 13 = ds0[7]: tile_y mask, U1 [21:12]
        .word 0          ; dword 14 = ds1[6]: unused
        .word 0          ; dword 15 = ds1[7]: unused
        .word 0x00400000 ; dword 16 = ds0[8]: ENDOFRENDER, U1 bit 22
        .word 0          ; dword 17 = ds0[9]: unused
        .word 0          ; dword 18 = ds1[8]: unused
        .word 0          ; dword 19 = ds1[9]: unused

        ; PTOFF: ir1 bit 3.
        and ds1[48], ir1, ds1[2]
        tstz p0, ds1[48]
        (p0) bra not_ptoff
        movs doutu, ds0[0], ds0[1], ds0[0], ds0[0]      ; U0, U1 (U2 repeats ds0[0])
        halt

not_ptoff:
        ; End of tile: ir1 bit 1.
        and ds1[48], ir1, ds1[3]
        tstz p0, ds1[48]
        (p0) bra not_eot
        ; With ir1 bit 8 also set, run the end-of-render task instead.
        not ds1[48], ir1
        and ds1[48], ds0[4], ds1[48]                    ; ~ir1 & 0x100: zero if bit 8 set
        tstz p0, ds1[48]
        (p0) bra eor
        ; Write the attribute (ds1[0]) for the end-of-tile task.
        movs douta, ir1.l, ds1[0], ir1.l, ir1.l
        ; U1 = base | tile_x (ir0[8:0] at [11:3]) | tile_y (ir0[18:9] at
        ; [21:12]) | ENDOFRENDER (ir1 bit 2 at [22]), built in ds0[48].
        shl ds1[48], ir0, 3
        and ds1[48], ds0[5], ds1[48]                    ; tile_x
        or ds0[48], ds0[6], ds1[48]                     ; base | tile_x
        shl ds1[48], ir0, 3
        and ds1[48], ds0[7], ds1[48]                    ; tile_y
        or ds0[48], ds0[48], ds1[48]
        shl ds1[48], ir1, 20
        and ds1[48], ds0[8], ds1[48]                    ; ENDOFRENDER
        or ds0[48], ds0[48], ds1[48]
        movs doutu, ds1[1], ds0[48], ds0[48], ds0[48]   ; U0 = ds1[1], U1 = ds0[48]
        halt

not_eot:
        ; End of render: ir1 bit 2.
        and ds1[48], ir1, ds1[4]
        tstz p0, ds1[48]
        (p0) bra done
eor:
        movs doutu, ds0[2], ds0[3], ds0[2], ds0[2]      ; U0, U1 of the EOR task
done:
        halt
