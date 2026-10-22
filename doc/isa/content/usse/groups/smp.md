<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Texture sampling: `smp1d`, `smp2d`, `smp3d`

Class 28 **[RE]**: the dimension (`1d`, `2d`, `3d`, `enc[43:42]`) and a LOD mode (none,
`bias`, `replace`, `grad`, `enc[41:40]`; `3d` with a LOD mode does not decode); flags
`.skipinv`, `.syncs`, `.nosched`, the PCF mode (`.pcf`, `.pcff16`) and the SRD mode (`.sinf`,
`.rsd`); a destination `r` or `pa` with a two-bit write mask or a repeat count; the
coordinates as a source-0 operand with a format (`.flt16`, `.c10`); the texture state as a
source-1 operand; the LOD value as source 2 when a LOD mode is set; a data fence `drc0`-`drc3`.

- **Semantics [HW].** Source 1 holds the four texture state words (`s` to `s + 3`, the `doutt`
  words of our copies), source 0 the normalized coordinates (`c`, `c + 1`; `.flt16`: two halves
  of one register); the texel arrives in the destination once `drcN` is waited for. Point
  sampling clamps to the edge (*Texture sampling*). Filtering, the LOD values, `.pcf` and the
  SRD modes are **[open]**. Our copy path does not use it: the PDS issues the texture fetch
  (`doutt`) and the result arrives in `pa0` before the task starts
  (`design/sgx545_transfer.md` §4.4.3) **[HW]**.
- **In the firmware:** not used.

<!-- @example usse smp2d r0, r4, c0, drc0 -->

<!-- @group usse class=28 -->
