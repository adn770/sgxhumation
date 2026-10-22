<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Texture sampling `smp`

*Measured on the hardware (2026-10-22).* **`smp2d d, c, s, drcN` samples the texture whose four
state words are in `s`, `s + 1`, `s + 2`, `s + 3` at the normalized coordinates in `c` and
`c + 1`, and writes the texel to `d` when the fence `drcN` is waited for** **[HW]** (probe
`smp-point`; `design/sgx545_hw_spec.md` §14). The state words are those the PDS gives the
texture unit for our copies (`doutt`, `design/sgx545_transfer.md` §4.4.3), built in temporaries
by the program itself:

- point sampling picks texel `floor(u × width)`, `floor(v × height)`; coordinates outside
  `[0, 1)` **clamp to the edge**; the texel arrives as stored (the raw B8G8R8A8 dword);
- `smp1d` reads `u` alone, row 0; `.flt16` takes both coordinates as halves of one register
  (`u` low); `smp2dbias`, `smp2dreplace`, `smp2dgrad` with a LOD of 0 sample the same texel;
- with `.repeat2` or a write mask the job faults at an address made of the state's fourth word,
  as if the next iteration read the state one register on **[infer]** (probe `smp-repeat-mask`).

**Undecided:** filtering, the other formats and sampler fields of the state words, mipmaps
and what the LOD values do, `.pcf`, `.sinf`/`.rsd`, `.c10` coordinates, `smp3d`, and state
read from `sa` or `c` (only temporaries were used).
