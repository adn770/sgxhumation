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

## Build & run (on cachix — CachyOS, multilib already present)

```sh
# libs staged at ~/sgxhumation-uk/lib/ ; symlink the unversioned sonames:
ln -sf libsrv_um.so.1.7.862890   ~/sgxhumation-uk/lib/libsrv_um.so
ln -sf libsrv_init.so.1.7.862890 ~/sgxhumation-uk/lib/libsrv_init.so
gcc -m32 -shared -fPIC fakedrm.c -o fakedrm.so -ldl
gcc -m32 loader.c -o loader -ldl
LD_LIBRARY_PATH=~/sgxhumation-uk/lib LD_PRELOAD=./fakedrm.so ./loader
cat ~/sgxhumation-uk/drm.log
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

Disassemble the USSE microkernel (reMESA / `sgx540-reversing` tooling), cross-check against
SGX535-reMESA captures, and feed the decoded EDM entrypoints / register writes into
`design/sgx545_hw_spec.md` for the HW bring-up plan.
