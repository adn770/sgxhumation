<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Synchronisation and misc: `nop`, `wdf`, `idf`, `setm`, `wop`, `lock`, `release`

Class 31 **[RE]**: `nop` is flow-control operation 5; the rest are the "other" group
(`enc[53:52]` = 2) with the operation in `enc[58:56]`: 0 `idf`, 1 `wdf`, 2 `setm`, 5
`lock`/`release` (by `enc[0]`, a three-bit mutex number), 7 `wop`.

- **`nop`** does nothing **[RE]**; with `.end` it ends the program **[HW]** (our USE
  secondary program). `.tog` and `.trigger` are **[open]**.
- **`wdf drcN`** waits for the loads counted on `drcN` **[RE/HW]**; **`idf drcN, bif`** is a
  fence on memory writes counted the same way **[infer]** (*Memory, data fences and the
  register file*).
- **`wop #n`** waits for the outputs to drain before an emit **[infer]**: our state programs
  issue `wop #0` between `emitvcbst` and `emitmtest`, as the captured ones do **[CAP/HW]**.
  `.twopart` is **[open]**.
- **`lock #n` / `release #n`** take and give back one of eight mutexes **[infer]**. The firmware
  executes `lock` twice and never `release` (whether something else releases the mutex is
  **[open]**).
- **`setm #n`** (a four-bit number) is **[open]**.

**In the firmware [RE].** 807 `wdf` (711 on `drc0`), 124 `idf ..., bif`, 87 `nop` (5 with
`.end`, 1 with `.nosched`), 2 `lock`.

<!-- @prog use_mtest18.s match='wdf|wop|nop' -->

<!-- @example usse idf drc1, bif -->

<!-- @group usse class=31 pat='^(nop|idf|wdf|setm|wop)|^\{0\}\{1\} #\{2\}$' -->
