; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; use_ptoff.s -- the PTOFF task of a 3D render (design/sgx545_3d.md 9.4).
;
; pds_pixevent.s starts it for the PTOFF pixel event (ir1 bit 3). A transfer never
; sees that event and points the PTOFF case at its end-of-tile task; a 3D render
; gives it a task of its own that only signals ptoff and ends [infer: from the name,
; the instruction the closed 3D driver's PTOFF task runs].

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        ptoff.end
