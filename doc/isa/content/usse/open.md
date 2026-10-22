<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Open questions

Beyond the undecided parts of the hardware probe results, by how much they would matter to an
open driver:

1. **The behaviour of the groups no program of ours has run**: the derivatives, sampling,
   visibility and video groups and the dual issue. Encodings are complete; behaviour needs
   probes or a working compiler's output to compare against. Of the probed groups, `fdpc`'s
   clip-plane result, the colour repeats, `fpma.sat` and the `c10` to 16-bit unpacks remain.
2. **The MOE state as a whole**: `smoa`, `smbo`, `smr`, `imo`, `setfc` and what `moest`
   returns, beyond the `smlsi` increments the probes pinned.
3. **`.skipinv`, `.syncs`, `.nosched`** and the other scheduling flags: what changes when they
   are set or clear (performance only, or correctness).
4. **`br`'s offset base**, so that the assembler can take labels for it.
5. **The constant and global banks**: what `c` and `g` hold and who writes them.
6. **`Pn`**, the eighth predicate value.
7. **Emits**: `#incp`, `.cut`, `.tasks`/`.taske` and the exact role of `.twopart`/
   `.threepart`; the PBE sideband bits beyond the pack mode and `TILERELATIVE`.
8. **`idf` modes** other than `bif`, `lock`/`release`, `setm`, and the cache-control operands.
9. **Instruction timing and hazards**: how many instructions a result takes to be readable,
   whether the hardware interlocks, and what `.nosched` has to do with it.
10. **Bank 7** register numbers beyond `i3`, and the destination bank 6 value 0 (`i.`); whether
    `i0`-`i2` are per instance.
