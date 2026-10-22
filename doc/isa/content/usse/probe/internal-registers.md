<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Internal registers

*Measured on the hardware (2026-10-22).* **`i0`-`i2` are ordinary 32-bit registers to `mov`,
one set per instance** **[HW]**: a fill that gives every pixel its own `i0`-`i2` (round 3, from
the pixel's coordinates) reads each pixel's own values back (probe
`internal-registers-per-pixel`). **`i3` reads `i0` of the next instance of the pixel's 2x2
quad**, in the order top-left, top-right, bottom-left, bottom-right and round again, and **a
write to `i3` is dropped** **[HW]**. Round 1 saw `i3` read "`i0`" only because every pixel held
the same value.

**They keep their values from one task to the next**, also from one client to another **[HW]**:
a fill writes `i0`-`i2`, and the next probe, in another process, reads them back (probe
`internal-registers`; `fddp` leaves its result there too). The next task's pixels see the
values of other pixels: a whole quad of instances moves to another quad of pixels, its four
positions kept, so `i3` stays within the quad (probe `internal-registers-across-tasks`,
recorded). They are not cleared by a task start, as the temporaries are not
(`design/sgx545_render_uapi.md` §7).

`efo` reads `i0`/`i1` and writes them after its own reads; **`efo.gpiext`** reaches the other
instances of the quad (*`efo`*).

*Round 4 (2026-10-22).* **Bank 7 numbers past `i3` name the other instances of the quad**: number
`n`, taken modulo 16 (`i31`, `i63` and `i127` read as `i15`), is `4m + j`; `j` = 0-2 reads `ij` of
the instance `m` places on (TL, TR, BL, BR, round again), and `j` = 3 reads `i2` of that instance,
but `i0` of the next one for `i3` itself. **Writes to `i4`-`i7` are dropped**: no instance's
`i0`-`i2` change **[HW]** (probe `internal-registers-bank7`, every pixel). The carries of
`imae`/`ima32` go through `iN` (*Integer multiply-add modes*).
