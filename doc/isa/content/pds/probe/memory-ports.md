<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `load`, `store`, the fences and the other ports

*Not probed yet.* In the exploration table `tools/usse-asm/progs/probes/explore/pds1/pds-c.toml`
(recorded, not run by default, after the others since a wrong guess may stop the EDM):
`pds-x-load-store` loads from four addresses and stores then loads with and without `rwfence`;
`pds-x-ports` writes `mr`, `tim`, `ir0`, `slc`, `doutc` and `douts` with `movs` (reading
`mr01`/`mr23`, `tim` and `ir0` back) and runs `movsa douta`. The `doutu` `EXE` width (*Open
questions* 7) needs USE code above 512 KiB of its heap, which the driver cannot place yet.
