<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Dual issue

*Measured on the hardware (2026-10-22).* **A dual-issue word runs two operations that read
before either writes** **[HW]** (probes `dual-issue`, `dual-issue-predicates`; 144 pairings
explored by `x-dual`; `design/sgx545_hw_spec.md` §14):

- the primary computes on its operands as alone (`fssq d, a, b` is `a·a + b·b`, each product
  rounded), the secondary on `i0`-`i2` and, for a source naming a register, **the primary's
  source in that position, without its `.neg`**;
- both read the values from before the word: a primary reading `i0` gets the old `i0` while the
  secondary writes it, and a secondary reading `i0` the old one while the primary writes it;
  when both write one register, **the secondary's value stays**;
- `.repeat2` repeats **both**: the primary's operands advance, the secondary runs twice on the
  internal registers (`fadd i0, i0, i1` adds `i1` twice);
- the two-bit predicate (always, `p0`, `p1`, `!p0`) **covers both operations**.

**Pairs that co-issue** cleanly: with a `mov` primary every secondary but `ima32`; with `fmad`
or `ima32` the secondaries `fmad`, `fadd`, `fsub`, `fmul`, `fsubflr`, the single-source
functions and `mov` (`ima32` also `fmad16`, `fmsa`); with `fadm`, `fmsa` or `fssq` the
add-type and single-source secondaries and `mov`; with `fminmax` only `fmul`, the
single-source functions and `mov`. **Other pairs share a unit and give mixed results**
(probe `dual-issue-conflicts`, recorded): the secondary gets the primary's intermediate value
(`fadm` + `fmul` gives the primary's second product, `fminmax` + `fadd` its comparison
difference, `fmad` + `fmsa` the primary's result) or the primary is lost (`fmad` + `fmad16`
writes 0). An `fddp` primary writes its second product to the secondary's destination and
the secondary nothing.

*Round 4 (2026-10-22).* **`ima32` as a secondary computes two 16-bit lanes**, `a × b + c` in
each, wrapping, as `ima16` with `u16` sources; next to an `fmad`, `fadm`, `fmsa` or `fssq`
primary its `b` is **the primary's source 1** whatever it names, next to a `mov` the register it
names **[HW]** (probe `dual-issue-ima32`, integer and float inputs). **The pairs that share a
unit are deterministic** (probe `dual-issue-shared`, new values; round 3's cases fit too):

- a multiplying secondary (`fmul`, `fmad`) of a two-product primary (`fadm`, `fmsa`, `fssq`)
  gets the primary's second product, plus its own `c` for `fmad`;
- an `fmsa` secondary of `fmad`, `fadm` or `fmsa` writes the primary's result, as does an
  `ima32` secondary of an `ima32` primary;
- the add-type and `fmad` secondaries of `fminmax` get `max(a, b) - c` (the primary's last
  comparison difference);
- `fmad` + `fmaxmin` computes `a·b - c` for the primary and `b - c` for the secondary;
- `fmad` + `fmad16`: the secondary as alone, the primary 0.

**Undecided:** the secondaries' half-float (`enc[54]`) forms, `fminmax` + `ima32`.
