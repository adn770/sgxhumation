; use_eor.s -- the end-of-render program (template USE_EOR, id 3).
;
; Runs once after the last tile of a render: an emitpix2 to the PBE with zero
; words and sideband 0x200, which the closed driver issues to finish the
; render (design/sgx545_transfer.md 4.2.4). One temporary (TQ_META temps 1).
; Never patched.

        phas #0, #0, pixel, end, parallel       ; the phase marker, as in use_fill.s
        mov.skipinv r0, #0x00000000             ; r0 = 0
        ; Emit r0 (0) and the immediate #0 as words 2 and 3, sideband 0x200; end.
        emitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200
