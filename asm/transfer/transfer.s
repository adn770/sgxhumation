; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; transfer.s -- the nine transfer-queue templates as one sgx-asm source
; (design/sgx545_fw_format.md §9-§11, design/sgx545_transfer.md §4).
;
; Each section is a template, its id the template id; the programs are the
; single-ISA sources next to it. The .patch records are the driver's
; TQ_PATCHES (format §9.4), in its order: fwcutter's test_asmmap.py checks
; that this gives ours.py's templates and facts.TQ_PATCHES exactly.

        ; per-transfer parameters (format §9.6)
        .equ P_COLOUR, 0
        .equ P_PBE0, 1
        .equ P_SIDEBAND, 5
        .equ P_TEX0, 6
        .equ P_TEX1, 7
        .equ P_TEXADDR, 8

        .equ DOUTU1_PIXEL, 4                    ; SDSOFT
        .equ DOUTU1_TEXTURE, 0x02000004         ; SDSOFT | TEXTUREDEPENDENCY
        .equ DOUTU1_EOT, 0x00800000             ; PBENABLE
        .equ DOUTU1_EOR, 0x00C00000             ; PBENABLE | ENDOFRENDER
        .equ DOUTI0_TC0, 0x40012001             ; TEXISSUE TC0 | USEISSUE NONE | TEXLASTISSUE
        .equ DOUTT3, 0xA0800000                 ; T3 as set_tag_state sets it

        ; U0, U1, U2 of a PDS program that starts USE program \use (data dwords 0-2)
        .macro kick use, u1
        .patch word32, doutu(\use), 0
        .patch word32, const(\u1), 4
        .patch word32, const(0), 8
        .endm

        .section use_fill, usse, 0
        .include "use_fill.s"
        .patch usse_limm, param(P_COLOUR), 8

        .section use_sec, usse, 1
        .include "use_sec.s"

        .section use_eot, usse, 2
        .include "use_eot.s"
        ; the generator numbers the four movs r0..r3 before the immediates
        .patch word32, const(1 << 21), 16
        .patch word32, const(2 << 21), 24
        .patch word32, const(3 << 21), 32
        .patch usse_limm, param(P_PBE0), 8
        .patch usse_limm, param(P_PBE0 + 1), 16
        .patch usse_limm, param(P_PBE0 + 2), 24
        .patch usse_limm, param(P_PBE0 + 3), 32
        .patch emitpix_sb, param(P_SIDEBAND), 48

        .section use_eor, usse, 3
        .include "use_eor.s"

        .section pds_prim, pds, 4
        .include "pds_prim.s"
        kick use_fill, DOUTU1_PIXEL

        .section pds_sec, pds, 5
        .include "pds_sec.s"
        kick use_sec, DOUTU1_PIXEL

        .section pds_pixevent, pds, 6
        .include "pds_pixevent.s"
        ; ds0[0], ds0[1] PTOFF task; ds1[0] douta source; ds1[1] EOT U0;
        ; ds0[2], ds0[3] EOR task; ds0[6] EOT U1 base
        .patch word32, doutu(use_eot), 0
        .patch word32, const(DOUTU1_EOT), 4
        .patch word32, const(0), 8
        .patch word32, doutu(use_eot), 12
        .patch word32, doutu(use_eor), 16
        .patch word32, const(DOUTU1_EOR), 20
        .patch word32, const(DOUTU1_EOT), 48

        .section use_copy, usse, 7
        .include "use_copy.s"

        .section pds_prim_1src, pds, 8
        .include "pds_prim_1src.s"
        kick use_copy, DOUTU1_TEXTURE
        ; DOUTI, then the texture state words T2, T3, T0, T1
        .patch word32, const(DOUTI0_TC0), 16
        .patch word32, const(0), 20
        .patch word32, param(P_TEXADDR), 24
        .patch word32, const(DOUTT3), 28
        .patch word32, param(P_TEX0), 32
        .patch word32, param(P_TEX1), 36
