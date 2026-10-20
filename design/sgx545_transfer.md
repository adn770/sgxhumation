# SGX545 transfer command — host contract and EDM behaviour

SGX545 has no 2D core (`sgx545_hw_spec.md` §13.1). Every blit, fill and copy goes through the
**transfer queue (TQ)**: the host queues an `SGXMKIF_TRANSFERCMD` in a per-context client CCB
and kicks `SGXMKIF_CMD_TRANSFER` through the kernel CCB. The EDM then runs the command as a
small **3D render with no TA phase**: it loads ISP/pixel-pipe registers from the command,
starts the ISP on a host-built control stream, and a USSE pixel shader plus an end-of-tile
program write the result through the pixel back end (PBE).

This document records what the host must provide for one transfer, the exact structure
layouts for our build, what the EDM handler at `0x818` reads and programs, and a minimal
recipe for the first hardware test.

Conventions:

- **Sources.** `CDV` = Intel's GPL Cedarview kernel,
  `reference/preserved/cedarview-gfx-blob/extracted/cdv-gfx-drivers-1.0.3_bee/src/`
  `cedarview-kernel-v1.0.3_bee.patch`, files under `drivers/staging/cdv/pvr/`; cited as
  `sgxtransfer.c:SGXSubmitTransferKM`. `PSP2` = `reference/PVR_PSP2` (DDK **1.8@869593**,
  the closest client-side source we have; Imagination headers carry an "all rights reserved"
  notice, so this repo takes only field names, offsets and facts from it, never code).
  `OPVR` = `reference/linux_openpvrsgx/drivers/gpu/drm/pvrsgx/<version>`.
- Ukernel locations are **byte offsets** into `sgx545_ukernel_template.bin` (instruction
  index × 8), as in `sgx545_hw_spec.md` §13.
- **[infer]** marks inferences. Everything else is read from source, from the captured
  struct sizes (§13.3), or from the ukernel disassembly.

## 0. Build configuration that matters here

- `TRANSFER_QUEUE` is on (CDV `Makefile`); `SUPPORT_SID_INTERFACE`, `PDUMP`, `NO_HARDWARE`
  and `FIX_HW_BRN_31620` are off. So the bridge passes plain handles and the kernel takes the
  non-SID `PVRSRV_TRANSFER_SGX_KICK`.
- The **kernel** feature list for SGX545 (CDV `hwdefs/sgxfeaturedefs.h`) is trimmed. The
  **client** list (`PSP2 hwdefs/sgxfeaturedefs.h`, `#if defined(SGX545)`) is the full one and
  adds, among others, `feature_tag_yuv_to_rgb` (without `_HIPREC`) and
  `feature_tag_lumakey`. Both change the `SGXMKIF_TRANSFERCMD` layout (§2.3), and the
  captured size `0x4c8` confirms that the ukernel was built with them.
- Not defined for SGX545: `feature_pixel_pdsaddr_full_range`,
  `feature_use_high_precision_fir_coeff`, `feature_isp_context_switch`,
  `SGX_FEATURE_FAST_RENDER_CONTEXT_SWITCH`, `SGX_FEATURE_2D_HARDWARE`, `cr_4kx4k_render`.
  Defined: `cr_double_pixel_partitions` (`0x148`, `PSP2 hwdefs/sgx545defs.h`).
  `SUPPORT_DISPLAYCONTROLLER_TILING` must be off (it would make the command `0x4cc`).
- Limits (CDV `include4/sgxapi_km.h`): `SGX_MAX_SRC_SYNCS = 8`, `SGX_MAX_DST_SYNCS = 1`,
  `SGX_MAX_TRANSFER_STATUS_VALS = 2` (so `SGXTQ_MAX_STATUS = 4`),
  `SGX_MAX_TRANSFER_SYNC_OPS = 5`. Client side (`PSP2 include4/sgxapi.h`):
  `max_transfer_updates = 5`, so `max_updates = 5 + 96 = 101`.
- `PVRSRV_DEVICE_SYNC_OBJECT` is **16 bytes** in DDK 1.7/1.8 (`servicesint.h`):
  `ui32ReadOpsPendingVal`, `sReadOpsCompleteDevVAddr`, `ui32WriteOpsPendingVal`,
  `sWriteOpsCompleteDevVAddr`. From 1.9 on it grows to 24 bytes (`ReadOps2` pair), so later
  layouts cannot be reused.

## 1. Host side: from client to kernel CCB

### 1.1 Objects and who owns them

- **Client (transfer) CCB.** Allocated by userspace `create_ccb` in client memory the kernel
  can map: a circular byte buffer of `SGXMKIF_TRANSFERCMD`s.
- **Client CCB control.** Allocated by userspace: `PVRSRV_SGX_CCB_CTL` (8 B), write offset
  at `+0`, read offset at `+4`.
- **HW transfer context.** Allocated by the kernel (`SGXRegisterHWTransferContextKM`) in
  KERNEL_DATA (`0xF0000000`): `SGXMKIF_HWTRANSFERCONTEXT` (`0xb4`).
- **Sync objects.** `PVRSRVAllocSyncInfo`, SYNCINFO heap (`0xE0000000`): `PVRSRV_SYNC_DATA`
  with write/read pending and complete counters.
- **Fence-ID word.** Userspace (`fence_id_mem_info`), any GPU-visible heap: the last
  completed blit's fence ID.
- **TQ programs and streams.** Userspace circular buffers in the PDS, USSE and control heaps
  (§4).

Client CCB constants (`PSP2 client_mkif.h`): `ccb_size = 64 KiB`,
`ccb_alloc_gran = 64`. Offsets wrap with `update_ccb_offset(off, size, ccbsize) =
(off + size) & (ccbsize - 1)` (`PSP2 utils_client.h`). The CCB control's write offset is
advanced by the client; the read offset is advanced by the EDM.

### 1.2 Creating the context — `SGXRegisterHWTransferContextKM`

