# SGXhumation — RE toolchain

Reverse-engineering tools for the project. Everything here is **gitignored** except this file
(the builds are large third-party artifacts — rebuild from source using the notes below).
Static RE of the Cedarview blob's `i386 ELF` binaries needs no Linux hardware, so this runs
anywhere — including a MacBook on the go.

## radare2 (built from source, macOS)

Version 6.2.5, built with the dependency-light **acr** build (needs only Apple clang + GNU
make — no meson/ninja/brew). Installed into a self-contained, workspace-local prefix.

Rebuild:

```sh
cd tools
git clone --depth 1 https://github.com/radareorg/radare2.git
cd radare2
./configure --prefix="$PWD/../radare2-prefix"
make -j"$(sysctl -n hw.ncpu)"
make install
```

Use (add the prefix to PATH for the session):

```sh
export PATH="$HOME/src/sgxhumation/tools/radare2-prefix/bin:$PATH"
radare2 -v          # radare2 6.2.5 @ darwin-arm_64
rabin2 -I <file>    # quick binary triage (arch/bits/class)
```

Update later with `git -C tools/radare2 pull && (cd tools/radare2 && make -j && make install)`.

## What radare2 does / doesn't cover here

- **Covers:** all host-side `i386 ELF` libs in the Cedarview blob
  (`reference/preserved/cedarview-gfx-blob/extracted/.../usr/lib/*.so.1.7.862890`) —
  `libsrv_init`, `libsrv_um`, `libglslcompiler`, the GLES/EGL stack. Ordinary x86; r2's
  wheelhouse.
- **Does NOT cover:** the USSE microkernel / shader bytecode — not a stock r2 architecture.
  That needs the project-specific tooling in `reference/SGX535-reMESA` and
  `reference/sgx540-reversing`.

## Suggested companions (not yet installed)

- **Ghidra** — its decompiler turns these libs into readable C; ideal for tracing how
  `SGXDevInitPart2` assembles the init scripts and where the microkernel blob is embedded.

## First RE target (spec §7.1 critical path)

`libsrv_init.so` is a thin stripped wrapper: `SrvInit` → `SGXGetInfoForSrvInit` +
`SGXDevInitPart2` (both imports). So dissect **`libsrv_um.so`** for `SGXDevInitPart2`: find
where it builds the `SGX_INIT_COMMAND` register-write script and whether the EDM microkernel
image is an embedded data blob. That is the piece Goal-2 ("load the DDK on hardware") needs.
