; use_fill.s -- the solid-fill pixel shader (transfer template USE_FILL, id 0).
;
; The ISP background object covers the fill rectangle, and this program runs
; once per pixel of it (design/sgx545_transfer.md 4.2.2). It writes the fill
; colour to output register o0; the end-of-tile program (use_eot.s) then packs
; o0 into the surface with pack mode 0, so the colour is the 0xAARRGGBB dword.
;
; Per transfer, the driver patches the colour into the immediate of the second
; instruction (sgx545_fw_format.md 9.4: PARAM 0, a USSE_LIMM patch at byte 8).
; Each colour therefore needs its own copy of the program. The template keeps
; 0 there.

        ; Phase marker, the first instruction of every pixel program here: no
        ; further phase (next program #0, #0 temporaries), pixel rate, wait
        ; condition "end", instances run in parallel.
        phas #0, #0, pixel, end, parallel

        ; o0 = the colour. A long immediate: the 32 bits are scattered over
        ; the word (the USSE_LIMM split). .skipinv runs it for every
        ; instance, .end ends the program.
        mov.skipinv.end o0, #0x00000000
