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

**Blocker / next step:** no ukernel copy yet — our zeroed `ENUM_DEVICES`(12) OUT reports **zero
devices**, so `DevInitSGX` (which uploads the ukernel) is never reached. To finish:
1. Craft the OUT of **`ENUM_DEVICES`(12)** to list one device and **`ACQUIRE_DEVICEINFO`(13)** to
   return a valid **SGX545** device (type 7). Structs: `PVRSRV_BRIDGE_OUT_ENUMDEVICE` /
   `PVRSRV_DEVICE_IDENTIFIER` in `reference/sgx545-ce/src/services4/include/` + `pvr_bridge.h`.
   (The shim already has `ok_out()`; add a per-`bridgeID` responder keyed on `nr`.)
2. Then `DevInitSGX...` runs; mock `AllocDeviceMem` to hand back a host buffer; the ukernel's
   large `PVRSRVMemCopy` fires and the interposer dumps `src`+bytes → carve the ukernel.
3. Disassemble the USSE with reMESA/sgx540 tooling.

Alternative if the mock RE proves slow: the `PVRSRVMemCopy` interposer already logs every copy's
`src`; once any run reaches the ukernel copy, `src` points straight at the blob in `libsrv_um`.
