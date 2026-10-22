<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Internal registers

*Measured on the hardware (2026-10-22).* **`i0`-`i2` are ordinary 32-bit registers to `mov`,
and they keep their values from one task to the next**, also from one client to another
**[HW]**: a fill writes `i0`-`i2`, and the next probe, in another process, reads the same values
back (probe `internal-registers`; `fddp` leaves its result there too). **`i3` reads as `i0`, and a
write to `i3` is dropped** **[HW]**. They are not cleared by a task start, as the temporaries are
not (`design/sgx545_render_uapi.md` §7).

`efo` reads `i0`/`i1` and writes them after its own reads. After an `efo.gpiext`, one pixel in
four reads values another instance wrote (*`efo`*), the first sign that these registers are
not simply private to an instance.

**Undecided:** whether `i0`-`i2` are per instance or shared (our probes saw one value in every
pixel), and how the carries use them.
