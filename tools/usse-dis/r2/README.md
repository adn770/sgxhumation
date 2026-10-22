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
  immediate or by a register: **[HW]** an amount of 32 and over gives 0 for `shl`/`shr` and
  the sign for `asr`, and `rol` takes it mod 32; `~` complements the amount first;
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
- `fmad d, a, b, c`, `frcp`, `frsq`, `fsqrt`, with
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
- **[HW]** **write masks** (`d.xyzw` and every other mask on `mov`, the bitwise group and the
  shifts, `frcp`/`frsq`/`fsqrt`, `movc` and the packs): component k writes `d + k` from every
  register source `+ k` (an immediate stays), a repeat of four with gaps; mask 0 (`d.`) writes
  nothing and gets no ESIL;
- **[HW]** **float repeats with `.incsXYZ`**: `fmad`, `fminmax` and `fmaxmin` `.repeatN`; X, Y
  and Z let source 0, 1 and 2 advance, the destination always advances, and without `.incs`
  every source does; the iterations run one after the other, the order measured on `mov`
  repeats [infer for the floats];
- **[HW]** **dot products**: `fdp.repeatN d, a, b` writes the `N`-term dot product to `d + N -
  1` only, each product rounded to binary32 and flushed, then the sums in order, each rounded;
  `fddp.repeatN d, iM, s0, s1, s2` writes `s0·s1` there and `s0·s2` to `i0`/`i1` (none when `d +
  N - 1` is one of the sources the second product reads);
- **[HW]** **`fminmax d, a, b, c` = `min(max(a, b), c)`** and **`fmaxmin` = `max(min(a, c),
  b)`**, IEEE minNum/maxNum (a NaN loses, `-0 < +0`) on the flushed bit patterns, through `d`
  (none when `d` is the operand the second step reads);
- **[HW]** **`movc.<i8|i16|i32|i10|flt>.<tz|tnz|tn|tp|z> d, s0, s1, s2`**: a select per lane
  of `s0` (`.i10`: three 10-bit lanes, bits 30-31 cleared; `.flt`: the float with denormals as
  zero, a NaN failing all but `tnz`); **`movmsk.<mask|u8|flt|flt16> d, s1, s2.c`**: `s1` into
  `d` when bit 0 or 7 of byte `c`, the float or the half `c` (bits 0-15, 8-23, 16-31) is at
  least 0.5;
- **[HW]** **packs** `pck<u8|s8|o8|u16|s16|c10>f32[.scale]` with byte masks, and **unpacks**
  `unpckf32<u8|s8|o8|u16|s16>[.scale]` and `unpck<u8|s16|u16><s8|u8>`: lanes s1, s2, s1, s2
  (bytes) or s1, s2 (halves), the measured byte-mask rule, saturation, `.scale` factors
  (255, 127 clamped to ±127, 127 + 128 clamped to 1..255, 65535, 32767 clamped to ±32767,
  `c10` 255 clamped to ±510), the rounding mode unscaled and nearest even scaled, correctly
  rounded scaled unpacks with -1 clamped, `unpcku8s8` turning a negative into `0x7f`;
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
  new profile registers `il`/`ih`); an index outside them moves nothing;
- **[HW]** the **colour operations** on 8-bit operands (`sop2`, `sop3`, `lrp1`, `lrp2`,
  `sop2wm`): bytes 0-2 colour, byte 3 alpha, `x` standing for `x/255`; `add`/`sub` sum the
  exact products, divide by 255 once, round and saturate; `min`/`max` compare the sources;
  `asat`, `s2scale` (doubles the result), `zeros` (a signed `s2`), `comp`, `neg`, the lerps by
  `s0`/`s0a` and `sop2wm`'s byte mask as measured;
- **[HW]** the **integer dot products** `u8dot3`/`u8dot4`/`u16dot3`/`u16dot4` and their `off`
  forms: byte products summed (`off`: `(x - 128)(y - 128)`), scaled by `Cx`/`Ax`, rounded and
  saturated, `u16` as a 15-bit fraction in both halves; `u8dot3`'s `aadd`/`asub`/`aintrp1`/
  `aintrp2` alpha;
