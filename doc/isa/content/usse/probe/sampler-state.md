<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Sampler state

*Measured on the hardware (2026-10-22).* Round 4 varied the four state words an `smp` program
builds (T0-T3, the `doutt` words; `design/sgx545_transfer.md` §4.4.3) on a texture in the
scratch buffer whose dwords count up, so every result names the texels it came from **[HW]**
(table `sampler.toml`; `design/sgx545_hw_spec.md` §14):

- **Filtering (T0 bits 13:12, probes `smp-bilinear`, `smp-filter-modes`).** 1 and 2 are
  bilinear, 0 and 3 point. **Bilinear weights are truncated to 1/64 and each 8-bit channel is
  rounded to nearest, halves up**, before any conversion: between texels 255 and 15 at
  `v + k/256` the f32 result steps every four `k`. The minification field does not act while
  magnifying (every case magnifies: the LOD forms never left level 0).
- **Addressing (T0 bits 8:6 U, 5:3 V; `smp-address`).** A stride texture (T1 type 6, our copy
  sources) clamps to the edge whatever the modes say. On a twiddled texture (type 0) **0
  repeats, 1 mirrors (period twice the size), 2 clamps to the edge, 3 mirrors once** (the
  absolute coordinate, then clamped); bits 2:0 do not act on `u`. Modes 4-7 read other texels
  (`smp-address-border`, recorded).
- **Twiddled layout (`smp-twiddled`).** Texel `(x, y)` lies at the Morton index with **`y` in
  bit 0, `x` in bit 1**, and so on; a non-square texture puts the longer side's extra bits
  above (16 × 16, 8 × 8, 16 × 4 and 4 × 16 tried).
- **Formats (T1 bits 28:24, no conversion; `smp-formats`).** 0 and 1 return the byte (`s8`
  not extended), 7-11 the half, 12, 15-18, 22 and 31 the dword; **2, 3, 4 and 5
  (`4444`, `8332`, `1555`, `565`) expand to B8G8R8A8 by repeating each field's bits**, alpha
  `0xff` when the format has none; 13 swaps red and blue (R8G8B8A8), 19 clears the sign bit,
  21 the top byte, 30 sets it. 6, 14, 20 and 23-29 (signed, 10-bit, compressed or YUV
  layouts) are recorded (`smp-formats-other`); 14 writes two registers.
- **Conversion (T3 bits 17:16, normalization bit 15; `smp-convert`).** 1 gives **c10**: the
  B, G, R channels in the 10-bit fields of the first register (the 8-bit value unscaled,
  1.0 = 255 as `.c10` operands read it) and A in the second; 2 gives **f16** pairs R, G then
  B, A (alpha 1.0 when the format has none); 3 gives **f32** B, G, R, A, one register each.
  Values are the integers (0-255, 0-65535) unless bit 15 is set, which divides by 255 (by
  65535 for `u16`). Data size field (T3 bits 26:23) changes nothing seen. **Without alpha
  (565) c10 and f32 leave its register unwritten** (c10 one register, f32 three) and f16 gives
  1.0 even unnormalized. **The other formats convert their channels** (cases 24-47): format 0
  is one u8 channel, 7 two (the low byte first), 9 one u16; c10 puts channel k in the k-th
  10-bit field of one register (a u16 as its top byte: `0xd000` gives `0xd0`), f16 packs the
  channels in pairs (low half first: format 7's `0x0451` gives `h(81) | h(4) << 16`), f32 gives
  one register per channel.
- **PCF (`smp-pcf`).** `smp2d.pcf` compares **texel OP reference**, the reference being the
  f32 in the register after the coordinates (`.pcff16`: an f16, low half), by T3 bits 31:29
  (0 never, 1 `<`, 2 `==`, 3 `<=`, 4 `>`, 5 `>=`, 6 `!=`, 7 always), and returns 1.0 for an f32
  texture (format 18) or `0xffff` for `u16` (format 9, compared as texel / 65535) when it
  holds, else 0; with bilinear filtering the four results are averaged (`0x8000` for one of two
  texels at half weight, the only split measured). A colour format ignores `.pcf`.
- **Other state bits (exploration `x-smp2`).** Setting T0 bits 27, 30 or 31, or T3 bits outside
  the fields above (0-7, 10, 14, 18, 19, 27, 28), reads other texels or values: they act, how
  is not known.

*Simulator check, 2026-10-23.* The simulator's texture unit
(`crates/sgx-sim`, `tex.rs`) follows this page and *Texture sampling*; it reproduces every
recorded sampling vector it models and stops on the rest: the LOD values, the bits above, the
recorded formats and modes, tiled textures, and filtered `.pcf` at other weights.

**Undecided:** tiled textures (T1 type 4), mipmaps and the LOD values (`smp-lod`: neither `replace`,
`bias` nor `grad` left level 0 with T0 bits 20:17 at 0, 12 or 15), the border modes, the recorded
formats, `.sinf`/ `.rsd`, `smp3d`, and state from `sa` or `c`.
