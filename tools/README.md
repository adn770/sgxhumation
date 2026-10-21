# SGXhumation — tools

Host-side tools of the project. None of them needs the Cedarview hardware: they run on any
Linux or macOS machine with a C11 compiler, GNU make and Python 3.10+ (pytest for the tests).

## What is tracked

- `usse-dis/`: clean-room USSE and PDS disassemblers (`usse-dis`, `pds-dis`), the oracle
  checks, and `r2/`, the radare2 `usse` arch plugin. See its README.
- `usse-dis/refdisasm/`: sources of a local reference disassembler built from the PVR_PSP2
  corpus in `reference/`. The binaries are never distributed.
- `usse-asm/`: clean-room USSE and PDS assemblers (`usse-asm`, `pds-asm`) and `progs/`, the
  transfer programs as our own assembly source.
- `fwcutter/`: `sgx545-fwcutter`, which cuts `sgx545_ukernel.fw` out of your own copy of the
  closed `libsrv_init.so`, and the reference loader `fwload.py`.
- `arch32/`: Arch Linux 32 Docker images for building i686 userspace (IGT).

Everything else under `tools/` is gitignored: build outputs, the cut `.fw` files, and the local
radare2 source tree and install prefix described below.

## radare2 (built from source)

radare2 reads the host-side `i386 ELF` libraries of the Cedarview blob (`libsrv_init`,
`libsrv_um`, `libglslcompiler`, the GLES/EGL stack) like any x86 code, and with the plugin in
`usse-dis/r2/` it also disassembles and assembles USSE code. Version 6.2.5 is the one in use.
The dependency-light **acr** build needs only a C compiler and GNU make (no meson or ninja),
and installs into a prefix inside `tools/`:

```sh
cd tools
git clone --depth 1 https://github.com/radareorg/radare2.git
cd radare2
./configure --prefix="$PWD/../radare2-prefix"
make -j"$(getconf _NPROCESSORS_ONLN)"
make install
```

Use it with the prefix on `PATH`, from `tools/`:

```sh
export PATH="$PWD/radare2-prefix/bin:$PATH"
radare2 -v          # radare2 6.2.5
rabin2 -I <file>    # quick binary triage (arch/bits/class)
```

Update later with `git -C tools/radare2 pull && make -C tools/radare2 -j && make -C tools/radare2
install`.

The out-of-tree plugin (`make -C usse-dis/r2 install`) builds against this prefix by default;
set `R2_PREFIX` to use a system radare2 instead. The in-core `usse` and `pds` plugins, with the
ESIL emulation, live in a radare2 fork, which `tools/radare2` can be a clone of instead of
upstream: `usse-dis/r2/README.md` explains the fork, `usse-dis/r2/sync-fork.sh` copies the
decoder cores into it, and `usse-dis/r2/upstream/` holds its patches.
