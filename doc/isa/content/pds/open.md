<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Open questions

1. **The ports** `doutc`, `douts`, `slc`, `mr`, `tim` and `ir0` as destinations, and
   `movsa` on ports other than `doutd`.
2. **`if0`/`if1`**, and how `aluz`/`alun` are set.
3. **`load`/`store`**: the address base, the timing, and their relation to the fences.
4. **`mov128` and `swap`**, `mr01`/`mr23`, `tim`, `ir2`.
5. **ALU details**: `adc`/`sbc` carries, `alum`, negative shift amounts, `mul` signedness.
6. **`call`/`rtn`**: a return stack, and its depth.
7. **The `doutu` word** on this build: whether `EXE` really stops at bit 19 (so that `CBASE`
   exists) or the 18 PC bits reach bit 21; our programs keep code low enough for both
   readings to agree (`design/sgx545_transfer.md` §4.2.4).

8. **Semantics stated only by name**: `xor`, `nor`, `nand`, `add`, `sub`, `tstn`, `abs`,
   `call`/`rtn` and the fences, beyond what our programs use.
