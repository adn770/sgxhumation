<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The output ports

A `movs` (or `movsa`) writes four source words to a **port**; the `dout*` ports issue work to
other units **[RE]**. What each port does with its words, where we know it:

| port | words | known meaning |
|------|-------|---------------|
| `doutu` | U0, U1, U2 | start a USSE task **[HW]** |
| `doutd` | address, control | DMA memory into the USSE attributes **[HW]** |
| `douta` | value, control | write one attribute **[CAP/HW]** |
| `douti` | two words | issue iterator work **[HW]** |
| `doutt` | T0-T3 | the state of a texture fetch **[HW]** |
| `doutc`, `douts`, `slc`, `mr`, `tim`, `ir0` | | **[open]** |

- **`doutu`** **[HW]**. U0 holds the code address: `CBASE` (`USE_CODE_BASE_n`) in bits 3-0 and
  the instruction index above it (`EXE`, bits 19-4 on this build's layout), plus the
  temporaries in fours (`TRC`) in bits 31-24. U1 holds flags: bit 2 `SDSOFT` for the pixel
  and secondary programs, bit 23 `PBENABLE` for the end-of-tile task, bit 22 `ENDOFRENDER`,
  bit 25 `TEXTUREDEPENDENCY` (wait for the texture result); U2 is 0 in all our programs
  (`design/sgx545_transfer.md` §4.2.4, §4.4.3). A `movs` always names four sources; `doutu`
  uses three.
- **`doutd`** **[CAP/HW]**. The first word is the source address; the second is
  burst size − 1 (bits 3-0), lines − 1 (7-4), the destination attribute (19-8) and
  stride − 1 (29-22) (`design/sgx545_3d.md` S.2). With `movsa` the third source is added to
  the address: our vertex program fetches vertex `index × stride` this way.
- **`douta`** writes its first word to the attribute named by bits 19-8 of the second
  **[CAP]**; our pixel-event program uses it before starting the end-of-tile task.
- **`douti`** and **`doutt`** **[HW]**: our copy's primary program issues one iterator word pair
  (one texture fetch from the first layer, no USE issue) and the four texture state words
  T0-T3, and the texel arrives in `pa0` of the task `doutu` started
  (`design/sgx545_transfer.md` §4.4.3).