- **[HW]** `fpma` = `c0 + c1 × c2` on 8-bit channels; `efo` (the printed dataflow, every
  product and sum rounded to binary32, the destination's `i0`/`i1` read before the writes; not
  `.gpiext`, see below); `fadm` = `a·c + b·c`, `fmsa` = `a·a + b·c`, `fsubflr` =
  `a - floor(b)`; `statomic`; `movmsk.<fmt>.smsk` (bit 0 of `s0` too, except for `.mask`).
  These helpers evaluate each operand once with `DUP`/`SWAP`: ESIL keeps the strings it stacks
  in a ring (32 bytes per stack slot), so a value left on the stack while hundreds of others are
  pushed is overwritten and the expression fails;
- **[HW]** the **test channels**: one result per channel, the 32-bit result as channel 0 and a
  16-bit op's halves as channels 0 and 2, a channel without a lane testing as a positive
  non-zero value; `chan0`-`chan3`, `andall`/`orall` (all four), `and02`/`or02` (0 and 2);
- **[HW]** `rlp d, s, pN` = `s << 1 | pN`, then `pN` = bit 31 of `s`, `p4` and up aliasing
  `p0`-`p3`; `adif` (`|a - b|` per byte) and `adifsum` (their sum plus a 16-bit half of `s0`);
  `unpcku16c10`/`unpcks16c10`, which put lane 0's bits 4, 6 and 8 into bits 15, 1-14 and 0
  whatever the selects; `sop2`, `fpma` and the integer dot products with `.repeatN` (every
  operand advances);
- **[HW]** **dual issue** (`fmad r10, r1, r2, r3+fadd i0, i1, i2`) for the pairs that
  co-issue cleanly (a `mov`, `fmad`, `ima32`, `fadm`, `fmsa`, `fssq` or `fminmax` primary with
  a secondary that needs no unit the primary uses): both operations read before either
  writes, the secondary wins a shared destination, `.repeat2` runs both (the primary's
  operands advance), and the predicate covers both; a word where each operation writes what
  the other reads gets no ESIL.

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

Patches 0012, 0013 and 0014 change none of these numbers: the microkernel uses none of the forms
they add. By group, what 0012 models and what it leaves:

- **write masks:** on `mov`, `and`/`or`/`xor`, the shifts, `frcp`/`frsq`/`fsqrt`, `movc` and
  the packs; not mask 0, nor masks on the test group;
- **float repeats:** `fmad`, `fminmax`, `fmaxmin` with `.repeatN` and `.incsXYZ`; not the
  repeats of `frcp`/`frsq`/`fsqrt`;
- **dot products:** `fdp`, `fddp`, any repeat and `.incsXYZ`; not `fdpc`/`fddpc`;
- **conditional moves:** `movc` (every type and test), `movmsk` `.mask`/`.u8`/`.flt`/`.flt16`;
  not `movc` repeats or `.smsk`;
- **packs:** from `f32` to `u8`, `s8`, `o8`, `u16`, `s16`, `c10`; unpacks to `f32` from the 8-
  and 16-bit integers, and to `u8`/`u16`/`s16` from `u8`/`s8`; not `f16` results or sources,
  `c10` sources or `statomic`.

What 0013 models and what it leaves:

- **colour operations:** `sop2`, `sop3`, `lrp1`, `lrp2`, `sop2wm` on 8-bit register operands;
  not `.c10` operands (pinned, but not modelled), immediates or repeats;
- **integer dot products:** every form, `off`, `.comp` sources and the `u8dot3` alpha; not
  `.c10` operands or repeats;
- **floats:** `efo`, `fadm`, `fmsa`, `fsubflr`; not `efo.gpiext` (one pixel in four reads
  another instance's values), `efo` sources with a half-float reading, repeats, `fmad16` (half
  floats) or `flog`/`fexp`/`fsin`/`fcos` (no ESIL operation; measured: `log2`, `2^x`, quarter
  turns);
- `fpma` on 8-bit operands, `statomic`, `movmsk .smsk`.

What 0015 models and what it leaves (**[HW]**, round 4 of the probes):