Bridge `PVRSRV_BRIDGE_SGX_REGISTER_HW_TRANSFER_CONTEXT` (`SGX_CMD_BASE + 26`, CDV
`sgx_bridge.h`). IN: `hDevCookie`, `pHWTransferContextCpuVAddr` (a user copy of the whole
context), `ui32HWTransferContextSize`, `ui32OffsetToPDDevPAddr`, `hDevMemContext`. OUT:
handle and `sHWTransferContextDevVAddr`.

`sgxutils.c:SGXRegisterHWTransferContextKM`:

1. Allocates `ui32HWTransferContextSize` bytes, 32-byte aligned, in **KERNEL_DATA** with
   `READ|WRITE|NO_SYNCOBJ|edm_protect|CACHE_CONSISTENT`.
2. Copies the user's context image in verbatim. **The client fills every field**; the kernel
   does not interpret it.
3. Looks up the memory context and writes its page-directory physical address
   (`pfnMMUGetPDDevPAddr`) at `ui32OffsetToPDDevPAddr`, which is
   `offsetof(SGXMKIF_HWTRANSFERCONTEXT, common.sPDDevPAddr)` = **`0x8`** (§2.2).
4. Returns the device virtual address. Unregistering (`…+27`) sends
   `SGXMKIF_CMD_CLEANUP` type `PVRSRV_CLEANUPCMD_TC` (3) with this address
   (`SGXCleanupHWTransferContextCallback` → `SGXCleanupRequest`).

So for our port the context is just a `0xb4`-byte block in KERNEL_DATA that we fill
ourselves. The closed `libsrv_um` `SGXCreateTransferContext` (`0x112d0`) builds its template
as: `+0x00 = 1`, `+0x04 = 0` (priority), `+0x08 = 0` (patched by the kernel), `+0x0c` CCB
device address, `+0x10` CCB-control device address, `+0x14 = 0`, the snapshots zero,
`+0xa8` = PID, `+0xac`/`+0xb0` = 0. Flag `0x1` is `hwc_flags_new_context`
(`PSP2 client_mkif.h`); the EDM flushes the PDS/USE caches on the first kick and clears
it (§3.1).

### 1.3 Building the command — userspace

`PSP2 transfer_queue.c:SGXQueueTransfer` → `dispatch_3d_blit` → one `Prepare*` function per
blit type fills a `SGXMKIF_TRANSFERCMD` in host memory plus a `PVRSRV_TRANSFER_SGX_KICK`.
Common setup in `dispatch_3d_blit`:

- `ui32Flags |= SGXMKIF_TQFLAGS_TATQ_SYNC / _3DTQ_SYNC` on request; `KEEPPENDING` /
  `NOSYNCUPDATE` per the caller's flags; `DUMMYTRANSFER` (`0x20`) for no-op passes.
- `fence_id` = the context's running fence counter (pre-increment).
- `sHWRegs.double_pixel_partitions = 0`.
- `num_pixel_partitions = default_max_num_pixel_partitions (31) <<
  cr_dms_ctrl_max_num_pixel_partitions_shift (22)` = **`0x07c00000`** for SGX545
  (`PSP2 info_client.h`).

`SGXSubmitTransfer` then, per command:

1. Real size = `sizeof(SGXMKIF_TRANSFERCMD)` minus the unused tail of `updates[]`; rounded
   up to 64 bytes and stored in `ui32Size`.
2. `sgx_acquire_ccb` reserves space; `psKick->ui32SharedCmdCCBOffset = WriteOffset +
   offsetof(SGXMKIF_TRANSFERCMD, shared_state)` (= `+0xac`).
3. Appends the caller's status updates and then **the fence update** to
   `shared_state.sCtlStatusInfo[]`: `{fence_id_mem_info->sDevVAddr, fence_ids[i]}`.
4. Copies the command into the CCB, advances `*pui32WriteOffset` by `ui32Size`, sets
   `psKick->sHWTransferContextDevVAddr`, and calls the `SGX_SUBMITTRANSFER` bridge.

The closed Cedarview `libsrv_um.so.1.7.862890` does the same (radare2 on
`reference/preserved/cedarview-gfx-blob/extracted/cedarview-userspace-v1.0.3_bee/usr/lib/`):

- `SGXCreateTransferContext` (`0x112d0`) allocates the client CCB as **`0x10000 + 0x4c8`**
  bytes (one maximum command of overrun past the wrap point) and an 8-byte control block
  (`+0` write, `+4` read offset). It also allocates the TA/3D sync objects, the fence word,
  "USSE code" (`0x800`), "ISP stream" (`0x4800`), "PDS code" (`0x1800`) and "PDS - 1src"
  (`0x400`) buffers, and copies its built-in transfer programs into device memory (§4.3).
  It registers the context with bridge `0xc01c676d` (`SGX_CMD_BASE + 26`).
- `SGXQueueTransfer` (`0x1b250`) rejects `transfer_dispatch_disable_3d`: every transfer
  runs on the 3D pipe. Its submit loop sets `ui32Size = (0x1a0 + 8 × NumUpdates + 0x3f) &
  ~0x3f`, copies `0x1a0 + 8 × NumUpdates` bytes into the CCB, advances the write offset and
  calls bridge `0xc01c6760` (`SGX_CMD_BASE + 13`, in-size `0x58`). It retries while the
  bridge returns `PVRSRV_ERROR_RETRY`.
- `libpvr2d` has no 2D-core path: `PVR2DBlt` → `PVR2DBltClipped` → `SGXQueueTransfer`. The
  X driver (`pvr_drv.so`) calls `SGXQueueTransfer` for copies and composites but does solid
  fills in software ("[PVR] Soft solid fill always").

### 1.4 Kernel submit — `SGXSubmitTransferKM`

Bridge `PVRSRV_BRIDGE_SGX_SUBMITTRANSFER` (`SGX_CMD_BASE + 13`), IN =
`{ui32BridgeFlags, hDevCookie, PVRSRV_TRANSFER_SGX_KICK sKick}`. The kick (CDV
`services4/include/sgxinfo.h`) is `hCCBMemInfo`, `ui32SharedCmdCCBOffset`,
`sHWTransferContextDevVAddr`, `hTASyncInfo`, `h3DSyncInfo`, `ui32NumSrcSync`,
`ahSrcSyncInfo[5]`, `ui32NumDstSync`, `ahDstSyncInfo[5]`, `ui32Flags`, `ui32PDumpFlags`,
`hDevMemContext`.

