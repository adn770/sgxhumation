<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Register file access: `ldr`, `str`

Class 31, other group, operation 6 **[RE]**. `ldr rD, #idx, drcN` reads SGX register `idx`
(dword index, offset `idx × 4`) into a temporary or `pa` register, counted on a data fence;
`str #idx, src` writes it. The index is a 14-bit immediate or a source-2 register; flags
`.skipinv` and `.nosched`.

- **Semantics [HW].** `ldr r0, #7, drc0` returns `CORE_ID` (`0x01150000`) and `ldr r0, #801`
  returns `BIF_DIR_LIST_BASE0`, from a client pixel shader on our render node; a client
  `str #671` (`USE_G0`) changed nothing (`design/sgx545_render_uapi.md` §7 Q4). The firmware
  programs the whole GPU with these two instructions.
- **Pending hardware probe:** the 14-bit index mask (which index bits the hardware decodes).
- The decoder holds two copies of these forms (an older one and the class-31 family's); the
  older ones match first.

**In the firmware [RE].** 196 `ldr` and 422 `str`; those with an immediate index touch 191
distinct registers, all of them named SGX545 registers (`design/sgx545_hw_spec.md` §13.6,
`tools/usse-dis/regtouch.py`).

<!-- @example usse ldr r0, #7, drc0 -->

<!-- @example usse str #671, r1 -->

<!-- @group usse class=31 pat='^(ldr|str)' -->
