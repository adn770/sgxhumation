<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Float min/max: `fminmax`, `fmaxmin`

Class 3 **[RE]**: the class-0 layout with three sources, the operation in `enc[41]` (`enc[42]`
does not decode).

- **`fminmax d, a, b, c` = `min(max(a, b), c)`** and **`fmaxmin d, a, b, c` = `max(min(a, c),
  b)`** **[HW]**: both clamp `a` to `[b, c]`, and differ when `b > c` (`c`, `b`). min and max
  are IEEE minNum/maxNum (a NaN loses, `-0 < +0`), denormals flushed **[HW]** (*Float min/max*).
- **`.incsXYZ`** as for `fmad` **[HW]**.
- **In the firmware:** not used.

<!-- @example usse fminmax r0, r1, r2, r3 -->

<!-- @group usse class=3 -->
