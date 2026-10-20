; use_sec.s -- the USE secondary program of a transfer (template USE_SEC, id 1).
;
; The PDS secondary program (pds_sec.s) kicks this once; a transfer has no
; per-tile secondary work, so it only ends (design/sgx545_transfer.md 4.2.4).

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        nop.end                                 ; nothing to do: end the program
