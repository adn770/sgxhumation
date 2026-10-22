<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Register offsets of loads and stores

*Measured on the hardware (2026-10-22).* An immediate offset counts elements of the access size
**[HW/RE]**. A **register** offset `o` (`ldad d, [b,+o]`, `stad [b,+o], s`) is
**`(o >> 16) × (o & 0xffff)` bytes**, both halves unsigned, added modulo 2^32 **[HW]**: `0x00040003`
is +12, `0x00010001` +1, `0x01000001` +256, `0x00000005` +0, and `0xffffffff` faults at the base
minus `0x1ffff`. `[b,-o]` subtracts; `ldad.fetchN [b,+o]` starts one element after, as the immediate
forms do. A load from an address that is not a multiple of 4 reads those bytes; a store there lands
nowhere and does not fault (probes `ldad-reg-off`, `ldad-reg-off-sign`, `ldad-reg-off-neg`,
`ldad-reg-off-fetch`, `stad-reg-off`, `stad-reg-off-unaligned`; `design/sgx545_hw_spec.md` §14). The
firmware's register offsets OR `0x00040000` (a stride of 4) into a dword index.

**Undecided:** the word and byte forms (`ldaw`/`staw`, ...), the pre- and post-increment forms with
a register offset (what is written back), and the negative fetch `[b,-o]`; the same probes on those
forms would settle them.
