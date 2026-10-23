; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; fmad_tie.s -- a probe case in the shape of probes/dump.s (doc/isa tutorial, "Testing an
; instruction yourself"): (1 + 2^-12)^2 + 0, stored to the scratch buffer and the pixel.

        mov r1, #0x3f800800                     ; 1 + 2^-12
        fmad r10, r1, r1, #0                    ; the case under test
        mov r29, #0x5caf0000                    ; the scratch buffer (patched)
        stad [r29,+#0], r10
        mov.end o0, r10
