; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; use_copy.s -- the copy pixel shader (template USE_COPY, id 7).
;
; For a copy, the PDS primary program (pds_prim_1src.s) has the texture unit
; fetch the source texel of each pixel into primary attribute pa0, and makes
; this task wait for it (DOUTU U1 TEXTUREDEPENDENCY). The shader passes the
; texel on to o0; the end-of-tile program packs it as for a fill
; (design/sgx545_transfer.md 4.4.3). Never patched: one copy serves every blit.

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        mov.skipinv.end o0, pa0                 ; o0 = the fetched texel; end
