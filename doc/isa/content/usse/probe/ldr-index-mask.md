<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The 14-bit `ldr`/`str` index

*Pending hardware probe (2026-10-22).* The immediate register index of `ldr`/`str` has 14 bits
(a reach of 64 KiB of register space) **[RE]**; the SGX545 register block the firmware uses
needs far fewer (its highest index is 928). Whether the hardware decodes all 14
bits, wraps at a smaller size, or reaches other register blocks with the high bits is not
pinned, and matters for a program validator.
