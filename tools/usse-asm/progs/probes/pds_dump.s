; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; PDS probe template (sgxfive-test probe --pds 36 --use pa_dump.bin): inputs A-H in
; ds0[2]-ds0[9] and I-P in ds1[2]-ds1[9] (a table without I-P repeats A-H there), the
; operation(s) under test OP, then ds0[2]-ds0[9] to pa0-pa7 and ds1[2]-ds1[9] to pa8-pa15
; with douta; pa_dump.s stores pa0-pa15 to scratch dwords 0-15 (expected as a list).
;
; Data rows (design/sgx545_pds_isa.md 3.1): row r holds ds0[2r], ds0[2r+1], ds1[2r],
; ds1[2r+1]. A movs reads at most one ds0 pair and one ds1 pair, so the douta control word
; (the attribute in bits 19-8) sits in the other bank from its value.
        .word 0, 0, 0, 0                        ; row 0: U0, U1, U2 (sgxfive-test), unused
        .word {A}, {B}, {I}, {J}                ; rows 1-4: the inputs
        .word {C}, {D}, {K}, {L}
        .word {E}, {F}, {M}, {N}
        .word {G}, {H}, {O}, {P}
        .word 0x800, 0x900, 0x000, 0x100        ; rows 5-8: ds0[10+k] attribute 8+k,
        .word 0xA00, 0xB00, 0x200, 0x300        ; ds1[10+k] attribute k
        .word 0xC00, 0xD00, 0x400, 0x500
        .word 0xE00, 0xF00, 0x600, 0x700

        {OP}

        movs douta, ds0[2], ds1[10], ds0[2], ds0[2]
        movs douta, ds0[3], ds1[11], ds0[3], ds0[3]
        movs douta, ds0[4], ds1[12], ds0[4], ds0[4]
        movs douta, ds0[5], ds1[13], ds0[5], ds0[5]
        movs douta, ds0[6], ds1[14], ds0[6], ds0[6]
        movs douta, ds0[7], ds1[15], ds0[7], ds0[7]
        movs douta, ds0[8], ds1[16], ds0[8], ds0[8]
        movs douta, ds0[9], ds1[17], ds0[9], ds0[9]
        movs douta, ds1[2], ds0[10], ds1[2], ds1[2]
        movs douta, ds1[3], ds0[11], ds1[3], ds1[3]
        movs douta, ds1[4], ds0[12], ds1[4], ds1[4]
        movs douta, ds1[5], ds0[13], ds1[5], ds1[5]
        movs douta, ds1[6], ds0[14], ds1[6], ds1[6]
        movs douta, ds1[7], ds0[15], ds1[7], ds1[7]
        movs douta, ds1[8], ds0[16], ds1[8], ds1[8]
        movs douta, ds1[9], ds0[17], ds1[9], ds1[9]
        movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]
        halt
