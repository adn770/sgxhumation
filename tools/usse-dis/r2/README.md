# radare2 `usse` arch plugin — PowerVR SGX Series-5 USSE

A radare2 `RArch` plugin that disassembles PowerVR SGX **USSE** code (Series-5 / SGX5xx). There
is no PowerVR/USSE architecture in upstream radare2 today — this is intended to be contributed.

All decoding lives in the standalone clean-room core (`../usse.c` / `../usse.h`); this directory
is just the radare2 adapter (`arch_usse.c`): it maps the decoder to `RAnalOp` — mnemonic, op
type, and branch targets (so r2's control-flow analysis and graphs work).

## Build & install (out-of-tree)

Uses the workspace r2 build by default; set `R2_PREFIX` for a system install.

```sh
make            # -> arch_usse.{dylib,so}
make install    # copies to $(radare2 -H R2_USER_PLUGINS)
```

## Use

```sh
radare2 -a usse -b 32 sgx545_ukernel_template.bin
# then: pd 40      (disassemble), aa; agf   (analyze + graph — branches resolve)
```

Verified: `pd` over the extracted SGX545 microkernel matches the authoritative reference
disassembler (PVR_PSP2 `ref_usse_disasm`) on 6631/6637 instructions; the 6 differences are zero-padding
words, which r2 correctly shows as `invalid`. `ba`/`br`/`ba.savelink` set `op->jump`/`fail` and
type (JMP/CJMP/CALL), so jump arrows and function graphs render.

## Assemble

The plugin also assembles, through the same form table as `tools/usse-asm`: any text the
disassembler prints assembles back (`rasm2 -a usse -b 32 'ldr r0, #7, drc0'`, `pa`, `wa` in
radare2). r_asm lowercases what it assembles, so the form engine matches letters in either
case. `test_r2.py` round-trips several hundred random decodable words through `rasm2` in both
directions (needs `make install`).

The fork (`tools/radare2`, `upstream/` holds its patches) carries the same core and both
encoders, USSE and PDS. `sync-fork.sh` copies the cores into it after a change here; rebuild
the fork and run its asm tests with `r2r test/db/asm/usse test/db/asm/pds`. Its in-core
`usse` plugin takes precedence over this out-of-tree one, so keep them in sync.

## Emulation (ESIL, fork only)

The fork's in-core plugin also carries a register profile (`regs.h`) and ESIL, so `aes`/`aesu`/
`aae` emulate; this out-of-tree plugin decodes and assembles only. The ESIL is built from the
decoded text (`libr/arch/p/usse/plugin.c`) and is deliberately conservative: an instruction gets
ESIL only when every operand is a register of the profile or an immediate, every modifier has a
pinned meaning and every flag is a scheduling flag (`.skipinv`, `.syncs`, `.nosched`, `.end`)
that does not change what one instance computes. Tests: `test/db/esil/usse` in the fork.

Modelled:

- `mov`, `and`/`or`/`xor` (also with an inverted `~src2`), `shl`/`shr`, `rol`/`asr` by an
  immediate below 32;
- the **test group** for 32-bit integer results on `chan0`: `<op>.test<S><a|o><Z>.chan0 dst, pN,
  src1, src2` computes the result, sets `pN` and writes `dst` unless it is `!reg`. Sign test `n`
  is bit 31 (the firmware tests flag bits by shifting them to the top: `shl.testnat #25`), zero
  tests `z`/`nz`; for the bitwise group and shifts, and the zero tests of `iadd32`/`iaddu32`;
- `imae.{u,s} d, a.{low,high}, b.{low,high}, c, u32` (`d = a16 * b16 + c`, halves zero- or
  sign-extended; the firmware's add idiom `imae.u r2, r4.low, #1.low, r2, u32`) and
  `ima32.{u,s} d, a, b, c` (low 32 bits of `a * b + c`);
- `fmad d, a, b, c` (`a * b + c`, without increments or with `.incs000`), `frcp`, `frsq`,
  `fsqrt`, with `.neg`/`.abs`/`.neg.abs` (`-|x|`) source modifiers acting on the sign bit;
- `ba`/`br`, `.savelink` and `lapc`: `pclink` holds an **instruction index**, the next
  instruction's (the firmware builds return addresses as indices: `mov r15, #0x5C2` ...
  `mov pclink, r15` returns to instruction 0x5C2), so `lapc` jumps to `pclink * 8`;
- loads and stores as in `../README.md` (immediate offsets, `memop.h`);
- **SGX register-file accesses**: `ldr rD, idx, drcN` and `str idx, src`, with the dword index
  `idx` an immediate or a register (offset = idx × 4). ESIL places the register file in a
  window above the USSE's 32-bit address space, at `0x100000000 + offset`
  (`USSE_CR_ESIL_BASE`; `esil.addr.size` must stay 64, the default). Pinned by the hardware
  (`ldr r0, #7` returns `CORE_ID`, `ldr r0, #801` the page-directory base register:
  `design/sgx545_render_uapi.md` Q4) and by the firmware, which programs its registers this way
  (`../regtouch.py`) and loads the same index into a register for the register forms
  (`mov rI, #0x321` then `ldr rD, rI`, the immediate form's `#801`);
- `mov.repeatN d, s`, unpredicated, between ranges that do not overlap: `d+i = s+i` for
  `i < N`, the increments a task starts with. Our vertex and state-copy programs
  (`tools/usse-asm/progs/use_vtx.s`, `use_mtest*.s`) copy their attributes to the outputs
  this way and render correctly on the hardware (`design/sgx545_3d.md` §14).

The window is plain memory. Map some to emulate the firmware, and load the register names as
flags (`harness/ukernel-extract/make_r2.sh` adds one per register the firmware touches,
`../regtouch.py --r2-flags`):

```sh
radare2 -a usse -b 32 -i sgx545_ukernel.r2 sgx545_ukernel_template.bin
[0x00000000]> o malloc://0x10000 0x100000000
[0x00000000]> wv4 0x01150000 @ sgx.EUR_CR_CORE_ID
[0x00000000]> s edm.getmiscinfo; aei; aeim; aeip; aes 3; ar r3     # 0x01150000
```

Registers with side effects (status, clear-on-read, the kick and event registers), the `drc`
wait of `ldr` and register-supplied indexes of 0x4000 and over are not modelled. The MOE state
that `smlsi`/`smoa`/`smbo`/`smr` set (other increments for repeats) is not tracked either; no
repeat of the microkernel follows one.

Floats are IEEE binary32 bit patterns in the 32-bit registers, computed through ESIL's doubles
(`F2D` ... `D2F`, round to nearest). That is an approximation: the hardware's rounding, its
`frcp`/`frsq` precision and its denormal handling are not characterised, and r2's `D2F`
returns the low half of a double for an infinite or NaN result (`frcp` of 0, `fsqrt` of a
negative), so special values are not modelled either.

Coverage on the SGX545 microkernel (6637 words; `aoj` op->esil set, per base mnemonic), after
patches 0005, 0008 and 0010 of `upstream/`:

| | 0005 | 0008 | 0010 |
|---|---|---|---|
| all words | 3715 (56.0%) | 4758 (71.7%) | 5376 (81.0%) |
| `and` / `xor` / `or` | 120/602, 20/199, 199/208 | 602/602, 199/199, 208/208 | same |
| `imae` / `ima32` / `iaddu32` | 0/258, 0/10, 0/8 | 258/258, 10/10, 8/8 | same |
| `shl` / `rol` / `fmad` / `lapc` | 67/95, 0/1, 0/6, 0/74 | 83/95, 1/1, 6/6, 74/74 | same |
| `ldr` / `str` | 0/196, 0/422 | 0/196, 0/422 | 196/196, 422/422 |

Our own pixel and vertex programs (`tools/usse-asm/progs/use_*.s`, 46 words) go from 10 to 14:
every instruction with a register effect has ESIL except `p0 ldad.fetch4 pa0, [pa0,+#0]`, a
load that never runs and whose base is also its destination.

Of the 1261 microkernel words left: 1043 have no register effect to model (`wdf` 807, `idf`
124, `nop` 87, `smlsi`, `phas`, `ci`/`cfi`, `lock`, `emitpds`), 157 are `mov`s through the
index registers (`i.l`, `sa[il + #0]`), 43 are 16-bit ALU ops (`isub16`/`isubu16` tests,
`ima16`), 12 are `shl.testpat` and 6 are loads/stores with register offsets or a clobbered
base. Each needs a hardware probe first (below).

`esil_check.py` measures this and checks the ESIL (tests: `test_esil_check.py`); point
`R2_PREFIX` at an install of the fork:

```sh
R2_PREFIX=/path/to/fork-prefix ../../../.venv/bin/python esil_check.py survey uk.bin  # table
R2_PREFIX=... esil_check.py tokens uk.bin       # every token an operator, number or register
R2_PREFIX=... esil_check.py cross --count 3000  # random modelled instructions vs a Python model
```

Deliberately not modelled, because the semantics are not pinned by the text, the firmware or
probing (the reference disassembler is a black box that only prints text):

- **repeats** other than the plain `mov.repeatN` above: predicated, overlapping, of any other
  operation, `.incsXYZ` other than 000, and `.partial` / `.onceonly`; earlier versions gave
  `mov.repeatN` and the bitwise group a single-instance ESIL, which was wrong and is gone;
- **write masks** (`d.xyz`, `.bytemask`), the index registers and indexed operands, register
  shift amounts for `rol`/`asr` and the test group (amounts of 32 and over), `~` on shift
  amounts;
- the **test sign `p`** (positive: `> 0` or `>= 0` is not pinned), `a`/`o` joins with a `t`
  side (whether `t` means "true" or "not tested" changes them), sign tests of the adds,
  channels other than `chan0`, the mask forms (`.bmsk`/`.wmsk`/`.dmsk`), and every 8/16-bit
  and float test (`isub16`, `iadd8`, `fadd.test...`: lane width, saturation and float zero);
- `imae` with saturation (`.usat`/`.ssat`), carries (`coe`/`cie`), `.rsN` or a 16-bit `src2`
  format (`u16`/`z16`); `ima32` with modifiers or a second destination; `ima8`/`ima16`;
- floats with immediates, `.flt16`/`.c10`/swizzled sources, `.tpres`, the dual-issue forms
  (`fmad ...+fadd ...`), `fmsa`/`fadm`/`fsubflr`/`fminmax`/`fmaxmin`/`fdp`/`fddp`/`efo`/`fpma`
  (operand roles not pinned) and `fexp`/`flog`/`fsin`/`fcos` (no ESIL operation);
- every **pack/unpack** conversion: the destination byte mask, the source lane selects and the
  rounding modes would all be guesses;
- sampling, `eldd`/`eldq`, `movc`/`movmsk`, `idiv` and the special class.

Registers outside the profile (`r64`+, `pa32`+, `sa32`+, ...) never appear in the ESIL, so most
random words get none; on a uniform 200k-word sample only 246 of 153466 decodable words do.

### Probes for the next hardware session

What is left needs the GPU to answer. Each probe is a client fill on the render node, as for
the register-access question (`sgxfive-test q4`, `design/sgx545_render_uapi.md` Q4): a pixel
program, assembled with `tools/usse-asm`, sets its inputs with `mov rN, #imm`, runs the
instruction under test and writes one result to `o0`; the end-of-tile program stores `o0` as
the pixel, and the host reads the surface back. A predicate is read with `mov o0, #0` and
`pN mov o0, #1`. Run every input as its own fill, so no result depends on another.

- **Test sign `p`.** `shl.testpat.chan0 !c0, p0, rX, #0` with rX = 0 (1 and 0x80000000 as
  controls): `p0` = 1 means `>= 0`, 0 means `> 0`.
- **`t` in a join.** `and.testnot.chan0 !c0, p0, rX, rX` with rX = 1: `p0` = 1 means `t` is
  "true", 0 means "not tested".
- **`isub16` lanes.** `isub16.testtaz.chan0 rD, p0, rA, #1` with rA = 0x00010001, 0 and
  0x00018000: is the upper half of `rD` kept, zeroed or borrowed into; does 0 give 0xffff,
  0xffffffff or 0 (saturation); is `p0` taken from 16 or 32 bits.
- **`isub16` sign.** `isub16.testnat.chan0 !c0, p0, rA, #0` with rA = 0x00008000 and
  0x80000000: which of bit 15 and bit 31 is the sign. `isubu16.testtaz` with rA = 0: wraps or
  saturates.
- **`ima16` formats.** `ima16.s rD, rA, #3, rC.neg, u16, u16` with rA = 0x00020005 and
  rC = 0x00010007: which halves multiply and add, and what `.neg` and the two `u16` apply to.
- **Index unit and base.** The secondary PDS program loads distinct values into sa0-sa7; then
  `mov i.l, rK` and `mov o0, sa[il + #n]` for K = 0..3 and n = 0, 1: the unit of `il` (one
  register or a pair), its origin (sa0) and the role of `#n`.
- **Index width.** The same with K = 0x40001 (bit 18, which the firmware's page-table loop
  sets) and 0x10001, then `mov sa[il + #0], rV` and a read of sa1: which bits of `i.l` count,
  and whether the store form mirrors the load.
- **Register offsets.** `stad [rB,+rO], rV` with rO = 1 and 0x40001, then a read of the
  buffer: element or byte units, and what bit 18 of the offset does.
- **Repeat order.** `mov.repeat2 r2, r1` with r1 = A, r2 = B, r3 = C, then read r3: A means one
  iteration after the other, B that the sources are read first.
- **Repeat predicate.** `p0 mov.repeat2 o0, rS` with p0 = 1 and p1 = 0: o1 written means one
  predicate for the instance, not written a predicate per iteration.
- **Repeat increments.** `smlsi` with increments 2 and 0, then `mov.repeat2`: the MOE state
  ESIL would have to track.
- **Register index range.** `ldr rD, rI, drc0; wdf drc0` with rI = 0x4007: `CORE_ID` means the
  index is masked to 14 bits.
- **Float rounding.** `fmad` on a halfway product, `frcp` of 3.0 and a denormal product: round
  to nearest even, the `frcp` bits, flush to zero.

The register-file writes stay a host-side question: client `str` was not seen to land (Q4), so
probe them, if at all, only from the EDM's own task, with registers that are safe to write.

(The `WARN: Cannot find asm.parser for usse` message is benign — `asm.parser` is the optional
AT&T/Intel-style syntax switcher; disassembly is unaffected.)

## Upstreaming to radareorg/radare2

The plugin is structured to drop into the r2 tree with minimal change:

1. Create `libr/arch/p/usse/` containing `plugin.c` (this `arch_usse.c`, renamed), plus the core
   `usse.c` + `usse.h`. The core is already radare2-free and self-contained.
2. Add the build entries: a `usse` stanza in `libr/arch/p/meson.build` and the `arch.usse`
   entry in the plugin list (`libr/arch/meson.build` / `meson_options.txt`), mirroring an
   existing small arch (e.g. `chip8`).
3. The plugin already follows upstream conventions: `const RArchPlugin r_arch_plugin_usse`,
   `RLibStruct radare_plugin` guarded by `R2_PLUGIN_INCORE`, `LGPL-3.0-only`.

Clean-room note for the PR: the decoder was written independently and validated *against* the
output of Imagination's `ref_usse_disasm` (used only as a black-box oracle; see `../refdisasm/`). No
Imagination source is copied or vendored — only our own code is contributed.
