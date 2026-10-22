# Upstream PR: PowerVR SGX Series-5 USSE arch plugin → radareorg/radare2

Branch **`arch-usse`** of the fork **github.com/adn770/radare2**, 11 commits on
`a7a1b17` (radare2 master). The patches here are `git format-patch a7a1b17..arch-usse`.

Update (2026-10-22): commits 9-11 sit on the published `839734d` in the local branch
**`arch-usse`** (`613196a`), a fast-forward of the fork's published `arch-usse`: push it when
ready. Commit 9 copies the current cores (`../sync-fork.sh`), commits 10 and 11 extend the
ESIL; commit 11 follows the hardware probes (`../README.md`, "Hardware probes"). Commits 1-10
are unchanged (only their `N/11` subject numbering moved).

Status (2026-10-21): the branch was rebuilt twice so that no firmware word is left in it (see
[Provenance](#provenance)).
- First rebuild (`arch-usse-rewrite`): no test carries firmware words. Only the test files
  changed, and the commit messages only where they describe the tests (commits 2, 3, 6 and 7).
  Tests for control forms the early decoders matched only as whole words arrive with the commit
  that decodes them in general (6 or 7).
- Second rebuild (`arch-usse-rewrite2`): no code carries them either. Commits 1 and 6 lose the
  first decoder's exact-match cases (whole firmware words with their text: `ci`/`cfi`,
  `emitpds`, `smlsi`, `phas`, `lock #0`, `lapc` and a few ALU and load/store corners). Those
  words do not decode until commit 7 brings the general forms. Commit 1 also loses its
  exception for the two `.fcfill` firmware loads, which it now prints as `.bpcache` like every
  other load of their form, until commit 6 reads the cache bits per core. Its message lists
  `idf`/`wdf`/`nop` as the control forms and 98% (was 99.9%) as its microkernel agreement.
  Commits 2-5 and 7-8 change only by the `usse.c` they inherit; the tip tree is identical to
  `arch-usse-rewrite`.

The branch to publish is **`arch-usse-rewrite2`** (`839734d`) in the local fork: move
`arch-usse` to it (`git reset --hard arch-usse-rewrite2` on `arch-usse`), then force-push it to
the fork before the PR is opened. The original tip is kept as `arch-usse-backup-2026-10-21`.

Open the PR: https://github.com/adn770/radare2/pull/new/arch-usse, with base
`radareorg/radare2:master`.

## Commits

1. `##arch: add PowerVR SGX Series-5 USSE disassembler`: the RArch decode plugin + core
   (control forms with too few samples to fix their fields stay undecoded until commit 7).
2. `##asm: USSE pseudo syntax + disassembler tests`: `asm.pseudo` rendering +
   `test/db/asm/usse` (21 tests).
3. `##arch: USSE register profile, op metadata, ESIL`: `regs.h` (249-reg profile),
   `op->val`/`direction`, and ESIL for the scalar control subset (emulation/`aae` resolves
   computed addresses). Layered on the unchanged decoder; disassembly text is unchanged.
4. `##arch: add PowerVR SGX Series-5 PDS data sequencer disassembler`: a second, separate
   `pds` arch (4-byte instructions) for the programs that start USSE tasks; the microkernel is
   entered through them. Op types + branch targets; `test/db/asm/pds`.
5. `##arch: USSE load/store fetch and increment semantics`: `memop.h` models what
   `ldad`/`stad` actually access (a base+offset `.fetchN` starts one element after the printed
   offset; post-increment fetches write `b += N`). ESIL and `asm.pseudo` follow it; text
   unchanged. Tests `test/db/cmd/cmd_pseudo_usse`, `test/db/esil/usse`.
6. `##arch: Add USSE and PDS assemblers`: encode callbacks on the shared form table; the asm
   tests become `ad` (both directions), 25 USSE tests.
7. `##arch: Decode and assemble the whole USSE instruction set`: the complete core; 34 USSE
   tests, adding the control forms the earlier decoder did not decode in general.
8. `##arch: Emulate the USSE test group, multiply-adds and floats`: ESIL for the 32-bit test
   group, `rol`/`asr`, `~src`, `imae`/`ima32`, `fmad`/`frcp`/`frsq`/`fsqrt` and `lapc`
   (`pclink` is an instruction index); repeats, write masks and registers outside the
   profile get none. 9 new `test/db/esil/usse` tests (`../README.md` lists the unmodelled
   forms).
9. `##arch: Sync the USSE and PDS cores with sgxhumation`: the assembler writes the operands
   that `emitvcbst`/`emitvcbvtx`/`emitmtest` do not print as the closed compiler's programs
   do; SPDX lines on the core files. 3 new `ad` tests.
10. `##arch: Emulate USSE register-file accesses and mov repeats`: ESIL for `ldr`/`str` (the
    SGX register file, dword index × 4, in a window at `0x100000000`), and for unpredicated
    `mov.repeatN` between disjoint ranges. 5 new `test/db/esil/usse` tests; 81.0% of the
    microkernel's words have ESIL (was 71.7%).
11. `##arch: Emulate the USSE forms pinned by hardware probes`: what our own probe programs
    measured on an SGX545: test sign `p` (`>= 0`) and `t` (true) in joins, the adds' 16-bit
    src1 (the 32-bit sum was wrong), `isub16`/`isubu16` and `ima16` lanes, register shift
    amounts, the index register (`il`/`ih` in the profile, `r[il + #n]`, `sa[il + #n]`),
    register offsets of loads and stores, `ldr`'s 14-bit register index, ordered and
    predicated `mov` repeats, and `fmad`'s rounding and denormal flushing. 8 new
    `test/db/esil/usse` tests; 84.2% of the microkernel's words have ESIL.

## Files

| file | change |
|------|--------|
| `libr/arch/p/usse/plugin.c` | new: RArch adapter (decode → mnemonic, type, jump/fail) |
| `libr/arch/p/usse/usse.c` / `usse.h` | new: decoder core (r2-independent) |
| `libr/arch/p/usse/pseudo.c` | new: RAsm pseudo-C syntax (`asm.pseudo`) |
| `libr/arch/p/usse.mk`, `libr/asm/p/usse.mk` | new: acr build fragments |
| `libr/arch/meson.build`, `libr/asm/meson.build` | +sources |
| `libr/meson.build` | +`usse` in `arch_plugins` and `asm_plugins` |
| `libr/include/r_arch.h`, `libr/include/r_asm.h` | +extern decls |
| `dist/plugins-cfg/plugins.def.cfg` | +`arch.usse`, `asm.usse` |
| `test/db/asm/usse` | new: 21 USSE `d` tests; `ad` and 25 in commit 6, 34 in commit 7, 37 in 9 |
| `libr/arch/p/pds/{plugin.c,pds.c,pds.h}`, `libr/arch/p/pds.mk` | new: PDS arch plugin + core |
| `test/db/asm/pds` | new: 33 PDS tests; commit 6 adds 9 canonical `ad` twins |
| `libr/arch/p/usse/memop.h` | new: load/store addressing model shared by ESIL and pseudo |
| `test/db/cmd/cmd_pseudo_usse`, `test/db/esil/usse` | new: pseudo + ESIL/emulation tests |

(`libr/config.mk` / `libr/config.h` / `plugins.cfg` are generated by `./configure`, not
committed.)

## Provenance

- The decoders and encoders were written by us. Their output syntax follows Imagination's
  `ref_usse_disasm` and the PDS disassembler, so that listings can be compared line by line.
- Those reference disassemblers were built locally from **PVR_PSP2**, an
  Imagination-confidential source tree that circulates publicly without a licence. They were
  used as black-box oracles: we fed them words and compared their text with ours. Its headers
  were also consulted for instruction and field names. No code or text from it is copied into
  these patches, and it is not needed to build or test them.
- The validation corpus was the SGX545 microkernel and PDS entry programs from Intel's
  Cedarview driver package, plus the SGX540 ukernel/TA/3D/SPM programs and random words. None of
  it is included.
- The tests use instructions we wrote for them, encoded with our own assembler
  (`tools/usse-asm` in sgxhumation, the same form table as commit 6). No test word comes from
  firmware. The expected text is our disassembler's, with the semantics the oracle showed. For
  the ESIL tests, the expected values follow the modelled semantics.

## PR description (paste into GitHub)

> **##arch: PowerVR SGX Series-5 USSE disassembler**
>
> Adds a `usse` architecture plugin for the PowerVR SGX USSE ISA (Series-5 / SGX5xx), with
> fixed 8-byte little-endian instructions. It decodes and assembles the whole instruction set
> (`mov`/`limm`, integer and float ALU, loads/stores, texture sampling, control flow, cache and
> MOE control), with `op->type` + jump/call/fail so analysis and graphs work. It also includes
> `asm.pseudo` rendering, a register profile and ESIL for a conservative subset (the SGX register
> file that `ldr`/`str` reach is emulated in a window above 4 GiB). `arch usse`, `bits 32`,
> little-endian.
>
> Also adds a `pds` architecture for the SGX **PDS** (Programmable Data Sequencer), with fixed
> 4-byte instructions (`movs` to the `dout*` issue ports, `mov16`–`mov128`, `load`/`store`,
> ALU, `tstz`/`tstn`, `bra`/`call`/`halt`), which also assembles.
>
> How it was made: the decoders were written independently. They were checked against
> Imagination's reference disassemblers, built locally from a third-party copy of their
> (confidential) source and used only as black-box oracles. Their headers were consulted for
> instruction and field names. No vendor code or text is included. On the SGX545 microkernel,
> 99.9% of the instructions decode identically (the rest are padding words), with 0 wrong
> across 17k+ SGX540/SGX545 instructions. The PDS decoder matches its oracle on the
> microkernel's entry programs and on a 128k-word random corpus. The tests use instructions we
> wrote and assembled ourselves, not firmware.

## Verification (2026-10-21, `arch-usse-rewrite2`; commits 9-10 on `arch-usse-esil`; 11: 10-22)

- meson debug build and install of every commit; `r2r` on the test files present at that
  commit passes with no failures:

  | commit | `asm/usse` | `asm/pds` | `esil/usse` | `cmd/cmd_pseudo_usse` |
  |--------|-----------:|----------:|------------:|----------------------:|
  | 1      | -          | -         | -           | -                     |
  | 2, 3   | 21         | -         | -           | -                     |
  | 4      | 21         | 33        | -           | -                     |
  | 5      | 21         | 33        | 2           | 2                     |
  | 6      | 25         | 42        | 2           | 2                     |
  | 7      | 34         | 42        | 2           | 2                     |
  | 8      | 34         | 42        | 11          | 2                     |
  | 9      | 37         | 42        | 11          | 2                     |
  | 10     | 37         | 42        | 16          | 2                     |
  | 11     | 37         | 42        | 24          | 2                     |

- No test or code, in any commit, contains a 64-bit word of the SGX545 microkernel (with its
  auxiliary kernel programs and init info) or of the SGX540 ukernel/TA/3D/SPM programs, nor a
  non-zero 32-bit word of the PDS programs (`git log -p` scan of hex in either byte order).
  The only matches are form signatures, fixed opcode bits with the operand fields zero, which
  equal the zero-operand instance of the form: `nop`, `idf`/`wdf` and the two `pclink` moves
  (all eight commits), and the class-5 `mov.end` form, a case of commit 1's register-move
  switch (commits 1-5). Commits 9 to 11 add no such word either, nor one of the closed
  driver's captured 3D programs (commit 11's tests first had four instructions that equal
  microkernel words, such as `mov i.l, r1`; their operands were changed).
- Commit 11's ESIL agrees with an independent Python model of the measured semantics
  (`../esil_check.py cross`) on 7000 random modelled instructions (seeds 7, 1, 2), and every
  ESIL token on the microkernel is an operator, a number or a profile register.
- Microkernel agreement with the tip decoder (0 wrong against `ref_usse_disasm`): commit 1, 98.3%
  (one `.fcfill` load printed as `.bpcache`); commit 6, 98.5% (0 wrong); tip, 99.9%.
- Earlier check, not repeated on the rebuilt branches (the tip tree is unchanged): `rasm2 -L`
  lists `pds`; `af`/`afb` on the SGX545 event-dispatch program builds the expected 15 basic blocks.
