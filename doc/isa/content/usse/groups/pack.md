<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Pack and unpack: `pck*`, `unpck*`, `statomic`

Class 8 converts between data formats **[RE]**. `enc[43:38]` names the destination and source
formats together: `pck<dst><src>` packs to a narrower format, `unpck<dst><src>` widens, with
`u8`, `s8`, `o8` (offset 8-bit), `u16`, `s16`, `f16`, `f32` and `c10` (value 8 is `statomic`
instead). Each instruction takes two sources, each with a component select (`.0`-`.3`), a
destination byte mask (`.bytemaskXXXX`, all four bytes print nothing), a rounding mode
(`nearest`, `down`, `up`, `zero`) and `.scale` (`.renable` for the float-to-float ones).

- **What is known [CAP].** The closed GLES2 fragment program of our capture converts its four
  f16 colour components to 8-bit with `pcku8f16` into an attribute register
  (`design/sgx545_3d.md` §9.3), and its clear vertex program unpacks the four bytes of a
  vertex colour to floats. So the group does carry colour conversion, as the names say.
- **Not probed yet:** which lanes each source and component select feeds, the byte order of
  the destination mask, the rounding modes and `.scale` are **[open]** (*Pack and unpack
  lanes*).
- **`statomic`** (a store-atomic?) is **[open]**.
- **In the firmware:** not used.

<!-- @example usse pckf16f32 r0, r1.0, r1.1, nearest -->

<!-- @example usse unpckf32u8 r0, r1.0, r1.1, nearest -->

<!-- @group usse class=8 -->
