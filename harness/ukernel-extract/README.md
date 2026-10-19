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

## Status (2026-10-19)

**`PVRSRVConnect` + `SrvInit` now succeed (return 0); the harness drives the closed driver
through its init.** Fixes that got us here (from disassembly of `PVRSRVConnect`→`0x6840`→
`fcn.0000b9d0`): `drmOpen` returns a *real* `/dev/null` fd (so the driver's `fcntl`/`close`
work), and `drmCommandNone(21)` returns **non-zero** (the connect helper tests it as a boolean
and treats 0 as failure). Each bridge call's OUT is zeroed → reads `PVRSRV_OK`.

**Observed bridge sequence** (generic PVRSRV cmds, `_IOWR('g',…)` encoded, nr = id&0xff):
`CONNECT`(0) → `ENUM_DEVICES`(12) → `ACQUIRE_DEVICEINFO`(13) → cmds 67/68.

**Now implemented** (`ok_out()` is a per-command responder, `nr = bridgeID & 0xff`,
`CORE_CMD_FIRST=0`): `ENUM_DEVICES`(nr 0) reports **1 SGX device (type 7)**; `ACQUIRE_DEVICEINFO`
(nr 1) returns a non-null `hDevCookie`. Result: `SrvInit` now **attempts the full SGX device
init** — `PVRSRVConnect` reaches error 26 (past enum/acquire), `SrvInit` segfaults on the
following device-init commands (`nr=0xa1/0xa2`) because our zeroed OUT returns null/fake handles
it then dereferences.

**Next step — the multi-command init mock (a few iterations):**
1. Make handle-returning OUTs point into a **zeroed arena** (one static `calloc`) instead of
   null/`0xDEAD0001`, so the driver's handle derefs read 0 and don't segfault — this should
   leapfrog several crashes at once. Decode `nr=0xa1/0xa2` and the following cmds against
   `pvr_bridge.h` / `sgx_bridge.h` and fill only what the driver actually reads.
2. For `AllocDeviceMem`/the SGX-code-heap alloc, return a **real host buffer** (and record it);
   the ukernel `PVRSRVMemCopy` into it fires → the interposer dumps `src`+bytes.
3. Carve the ukernel; disassemble the USSE with reMESA/sgx540 tooling.

The `PVRSRVMemCopy` interposer already logs every copy's `src`; the first run that reaches the
ukernel copy reveals its location in `libsrv_um` directly.
