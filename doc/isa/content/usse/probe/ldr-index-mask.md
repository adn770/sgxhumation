<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### The 14-bit `ldr`/`str` index

*Measured on the hardware (2026-10-22).* The immediate register index of `ldr`/`str` has 14 bits (a
reach of 64 KiB of register space) **[RE]**, and **`ldr rD, rI` masks a register index to 14 bits**
**[HW]**: `0x4007`, `0x8007`, `0x10007` and `0x40007` read `CORE_ID` like 7 (probe `ldr-index-mask`;
`design/sgx545_hw_spec.md` §14). A validator can reduce an index modulo `0x4000`.

**Undecided:** `str` with a register index (a client `str` was not seen to land; a probe from the
EDM's own task, on a register that is safe to write, would settle it).
