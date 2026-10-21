; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; pds_prim_1src.s -- the PDS primary program of a copy, single_source
; (template PDS_PRIM_1SRC, id 8).
;
; Per pixel batch: start the copy shader (use_copy.s), have the iterator
; issue one texture fetch, and give the texture unit the four state words of
; the source texture (design/sgx545_transfer.md 4.4.3).
;
; Data segment, 12 dwords (banks interleave by pairs of dwords). The driver
; patches dwords 0-2 and 4-9 per transfer (sgx545_fw_format.md 10.4).

        .word 0         ; dword 0  = ds0[0]: DOUTU U0 of USE_COPY (patched)
        .word 0         ; dword 1  = ds0[1]: U1 = 0x02000004, SDSOFT | TEXTUREDEPENDENCY
        .word 0         ; dword 2  = ds1[0]: U2 = 0
        .word 0         ; dword 3  = ds1[1]: unused
        .word 0         ; dword 4  = ds0[2]: DOUTI word 0 = 0x40012001 (one fetch, layer 0)
        .word 0         ; dword 5  = ds0[3]: DOUTI word 1 = 0
        .word 0         ; dword 6  = ds1[2]: T2, the source address
        .word 0         ; dword 7  = ds1[3]: T3 = 0xa0800000
        .word 0         ; dword 8  = ds0[4]: T0, filter and stride
        .word 0         ; dword 9  = ds0[5]: T1, type, format and size
        .word 0         ; dword 10 = ds1[4]: unused
        .word 0         ; dword 11 = ds1[5]: unused

        ; Start the USE task: U0, U1, U2 (the fourth source is not used).
        movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]
        ; The iterator: DOUTI words 0 and 1 (the last two sources unused).
        movs douti, ds0[2], ds0[3], ds0[2], ds0[2]
        ; The texture state T0, T1, T2, T3, in that order: the fetch result
        ; lands in pa0 of the USE task.
        movs doutt, ds0[4], ds0[5], ds1[2], ds1[3]
        halt
