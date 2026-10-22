<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Scheduling flags

Most groups carry a handful of one-bit flags printed after the mnemonic. Their bit positions
are in each group's field list; what is known of their effect:

- **`.end`** ends the program after this instruction **[HW]** (*Program counter, calls and
  the end of a program*).
- **`.skipinv`** executes the instruction for every instance, invalid ones included
  **[infer]**: the name, and the fact that the closed compiler's pixel and state programs
  (and ours, which follow them) set it on the instructions that must run regardless of
  coverage. Whether leaving it clear really skips instances the hardware marks invalid is
  **[open]**; the firmware, whose tasks have a single instance, uses it only twice.
- **`.nosched`** is a scheduling hint, probably "do not switch to another task after this
  instruction" **[infer]**; it changes nothing a fill's pixels see **[HW]** (*`.tpres`,
  `.partial`, `.nosched`, `.syncs`*). Our programs carry it only where the captured programs do
  (`nop.nosched` before the DMA wait); the firmware uses it once, in the same idiom.
- **`.syncs`** decodes in most groups; **in a fill it hangs the task** **[HW]** (on `mov` and
  `fmad`), so it waits for something a fill never gives; what is **[open]**. Unused by the
  firmware and by us.
- **`.partial`** (the bitwise group) changes nothing a fill sees **[HW]**; **`.onceonly`** and
  `.partial` on the test group are **[open]**.
- **`.freep`** on the emits frees the task's resources once the emit is done **[infer]**:
  every final emit of our pixel and state programs carries it, as the captured ones do.
- **`.tog`** and **`.trigger`** (`nop`), **`.tpres`** (single-source floats), **`.gpiext`**
  (`efo`), **`.twopart`**/**`.threepart`**/**`.cut`** (the vertex and MTE emits) and
  **`.tasks`**/**`.taske`** (`emitpds`) are discussed with their groups; most are **[open]**.

The radare2 emulation of the firmware assumes that `.skipinv`, `.syncs`, `.nosched` and `.end`
do not change what one instance computes, and ignores them (`tools/usse-dis/r2/README.md`);
nothing contradicts that so far.
