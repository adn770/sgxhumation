; SPDX-License-Identifier: MIT
; SPDX-FileCopyrightText: 2026 Josep Torra
; Index registers on the secondary attributes, read only: i.l = K, then
; o0 = sa[il + #0] xor SA (0 when they are the same register).
phas #0, #0, pixel, end, parallel
mov.skipinv r20, #{K}
mov i.l, r20
mov r29, sa[il + #0]
mov r28, {SA}
xor.skipinv.end o0, r29, r28