`sgxtransfer.c:SGXSubmitTransferKM` touches only the shared part of the command, in the
client CCB:

1. Bounds-checks the offset (`CCB_OFFSET_IS_VALID`) and maps
   `shared_ptr = CCB base + ui32SharedCmdCCBOffset`.
2. TA and 3D syncs (only with `TATQ_SYNC` / `3DTQ_SYNC`): `ui32{TA,3D}SyncWriteOpsPendingVal =
   WriteOpsPending++`, `…ReadOpsPendingVal = ReadOpsPending`, and the two complete addresses.
   Without them both addresses are written 0.
3. De-duplicates the source and destination syncs by `sWriteOpsCompleteDevVAddr` and stores
   the counts in `ui32NumSrcSyncs` / `ui32NumDstSyncs`.
4. Unless `KEEPPENDING`: snapshots each sync into `asSrcSyncs[]` / `asDstSyncs[]` (current
   pending values, complete addresses), **then** bumps `ReadOpsPending++` on sources and
   `WriteOpsPending++` on destinations.
5. Builds the kernel command: `sCommand.ui32Data[1] = sHWTransferContextDevVAddr`, all other
   words 0, and calls `SGXScheduleCCBCommandKM(…, SGXMKIF_CMD_TRANSFER, …)`. That fills
   `ui32ServiceAddress = aui32HostKickAddr[TRANSFER]` (`0x103`) and the accumulated
   `ui32CacheControl`, and kicks as in `sgx545_hw_spec.md` §13.10.
6. On `PVRSRV_ERROR_RETRY` it rolls back every pending increment.

**The kernel never writes the context's CCB pointers or the command's register block.** Its
only per-command inputs to the EDM are the shared block and `ui32Data[1]`.

### 1.5 Sync objects and completion

`PVRSRV_SYNC_DATA` (CDV `include4/servicesext.h`) is `ui32WriteOpsPending`,
`ui32WriteOpsComplete`, `ui32ReadOpsPending`, `ui32ReadOpsComplete`, `ui32LastOpDumpVal`,
`ui32LastReadOpDumpVal`. `sWriteOpsCompleteDevVAddr` / `sReadOpsCompleteDevVAddr` point at
the `…Complete` words. The protocol:

- The EDM starts a command only when, for every source and destination sync,
  `*read_ops_complete == ReadOpsPendingVal` and `*WriteOpsComplete == WriteOpsPendingVal`
  (§3.1 step 5). A null address skips the check, so **a command with no syncs is always
  ready**.
- On completion it writes `WriteOpsComplete = PendingVal + 1` for the TA/3D blocks, every
  `sCtlStatusInfo[i]` value to its address (the fence ID among them), and, unless
  `NOSYNCUPDATE`, `read_ops_complete = ReadOpsPendingVal + 1` per source and
  `WriteOpsComplete = WriteOpsPendingVal + 1` per destination (§3.1 step 8).
- The EDM then raises **`EUR_CR_EVENT_STATUS.SW_EVENT`** (bit 14). The init script enables
  exactly that bit for the host (`EVENT_HOST_ENABLE = 0x4000`, §13.5).
  `sgxinit.c:SGX_ISRHandler` acknowledges it through `EVENT_HOST_CLEAR` (`SW_EVENT |
  MASTER_INTERRUPT`); the MISR handles hardware recovery and re-runs the command queues. The
  waiter (userspace or kernel) just polls the sync or fence word.

## 2. Structure layouts for our build

All three sums equal the sizes the EDM reported through GETMISCINFO (§13.3, §13.10a).

### 2.1 `SGXMKIF_TRANSFERCMD_SHARED` — `0xdc` [DDK]

CDV `services4/include/sgx_mkif_km.h`; identical in `OPVR 1.8.869593`.

| off | field | size |
|---|---|---|
| `0x00` | `ui32NumSrcSyncs` | 4 |
| `0x04` | `asSrcSyncs[8]` (`PVRSRV_DEVICE_SYNC_OBJECT`) | `0x80` |
| `0x84` | `ui32NumDstSyncs` | 4 |
| `0x88` | `asDstSyncs[1]` | `0x10` |
| `0x98` | `ui32TASyncWriteOpsPendingVal` | 4 |
| `0x9c` | `sTASyncWriteOpsCompleteDevVAddr` | 4 |
| `0xa0` | `ui32TASyncReadOpsPendingVal` | 4 |
| `0xa4` | `sTASyncReadOpsCompleteDevVAddr` | 4 |
| `0xa8` | `ui323DSyncWriteOpsPendingVal` | 4 |
| `0xac` | `s3DSyncWriteOpsCompleteDevVAddr` | 4 |
| `0xb0` | `ui323DSyncReadOpsPendingVal` | 4 |
| `0xb4` | `s3DSyncReadOpsCompleteDevVAddr` | 4 |
| `0xb8` | `ui32NumStatusVals` | 4 |
| `0xbc` | `sCtlStatusInfo[4]` (`CTL_STATUS`: `sStatusDevAddr`, `ui32StatusValue`) | `0x20` |

Within a sync object: `+0` read-ops pending value, `+4` read-ops-complete address, `+8`
write-ops pending value, `+0xc` write-ops-complete address.

Version notes: DDK 1.5 (`OPVR 1.5.15.3226`) had a single src/dst pair inline. DDK 1.9
(`OPVR 1.9.2253347`) keeps 8 src / 1 dst but with 24-byte sync objects (`0x124`).
DDK 1.14 and `generic` use 6 src / 2 dst (`SGX_MAX_SRC_SYNCS_TQ`/`_DST_SYNCS_TQ`) of 24 bytes
(`0x10c`). The 1.14 header comments the two TA/3D quadruples as "KEEP THESE 4 VARIABLES
TOGETHER FOR UKERNEL BLOCK LOAD".

### 2.2 `SGXMKIF_HWTRANSFERCONTEXT` — `0xb4` [PSP2]

The kernel headers never define it (only its size, in `SGX_MISCINFO_STRUCT_SIZES`). The
definition is in `PSP2 services4/include/client_mkif.h`. With no ISP context switch on
SGX545, the conditional ISP-resume block drops out:

