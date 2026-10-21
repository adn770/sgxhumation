# Ukernel extraction harness (mock-and-intercept)

Goal: pull the SGX545 USSE **microkernel** binary out of the closed Cedarview usermode
(`libsrv_um.so.1.7.862890`) **without GPU, kernel module, or risk to any machine** — by running
the i386 driver under a stubbed kernel interface and capturing the ukernel at the moment it is
copied into "device" memory.

Why this works (see spec §7, §11): the ukernel is static data in the usermode lib; init
*allocates a GPU-mapped buffer, `memcpy`s the ukernel in, writes a few registers, kicks once,
and polls one memory flag*. No per-chunk HW handshake. So a fake `libdrm` + an interposed
`PVRSRVMemCopy` can make the driver reveal the ukernel into a buffer we own.

## Pieces

- `fakedrm.c` → `fakedrm.so` (LD_PRELOAD): the entire kernel interface is `libdrm`
  (`drmOpen` + `drmCommandNone`/`drmCommandWrite`). The shim fakes a device fd, logs the PVR
  **bridge-command sequence**, zeroes each bridge call's OUT buffer so it reads `PVRSRV_OK`, and
  **interposes `PVRSRVMemCopy`** to log/flag large copies (the ukernel upload).
- `loader.c` → `loader`: `dlopen`s `libsrv_um` + `libsrv_init`, calls `PVRSRVConnect` (and will
  drive the SGX device init once connect succeeds).

## Build & run (any x86-64 Linux with 32-bit multilib)

Both programs use one staging directory, `$SGX_UK_DIR` (default `~/sgxhumation-uk`, see
`ukdir.h`): the closed libraries go in its `lib/`, and the shim writes `drm.log` and `dump/`
there.

```sh
export SGX_UK_DIR=~/sgxhumation-uk
# libs staged at $SGX_UK_DIR/lib/ ; symlink the unversioned sonames:
ln -sf libsrv_um.so.1.7.862890   $SGX_UK_DIR/lib/libsrv_um.so
ln -sf libsrv_init.so.1.7.862890 $SGX_UK_DIR/lib/libsrv_init.so
gcc -m32 -shared -fPIC fakedrm.c -o fakedrm.so -ldl
gcc -m32 loader.c -o loader -ldl
LD_LIBRARY_PATH=$SGX_UK_DIR/lib LD_PRELOAD=./fakedrm.so ./loader
cat $SGX_UK_DIR/drm.log
```

## Status (2026-10-19) — UKERNEL EXTRACTED ✅

**`SrvInit` runs to completion (returns 0) and uploads the microkernel; the harness captures
it.** Output in `extracted/` (see `extracted/MANIFEST.md`): the **53096-byte (0xCF68) SGX545
USSE microkernel**, its 5 auxiliary USSE programs, and their provenance. The ukernel is a
static blob in **`libsrv_init.so` at file offset `0x2080`**, patched in 54 bytes (18 × 24-bit
address fields) at init.

### How the init path was solved (the decode chain)

- **Command numbering:** bridge ID low byte = absolute command index. The closed lib is built
  **PDUMP+OVERLAY disabled**, so `SGX_CMD_BASE=83` and `INITSRV_CONNECT=67`. This makes the
  crashing cmds decode to `INITSRV_CONNECT(67)`, `SGX_GETMISCINFO(97)`, `SGXINFO_FOR_SRVINIT(98)`.
- **Fault pinpointing:** `loader.c` installs a `SIGSEGV` handler printing the faulting PC +
  accessed addr + lib bases. First fault was `libsrv_init+0xa9a` reading `[NULL+8]` while
  iterating `asHeapInfo[32]` (24-B stride) and switching on `ui32HeapID & 0xffffff` (IDs 0..6).
  Fix: `SGXINFO_FOR_SRVINIT` returns 7 real heaps (IDs/bases per `sgxapi_km.h`/`sgxconfig.h`;
  **ID 2 = KERNEL_CODE**, where the ukernel goes).
- **The mmap dance:** after that, srvinit does, per buffer, `ALLOC_DEVICEMEM(6)` →
  `MHANDLE_TO_MMAP_DATA(11)` → **`syscall(SYS_mmap2)`** (libsrv_um imports `syscall`+`munmap`,
  **not** `mmap`). The shim fills the 76-B ALLOC OUT (field offsets pinned empirically via
  sentinels: `hMappingInfo` @ `o[11]`, `sCpuPAddr` is 64-bit), answers MHANDLE with a cookie,
  and **interposes `syscall`** to serve a real anon buffer per allocation.
