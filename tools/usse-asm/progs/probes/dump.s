; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Probe template: inputs A-H in r1-r8, r10-r17 preset to distinct bytes (0xA0A1A2A3, then
; +0x04040404 each), the instruction(s) under test OP, then r10-r17 and r1-r8 stored to
; scratch dwords 0-15 (expected as a list) and register OUT as the pixel.
phas #0, #0, pixel, end, parallel
mov.skipinv r1, #{A}
mov.skipinv r2, #{B}
mov.skipinv r3, #{C}
mov.skipinv r4, #{D}
mov.skipinv r5, #{E}
mov.skipinv r6, #{F}
mov.skipinv r7, #{G}
mov.skipinv r8, #{H}
mov.skipinv r10, #0xA0A1A2A3
mov.skipinv r11, #0xA4A5A6A7
mov.skipinv r12, #0xA8A9AAAB
mov.skipinv r13, #0xACADAEAF
mov.skipinv r14, #0xB0B1B2B3
mov.skipinv r15, #0xB4B5B6B7
mov.skipinv r16, #0xB8B9BABB
mov.skipinv r17, #0xBCBDBEBF
{OP}
mov.skipinv r29, #0x5CAF0000
stad [r29,+#0], r10
stad [r29,+#1], r11
stad [r29,+#2], r12
stad [r29,+#3], r13
stad [r29,+#4], r14
stad [r29,+#5], r15
stad [r29,+#6], r16
stad [r29,+#7], r17
stad [r29,+#8], r1
stad [r29,+#9], r2
stad [r29,+#10], r3
stad [r29,+#11], r4
stad [r29,+#12], r5
stad [r29,+#13], r6
stad [r29,+#14], r7
stad [r29,+#15], r8
idf drc0, bif
wdf drc0
mov.skipinv.end o0, {OUT}
