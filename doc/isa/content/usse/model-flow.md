<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### Program counter, calls and the end of a program

- **The PC counts instructions** from the code base the task was started from **[HW]**. The
  PDS starts a task with a `doutu` word naming a code base (`USE_CODE_BASE_n`, n in
  `U0[3:0]`) and an instruction index; the SGX545 has 18 PC bits, which is also the width of
  the branch target fields **[CAP/RE]**.
- **`ba #n`** jumps to instruction `n` of the code base (absolute) **[RE/HW]**. **`br`** takes
  a signed 18-bit offset; the instruction it is counted from is **[open]**, so the assembler
  accepts only a number for `br`, not a label (`tools/usse-asm/README.md`).
- **Calls.** `ba.savelink` (and `br.savelink`) also write the return address to `pclink`;
  `lapc` jumps to `pclink`. `pclink` holds the **index of the instruction after the call**
  **[RE]**: the firmware builds return addresses as instruction indices (it loads an index
  into a temporary and moves it to `pclink` before a `lapc`), and the kernel CCB dispatch
  jumps to a handler the same way (`mov pclink, r4; lapc`, `design/sgx545_hw_spec.md`
  §13.10). There is one link register, so a routine that calls another saves `pclink` in a
  temporary first (`mov rN, pclink` ... `mov pclink, rN`) **[RE]**.
- **Branch flags [open].** `.nosched`, `.synce`/`.syncent` and `.mon` decode on `ba`, `br`
  and `lapc`; their effect is not established, and the firmware uses none of them.
- **`bexception` [open].** Decodes; never used.
- **The end of a program [HW].** A program ends at an instruction carrying `.end`, which
  still executes. There is no return to the PDS: every one of our programs ends this way
  (`mov.skipinv.end`, `nop.end`, `emitpix2.end.freep`, `ptoff.end`, ...). The firmware's tasks
  end in a shared routine with a `nop.end` or `mov.end` (eight `.end` in all).

**In the firmware [RE].** 954 `ba` (232 of them `.savelink` calls), 12 `br`, 74 `lapc`, 205
moves into `pclink` and 193 out of it.