- **Capture:** `PVRSRVMemCopy` into the KERNEL_CODE buffer fires → alloc #9 (heap `beef0002`,
  0xCF68) is the ukernel. Buffers are dumped at `munmap`/exit; the static carve from
  `libsrv_init.so+0x2080` matches the runtime image except the 54 patched bytes.

### Next

Done: the ukernel is disassembled (`tools/usse-dis/`) and its EDM structure decoded (spec
§13). Open items are in spec §13.8 — chiefly the EDM's PDS program and its `sa` binding.

## Init-info capture & decode

`fakedrm.c` also dumps the IN buffer of `SGX_DEVINITPART2` (bridge nr 99) — the complete
`SGX_BRIDGE_INIT_INFO` srvinit builds — to `$SGX_UK_DIR/dump/devinitpart2_in.bin`, kept here as
`extracted/sgx545_initinfo.bin` (with the control-block buffers `alloc00..08_*.bin`).

```sh
../../.venv/bin/pytest -q test_initinfo.py              # layout + capture checks
../../.venv/bin/python initinfo.py extracted/sgx545_initinfo.bin --r2 handlers.r2
```

`initinfo.py` decodes the control-block handles, the per-command EDM handler addresses, the
three register init scripts (with register names), EDM task / clock-gating registers, MKIF
struct sizes and init memory. `sgx545_ukernel.r2` is the combined radare2 annotation script:
the handler labels from `initinfo.py`, the register-access comments from
`tools/usse-dis/regtouch.py`, plus boot labels (`boot.entry`, `boot.main`,
`dpm.state_clear`). Findings: `design/sgx545_hw_spec.md` §13.

## Layout captures (for the fwcutter loader)

`fakedrm.c` takes two more knobs: `MOCK_HEAP_START` (where the first buffer of each heap goes;
default `0x1000`) and `MOCK_HEAP_BASE_<id>` (heap base override, `sgxapi_km.h` heap ids). Two
captures are kept as `extracted/layout_base/` (defaults) and `extracted/layout_moved/`:

```sh
MOCK_HEAP_START=0x5000 MOCK_PDPADDR=0x2345000 MOCK_HEAP_BASE_2=0xE8100000 \
MOCK_HEAP_BASE_3=0xF0100000 MOCK_HEAP_BASE_4=0xF4100000 MOCK_HEAP_BASE_5=0xFC100000 \
MOCK_HEAP_BASE_6=0xE4100000 LD_LIBRARY_PATH=$SGX_UK_DIR/lib LD_PRELOAD=./fakedrm.so ./loader
```

`tools/fwcutter/test_fwload.py` replays both layouts through the reference loader and checks
the result against these dumps, byte for byte.

## 3D capture (the closed GLES2 stack, one triangle)

`tri.c` draws one constant-colour triangle with EGL + GLES2 into a 64×64 pbuffer and calls
`glFinish`. Run under `fakedrm.so`, the whole closed stack (libEGL → libIMGegl → libGLESv2 →
libglslcompiler/libusc → libsrv_um) initialises, compiles both shaders, records the frame and
submits it through two `SGX_DOKICK`s, with no GPU and no kernel. Findings:
`design/sgx545_3d.md`.

Pieces:

- `tri.c` loads libEGL/libGLESv2 with `dlopen` from `$SGX_UK_DIR/lib`, so it needs no closed
  headers. It marks each phase in the bridge log through `fakedrm_mark()`.
- `fakewsegl.c` → `libpvrFAKEWSEGL.so`: our own window-system module, a WSEGL
  (interface version 2: a version word and 14 entry points, as the closed modules export it)
  with one RGB565 configuration (ARGB8888 with `FAKEWSEGL_FORMAT=8888`) and no native
  drawables. libIMGegl loads the module named by
  the `WindowSystem` app hint; `fakedrm.c` redirects `/etc/powervr.ini` to
  `$SGX_UK_DIR/powervr.ini`. A pbuffer needs nothing else from the window system.
