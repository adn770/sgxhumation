; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; A USE program for PDS probes whose result differs per pixel batch (sgxfive-test probe
; --pds --use --pixels): the PDS program's pa0 as the pixel.
phas #0, #0, pixel, end, parallel
mov.skipinv.end o0, pa0
