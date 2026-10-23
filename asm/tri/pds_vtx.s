; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; pds_vtx.s -- the vertex PDS program of the first triangle (design/sgx545_3d.md S.1).
;
; The VDM runs it for each vertex of an index list, with the vertex index in ir0. It
; DMAs one vertex (four dwords: x, y, z, w) from the vertex buffer into the USSE
; primary attributes pa0-pa3, then starts the vertex shader (use_vtx.s). The vertex
; address is base + index * stride: movsa adds its third source (ds1[50] = ir0 * stride)
; to the DMA address [CAP: the closed driver's vertex fetch has this form].
;
; Data segment, 8 dwords (banks interleave by pairs of dwords, hardware spec 13.8). The
; driver patches dwords 0, 1, 2 and 4:
;   dword 0 = ds0[0]: the vertex buffer address
;   dword 1 = ds0[1]: the doutd word: burst - 1 (3:0) = 3, lines - 1 (7:4) = 0, the
;                     destination attribute (19:8) = 0, stride - 1 (29:22) = 0 (S.2)
;   dword 2 = ds1[0]: the vertex stride in bytes (16)
;   dword 4 = ds0[2]: DOUTU U0 of the vertex shader (code base 10, the vertex heap)
; and keeps U1 (dword 5 = ds0[3]) and U2 (dword 3 = ds1[1]) at 0.

        .word 0          ; dword 0 = ds0[0]: vertex buffer address (patched)
        .word 0          ; dword 1 = ds0[1]: doutd word (patched)
        .word 0          ; dword 2 = ds1[0]: stride (patched)
        .word 0          ; dword 3 = ds1[1]: DOUTU U2 = 0
        .word 0          ; dword 4 = ds0[2]: DOUTU U0 (patched)
        .word 0          ; dword 5 = ds0[3]: DOUTU U1 = 0
        .word 0          ; dword 6 = ds1[2]: unused
        .word 0          ; dword 7 = ds1[3]: unused

        ; ds1[50] = vertex index * stride (16-bit operands).
        mul ds1[50], ir0.l, ds1[0].l
        ; DMA the vertex: address ds0[0] + ds1[50], control word ds0[1].
        movsa doutd, ds0[0], ds0[1], ds1[50], ds1[50]
        ; Start the shader once (if0), with U0, U1, U2.
        (if0) movs doutu, ds0[2], ds0[3], ds1[1], ds1[1]
        halt
