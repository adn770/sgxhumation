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
- **Lanes [HW].** Source 1 and 2 fill the destination's lanes in turn (bytes 1, 2, 1, 2;
  halves 1, 2; `c10` lanes 1, 2, 1); the byte mask enables byte N with bit N and decides which
  source each enabled byte takes; component selects pick a byte, a half or a `c10` lane
  (*Pack and unpack lanes*).
- **Conversions [HW].** Unscaled conversions round in the instruction's mode and saturate;
  `.scale` maps 1.0 to 255, 127, 65535 or 32767 (`o8`: `x × 127 + 128`), symmetric for the
  signed formats, always to nearest even; `c10` is `x × 255` clamped to ±2.0; `pckf16f32`
  truncates unless `.renable`, which rounds to an integer; unpacks invert these exactly
  (*Pack and unpack conversions*).
- **`statomic`** (a store-atomic?) is **[open]**: one run wrote `0x78007800` to its
  destination whatever its inputs and rounding mode.
- **In the firmware:** not used.

<!-- @example usse pckf16f32 r0, r1.0, r1.1, nearest -->

<!-- @example usse unpckf32u8 r0, r1.0, r1.1, nearest -->

<!-- @group usse class=8 -->
