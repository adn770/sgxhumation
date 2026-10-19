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

**Foundation built & validated.** On cachix: `libsrv_um`/`libsrv_init` load (all i386 deps
resolve against system `/usr/lib32`), `PVRSRVConnect` executes and opens the DRM device with the
real busid (`pci:0000:00:02.0`), the fake libdrm intercepts the calls, and `PVRSRVMemCopy` is
interposed and ready to dump the ukernel.

**Blocker / next step:** `PVRSRVConnect` returns error `4` after `drmCommandNone(idx=21)` +
`drmDropMaster` — the services connect bails *before* the init/ukernel path. To proceed:
1. Disassemble `PVRSRVConnect` (in `libsrv_um`) to see what it checks after `drmCommandNone(21)`
   and why it returns 4; satisfy it (likely a specific OUT value / a `drmCommandWrite` connect
   with a version/build-options reply — use the DDK structs in `reference/sgx545-ce` headers).
2. Then drive `PVRSRVEnumerateDevices` → `PVRSRVAcquireDeviceData(SGX)` → `DevInitSGX...`,
   faithfully mocking those bridge OUTs (device list reporting an SGX545, handles, and an
   `AllocDeviceMem` that returns a host buffer).
3. When the ukernel's large `PVRSRVMemCopy` fires, the interposer dumps `src` (its location in
   `libsrv_um`) and the bytes → carve the ukernel; disassemble USSE with reMESA/sgx540 tooling.

Alternative if the connect RE proves slow: hook `PVRSRVMemCopy` + walk the static blob by
analysing `libsrv_um` data directly (the copy's `src` points straight at it).
