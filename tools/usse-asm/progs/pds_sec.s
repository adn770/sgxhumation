; pds_sec.s -- the PDS secondary program, BASIC (template PDS_SEC, id 5).
;
; Starts the USE secondary program (use_sec.s) once per transfer, exactly as
; pds_prim.s starts the fill shader. This build carries the same bytes for
; both; they keep separate templates because other builds may differ
; (sgx545_fw_format.md 9.2). The driver patches U0 = the DOUTU word of
; USE_SEC, U1 = 4, U2 = 0.

        .word 0         ; dword 0 = ds0[0]: U0 (patched)
        .word 0         ; dword 1 = ds0[1]: U1 (patched)
        .word 0         ; dword 2 = ds1[0]: U2 (patched)
        .word 0         ; dword 3 = ds1[1]: unused

        movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]      ; kick the USE task (U0-U2)
        halt
