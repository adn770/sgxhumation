; pds_prim.s -- the PDS primary program of a fill, KICKONLY (template
; PDS_PRIM, id 4).
;
; The background object names this program; the PDS runs it per pixel batch
; and it only starts the fill shader (use_fill.s) as a USE task: a DOUTU
; issue of the three words U0-U2 (design/sgx545_transfer.md 4.2.4).
;
; Data segment (the first 4 dwords; banks interleave by pairs of dwords,
; hardware spec 13.8). The driver patches dwords 0-2 (sgx545_fw_format.md
; 9.4): U0 = the DOUTU word of USE_FILL (EXE [19:4], CBASE [3:0], TRC
; [31:24]), U1 = 4 (SDSOFT), U2 = 0.

        .word 0         ; dword 0 = ds0[0]: U0 (patched)
        .word 0         ; dword 1 = ds0[1]: U1 (patched)
        .word 0         ; dword 2 = ds1[0]: U2 (patched)
        .word 0         ; dword 3 = ds1[1]: unused

        ; Issue U0, U1, U2 to the USE through the DOUTU port. A movs always
        ; names four sources; DOUTU takes three, the fourth repeats ds0[0].
        movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]
        halt
