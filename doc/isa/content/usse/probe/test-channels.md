<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Test channels

*Measured on the hardware (2026-10-22).* **A test gives one result per channel, and the channel
field picks one or combines them** **[HW]** (probe `test-channels`, 360 cases; `design/
sgx545_hw_spec.md` §14):

- an 8-bit operation (`iadd8`, ...) has four channels, **bytes 0-3**, each tested on its own
  (`n` is the byte's bit 7);
- a 16-bit operation has **the low half as channel 0 and the high half as channel 2**;
- a 32-bit or float operation has its value as channel 0;
- **a channel with no lane tests as a positive non-zero value**: `z` and `n` false, `nz` and `p`
  true. That is why round 1 saw `chan1`-`chan3` clear the predicate on a zero test;
- `chan0`-`chan3` take one channel, **`andall`/`orall` all four** (and/or), **`and02`/`or02`
  channels 0 and 2**.

**Undecided:** the float tests beyond `fadd` and the mask forms of class 15
(`.bmsk`/`.wmsk`/`.dmsk`).
