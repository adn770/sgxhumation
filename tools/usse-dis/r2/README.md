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

Items marked **[HW]** were measured on the laptop with our own programs (`tools/usse-asm/
probe.py`, results in "Hardware probes" below).

- `mov`, `and`/`or`/`xor` (also with an inverted `~src2`), `shl`/`shr`/`rol`/`asr` by an
  immediate below 32 or by a register: **[HW]** a register amount of 32 and over gives 0 for
  `shl`/`shr` and the sign for `asr`, and `rol` takes it mod 32;
- the **test group** on `chan0`: `<op>.test<S><a|o><Z>.chan0 dst, pN, src1, src2` computes the
  result, sets `pN` and writes `dst` unless it is `!reg`. Sign test `n` is bit 31 (the firmware
  tests flag bits by shifting them to the top: `shl.testnat #25`), **[HW]** `p` is its
  complement (`>= 0`) and `t` is true, so an `o` join with a `t` side is always true; zero tests
  `z`/`nz`. For the bitwise group, the shifts, `iadd32` and `iaddu32` on the 32-bit result:
  **[HW]** the adds take src1's low 16 bits, sign-extended (`iadd32`) or zero-extended
  (`iaddu32`), plus the 32-bit src2 (the firmware's increment `iaddu32 r3, p0, #1, r3`), and
  `iaddu32` is unsigned: `n` is never and `p` always true. For `isub16`/`isubu16`: **[HW]** both
  16-bit lanes subtract, wrapping (an immediate is subtracted from both), and `chan0` tests the
  low lane (sign bit 15); `isubu16` only with the sign test `t`;
- **[HW]** `ima16.{s,u} d, a, b, c[.neg], u16, u16`: `a * b + c` (or `- c`) in each 16-bit lane,
  wrapping; an immediate `b` or `c` is in both lanes;
- `imae.{u,s} d, a.{low,high}, b.{low,high}, c, u32` (`d = a16 * b16 + c`, halves zero- or
  sign-extended; the firmware's add idiom `imae.u r2, r4.low, #1.low, r2, u32`) and
  `ima32.{u,s} d, a, b, c` (low 32 bits of `a * b + c`);
- `fmad d, a, b, c` (without increments or with `.incs000`), `frcp`, `frsq`, `fsqrt`, with
  `.neg`/`.abs`/`.neg.abs` (`-|x|`) source modifiers acting on the sign bit. **[HW]** `fmad`
  rounds `a * b` to binary32, then the sum, both to nearest even (it is not fused), and flushes
  denormal inputs, product and result to a zero of their sign; the float sources of `frcp`,
  `frsq` and `fsqrt` are flushed too (`frcp` of a denormal is `+inf` on the hardware);
- `ba`/`br`, `.savelink` and `lapc`: `pclink` holds an **instruction index**, the next
  instruction's (the firmware builds return addresses as indices: `mov r15, #0x5C2` ...
  `mov pclink, r15` returns to instruction 0x5C2), so `lapc` jumps to `pclink * 8`;
- loads and stores as in `../README.md` (immediate offsets, `memop.h`), and **[HW]** with a
  register offset (`ldad d, [b,+o]`, `[b,-o]`, `ldad.fetchN d, [b,+o]`, `stad [b,+o], s`):
  the offset register holds two unsigned 16-bit halves whose product is the offset in bytes
  (the firmware ORs `0x00040000`, a stride of 4, into a dword index), added modulo 2^32; a
  fetch starts one element after it, as with immediates; a load from an address that is not
  a multiple of 4 reads those bytes, a store there does not land;
- **SGX register-file accesses**: `ldr rD, idx, drcN` and `str idx, src`, with the dword index
  `idx` an immediate or a register (offset = idx × 4). ESIL places the register file in a
  window above the USSE's 32-bit address space, at `0x100000000 + offset`
  (`USSE_CR_ESIL_BASE`; `esil.addr.size` must stay 64, the default). Pinned by the hardware
  (`ldr r0, #7` returns `CORE_ID`, `ldr r0, #801` the page-directory base register:
  `design/sgx545_render_uapi.md` Q4) and by the firmware, which programs its registers this way
  (`../regtouch.py`) and loads the same index into a register for the register forms
  (`mov rI, #0x321` then `ldr rD, rI`, the immediate form's `#801`). **[HW]** `ldr` masks a
  register index to 14 bits (`0x4007`, `0x10007`, `0x40007` read `CORE_ID`);
- `mov.repeatN d, s`: `d+i = s+i` for `i < N`, the increments a task starts with. Our vertex
  and state-copy programs (`tools/usse-asm/progs/use_vtx.s`, `use_mtest*.s`) copy their
  attributes to the outputs this way and render correctly on the hardware (`design/
  sgx545_3d.md` §14). **[HW]** The iterations run one after the other in ascending order, so
  overlapping ranges propagate (`mov.repeat3 r2, r1` copies `r1` to `r2`-`r4`), and a predicate
  is tested once for the whole instruction;
- **[HW]** the index register: `mov i.l, src`, and moves through `r[il + #n]` and `sa[il + #n]`
  (the firmware's `mov r5, sa[il + #0]`): the register is the bank's `i.l + n`-th, and bits 10
  and up of `i.l` are ignored. ESIL cannot name a register from a value, so these are chains
  of conditionals over the bank's registers in the profile (`r0`-`r63`, `sa0`-`sa31`, plus the
  new profile registers `il`/`ih`); an index outside them moves nothing.

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
wait of `ldr` and `str` register indexes of 0x4000 and over are not modelled. The MOE state
that `smlsi`/`smoa`/`smbo`/`smr` set (other increments for repeats) is not tracked either; no
repeat of the microkernel follows one. **[HW]** `smlsi #I0, #I1, #I2, #I3, ...`: `I0` is the
destination's increment and `I2` the source's for a following `mov.repeatN` (`I1`, `I3` do not
affect it), so tracking it is possible once a program needs it.

Floats are IEEE binary32 bit patterns in the 32-bit registers, computed through ESIL's doubles
(`F2D` ... `D2F`, round to nearest even). The rounding and the flushing above are the
hardware's **[HW]**; what is left approximate: `fmad`'s sum goes through a double, so a sum of
very different magnitudes can round twice; `frcp`/`frsq`/`fsqrt` are correctly rounded in the
ESIL but only within one ulp on the hardware (**[HW]**: 46, 42 and 44 of 48 random inputs
exact, the others one ulp off, at most 0.65 ulp from the true value); and special values are
not modelled, because r2's `D2F` returns the low half of a double for an infinite or NaN
result (the hardware gives `+inf` for `frcp` of 0 and `frsq` of 0, `-inf` for `frcp` of -0,
and the NaN `0x7fffffff` for `frsq`/`fsqrt` of -1).

Coverage on the SGX545 microkernel (6637 words; `aoj` op->esil set, per base mnemonic), after
patches 0005, 0008, 0010 and 0011 of `upstream/` (`=`: as in the column before):

| | 0005 | 0008 | 0010 | 0011 |
|---|---|---|---|---|
| all words | 3715 (56.0%) | 4758 (71.7%) | 5376 (81.0%) | 5591 (84.2%) |
| `and` / `xor` / `or` | 120/602, 20/199, 199/208 | 602/602, 199/199, 208/208 | = | = |
| `imae` / `ima32` / `iaddu32` | 0/258, 0/10, 0/8 | 258/258, 10/10, 8/8 | = | = |
| `shl` / `rol` / `fmad` / `lapc` | 67/95, 0/1, 0/6, 0/74 | 83/95, 1/1, 6/6, 74/74 | = | 95/95, = |
| `ldr` / `str` | 0/196, 0/422 | 0/196, 0/422 | 196/196, 422/422 | = |
| `mov` / `stad` | not measured | 1340/1497, 345/349 | = | 1497/1497, 348/349 |
| `isub16` / `isubu16` / `ima16` | 0/40, 0/1, 0/2 | = | = | 40/40, 1/1, 2/2 |

Our own pixel and vertex programs (`tools/usse-asm/progs/use_*.s`, 46 words) go from 10 to 14:
every instruction with a register effect has ESIL except `p0 ldad.fetch4 pa0, [pa0,+#0]`, a
load that never runs and whose base is also its destination.

Of the 1046 microkernel words left, 1043 have no register effect to model (`wdf` 807, `idf`
124, `nop` 87, `smlsi`, `phas`, `ci`/`cfi`, `lock`, `emitpds`). The other three are two
`ldad.fetch2 r2, [r2,+#1]` whose base is also a destination, and `stad [sa0,+r2], sa[il + #0]`,
a store whose data is an indexed register.

`esil_check.py` measures this and checks the ESIL (tests: `test_esil_check.py`); point
`R2_PREFIX` at an install of the fork:

```sh
R2_PREFIX=/path/to/fork-prefix ../../../.venv/bin/python esil_check.py survey uk.bin  # table
R2_PREFIX=... esil_check.py tokens uk.bin       # every token an operator, number or register
R2_PREFIX=... esil_check.py cross --count 3000  # random modelled instructions vs a Python model
```

Deliberately not modelled, because the semantics are not pinned by the text, the firmware or
probing (the reference disassembler is a black box that only prints text):

- **repeats** other than `mov.repeatN`: of any other operation, after `smlsi`/`smoa`/`smbo`,
  `.incsXYZ` other than 000, and `.partial` / `.onceonly`; earlier versions gave `mov.repeatN`
  and the bitwise group a single-instance ESIL, which was wrong and is gone;
- **write masks** (`d.xyz`, `.bytemask`), `~` on shift amounts, immediate shift amounts of 32
  and over (only register amounts were probed);
- the **index register** `i.h`, the `o` and `pa` banks through it, and indexed operands of
  anything but `mov` (the firmware's `stad [sa0,+r2], sa[il + #0]`);
- register-offset **word** loads and stores (`ldaw`/`staw`), with pre/post increments, and the
  negative fetch (`ldad.fetchN d, [b,-o]`);
- channels other than `chan0` (**[HW]**: `and` and `isub16` with `chan1`-`chan3` clear `pN`
  for a `z` test, whatever the value), `isubu16`'s sign tests, the mask forms
  (`.bmsk`/`.wmsk`/`.dmsk`), and every 8-bit and float test (`iadd8`, `fadd.test...`);
- `imae` with saturation (`.usat`/`.ssat`), carries (`coe`/`cie`), `.rsN` or a 16-bit `src2`
  format (`u16`/`z16`); `ima32` with modifiers or a second destination; `ima8`, and `ima16`'s
  other formats;
- floats with immediates, `.flt16`/`.c10`/swizzled sources, `.tpres`, the dual-issue forms
  (`fmad ...+fadd ...`), `fmsa`/`fadm`/`fsubflr`/`fminmax`/`fmaxmin`/`fdp`/`fddp`/`efo`/`fpma`
  (operand roles not pinned) and `fexp`/`flog`/`fsin`/`fcos` (no ESIL operation);
- every **pack/unpack** conversion: the destination byte mask, the source lane selects and the
  rounding modes would all be guesses;
- sampling, `eldd`/`eldq`, `movc`/`movmsk`, `idiv` and the special class.

Registers outside the profile (`r64`+, `pa32`+, `sa32`+, ...) never appear in the ESIL, so most
random words get none; on a uniform 200k-word sample only 275 of 153466 decodable words do
(246 before patch 0011).

### Hardware probes

What the text left open was asked of the GPU (2026-10-22): `tools/usse-asm/probe.py` runs a
table of probes (`tools/usse-asm/progs/probes/`), each a client fill on the render node whose
pixel program, assembled from our own source, presets its inputs with `mov rN, #imm`, runs the
instruction under test and writes one result to `o0` (a predicate as `mov r30, #0` then `pN mov
r30, #1`); the host reads the surface back, and a 64 KiB scratch buffer for loads and stores.
Every case runs as its own fill and its own process. A probe is settled when exactly one of its
hypotheses predicts every case; all were, except where noted. **[HW]** throughout:

- **Test sign `p`** is `>= 0`: `shl.testpat`/`and.testpat` set `p0` for 0, 1 and 0x7fffffff,
  not for 0x80000000.
- **`t` in a join** is true: `and.testnot`, `.testtoz`, `.testtonz` set `p0` for every input.
- **The adds** (`iadd32`, `iaddu32`) add src1's low 16 bits, sign- or zero-extended, to the
  32-bit src2: `0x12345678 + 0x11111111` gives `0x11116789`, `iadd32 0xffff + 1` gives 0, while
  `#1 + r` is the plain increment the firmware uses. Their tests look at that 32-bit result;
  `iaddu32`'s `n` is never and its `p` always set (also for `0x80000000`).
- **`isub16`/`isubu16`** subtract both 16-bit lanes, wrapping, with no saturation: `0x12340005 -
  #1` is `0x12330004`, `0 - #1` is `0xffffffff`, `0x00050007 - 0x00010002` is `0x00040005`, and
  `isubu16 0 - #1` is `0xffffffff`. `chan0` tests the low lane: `z` from its 16 bits, `n` from
  bit 15 (not bit 31), `p` is `>= 0` on it.
- **`ima16 ..., u16, u16`** computes `a * b + c` (`- c` with `.neg`) in each lane, wrapping:
  `0x00020005 * #3 - 0x00010007` is `0x00050008`, `0x0000ffff * #3` is `0x0000fffd`; immediates
  are in both lanes, a register `b` or `c` lane by lane.
- **Shifts by a register:** `shl`/`shr` by 32, 33, 64, 0x100 or 0xffffffff give 0, `asr` gives
  the sign, `rol` takes the amount mod 32; `shl.testnat` the same.
- **The index register:** `mov r30, r[il + #n]` reads register `i.l + n` counted from `r0`
  (unit one register; `#n` adds), and `mov r[il + #n], r21` writes it. Bits 10 and up of `i.l`
  are ignored (`0x401`, `0x40001`, `0xfffffc02` index `r1`, `r1`, `r2`); indexes 0x21 to 0x201
  past the program's 32 temporaries read the next instances' registers [infer: the share of
  pixels with other values grows with the index]. `sa[il + #n]` is the same on the
  secondaries, measured by writing `sa0`-`sa7` from the pixel program; about 1% of the pixels
  read another secondary there, as expected when 2048 instances write shared registers.
- **Register offsets of loads and stores:** the offset register `o` gives `(o >> 16) * (o &
  0xffff)` bytes, both halves unsigned: `0x00040003` is +12, `0x00010001` +1 (an unaligned load
  reads those bytes), `0x01000001` +256, `0x00000005` +0; with a top bit set the address leaves
  the buffer and faults at base + the unsigned product, modulo 2^32 (`0xffffffff` faults at base
  - 0x1ffff). `[b,-o]` subtracts; `ldad.fetch2 [b,+o]` starts one element after, as the
  immediate forms do. `stad` matches, but a store at a byte offset of 1, 2 or 3 lands nowhere in
  the buffer and does not fault.
- **`ldr rD, rI`** masks the index to 14 bits: 0x4007, 0x8007, 0x10007 and 0x40007 read
  `CORE_ID` like 7.
- **Repeats:** `mov.repeat2 r2, r1` leaves `r3 = r1`, so iterations run one after the other,
  ascending (`mov.repeat3 r1, r2` gives `r1 = r2`, `r2 = r3`, `r3 = r4`); `p0 mov.repeat2` with
  `p0 = 1, p1 = 0` writes both registers and with `p0 = 0, p1 = 1` neither: one predicate.
  `smlsi #I0, #I1, #I2, #I3, ...` before `mov.repeat2 r20, r10`: `I0` is the destination's
  increment, `I2` the source's (0 repeats a register, 2 skips one), `I1`/`I3` change nothing.
- **Floats:** `fmad` rounds to nearest even (`(1+2^-12)^2` and `(1+2^-12)(1+3*2^-12)`, both
  ties, round to even, also when negative), and rounds the product before adding:
  `(1+2^-12)^2 - (1+2^-11)` is 0, not 2^-24; 48 random triples agree with that model. Denormal
  results and inputs become zeros of their sign (`2^-70 * 2^-70` is 0, `-2^-70 * 2^-70 + -0`
  is -0, `2^-140 * 2^100` is 0). `frcp`, `frsq`, `fsqrt` are within one ulp but not correctly
  rounded (46, 42, 44 of 48 random normals exact); `1/3`, `1/5`, `1/sqrt(2)`, `sqrt(3)` are
  exact. Specials: `frcp(0) = +inf`, `frcp(-0) = -inf`, `frcp` of a denormal `+inf`,
  `frsq(0) = +inf`, `fsqrt(0) = 0`, and `frsq`/`fsqrt` of -1 give the NaN `0x7fffffff`.
- **Other channels:** `and.testtaz.chan1`..`chan3` and `isub16.testtaz.chan1` clear `p0` (and
  leave `p1`-`p3`) whatever the input, even when the tested value is zero; what they test is
  not known.

Not probed: write masks, `.incsXYZ`, pack/unpack lanes, `ldaw`/`staw` and pre/post-increment
register offsets, `i.h`, and immediate shift amounts of 32 and over. The register-file writes
stay a host-side question: client `str` was not seen to land (Q4), so probe them, if at all,
only from the EDM's own task, with registers that are safe to write.

Raw results of a run are kept by `probe.py run --out DIR` (`raw.txt`, `report.txt`); re-read
them with `probe.py eval DIR/raw.txt`.

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
