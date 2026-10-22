<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Write masks, byte masks and repeat counts

- **Destination write mask.** `enc[47:44]` (bits w, z, y, x from high to low) prints after the
  destination: `.x` alone prints nothing, `0` prints a lone `.`, the rest name the enabled
  components (`r1.xz`). It shares its bits with the repeat count, chosen by `enc[53]` **[RE]**.
  What a component is for a 32-bit register (a byte? a lane of a repeat?) is **[open]**, not
  probed yet (*Write masks*).

<!-- @example usse mov r1.xz, r2 -->

- **Byte masks.** The pack/unpack group writes `.bytemaskXXXX` (`enc[37:34]`, printed only
  when not all four bytes are enabled) and `sop2wm` always prints one (`.bytemask0000`-
  `.bytemask1111`) **[RE]**. A destination byte enable is the natural reading **[infer]**; the
  bit order is **[open]**, not probed yet (*Pack and unpack lanes*).
- **Repeat counts** (`.repeatN`) are described in *Repeats and the MOE state*.