| off | field | notes |
|---|---|---|
| `0x00` | `common.ui32Flags` | `hw_context`; `SGXMKIF_HWCFLAGS_*` |
| `0x04` | `common.ui32Priority` | scheduling priority |
| `0x08` | `common.sPDDevPAddr` | PD physical address; **written by the kernel** (§1.2) |
| `0x0c` | `ccb_base_dev_addr` | client CCB base (device virtual) |
| `0x10` | `ccb_ctl_dev_addr` | the whole `PVRSRV_SGX_CCB_CTL`; the EDM uses `+4` (read offset) |
| `0x14` | `ui32Count` | number of queued commands in the CCB |
| `0x18` | `src_sync_object_snapshot[8]` | `0x80` |
| `0x98` | `dst_sync_object_snapshot[1]` | `0x10` |
| `0xa8` | `ui32PID` | process ID |
| `0xac` | `prev_dev_addr` | previous context in the EDM's list |
| `0xb0` | `next_dev_addr` | next context in the EDM's list |

Context flags (`PSP2 client_mkif.h`): `NEWCONTEXT 0x1`, `DUMMYTRANSFER 0x2`,
`DUMMY2D 0x4`, `SUSPEND 0x10`, `PERCONTEXTPB 0x20`, `PERCONTEXT_SYNC_INFO 0x40`.

### 2.3 `SGXMKIF_TRANSFERCMD` — `0x4c8` [PSP2]

Also only in `PSP2 client_mkif.h` (a DDK 1.8 header). Resolved for SGX545 as in §0, the
fields sum to exactly `0x4c8`; without `TAG_YUV_TO_RGB` and `TAG_LUMAKEY` the sum would be
`0x488`. Each `sHWRegs` word is the value for one SGX register; the register column comes
from `PSP2 hwdefs/sgx545defs.h`.

| off | field | register |
|---|---|---|
| `0x000` | `sHWRegs.isp_bg_obj_tag` | `ISP_BGOBJTAG` `0x4c4` |
| `0x004` | `sHWRegs.isp_bg_obj` | `ISP_BGOBJ` `0x4bc` |
| `0x008` | `sHWRegs.isp_bg_obj_depth` | `ISP_BGOBJDEPTH` `0x4b8` |
| `0x00c` | `sHWRegs.isp_render` | `ISP_RENDER` `0x404` |
| `0x010` | `sHWRegs.isp_rgn_base` | `ISP_RGN_BASE` `0x408` |
| `0x014` | `sHWRegs.isp_ipf_misc` | `ISP_IPFMISC` `0x414` |
| `0x018` | `sHWRegs.bif_3d_req_base` | `BIF_3D_REQ_BASE` `0xcac` |
| `0x01c` | `sHWRegs.aa_mode_3d` | `3D_AA_MODE` `0x42c` |
| `0x020` | `sHWRegs.isp_multi_samp_ctl` | `ISP_MULTISAMPLECTL` `0x4c8` |
| `0x024` | `sHWRegs.edm_pixel_pds_exec` | `EVENT_PIXEL_PDS_EXEC` `0xa5c` |
| `0x028` | `sHWRegs.edm_pixel_pds_data` | `EVENT_PIXEL_PDS_DATA` `0xa60` |
| `0x02c` | `sHWRegs.edm_pixel_pds_info` | `EVENT_PIXEL_PDS_INFO` `0xa64` |
| `0x030` | `sHWRegs.isp_render_box_1` | `ISP_RENDBOX1` `0x40c` |
| `0x034` | `sHWRegs.isp_render_box_2` | `ISP_RENDBOX2` `0x410` |
| `0x038` | `sHWRegs.pixel_be` | `PIXELBE` `0x400` |
| `0x03c` | `sHWRegs.pds_exec_base` | `PDS_EXEC_BASE` `0xab8` |
| `0x040` | `sHWRegs.double_pixel_partitions` | `DOUBLE_PIXEL_PARTITIONS` `0x148` |
| `0x044` | `sHWRegs.csc_0_coeff_01` … `CSC0Coeff8` | 5 words, TAG YUV→RGB set 0 |
| `0x058` | `sHWRegs.csc_1_coeff_01` … `CSC1Coeff8` | 5 words, set 1 |
| `0x06c` | `sHWRegs.csc_scale` | |
| `0x070` | `sHWRegs.luma_key_coeff_0..2` | 3 words |
| `0x07c` | `sHWRegs.firh_filter_table` | |
| `0x080` | `sHWRegs.ui32FIRHFilter{Left,Right,Extra}{0,1,2}` | 9 words, to `0x0a3` |
| `0x0a4` | `ui32Size` | command size in the CCB, 64-byte multiple |
| `0x0a8` | `ui32Flags` | `SGXMKIF_TQFLAGS_*` |
| `0x0ac` | `shared_state` | §2.1, to `0x187` |
| `0x188` | `load_yuv_coefficients` | `IMG_BOOL` |
| `0x18c` | `load_luma_key_coefficients` | |
| `0x190` | `load_fir_coefficients` | |
| `0x194` | `num_pixel_partitions` | `DMS_CTRL` pixel-partition field, `0x07c00000` |
| `0x198` | `fence_id` | |
| `0x19c` | `num_updates` | ≤ 101 |
| `0x1a0` | `updates[101]` | `mem_update {update_addr, ui32UpdateVal}`, to `0x4c7` |

The header says `updates` is "the update or patch list, completed before kicking the cmd":
the EDM writes each value to its address, through the context's page directory, before it
starts the render (§3.1 step 6). The PSP2 header also notes "keep this aligned to at least
64 byte boundary".

The command carries no start word: the EDM writes `ISP_START_RENDER` itself (§3). The OPVR
kernels of every version (1.5 to 1.17) define only the shared part, so **no GPL source has
`SGXMKIF_TRANSFERCMD`**; the PSP2 layout is the only one, and the offsets the EDM
dereferences (§3) are the independent check.

### 2.4 Typical register values (`PSP2 transfer_queue.c`)

