<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Machine model

A USSE program runs as a **task** over a set of **instances** (pixels of a batch, vertices,
or the single instance of a firmware or state task) **[infer]**. Each instruction is executed
for the instances of the task; the predicates and `.skipinv` decide which. The model below is
what our programs, the firmware and the captures show; the parts nobody has measured are
marked.

### Register banks

| bank | names | what it holds |
|------|-------|---------------|
| temporaries | `r0`-`r123` (`r0`-`r127` as load destinations) | per-task scratch |
| primary attributes | `pa0`-`pa127` | per-instance inputs written by the PDS |
| secondary attributes | `sa0`-`sa127` | per-task (shared) inputs written by the PDS |
| outputs | `o0`-`o127` | results handed to the next unit by the emits |
| constants, globals | `c0`-`c63`, `g0`-`g63` | not established (one bank code; 64 up is `g`) |
| internal | `i0`-`i3` | the accumulators of `efo`, `fddp`, the dual issue and the carries |
| index | `i.l`, `i.h` (`il`, `ih` in operands) | the bases of indexed operands |
| link | `pclink` | the return address of a call |

- **Temporaries [HW].** The number of temporaries a task gets is set when it is started: the
  PDS's `doutu` word U0 carries it in fours (`TRC`, `design/sgx545_transfer.md` §4.2.4), and
  `phas` names it for the next phase. Our end-of-tile program asks for four and uses `r0`-`r3`.
  Temporaries are **not cleared** between tasks: a client shader that reads one before
  writing it sees what an earlier task left there, firmware data included
  (`design/sgx545_render_uapi.md` §7 Q4).
- **Primary attributes [HW].** The PDS fills them per task: `doutd` DMAs memory into them
  (the vertex fetch of our triangle lands in `pa0`-`pa3`), `douti` has the iterator write
  them and `doutt` returns a texture fetch into them (our copy shader reads its texel from
  `pa0`). Programs may also write them: our MTE state tasks load into `pa0` again.
- **Secondary attributes [CAP/RE].** Shared by the instances of a task and written by a
  secondary PDS program. The firmware keeps its base pointers there (`sa0`-`sa2`, DMAed from
  the TA/3D control block at boot) and some of its state (`sa8`, `sa9`, `sa19`), and it does
  write them (`design/sgx545_hw_spec.md` §13.9, `design/sgx545_3d.md` §12.1).
- **Outputs [HW].** Pixel programs leave the colour in `o0` for the end-of-tile emit; the
  vertex shader of our triangle writes `o0`-`o3`, and the state tasks copy whole state blocks
  through `o0`-`o17` before emitting them.
- **Constants and globals [open].** What `c` and `g` hold, and whether `g` is writable, is not
  established. The firmware reads two globals once each (as a source of an `xor` and of a
  `shl`) and otherwise names `c0` only as the dummy destination of a test that does not write
  its result (`!c0`, see the test group) **[RE]**.
- **Internal registers [RE].** `i0`-`i3` print where a temporary number reaches past the
  temporaries (bank 0, numbers 124-127; in the half-float and c10 readings, earlier). They
  are the accumulators of the dual-issue and `efo` groups and the carry registers of
  `imae`/`ima32`. **[HW]** `i0`-`i2` are per instance, hold what `mov` or `fddp` writes and keep it
  from one task to the next, also another client's; `i3` reads `i0` of the next instance of
  the 2x2 quad and drops writes, and `efo.gpiext` reaches across the quad (*Internal
  registers*, *`efo`*).
- **Index registers [RE].** A destination bank writes `i.l`, `i.h` or both (`i.lh`); an
  indexed operand `r[il + #n]` names a register of a bank (`r`, `o`, `pa`, `sa`) by index plus
  offset. The firmware loads `i.l` from a temporary 79 times and uses 79 indexed operands,
  all on the `sa` bank, as tables kept in the secondary attributes **[RE]**. **[HW]**
  `r[il + #n]` and `sa[il + #n]` are the bank's register `i.l + n`, counted from `r0`/`sa0`,
  and bits 10 and up of `i.l` are ignored (*Index registers*); `i.h` is **[open]**.
- **The register file.** SGX's own MMIO registers are reached with `ldr`/`str` by dword
  index, not through a bank (*Register file access*).
