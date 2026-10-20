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