`prepare_full_custom_op` (the "caller specifies everything" blit) shows the fixed parts:

| field | value |
|---|---|
| `ISPBgObjDepth` | `0x3f800000` (1.0f) |
| `ISPIPFMisc` | `ISP_IPFMISC_PROCESSEMPTY` (`0x100`) |
| `3DAAMode` | 0 |
| `ISPMultiSampCtl` | `0x88` (`MSAA_X0 = 8`, `MSAA_Y0 = 8`: one sample at pixel centre) |
| `PixelBE` | `0x80` (`ALPHATHRESHOLD = 0x80`) |
| `ISPRender` | caller's value; `prepare_fill_op` uses `FASTSCALE` (3), scan `TL2BR` (0) |
| `ISPRgnBase`, `ISPBgObjTag`, `ISPBgObj` | from the ISP stream builder |
| `Bif3DReqBase` | the 3D requestor base of the memory context |

The closed `libsrv_um` (`SGXQueueTransfer` body at `0x17280`) differs in a few values and
shows how the addresses are formed. `ctx[0x30]` there is the ISP-stream buffer's device
address `& 0xfff00000`:

| field | closed `libsrv_um` value |
|---|---|
| `ISPBgObjTag` | `(PDS program address − ctx[0x30]) >> 4 \| n << 25` (`TSPDATASIZE` = n) |
| `ISPBgObj` | `0x200` (`ENABLEBGTAG`), or `0x300` (`+ MASK`) in one mode |
| `ISPRender` | `… \| 3` (`FASTSCALE`) |
| `ISPRgnBase` | `(ISP stream address − ctx[0x30]) & ~3` |
| `Bif3DReqBase` | `ctx[0x30]` |
| `3DAAMode` | `0x80000000` (`GLOBALREGISTER`) |
| `EDMPixelPDSExec` | end-of-tile PDS program address `& ~0xf` |
| `EDMPixelPDSData` | its data size `>> 4` |
| `EDMPixelPDSInfo` | `(attributes × 4 + 15) >> 4` |
| `ISPRenderBox1` | `(y0 >> 4) \| (x0 >> 5) << 16` |
| `ISPRenderBox2` | `((y1 + 15) >> 4) − 1 \| (((x1 + 31) >> 5) − 1) << 16` |
| `PDSExecBase` | a context value (`ctx[0x2c]`) |

So the render box is in **32 × 16 tiles**, and ISP-stream and background-object addresses
are **relative to `BIF_3D_REQ_BASE`** (a 1 MiB-aligned window).
| `EDMPixelPDS*` | the pixel-event PDS program (`setup_pixevent_regs`) |
| `ISPRenderBox1/2` | destination rectangle (`setup_transfer_render_box`) |

The other `sHWRegs` values come from `setup_transfer_regs`, which is declared in
`PSP2 transfer_client.h` but implemented only in the closed `libsrv_um` (§4.3).

## 3. The EDM side — `SGXMKIF_CMD_TRANSFER` at `0x818`

Traced with the `usse` radare2 plugin and `tools/usse-dis` over the template; register names
from `tools/usse-dis/regtouch.py`. Labels below are ours.

**Disassembly note [DDK/HW].** For `ldad.fetchN rD, [rB,+#k]` (and `stad.fetchN`) the
accessed dwords are k+1 … k+N; plain `ldad [rB,+#k]` and the `#k++` forms access dword k.
The text matches `refdisasm`; the extra dword is the hardware's fetch behaviour, which the
PSP2 compilers compensate for (full table and evidence in `tools/usse-dis/README.md`). With
it the TA/3D sync blocks (`fetch4 [cmd,+#80]` / `+#84`) land on `0x144` / `0x154` and the
destination completion loop (`fetch2 [dst,+#1]`) on the sync object's `+8`/`+0xc`.

### 3.1 Control flow

1. **`edm.transfer` (`0x818`–`0x8e0`), the CCB handler.** `r1 = r7` = the context.
   It increments `ctx.ui32Count` (`+0x14`). If the count was already non-zero the context is
   already queued. Otherwise it appends the context to the transfer queue of priority
   `ctx.ui32Priority` (`+0x04`): head `sa13 + prio`, tail `sa14 + prio`,
   `old_tail.next_dev_addr (+0xb0) = ctx`, `ctx.prev_dev_addr (+0xac) = old_tail`. Then
   `ba 0x161` (byte `0xb08`). **The handler does no rendering itself.**
2. **Scheduler (`0xb08`).** Sets `sa11 = 0`, `sa12 = 3`, then:
   - `0x159e`: if `sa13 | sa17` (transfer or 3D queue, priority 0) is non-empty, calls
     `edm.request(0x40000)`;
   - `0x1592`: if `sa15` (TA queue) is non-empty, posts `0x20000`.

   This resolves part of §13.8's open question: request bit **`0x40000` = "3D-pipe work
   queued"**, **`0x20000` = "TA work queued"**.
3. **PDS #14 → `edm.work.dpm_store` (`0x73a0`).** Its request group `0x0004000f` includes
   `0x40000`; at `0x74f8` that bit calls **`0x1035` (byte `0x81a8`), the 3D-pipe scheduler**.
   The spec's `dpm_store` name undersells this task: it is the 3D/transfer task.
4. **`0x81a8`.** Bails out if `sa10 & 3` (cleanup pending) or `sa9 & 0xa`; if `sa9` bit 0
   (3D busy) is set it only checks readiness. Per priority it tries the transfer queue
   (`0x12dc`) **before** the 3D render queue (`0x105d`) **[infer: transfers win]**.
5. **Transfer pick (`0x12dc`, byte `0x96e0`).** Walks the list from `sa13` via
   `next_dev_addr`, skipping contexts with `ctx.ui32Flags & 0x10` (`SUSPEND`). The current
   command is at `ctx.ccb_base_dev_addr (+0x0c) + CCBCtl[+4]`, i.e. **the read offset is a byte
   offset** and `ccb_ctl_dev_addr` points at the whole `PVRSRV_SGX_CCB_CTL`. Ready means:
   - for every source and destination sync: `*RComp == RPend` and `*WComp == WPend` (a null
     address skips the test);
   - for the TA and 3D blocks: if the `WComp` address is non-null, both pairs match.

   With `NOSYNCUPDATE` and not `KEEPPENDING` it snapshots the src/dst pairs into the context
   (`+0x18`, `+0x98`). A not-ready context makes it try the next one; a ready one with the 3D
   idle falls through to the kick.
