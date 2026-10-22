<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Cache control `ci`, `cf`, `cfi`

*Measured on the hardware (2026-10-22).* **A load after `ci`, `cf` or `cfi`, on any of the
three level selections, with `.global` or `.dm_nomatch`, returns the pixel's own store**
**[HW]**: none of them drops a store or makes a later load read stale memory, and none
writes a register (probe `cache-control`, every pixel on its own dword).

**Undecided:** what they do to other units' view of memory (the texture cache, the host),
which our probes cannot see.
