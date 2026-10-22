<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Memory, data fences and the register file

- **Loads complete asynchronously.** A load (`ld*`, `eld*`, `ldr`, the samplers) names a data
  fence counter, `drc0` or `drc1` (two bits for `ldr` and the samplers, `drc0`-`drc3`), and
  `wdf drcN` waits until the loads counted on it have landed **[RE/HW]**. The firmware almost
  always follows its loads with a `wdf` before using the data (711 `wdf drc0`, 96 `wdf drc1`),
  and our programs wait with `wdf drc0` for the attribute DMA the PDS started.
- **`idf drcN, bif`** appears 124 times in the firmware, mostly right before a `wdf` on the
  same counter, after stores **[RE]**: it looks like a fence that makes earlier memory writes
  visible (through the BIF, the memory interface) and is counted on `drcN` **[infer]**. The
  `pixelbe` and reserved modes of `idf` are **[open]**.
- **Addresses.** Loads and stores take a base register and an offset counted in elements of
  the access size (4 bytes for `d`, 2 for `w`, 1 for `b`) **[DDK/HW]**; the addressing modes
  and the fetch rule are in *Loads and stores*. Addresses are device virtual addresses of the
  task's memory context **[HW]**.
- **The SGX register file** is not memory: `ldr rD, #idx` and `str #idx, src` read and write
  the SGX register at offset `idx × 4` **[RE/HW]**. A client pixel shader on our render node
  reads any register this way; a client store to the one register tried was dropped
  (`design/sgx545_render_uapi.md` §7 Q4).
- **Caches.** `ci`, `cf` and `cfi` invalidate, flush, or flush and invalidate the levels their
  last operand names (L0 and L1, L2, or all three) **[infer]**, from the names the reference
  disassembler prints; the cache mode flags of loads and stores (`.bpcache`, `.fcfill`,
  `.bypassl1`) are discussed under *Loads and stores*.