6. **Kick (`0x9aa0`–`0xa300`).**
   - `ctx.ui32Flags & 2` (`hwc_flags_dummy_transfer`): clears it and takes the dummy
     path.
   - Sets `sa5 = 0`, `sa7 = ctx`, `sa3 = cmd`, `sa6 = prio`.
   - **MMU:** `0x17ec(ctx, 2)` looks for a BIF directory list already holding
     `ctx.sPDDevPAddr`: it compares `BIF_DIR_LIST_BASE0` (`0xc84`) and `BASE1`–`14`
     (`0xc34 + 4n`), never list 15 (the EDM's). With no match it takes one round-robin
     (`TA3DCtl` dword 49), avoiding the TA's current index, writes the PD there, pulses
     `BIF_CTRL` (`0xc00`) invalidate/flush, and sets **`BIF_BANK0.INDEX_3D`** (`0xc78`
     bits [15:12]). With the context's `NEWCONTEXT` flag it also invalidates the PDS/USE
     caches.
   - `TA3DCtl` dword 51 = `ctx.ui32PID`. `KEEPPENDING` without `NOSYNCUPDATE` copies the
     snapshots back into the command.
   - **`updates[]`** are written now, before the render: `*update_addr = ui32UpdateVal`
     for `num_updates` entries, through the **context's** address space (`0x1864(1)` points
     `BANK0.INDEX_EDM` at the 3D's list, `0x1864(2)` restores 15).
   - `cmd.ui32Flags & 0x20` (`TQFLAGS_DUMMYTRANSFER`): dummy path (step 9).
   - Otherwise it programs the registers of §3.3, logs HWPerf `TRANSFER_START` (type 2), sets
     `TA3DCtl` dword 57 = 0, dword 54 `|= 2`, `sa9 |= 5` (3D busy + transfer in flight), moves
     the context to the head of its queue, and writes **`cr_isp_start_render` (`0x428`)
     = 1** at `0xa2f8`.
7. **End of render.** In `edm.event.general` (`0x120`) and `edm.event.dpm_store`
   (`0x7108`), the `pa1` bit `0x10` path acknowledges `EVENT_HOST_CLEAR = 0x40000` =
   **`PIXELBE_END_RENDER`**. (So that event bit, which §13.8 labels `DPM_STATE_STORE`, is
   end-of-render in this path.) If `sa9` bit 2 is set it logs HWPerf `TRANSFER_END`
   (`0x10000002`) and goes `0xebb` → **`0x1461`, the transfer completion**. The job is
   identified only by `sa9` bit 2 and `sa3`/`sa7`: one transfer in flight at a time.
8. **Completion (`0x1461`).** Clears `sa9 &= ~0x30cf`, then on `cmd = sa3`:
   - TA and 3D blocks: `*WComp = WPend + 1`;
   - every `sCtlStatusInfo[i]`: `*sStatusDevAddr = ui32StatusValue` (the fence ID);
   - `0x14c5`: unless `NOSYNCUPDATE`, `*RComp = RPend + 1` per source and
     `*WComp = WPend + 1` per destination; then **`EUR_CR_EVENT_STATUS = 0x4000`
     (`SW_EVENT`)**, the host interrupt;
   - `0x14b5`: `CCBCtl.ReadOffset = (ReadOffset + cmd.ui32Size) & 0xffff` (a 64 KiB CCB),
     `ctx.ui32Count--`;
   - unlinks the context, or re-appends it at the tail if `Count` is still non-zero
     (round-robin), may post request `0x40`, re-checks the TA queue and loops to `0x81a8`.
9. **Dummy path (`0x14fa`).** Logs HWPerf `MK_TRANSFER_DUMMY` (`0x105`), does the BIF
   setup, the `updates` writes and all the completion writes of step 8, raises `SW_EVENT`,
   advances the CCB and loops. **No 3D register is touched.**

### 3.2 Memory the EDM reads and writes

- **Context.** Reads `+0x00` flags (bits 0, 1, 4), `+0x04`, `+0x08`, `+0x0c`, `+0x10`,
  `+0x14`, the snapshots, `+0xa8`, `+0xac`, `+0xb0`. Writes the flags (clears bits 0 and 1),
  `+0x14`, the snapshots, `+0xac`, `+0xb0`.
- **CCB control.** Reads and writes `+4`, the read offset.
- **Command.** Reads every field of §2.3; `fence_id` is loaded but no use was seen.
  Writes the sync snapshots back (copy-back with `KEEPPENDING`).
- **Sync objects, status words, `updates` targets.** Reads the `…Complete` words; writes
  them, the status words and the update targets.

Every offset the EDM uses agrees with the §2 tables, so the PSP2 layouts hold for this
ukernel.

### 3.3 Registers programmed per transfer

There is no table-driven register copy; each register is a fixed `str`:

1. `DMS_CTRL` (`0xa74`): bits [26:22] `|= num_pixel_partitions` (pre-shifted)
   **[infer]**.
2. `DPM_3D_DEALLOCATE` (`0x63c`) = 0, `isp_zls_ctl` (`0x480`) = 0: no depth load/store.
3. The 17 `sHWRegs` words of §2.3, `ISP_BGOBJTAG` … `DOUBLE_PIXEL_PARTITIONS`. Note that
   **`PDS_EXEC_BASE` (`0xab8`) is overwritten per transfer**, so the command must carry the
   right value (`0xE4000000` for our heaps) or later work breaks.
4. Only if the matching `load_flag*` flag is set: `TPU_YUV00`–`04`, `YUV10`–`14`, `YUVSCALE`
   (`0x820`–`0x848`); `TPU_LUMA0`–`2` (`0x860`–`0x868`); `USE_FILTER_TABLE` (`0xa9c`),
   `USE_FILTER0/1_*` (`0xa84`–`0xa98`), `USE_FILTER2_*` (`0xb24`–`0xb2c`).
5. Invalidations: `PDS_INV1` (`0xad4`), `PDS_INV_CSC` (`0xae0`), `TSP_PARAMETER_CACHE`
   (`0x43c`); `CACHE_CTRL` (`0x804`) bit 28 with a poll on `EVENT_STATUS` bit 26
   (`MADD_CACHE_INVALCOMPLETE`) and its ack; `ci.global` + `USE_CACHE` (`0xa08`); poll
   `PDS_CACHE_STATUS` (`0x138`) `& 0x4b` and ack via `PDS_CACHE_HOST_CLEAR` (`0x140`).
6. `ISP_START_RENDER` (`0x428`) = 1.

Nothing touches the TA, the DPM state, the ZLS buffers or any TSP/USE state beyond this.

### 3.4 What must already exist **[infer]**

Only what a TA-less ISP render needs:

- **region headers / control stream** at `ISP_RGN_BASE`, in the context's address space;
- a **background object** (`ISP_BGOBJ`, `ISP_BGOBJTAG`): its tag points at vertex/TSP data
  whose PDS pixel program runs the USSE fill or copy shader;
- a **pixel-event PDS program** (`EVENT_PIXEL_PDS_*`), which at end of tile runs the USSE
  end-of-tile (EOT) program that emits the tile through the PBE (and the end-of-render
  program at the end);
- `PDS_EXEC_BASE`, and a valid page directory in `ctx.sPDDevPAddr`.

**No parameter buffer, DPM state or render target object is needed.** The EDM does not
interpret any of these; it only loads registers.

### 3.5 Constraints for the host

- `ctx.ui32Priority` must be **0**: only priority 0 is scanned, and other priorities alias
  the TA queue's `sa` slots **[infer]**.
- The read offset advances by `cmd.ui32Size`, so the host's write offset must use the same
  value (the 64-byte-rounded size, §1.3) and the CCB must be 64 KiB (mask `0xffff`).
- Only one transfer runs at a time, and a context is suspended by flag `0x10`.
- Completion is visible as: target words written, `CCBCtl[+4]` advanced, `ctx.Count`
  decremented, `EVENT_STATUS.SW_EVENT` set. **HostCtl is not touched** on this path.

## 4. Minimal recipe for the first hardware test

Two stages. Stage A needs no shader or ISP data at all and should run first.

### 4.1 Stage A — a dummy transfer (no 3D work)

**Confirmed on hardware (2026-10-20)** with the recipe below (gma500 `dri/0/sgx/xfer_dummy`,
`kernel/patches/`): `U` and `F` written, the CCB read offset advanced by `0x1c0`, the context
count back to 0 and one `SW_EVENT` interrupt, each about 140 µs after the kick, for eleven
transfers in a row. The sync-readiness test (second dummy below) passes too: held while the
destination's write counter is one short, run after the host completes it and sends
`PROCESS_QUEUES`; the EDM then sets that write counter to pending + 1 and leaves the
destination's read counter alone (`dri/0/sgx/xfer_sync`).

The `DUMMYTRANSFER` path (§3.1 step 9) runs every part of the queue except the render: queue
linking, the sync test, the BIF directory-list switch, `updates`, the status writes,
`SW_EVENT` and the CCB advance. Everything is known:

1. **Memory**, all in KERNEL_DATA so that the EDM's own (kernel) page directory maps it:
   - client CCB, 64 KiB + `0x4c8`, zeroed; CCB control, 8 bytes, zeroed;
   - HW transfer context, `0xb4` bytes, 32-byte aligned;
   - two scratch words `U` (update target) and `F` (fence/status target), zeroed.
2. **Context:** `ui32Flags = 0` (or 1), `ui32Priority = 0`, `sPDDevPAddr` = the kernel PD
   physical address (the one in `BIF_DIR_LIST_BASE15`), `ccb_base_dev_addr`, `ccb_ctl_dev_addr`,
   everything else 0.
3. **Command at CCB offset 0:**
   - `ui32Flags = 0x20` (`TQFLAGS_DUMMYTRANSFER`);
   - `shared_state`: zero syncs, TA/3D addresses 0, `ui32NumStatusVals = 1`,
     `sCtlStatusInfo[0] = {F, 0x1234}`;
   - `num_updates = 1`, `updates[0] = {U, 0xcafe}`;
   - `ui32Size = (0x1a0 + 8 + 0x3f) & ~0x3f = 0x1c0`.
4. **Host:** CCB control write offset = `0x1c0` (the EDM does not read it, but the host
   allocator does).
5. **Kernel CCB:** `SGXMKIF_COMMAND {0x103, cache control, ui32Data[1] = context address}`,
   then the usual kick (`ccb.py`). Send cache control `INVAL_DATA` (`0x8`) the first time
   **[infer: harmless, mirrors the DDK's accumulated flags]**.
6. **Expect:** `U = 0xcafe`, `F = 0x1234`, CCB control `+4` = `0x1c0`, `ctx.ui32Count = 0`,
   and `EVENT_STATUS` bit 14 (`SW_EVENT`) set. With the IRQ hooked up, the ISR sees
   `SW_EVENT` and acknowledges it through `EVENT_HOST_CLEAR = 0x80004000`. The kernel's
   GETMISCINFO keeps working afterwards.

A second dummy with one destination sync whose `WriteOpsComplete` is not yet at its pending
value should **stay queued** until the host writes the complete word and kicks
`PROCESS_QUEUES`. That tests the readiness check.

### 4.2 Stage B — a solid fill

The smallest real transfer is a fill: no source surface, no texture fetch. The DDK path is
`PSP2 transfer_queue.c:prepare_fill_op` with ROP `0xF0`: USSE program `use_blit_fill`
(the colour as a `limm` immediate), PDS primary `pds_primfrag_kickonly`, PDS secondary
`pds_secfrag_basic`, zero texture layers, render type `FASTSCALE`, scan `TL2BR`.

Known:

- a destination buffer, e.g. 64 × 32 ARGB8888 with a 256-byte stride, in any heap that the
  context PD maps;
- the fixed `sHWRegs` values of §2.4 (`IPFMisc`, `BgObjDepth`, `MultiSampCtl`, `PixelBE`,
  `3DAAMode`);
- `ISPRender = 3`; for (0,0)–(64,32) `ISPRenderBox1 = 0` and `ISPRenderBox2 = 0x00010001`
  (§2.4 formulas);
- `PDSExecBase = 0xE4000000`, `NumPixelPartitions = 0x07c00000`,
  `DoublePixelPartitions = 0`;
- `Bif3DReqBase`: our choice of a 1 MiB-aligned window holding the ISP stream and the
  background-object state;
- the USSE **end-of-render** program: `libsrv_um` writes a 3-instruction program at runtime
  (`fcn.1fd90`): `phas`, `mov r0, #0`, `emitpix2.end.freep`;
- the client CCB, context, sync and fence setup of §4.1.

Structure known, values not:

- the **PDS primary** (`doutu` only, kicking the fill shader): a `0x18`-byte template at
  `libsrv_um` `.rodata 0x227e0`;
- the **PDS secondary** (`BASIC`): `libsrv_um` templates;
- the **PBE state** for the destination (address, stride, format, tile box):
  `set_pbe_state`; PSP2 has `codegen/pixevent/pixel_event_pbe_setup.c`;
- the USSE **end-of-tile** program (`emitpix` with the PBE state words):
  `create_use_eot_handler`; PSP2 has `codegen/pixevent/pixevent.c:write_end_of_tile_code`;
- the **PDS pixel-event program** (EOT / EOR / PTOFF dispatch):
  `create_pds_pixevent_handler`; PSP2 has
  `codegen/pds/pds.c:pds_pixel_event_program`.

Unknown:

- the **ISP stream** at `ISPRgnBase` for a FASTSCALE render of one rectangle, built by the
  closed `create_isp_resource`;
- the **background-object state** behind `ISPBgObjTag` (PDS program pointers, TSP data
  size), from the same builder;
- the **USSE fill shader** (`limm` colour → output register, `.end`), a few instructions
  generated from a template in `libsrv_um`.

Placement rules: USSE code must sit under one of the pixel `USE_CODE_BASE_n`
(`0xF4000000 + n × 0x80000`, §13.5), and `doutu` names it relative to that base. PDS
programs sit in the PDSPIXEL heap (`0xE4000000`) and are named relative to `PDS_EXEC_BASE`.
Vertex/state data for the background object and the ISP stream are named relative to
`BIF_3D_REQ_BASE`.

### 4.3 Where the missing programs can come from

1. **Capture, preferred.** Run the closed `libpvr2d`/`libsrv_um` under the harness
   (`harness/ukernel-extract/fakedrm.c`): `PVR2DCreateDeviceContext` →
   `SGXCreateTransferContext`, then one `PVR2DBlt` (copy) or one `SGXQueueTransfer` of type
   FILL (blit type 4, `fcn.12480` **[infer]**). Have the mock `SGX_SUBMITTRANSFER` bridge
   dump the client CCB, the ISP-stream, PDS-code and USSE-code buffers. That yields a
   complete, known-good command with every unknown of §4.2 filled, as srvinit's capture did
   for the boot. The libraries' entry points are exported, so this needs no RE beyond the
   mock bridge calls `SGXCreateTransferContext` makes (device-memory allocation, sync
   allocation, register context).
2. **Static RE of `libsrv_um`.** The built-in programs are in `.rodata`: a USSE table at
   `0x21d60`–`0x227e0` (`0xa80` bytes, 52 programs; program 0 is the plain copy shader
   `mov.end o0, pa0`) and PDS templates at `0x227e0`–`0x22d80`. The ISP-stream writer is
   inside the `SGXQueueTransfer` body (`0x17280`–`0x1b24f`). These are closed bytes: read
   them for facts, never copy them into the repo.
3. **Clean-room generation.** PSP2's `codegen/pds`, `codegen/pixevent` and the USSE encoding
   tables describe how the pixel-event program, the EOT program and the PBE emit state are
   built. A clean-room generator (like `tools/usse-dis` for decoding) can produce them once a
   capture gives known-good output to diff against.

The realistic order is A (dummy, now), then capture (1) to get one real fill and one real
copy, then replay the captured command on the laptop with our own buffers, and only then
replace the captured programs with generated ones.

## 5. Open questions, by priority

1. **The ISP control stream for a FASTSCALE render on SGX545** (`ISPRgnBase` contents) and
   the background-object state behind `ISPBgObjTag`. Nothing in GPL or PSP2 source shows the
   format; the closed `create_isp_resource` code inside `libsrv_um` `0x17280` does. This
   blocks Stage B; a harness capture (§4.3) answers it without RE.
2. **Does the dummy transfer (§4.1) behave as traced on hardware?** It is the cheapest
   end-to-end test of queueing, `updates`, status writes, the BIF switch and `SW_EVENT`.
3. **The EOT/PBE programs for one surface format.** The PBE state word layout for SGX545
   (`EURASIA_PIXELBE*` in PSP2 `hwdefs`) and the exact EOT program the closed library
   generates. A capture gives the bytes; the PSP2 codegen gives the meaning.
4. **The fill shader** (`use_blit_fill`): which output register and how the colour
   `limm` is patched. A capture of a fill answers it, but the X driver never issues fills,
   so the capture must call `SGXQueueTransfer` with a FILL directly.
5. **`EVENT_PIXEL_PDS_INFO` encoding** on SGX545: the closed library writes
   `(attributes × 4 + 15) >> 4` while the PSP2 test uses `ATTRIBUTE_SIZE` with an
   alignshift. Confirm against `sgx545defs.h`.
6. **`ui32Priority`**: confirm that only 0 is safe (§3.5).
7. ~~**The `ldad.fetchN` offset rule** (§3).~~ Resolved: the decoder text matches
   `refdisasm` and the +1 is real; documented in `tools/usse-dis/README.md`, and the r2
   plugin's ESIL and pseudo follow it.
8. **The other request bits** (`0x80000`) and the `TA3DCtl` dwords 49–57 used here; the
   §13.8 request table should be updated with `0x20000` = TA work and `0x40000` = 3D-pipe
   work.
