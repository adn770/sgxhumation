<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Machine model

### Data registers

The data segment is loaded into two banks, `ds0` and `ds1`, of 64 dwords each **[RE]**. It is
stored **in rows of four dwords, two per bank**: row `r` holds `ds0[2r]`, `ds0[2r+1]`,
`ds1[2r]`, `ds1[2r+1]` **[CAP/HW]**. So data dwords 0-3 are `ds0[0]`, `ds0[1]`, `ds1[0]`,
`ds1[1]`, dwords 4-7 are `ds0[2]`, `ds0[3]`, `ds1[2]`, `ds1[3]`, and so on; every one of our
PDS programs documents its data segment this way, and the hardware reads them so.

The registers past the data segment are scratch: our programs compute in `ds0[48]`, `ds1[48]`
and `ds1[50]` **[HW]**, and `load`/`store` move memory to and from `ds0[48]`/`ds1[48]` and up
**[RE]**. Whether the banks are cleared between programs is **[open]**.

### Input and special registers

- `ir0`, input word 0: the vertex index of a vertex program **[HW]**, the tile position of
  the pixel-event program **[infer]**, the request mask the microkernel passes to its
  PDS #14 **[RE]**.
- `ir1`, input word 1: the event bits of the pixel-event program **[CAP/infer]**.
- `ir2`, input word 2 **[open]**.
- `tim`: a timer? **[open]** (readable, and a `movs` destination).
- `pc`: the program counter, readable by `mov16`/`mov32` **[RE]**.
- `mr01`, `mr23`: 64-bit sources of `mov64`; `mr` is also a `movs` destination **[open]**.

Some operand fields reach only a prefix of the specials (`ir0`, `ir1`; or `ir0`, `ir1`, `tim`),
and `mov16`/`mov32` number them with `pc` inserted **[RE]**: the field descriptions below say
which.

### Predicates

The predicate field `w[26:24]` reads `p0`, `p1`, `p2`, `if0`, `if1`, `aluz`, `alun` or nothing
(7) **[RE]**:

- `p0`-`p2` are written by `tstz` (`pN = (src == 0)`) and `tstn` (`pN = (src < 0)`
  **[infer]**). `tstz` setting its predicate on zero is **[CAP/HW]**: our pixel-event program,
  bit-identical to the closed driver's, skips each case with `tstz p0, x; (p0) bra` and the
  right task runs for each event.
- `if0` and `if1` are **[open]**. Our vertex program starts its shader under `(if0)`, as the
  closed driver's vertex programs do **[CAP]**; a reading such as "first instance of a
  batch" fits, nothing more.
- `aluz` and `alun` presumably test the zero and negative flags of the last ALU result
  **[infer]**.

`load`, `store`, the fences and `nop` use `w[26:24]` for other purposes or not at all, and
carry no predicate.
