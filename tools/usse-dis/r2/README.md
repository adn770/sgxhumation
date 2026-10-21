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
- loads and stores as in `../README.md` (immediate offsets, `memop.h`).

Floats are IEEE binary32 bit patterns in the 32-bit registers, computed through ESIL's doubles
(`F2D` ... `D2F`, round to nearest). That is an approximation: the hardware's rounding, its
`frcp`/`frsq` precision and its denormal handling are not characterised, and r2's `D2F`
returns the low half of a double for an infinite or NaN result (`frcp` of 0, `fsqrt` of a
negative), so special values are not modelled either.

Coverage on the SGX545 microkernel (6637 words; `aoj` op->esil set, per base mnemonic):

| | before | after |
|---|---|---|
| all words | 3715 (56.0%) | 4758 (71.7%) |
| `and` / `xor` / `or` | 120/602, 20/199, 199/208 | 602/602, 199/199, 208/208 |
| `imae` / `ima32` / `iaddu32` | 0/258, 0/10, 0/8 | 258/258, 10/10, 8/8 |
| `shl` / `rol` / `fmad` / `lapc` | 67/95, 0/1, 0/6, 0/74 | 83/95, 1/1, 6/6, 74/74 |

Of the 1879 words left: 1043 have no register effect to model (`wdf` 807, `idf` 124, `nop`
87, `smlsi`, `phas`, `ci`/`cfi`, `lock`, `emitpds`), 618 are `ldr`/`str` accesses to the SGX
register file (outside the emulated address space), 157 are `mov`s through the index
registers (`i.l`, `sa[il + #0]`), 43 are 16-bit ALU ops (`isub16`/`isubu16` tests, `ima16`),
12 are `shl.testpat` and 6 are loads/stores with register offsets or a clobbered base.

Deliberately not modelled, because the semantics are not pinned by the text, the firmware or
probing (the reference disassembler is a black box that only prints text):

- **repeats** (`.repeatN` with source/destination increments, `.incsXYZ` other than 000) and
  `.partial` / `.onceonly`; earlier versions gave `mov.repeatN` and the bitwise group a
  single-instance ESIL, which was wrong and is gone;
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
- `ldr`/`str`, sampling, `eldd`/`eldq`, `movc`/`movmsk`, `idiv` and the special class.

Registers outside the profile (`r64`+, `pa32`+, `sa32`+, ...) never appear in the ESIL, so most
random words get none; on a uniform 200k-word sample only 242 of 153466 decodable words do.

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