- `fakedrm.c` answers the client bridge calls (`bridgecmd.h` names all of them) on top of a
  device-memory model (`devmem.h`): the client heap list, allocations with sync objects,
  client info (with `asInitDevData` from a captured `devinit.bin`), the DDK version and build
  options, the global event object, HW render and transfer context registration. At each
  `SGX_DOKICK` it does what `SGXDoKickKM` does to the shared TA command (sync and status
  fields, `READY`) and the destination sync list, snapshots every live object, then completes
  the kick as the EDM would (status values, sync complete counters).
- `kick3d.py` decodes the kick structures and the VDM control stream and walks the pointers;
  `summary3d.py` lists every allocation with its role and cross references and disassembles
  the PDS and USSE programs (build them with `make -C ../../tools/usse-dis` and
  `make -C ../../tools/usse-dis pds-dis`).

```sh
# on an x86 Linux with 32-bit userspace (natively with gcc -m32, or in tools/arch32)
export SGX_UK_DIR=~/sgxhumation-uk3d          # lib/ = the whole closed usr/lib
cp extracted/sgx545_initinfo.bin $SGX_UK_DIR/devinit.bin
printf '[default]\nWindowSystem=libpvrFAKEWSEGL.so\n' > $SGX_UK_DIR/powervr.ini
gcc -m32 -shared -fPIC fakedrm.c -o fakedrm.so -ldl
gcc -m32 -shared -fPIC fakewsegl.c -o $SGX_UK_DIR/lib/libpvrFAKEWSEGL.so
gcc -m32 tri.c -o tri -ldl
LD_LIBRARY_PATH=$SGX_UK_DIR/lib LD_PRELOAD=./fakedrm.so ./tri
ls $SGX_UK_DIR/dump3d      # kickNN_in.bin + kickNN/ (objects.txt, objNNN_<dva>.bin), exit/
```

Copy `dump3d/` and `drm.log` to `extracted/gles2_tri/` (gitignored but for its `MANIFEST.md`),
then:

```sh
../../.venv/bin/python kick3d.py extracted/gles2_tri/dump3d/kick01_in.bin \
    extracted/gles2_tri/dump3d/kick01
../../.venv/bin/python summary3d.py extracted/gles2_tri/dump3d/kick01_in.bin \
    extracted/gles2_tri/dump3d/kick01 extracted/sgx545_initinfo.bin
```

What one run produces (2026-10-21): 2385 bridge log lines; 133 `ALLOC_DEVICEMEM`s plus two
registered contexts, 153 device objects (6.2 MiB) at each kick, among them a 4.1 MiB
per-context parameter buffer. All of it happens in `eglInitialize`, `eglCreatePbufferSurface`
and `eglCreateContext`; shader compilation and the draw calls make no bridge calls (the draw
allocates two more buffers), and both kicks happen in `glFinish`. The srvinit run is
unchanged: `loader` with this `fakedrm.so` reproduces the init info and the microkernel byte
for byte.

Status: the TA command, render details, RT data, PB descriptor and render context decode
with every field named; the VDM stream, the state, vertex, pixel and end-of-render PDS
programs and their USSE programs are found and disassembled; the fixed-function state words
are decoded from differential captures (below). Open: see the design note's open questions.

### Differential captures

`tri` takes `key=value` options (`triopts.h`), each changing one piece of GL state: size,
pixel format, colours, depth, stencil, blending, culling, colour mask, scissor, viewport,
primitive type, vertex positions, a varying colour. `variations3d.txt` lists one run per
line and `capture3d.sh` runs them, each in `$SGX_UK_DIR/var/<name>/` (links to `lib/`,
`devinit.bin`, `powervr.ini`; `SGX_UK_LIB` picks another `lib/`). The whole list takes a
few seconds.

```sh
SGX_UK_DIR=~/sgxhumation-uk3d ./capture3d.sh variations3d.txt          # or name...
# copy $SGX_UK_DIR/var/ to extracted/gles2_tri/var/ (gitignored; MANIFEST.md lists it)
../../.venv/bin/python state3d.py extracted/gles2_tri/var/base        # every state word
../../.venv/bin/python diff3d.py --all extracted/gles2_tri/var         # each against base
```

`state3d.py` names every state-carrying word of both kicks under a stable path (TA command,
register updates, render details, RT data, background object, the special object and
end-of-render programs, the end-of-tile PBE words, and per VDM block its PDS program, what it
DMAs, the USSE programs it starts and the decoded MTE state block). `diff3d.py` hides address
shifts and lists every other change with the flipped bits and the decoded fields. Findings:
`design/sgx545_3d.md`, "State words (differential captures)".