- `idiv` (the unsigned quotient to `d`, the remainder to `d + 1`, all ones for a zero
  divisor); `imae` with `.usat`/`.ssat`, `.rsN`, `u16` (sign-extended) and `z16` sources and
  `coe`/`cie` carries through `iN`; `ima32`'s `cie`; `ima16` with `u8`/`s8`/`o8` sources;
  `ima8` (`x + y * z` per byte, `.sat`); not `.usat` with a `u16` source 2;
- shift immediates of 32 and over and `~` amounts; the `Pn` predicate (never runs);
- **dual issue:** `ima32` as a secondary (two 16-bit lanes, `b` the primary's source 1 after a
  float primary) and the pairs that share a unit (a second product, the primary's result,
  `fminmax`'s `max(a, b) - c`); not `fmad` + `fmad16` or `fmad` + `fmaxmin`;
- bank-7 numbers past `i3` lose their ESIL: they read the other pixels of the quad.

What 0014 models and what it leaves:

- **tests:** every channel selector on the bitwise group, the shifts, `iadd32`/`iaddu32` and
  `isub16`/`isubu16`; not the 8-bit and float tests or `isubu16`'s signs;
- `rlp`, `adif`, `adifsum`, the `c10` to 16-bit unpacks; not `bilin`, `firv`, `firh`;
- **repeats** of `sop2`, `fpma` and the integer dot products; not their `.c10` operands;
- **dual issue:** the clean pairs; not the pairs that share a unit (measured to mix their
  results), `ima32` as a secondary, `fmad16`, `fexp`/`flog`/`fsin`/`fcos` or an `fddp`
  primary;
- not `fdsx`/`fdsy`, `efo.gpiext` and `i3`, which read other pixels of the 2x2 quad, nor
  `smp`, which reads texture memory through state words.

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

- **repeats** other than those of `mov`, `fmad`, `fminmax`/`fmaxmin` and `fdp`/`fddp`: of the
  other operations, after `smlsi`/`smoa`/`smbo`, and `.partial` / `.onceonly`; earlier versions
  gave `mov.repeatN` and the bitwise group a single-instance ESIL, which was wrong and is gone;
- write mask 0, write masks on the test group;
- **`i3`** and the bank-7 numbers past it, which read other pixels of the 2x2 quad and drop
  writes **[HW]**, and
  everything else that reads another pixel: `fdsx`/`fdsy` (coarse derivatives over the quad)
  and `efo.gpiext` (writes funnelled into the top-right pixel's `i0`) **[HW]**; ESIL emulates
  one instance;
- through the **index registers**, the `o` and `pa` banks and indexed operands of anything but `mov`
  (the firmware's `stad [sa0,+r2], sa[il + #0]`);
- register-offset **word** loads and stores (`ldaw`/`staw`), with pre/post increments, and the
  negative fetch (`ldad.fetchN d, [b,-o]`);
- `isubu16`'s sign tests, the mask forms (`.bmsk`/`.wmsk`/`.dmsk`), and every 8-bit, float
  and 16-bit multiply/add test (`iadd8`, `fadd.test...`, `imul16.test...`; **[HW]** their
  channels are bytes, the value, or the lanes before they are cut to 16 bits);
- `ima32` with modifiers or a second destination;
- floats with immediates, `.flt16`/`.c10`/swizzled sources, `.tpres`, `fmad16` (half-float
  arithmetic in ESIL is not cheap), `fdpc`/`fddpc` (where the clip-plane result goes is not known)
  and `fexp`/`flog`/`fsin`/`fcos` (no ESIL operation);
- `.c10` operands of the colour operations, the dot products and `fpma` (pinned for every
  colour operation but `lrp2`, not modelled: each lane needs its own clamp);
- the **pack/unpack** forms with `f16` results or sources and `c10` sources, and `.scale` on
  the integer-to-integer unpacks; NaN and infinite pack inputs;
- sampling (**[HW]**: filters, address modes, layouts, formats, conversions and PCF follow
  the four state words, but ESIL would have to walk the texture layout), `eldd`/`eldq`,
  `movc` repeats and the special class.

Registers outside the profile (`r64`+, `pa32`+, `sa32`+, ...) never appear in the ESIL, so most
random words get none; on a uniform 200k-word sample only 725 of 153466 decodable words do
(662 before patch 0015, 527 before 0014, 425 before 0013, 275 before 0012, 246 before 0011).

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

Round 1 of the ISA probes (2026-10-22, the same day; tables `fdp.toml`, `minmax.toml`,
`movc.toml`, `pack.toml`, `masks.toml`, mostly through the `dump.s` template, which stores
sixteen registers to the scratch buffer so that a case sees every register it writes; 61
probes, all settled but `movmsk-smsk`, which is recorded). **[HW]** throughout:

- **Write masks:** all 16 masks on `mov`, `or`, `and #0xff` and `frcp`, and masks on `movc` and
  a pack: `mov r10.xz, r1` writes `r10 = r1`, `r12 = r3`; `frcp r10.y, r1` writes `r11 =
  1/r2`; mask 0 writes nothing. A component is a register of a group of four, not a byte.
- **`.incsXYZ`:** `fmad.repeat2.incs001 d, a, b, c` gives `d = a·b + c`, `d+1 = a·b + (c+1)`;
  `incs010` advances `b`, `incs100` `a`; without `.incs` all three advance; `.incs000` repeats
  the first operation into `d+1`. The same for `fdp` (no source 0), `fddp`, `fminmax`.
- **`fdp`:** `fdp.repeatN r10, r1, r5` over `(1, 2, 4, 8)·(1, 16, 256, 4096)` writes 1, 33,
  1057, 33825 to `r10`, `r11`, `r12`, `r13` for N = 1-4 and leaves the others; 6 crafted and
  42 random cases fit "products rounded, added in order, each sum rounded" and no other order
  (`(1+2^-12)^2 - (1+2^-12)(1+2^-11)` is `-2^-12(1+2^-11)`); `inf·0` gives `0x7fc00000`,
  products of denormals or into the denormal range give 0, `-0 + -0` is -0. `fdpc` gives the
  same values and leaves `p0`-`p3` as they were. `fddp d, iM, s0, s1, s2` writes `s0·s1` and
  `s0·s2`; `fddpc` does not advance its sources.
- **Internal registers:** `mov i0..i2, ...` then `mov r, i0..i2` reads them back, and the next
  case, another process, reads the same values; `i3` always read what `i0` held, and a write to
  it changed nothing.
- **`fminmax`/`fmaxmin`:** the six orders of 1, 2, 3 give 2, 2, 2, 1, 2, 1 and 2, 3, 2, 3, 2, 2,
  and 10 random triples agree: `min(max(a, b), c)` and `max(min(a, c), b)`. 22 triples with
  NaN, zeros, infinities and denormals fit minNum/maxNum (NaN loses, `-0 < +0`) and no
  compare-and-select order.
- **`movc`:** per lane by type, as above, over 12 values per test and type (the 10-bit lanes'
  bits 30-31 come out 0 whatever `s1` and `s2` hold there). `.flt` treats `0x00000001` and `-0`
  as zero and a NaN as failing `tz`, `tn` and `tp|z`.
- **`movmsk`:** `.mask` moves for bytes 0x01, 0x03, 0x0f, 0xff, not 0x02, 0x04, 0x10, 0x80;
  `.u8` for 0x80, 0x81, 0xfe, not 0x7f, 0x40, 0x01; `.flt` for 0.5, 0.75, 2.0, `+inf`, not
  0.49999997, -0.5, NaN, -0; `.flt16` the same threshold on its half.
- **Packs:** `pcku8f32` with each of the 16 byte masks (source 1 = 1.0, source 2 = 2.0) fits
  only the rule above; saturation of 300, -5, 65536, -40000; `.scale` of 0.75, 0.3, 1.5, 0.1,
  1.0, 0.2 and their negatives; `.scale` rounding ignoring the mode; 96 random `[0, 1)` values;
  `c10` lanes and clamps; `pckf16f32` truncating, keeping a denormal (1e-7 gives `0x0001`) and
  saturating 70000 to `0x7bff`, and with `.renable` rounding 1.0005 to 1.0 (nearest) or 2.0
  (up). Every byte value through `unpckf32{u8,s8,o8}.scale` and 16 halves through the 16-bit
  forms give the correctly rounded quotient.

Round 2 of the ISA probes (2026-10-22, the same day; tables `colour.toml`, `intdot.toml`,
`fmad16.toml`, `efo.toml`, `fpma.toml`, `fmisc.toml`, `fsingle.toml`, `leftovers.toml`; 30
probes, all settled but `efo-gpiext-instances` and `unpack-c10-int`, which are recorded;
models fitted on about 2500 exploration fills first). **[HW]** throughout:

- **Colour operations:** 1872 `sop2` results (factor isolation, every operation, 24 random
  forms) fit `op(s1 × f1, s2 × f2)` with one rounding of the exact sum over 255; `(40c02080,
  12345678)` through `s1, s2, add` gives `0x79` in byte 0, not the `0x78` of rounding each
  product. `min`/`max` compare the sources (`s1, s2, min` equals `zero.comp, zero.comp, min`);
  `s2scale` doubles the result, also of `min`/`max`; `zeros.comp` turns `s2` into `s2 - 0.5`
  (0xff gives 0x7f) in `add`/`sub`; a leading `comp` complements `s1`; `asat.comp` is
  `min(1 - s1a, s2a)` (not `1 - asat`). `sop3`'s `neg` gives `-op(s1 × f2, s2 × f1)`, its
  `arsop` `-op(s1a × f2, s2a × a)` (an `add` is then always 0), `alrp` and `lrp1` lerp by `s0a`
  and `s0`, `lrp2` adds two selected products; `sop2wm` writes byte N with mask bit N. `.c10`
  operands read signed 10-bit lanes, clamped to ±510, the alpha from lane 0.
- **Integer dot products:** `u8dot4` of `0x01ff00fe` and `0xff00ff01` is `0x02020202` (509/255
  rounded, in every byte); `off` subtracts 128 from every byte (`(0x40c02080, 0x12345678)` gives
  `0x18`); `u16dot4` gives `0x7f7f` for `127 × 510 / 255²` of 32767; `Cx`/`Ax` scale before
  rounding; the `u8dot3` alpha forms read the sources' own alphas, also with `.comp` sources.
- **Floats:** `fmad16` is fused per half (20 random cases, half denormals kept), `.swzf32` reads
  the binary32 exactly (`8209.25 × 1.5 + 3` gives `0x7204`); `efo` with 3, 5, 7 and `i0`, `i1`
  = 11, 13 gives every printed sum (`m0 + m1` = 56, `i1 + i0` = 24, ...), `d = i0` is the old
  11, and 24 random cases round every product and sum; `.gpiext` left `i0`, `i1` alone and
  wrote `i2`, its multipliers reading `i2` and `i0` for the dumping instance, but whenever its
  multipliers or `d` read `i0`/`i1`, 512 of the 2048 pixels got another instance's value
  (`d = i0`: 56 instead of 11), every run; `fadm` of 3, 5, 7 is 56 and `fmsa` 44, and
  24 random cases each fit `a·c + b·c` and `a·a + b·c` with every product rounded; `fsubflr` of
  0.5 and 2.75 is -1.5. `flog` is `log2` (one ulp, or 2^-22 near 1), `fexp` `2^x` (two ulps),
  `fsin`/`fcos` take quarter turns with 15 fraction bits (96 random inputs each).
- **Leftovers:** min/max steps of two NaNs return the second (payload kept);
  `movmsk.<fmt>.smsk` needs bit 0 of `s0` too for `.u8`, `.flt`, `.flt16` but not `.mask` (16
  values of `s0`); `statomic` packs two signed bytes, negatives as 0, into both halves. No
  driver fault, hang, warning or memory growth in the runs, nor in five runs of the whole
  table (154 probes) in a row.

Round 3 of the ISA probes (2026-10-22, the same day; tables `gpiext.toml`, `fds.toml`,
`dual.toml`, `channels.toml`, `rlp.toml`, `smp.toml`, `cache.toml`, `video.toml`,
`leftovers3.toml`; models fitted on about 2000 exploration fills first). `sgxfive-test probe
--coords --pixels` runs a probe as the shader of a copy whose source texel is the pixel's
coordinates and reads all 2048 pixels back, so `coords.s` gives every pixel its own inputs
and a result names the pixel it came from. **[HW]** throughout:

- **Internal registers:** with `i0`-`i2` = `(2048 + p) · 2^(12k)` per pixel `p`, every pixel
  reads its own back; `i3` reads the next pixel's `i0` in the 2x2 quad (top-left reads
  top-right, top-right bottom-left, bottom-left bottom-right, bottom-right top-left). A later
  task without writes sees whole quads of the earlier task's values, quad positions kept.
- **`efo.gpiext`:** the multipliers read the pixel's `i2` and `i0` for `i0` and `i1`, an `i0`
  write lands in its `i2`, and after an `i1` write only the top-right pixel's `i0` changed, to
  the bottom-right pixel's value; during such an instruction the top-right pixel's `i0` reads
  give the top-left pixel's `i0`, and in 11 of 40960 reads the value the top-left pixel was
  writing. 300 jobs of two clients run concurrently never mixed their values.
- **Dual issue:** 144 primary/secondary pairs on 1.5, 2.25, 3 and `i0`-`i2` = 5, 7, 11; the
  clean pairs give both results as if alone; `fmad i0, ...+fadd i1, i0, i2` reads the old
  `i0`, `fmad i1, ...+fadd i1, ...` keeps the secondary's 16; `.repeat2 ...+fadd i0, i0, i1`
  gives 19; `fadm`+`fmul` gives the primary's second product 6.75, `fminmax`+`fadd` the
  comparison's -0.75. The two-bit predicate, 12 states, acts as printed on both.
- **Derivatives:** marks of the quad's pixels through `fdsx d, mark, 0` and `fdsx d, 0,
  -mark` give the top-right and top-left marks in all four pixels (`fdsy`: bottom-left).
- **Sampling:** a 16x16 texture in the scratch buffer, state words in `r20`-`r23`: texel
  `(3, 5)` at `(3.5, 5.5)/16` and at `(3.0, 5.0)/16`, `(2, 4)` just below, `(15, 15)` and the
  edge rows/columns outside `[0, 1)`; `.repeat2` and `.xy` fault at the fourth state word.
- **Test channels:** 360 cases (`iadd8`, `iadd16`, `iadd32`, `fadd`; `taz`, `nat`, `pat`);
  `rlp` over every predicate and value pattern; `adif`/`adifsum` on random bytes; every lane
  value of the `c10` to 16-bit unpacks; cache operations with each pixel on its own dword.

Round 4 (2026-10-22; tables `sampler.toml`, `dual4.toml`, `leftovers4.toml`, `bank7.toml`):

- **Sampler state:** bilinear weights truncated to 1/64, channels rounded to nearest; stride
  textures clamp, twiddled ones repeat, mirror, clamp or mirror once; Morton order `y` first;
  the formats' raw readings and the c10/f16/f32 conversions; PCF `texel OP reference`.
- **Integers:** `idiv` 100/7 gives 14 and 2, `x/0` all ones twice; `imae.usat` of
  `0xffff·0xffff + 0xfffffff0` is `0xffffffff`, `.ssat` of `-1·-1 - 16` is `-15`, `.rs4`
  shifts, `coe` sets `i0` to 1, `cie` adds it; `ima16` u8/s8/o8 bytes; `ima8` `x + y·z`.
- **Dual issue:** `ima32` secondaries on integer and float inputs, the shared pairs with
  1.25, 3.5, 2.5 and `i0`-`i2` = 4.5, 6, 9.
- **Others:** `br #k` counts from the `br`; `Pn` never runs; shift immediates and `~`; the
  16-bit tests see the unwrapped lane; bank 7 per pixel; `i.h`; `.syncs` hangs a fill.

Not probed: `ldaw`/`staw` and pre/post-increment register offsets, the clip-plane output of
`fdpc` (it is in no register a pixel task can read), mipmaps and LOD, `bilin`/`firv`/`firh`
beyond recording them, and the visibility group. The register-file writes
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
