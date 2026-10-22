<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Conditional and masked moves

*Measured on the hardware (2026-10-22).* **[HW]** throughout (probes `movc-*`, `movmsk-*`;
`design/sgx545_hw_spec.md` §14):

- **`movc.<type>.<test> d, s0, s1, s2`** is a select per lane: each lane of `d` is `s1`'s if the
  test holds for that lane of `s0`, else `s2`'s. Lanes: `.i32` one, `.i16` two, `.i8` four,
  `.i10` three 10-bit lanes at bits 0, 10, 20 (bits 30-31 of `d` become 0). Tests: `.tz` zero,
  `.tnz` not zero, `.tn` the lane's sign bit set, `.tp|z` clear. **`.flt`** tests the float:
  denormals and `-0` are zero, `.tn` is `< 0`, `.tp|z` `>= 0`, and a NaN fails all but `.tnz`.
- **`movmsk.<fmt> d, s1, s2.c`** moves `s1` into `d` when `s2` passes, else leaves `d` alone:
  `.mask` bit 0 of byte `c`, `.u8` bit 7 of byte `c`, `.flt` `s2 >= 0.5` (a NaN fails, `c`
  ignored), `.flt16` half `c >= 0.5` (`.0` bits 0-15, `.1` bits 8-23, `.2` and `.3` bits 16-31).
- **`.smsk`** adds source 0: the move also needs **bit 0 of `s0`** for `.u8`, `.flt` and
  `.flt16`, while `.mask.smsk` ignores `s0` (16 values of `s0` against a passing and a failing
  `s2` per format; probe `movmsk-smsk2`, 2026-10-22).
