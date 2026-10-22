<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Dual issue

*Measured on the hardware (2026-10-22).* **A dual-issue word runs two operations that read
before either writes** **[HW]** (probes `dual-issue`, `dual-issue-predicates`; 144 pairings
explored; `design/sgx545_hw_spec.md` §14):

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

**Undecided:** what `ima32` computes as a secondary (no product or sum we tried fits), and
the secondaries' half-float (`enc[54]`) forms.
