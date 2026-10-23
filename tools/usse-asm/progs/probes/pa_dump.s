; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; The fixed USE program of every PDS probe (sgxfive-test probe --pds --use): the PDS probe
; wrote its results to pa0-pa15 with douta; store them to scratch dwords 0-15 and mark the
; pixel.
phas #0, #0, pixel, end, parallel
mov.skipinv r29, #0x5CAF0000
stad [r29,+#0], pa0
stad [r29,+#1], pa1
stad [r29,+#2], pa2
stad [r29,+#3], pa3
stad [r29,+#4], pa4
stad [r29,+#5], pa5
stad [r29,+#6], pa6
stad [r29,+#7], pa7
stad [r29,+#8], pa8
stad [r29,+#9], pa9
stad [r29,+#10], pa10
stad [r29,+#11], pa11
stad [r29,+#12], pa12
stad [r29,+#13], pa13
stad [r29,+#14], pa14
stad [r29,+#15], pa15
idf drc0, bif
wdf drc0
mov.skipinv.end o0, #0x0DA7A000
