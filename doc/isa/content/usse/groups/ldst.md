<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Loads and stores: `ld*`, `st*`, `eldd`, `eldq`

Classes 29 (loads) and 30 (stores) **[RE]**. The mnemonic is `ld`/`st` plus an address mode and
a data type: `ldad` = load, absolute (`a`), dword (`d`); `l` and `t` are the other address
modes (local and tiled **[infer]**, both **[open]**), `w` and `b` 16- and 8-bit data. The base
is a source-0 operand, the offset a source-1 operand (an immediate or a register), and
`enc[41:40]` picks plain, pre-increment or post-increment addressing. Flags: `.skipinv`,
`.syncs`, `.nosched` (stores), the cache mode (`.bpcache`, `.fcfill`, `.bypassl1`), `.noread`
and `.rangeenable` (stores), and a count used as `.fetchN` (MOE-expand bit `enc[53]` clear)
or `.repeatN` (set). A load can name a range operand (source 2) and always names a data fence
`drc0`/`drc1`.

**Addressing [HW/RE].** Offsets count elements of the data size (4 bytes for `d`, 2 for `w`):

| form | elements accessed | base writeback |
|------|-------------------|----------------|
| `[b,+#k]` / `[b,-#k]` | `b+k` / `b-k` | none |
| `[b,++#k]` (pre) | `b+k` | `b += k` |
| `[b,#k++]` (post) | `b` | `b += k` |
| `.fetchN [b,+#k]` | **`b+k+1` ... `b+k+N`** | none |
| `.fetchN [b,#k++]` | `b+k` ... `b+k+N-1` | `b += N` |

A fetch with a base-plus-offset address starts **one element after** the printed offset:
`ldad.fetch4 r3, [r2,+#1]` loads dwords 2-5 into `r3`-`r6`. The plain forms and the
post-increment fetch were checked on the laptop (the kernel CCB fetch and the GETMISCINFO
reply, `design/sgx545_hw_spec.md` §13.10a) **[HW]**; the writeback and the base-plus-offset
fetch follow from the firmware's loops and structure offsets **[infer]**. A **register** offset
`o` (`[b,+rN]`) is `(o >> 16) × (o & 0xffff)` bytes, and a fetch starts one element after it
too **[HW]** (*Register offsets of loads and stores*); the firmware has four such stores.

- **Cache modes [open].** The firmware uses `.bpcache` on almost every access; `.fcfill` once.
  The SGX540 swaps the two cache-mode bits; `usse-dis --core 540` follows it.
- **`eldd`/`eldq`** (class 29 with `enc[54]` set) take a base, an index and an 11-bit offset
  and a source-2 operand, and load a dword or a quad **[RE]**; their semantics are **[open]**
  and the firmware does not use them.

**In the firmware [RE].** 598 loads and 349 stores, nearly all `ldad`/`stad` with `.bpcache`
on its control blocks through `sa0`-`sa2` or computed bases; 120 fetches; `ldaw` twice. Its
807 `wdf` (for 598 loads and 196 `ldr`) show the pattern: issue the loads, then wait on the
counter before the first use.

<!-- @prog use_vtx.s match='ldad' -->

<!-- @example usse ldad.fetch4 r3, [r2,+#1], drc0 -->

<!-- @example usse stad [r2,+#7], r9 -->

<!-- @group usse class=29,30 -->
