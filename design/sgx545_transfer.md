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
| `ISPBgObjTag` | `(BG-object data address − ctx[0x30]) >> 4 \| layers << 25` (§4.2.3) |
| `ISPBgObj` | `0x200` (`ENABLEBGTAG`); `0x300` (`+ MASK`) only for render type ≠ 3 |
| `ISPRender` | `scan \| type`; fills use type 3 (`FASTSCALE`) |
| `ISPRgnBase` | left 0 for `FASTSCALE`; else `(fast-2D stream − ctx[0x30]) & ~3` |
| `Bif3DReqBase` | `ctx[0x30]` |
| `3DAAMode` | `0x80000000` (`GLOBALREGISTER`) |
| `EDMPixelPDSExec` | end-of-tile PDS program address `& ~0xf` |
| `EDMPixelPDSData` | its data size `>> 4` |
| `EDMPixelPDSInfo` | `(attributes × 4 + 15) >> 4` |
| `ISPRenderBox1` | `(y0 >> 4) \| (x0 >> 5) << 16` |
| `ISPRenderBox2` | `((y1 + 15) >> 4) − 1 \| (((x1 + 31) >> 5) − 1) << 16` |
| `PDSExecBase` | a context value (`ctx[0x2c]`) |

So the render box is in **32 × 16 tiles**, and ISP-stream and background-object addresses
are **relative to `BIF_3D_REQ_BASE`** (a 1 MiB-aligned window). The `EDMPixelPDS*` words
come from `setup_pixevent_regs` and the render box from
`setup_transfer_clip_render_box`. The other `sHWRegs` values come from
`setup_transfer_regs`, which is declared in `PSP2 transfer_client.h` but implemented
only in the closed `libsrv_um`; §4.2 gives its body for the fill.

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

The smallest real transfer is a fill: no source surface, no texture fetch. This section
specifies one completely, from static analysis of the closed `libsrv_um.so.1.7.862890`
(radare2 and `objdump` on the i386 code; `tools/usse-dis` `refdisasm545`/`refpds545` and our
`usse-dis`/`pds-dis` on the device code it emits). `PSP2` sources were read for field names
only. Every value below was reproduced by a scratch model that rebuilds all the buffers
from the library and disassembles them; that model is not tracked.

Binary offsets are file offsets in `libsrv_um`, which equal the virtual addresses for
`.text` and `.rodata`. The library is position-independent: `ebx` = GOT = `0x307f4`, so
`[ebx − k]` names offset `0x307f4 − k`.

#### 4.2.1 The fill path in `libsrv_um`

`SGXQueueTransfer` (`0x1b250`) jumps to the body at `0x17280` (`eax` = context, `edx` =
`queue_transfer`). The body switches on the blit type (`[edx+4]`) through the table at
`0x22f94`; type 4 (`tq_fill`) lands at `0x18dd8` and calls the fill preparer `0x12480`.
That function is `PSP2 transfer_queue.c:prepare_fill_op`, call for call:

| closed function | PSP2 name | role in a fill |
|---|---|---|
| `0x1b4d0` | `get_pixel_formats` | PBE pack mode from the format table at `0x23240` |
| `0x1be00`, `0x1bf90` | `get_surface_stride`, `…Width` | stride in pixels, width |
| `0x1c3c0` | `set_pbe_state` | the five PBE state words (§4.2.5) |
| `0x1c550` | `create_use_eot_handler` | writes the EOT program via `0x20000` |
| `0x1c630` | `create_pds_pixevent_handler` | pixel-event PDS program via `0x1e1d0` |
| `0x1ced0` | `create_use_resource` | the fill shader (patches the colour) |
| `0x1c180` | `set_usse_kick` | `DOUTU` words for a USE program (§4.2.4) |
| `0x1c910` | `create_pds_prim_resource` | PDS primary `KICKONLY` |
| `0x1d1e0` | `create_use_secondary_resource` | USE secondary, re-points the `DOUTU` |
| `0x1cbb0` | `create_pds_sec_resource` | PDS secondary `BASIC` |
| `0x1d330` | `create_isp_resource` | the background-object data (§4.2.3) |
| `0x1d2b0` | `setup_transfer_clip_render_box` | `ISPRenderBox1/2` |
| `0x1c850` | `setup_transfer_regs` | the other `sHWRegs` (§4.2.6) |
| `0x1c810` | `setup_pixevent_regs` | (inlined into the above) `EDMPixelPDS*` |

For `custom_rop_3 = 0` or `0xF0` it selects USE program 3 (`use_blit_fill`), PDS
primary 0 (`KICKONLY`), PDS secondary 0 (`BASIC`), zero layers, render type 3, scan 0, and
for a one-pass format the colour is `colour` unchanged (`0x1280f`). Other ROPs pick USE
programs 24–34 and a single-source primary. The command is the zeroed `0x4c8` block at body
`esp+0xa8`, so every field not named below is 0.

The closed `client_transfer_context` is `0x188` bytes (`0x112f5`); the fields used
here are `+0x28` `use_exec_base` (PIXELSHADER heap base), `+0x2c` `pds_exec_base` (PDSPIXEL
heap base), `+0x30` `isp_stream_base` (stream buffer address `& 0xfff00000`), `+0x3c`/`+0x40`
/`+0x44` the PDS, USE and stream circular buffers, `+0x4c` `pds_prim_resources[5]`, `+0x60`
`pds_sec_resources[9]`, `+0x84` `pds_pixevent_handlers[2]`, `+0x8c`
`use_resources[53]`, `+0x160` the EOT handler, `+0x168` the EOR handler and `+0x16c`
`isp_resources[4]`. The PIXELSHADER, PDSPIXEL and GENERAL heaps hold the USE code
(`0x800` buffer, align 8), PDS code (`0x1800`, align 16) and ISP stream (`0x4800`, align 64).

#### 4.2.2 How the fill renders

Render type 3 is `cr_isp_render_type_fastscale` (`PSP2 hwdefs/sgxdefs.h`). In that mode
`setup_transfer_regs` does **not** set `ISPRgnBase` (it stays 0) and leaves `ISPBgObj =
ENABLEBGTAG`: the ISP reads no region headers or control stream. It rasterises the
**background object** over the render box, in 32 × 16 tiles (`isp_region_size_x/Y`
for SGX545). **[infer]**: the FASTSCALE ISP ignores `ISP_RGN_BASE`, as the closed code
leaves it 0.

Per pixel, the object's PDS state starts two tasks: the **PDS primary** (`KICKONLY`) issues
one `doutu` to the **fill shader**, which writes the colour to `o0`; the **PDS secondary**
(`BASIC`) issues one `doutu` to a no-op **USE secondary** program. At the end of each tile
the pixel-event PDS program (`EVENT_PIXEL_PDS_*`) runs the **end-of-tile (EOT)** program,
which loads the PBE state into `r0`–`r3` and emits the tile with `emitpix1`/`emitpix2`. At
the end of the render it runs the **end-of-render (EOR)** program. The EDM then sees
`PIXELBE_END_RENDER` and completes the transfer (§3.1 steps 7–8).

#### 4.2.3 The background object

`setup_transfer_regs` (`0x1c850`) sets `ISPBgObj = 0x200` (`ENABLEBGTAG`, bit 9; no
`MASK`), `ISPBgObjDepth = 0x3f800000` and

```
ISPBgObjTag = ((bg − ctx[0x30]) >> 4) | layers << 25
```

against `cr_isp_bgobj_tag` = `VERTEXPTR` [23:0] (16-byte units, relative to
`BIF_3D_REQ_BASE`) and `TSPDATASIZE` [30:24]. So `TSPDATASIZE` = 2 dwords per layer, and 0
for a fill. `bg` is `isp_resources[layers]`, written by `create_isp_resource`
(`0x1d330`) into the stream buffer: `0x44 + 24 × layers` bytes, 64-byte aligned. With
`P` = `PDS_EXEC_BASE` and zero layers it is 17 dwords:

| dword | value | meaning (`PSP2 sgxdefs.h` names) |
|---|---|---|
| 0 | `((sec − P) >> 4) \| (sec_data >> 4) << 26` | PDS state pointer, secondary |
| 1 | `pri_sz \| 3 << 16 \| sec_sz << 18` | PDS state size; fill: `0x00030000` |
| 2 | `((pri − P) >> 4) \| (pri_data >> 4) << 26` | PDS state pointer, primary |
| 3 | 0 | **[infer]** a state word the TQ leaves clear |
| 4 | `layers << 9 \| layers << 1` | vertex-format word |
| 5–8 | 0 | **[infer]** clip / sample-position words |
| 9–11.5 | vertex 0 = (x0, y0) | even slot |
| 11.5–13 | vertex 1 = (x1, y0) | odd slot |
| 14–16 | vertex 2 = (x0, y1) | even slot |

The pointer words are `BASEADD` [23:0] and `DATASIZE` [31:26] (`EURASIA_PDS_*` without
`feature_pixel_pdsaddr_full_range`). The size word is `PIXELSIZE` [6:0] =
`(pri_attr + 3) >> 2`, `USETASKSIZE` [17:16] = 3 and `SECATTRSIZE` [24:18] =
`(sec_attr + 3) >> 2`. The vertex-format word is `PITCHF` [15:8] = `PITCHB` [7:0] =
2 × layers.

Vertices use the SGX545 format `EURASIA_PARAM_VF_*`: X and Y are 24-bit fixed point with 8
fraction bits and an offset of 4096, i.e. `(c + 0x1000) << 8`, and Z is 0. An even vertex
at dword k is `Y | X[7:0] << 24`, then `X[23:8] | Z[15:0] << 16`, then `Z[31:16]` in the
low half of dword k + 2. The odd vertex that follows takes the high half of dword k + 2
(`Y[15:0]`), then `Y[23:16] | X << 8`, then `Z`. Coordinates are pixels, with `x1`
and `y1` exclusive. Three vertices define the rectangle: top-left, top-right, bottom-left.
With layers, the TSP coordinates (2 dwords per layer per vertex) sit between dword 2 and
the zero word, which is why `TSPDATASIZE` counts them.

For (0, 0)–(64, 32) the 17 dwords are:

```
04000002 00030000 04000000 00000000 00000000 00000000 00000000 00000000
00000000 00100000 00001000 00000000 10400010 00000000 00102000 00001000
00000000
```

(with the secondary at `P + 0x20`, the primary at `P`, both with 16 bytes of data).

#### 4.2.4 The programs

| program | source in `libsrv_um` | size | temps | data | per-transfer patch |
|---|---|---|---|---|---|
| fill shader (USE 3) | `.rodata 0x22218` + imm32 `0x1d1cc` | `0x10` | 0 | — | colour |
| USE secondary (USE 51) | `.rodata 0x223d8` (table `+0x678`) | `0x10` | 0 | — | none |
| PDS primary `KICKONLY` | `.rodata 0x227e0` | `0x18` | — | `0x10`, 0 attr | `DOUTU` of fill |
| PDS secondary `BASIC` | `.rodata 0x229a0` | `0x18` | — | `0x10`, 0 attr | `DOUTU` of USE 51 |
| EOT | generated by `0x20000` | `0x38` | 4 | — | PBE state |
| EOR | generated by `0x1fd90` | `0x18` | 1 | — | none |
| pixel event | `.rodata 0x23080` (via `0x1e1d0`) | `0xc4` | — | `0x50`, 1 attr | 7 dwords |

Sizes, temps, data lengths and attribute counts are the resource records that
`SGXCreateTransferContext` fills: the USE switch at `0x1193f` (jump table `0x21c60`, table
base `0x21d60`), the PDS primary loop at `0x11a17` (`0x11a61`), the secondary loop at
`0x11b06` (`0x11e34`), the pixel-event handlers at `0x117d8` (size from `0x1fd40` =
`0xc4`), the EOT handler at `0x11743` (a `0x50` slot) and the EOR at `0x116a9` (a static
`0x18`-byte allocation written once by `0x1fd90`).

The table at `0x21d60` holds 53 USE resources, 0–52, not 52: entries 1–52 go through the
jump table and entry 0 (the copy shader `phas; mov.skipinv.end o0, pa0`, `0x10` bytes at
`+0`) is the default case.
Entries 1, 2 and 3 are circular-buffer resources copied at use; 3 is the fill
(`0x123ac`), whose template the switch takes from `0x22218` rather than from an offset in the
table list. Entries 51 and 52 are the two USE-secondary programs (`0x1d1e0` picks 52 for
secondaries 1, 5, 6 and 51 otherwise).

**`DOUTU` words** (`0x1c180`), with `off` = program address − `ctx[0x28]` (the PIXELSHADER
heap base, `USE_CODE_BASE_0`):

```
U0 = ((off << 1) & 0xffff0) | ((off >> 19) & 0xf) | ((temps + 3) >> 2) << 24
U1 = 4            (pixel shader and USE secondary)
U2 = 0
```

`U0` is `EXE` (instruction index within the code base) in [19:4], the code-base selector
`CBASE` (`USE_CODE_BASE_n`) in [3:0] and `TRC` (temporaries, in fours) in [31:24]. `U1 =
4` is `pds_doutu1_sdsoft`, the same value `PSP2 opengles2/accum.c` uses for its
background shader. The EOT task uses `U1 = 0x00800000` (`PBENABLE`, bit 23) and the EOR
task `0x00c00000` (`PBENABLE | ENDOFRENDER`), both from `0x1c6f2`–`0x1c741`. These are the
SGX545 bit positions in `PSP2 opengles2` and `sgx_render_flip_test.c`. `PSP2 sgxdefs.h`
gives SGX545 18 PC bits, which would put `EXE` in [21:4] with no `CBASE`; keeping all code
in the first 512 KiB of the heap makes both readings agree (`CBASE` = 0).

**Fill shader.** The template is `phas #0, #0, pixel, end, parallel` + `nop.end`.
`0x1d1a8` replaces the second instruction with `mov.skipinv.end o0, #colour`: low word
`colour & 0x1fffff`, high word `((colour >> 17) & 0x1f0) | (colour >> 26) << 12 |
0xfca40001` (the `USSE_LIMM` field split of `sgx545_fw_format.md` §4, on base `0xfca40001`).
For `0xff336699` it is `00000000 fa440700 00136699 fca7f191`. The colour is an immediate, so
each colour needs its own copy of the program.

**USE secondary (51).** `phas …; nop.end`, unpatched: the secondary has no work.

**PDS primary and secondary.** Both templates are four data dwords and
`movs doutu, ds0[0], ds0[1], ds1[0]` (`0x07000305`) + `halt` (`0xaf000000`). The data rows
follow §13.8 of the hardware spec, so dwords 0, 1, 2 are `ds0[0]`, `ds0[1]`, `ds1[0]` =
`U0`, `U1`, `U2`; `0x1cb58` and `0x1ce58` write exactly those three.
`create_use_secondary_resource` runs after the primary is built and overwrites the shared
`U0`–`U2` with the kick of USE 51, which the secondary then takes.

**EOT** (`0x20000`, called from `0x1c61a` with the PBE words and the sideband): seven
instructions,

```
phas  #0, #0, pixel, end, parallel        00000000 fa440700
mov.skipinv r0..r3, #pbe[0..3]            (v & 0x1fffff) | i << 21,
                                          ((v >> 17) & 0x1f0) | (v >> 26) << 12 | 0xfca00000
emitpix1.freep #0, r0, r1                 00200080 fb200100
emitpix2.end.freep #0, r2, r3, r0, #sb    (sb << 22) & 0x0fc00000 | 0x00208180,
                                          0xfb240000 | (sb >> 3) & 0x1f8 | (sb << 10) & 0xc00000
```

The fixed words are imm32 operands at `0x2001e`, `0x20062`, `0x2007c`, `0x20083`, `0x20092`
and `0x200b3`. A `TILEXY` variant (not used by fills) prepends three more instructions.

**EOR** (`0x1fd90`, imm32 at `0x1fd9d`, `0x1fdab`, `0x1fdb2`, `0x1fdb9`): `phas`;
`mov.skipinv r0, #0`; `emitpix2.end.freep #0, r0, #0, r0, #0x200` =
`00000000 fa440700 00000000 fca00000 80200000 fb260040`.

**Pixel-event PDS program.** `0x1e1d0` copies the 196-byte template at `0x23080` (identical
bytes at `0x23160` serve the `TILEXY` handler) and sets the program's data size to `0x50`.
Its 20 data dwords map to banks as in §13.8; `0x1e24f`–`0x1e27d` patch seven of them:

| dword | bank | value |
|---|---|---|
| 0, 1 | `ds0[0]`, `ds0[1]` | PTOFF task `U0`, `U1` (the libsrv_um uses the EOT task here) |
| 2 | `ds1[0]` | 0 (the `douta` source) |
| 3 | `ds1[1]` | EOT `U0` |
| 4, 5 | `ds0[2]`, `ds0[3]` | EOR `U0`, `U1 = 0x00c00000` |
| 12 | `ds0[6]` | EOT `U1` base, `0x00800000` |

The rest are template constants (`8`, `2`, `0x100`, `0xff8`, `4`, `0x3ff000`, `0x400000`).
The code (`refpds545`) branches on the event input `ir1`: bit 3 runs the PTOFF task; bit 1
runs the EOT task after a `douta` and builds its `U1` as base | `tile_x` = `ir0[8:0]` at
[11:3] | `tile_y` = `ir0[18:9]` at [21:12] | `ENDOFRENDER` = `ir1` bit 2 at [22]; with bit 8
set it runs the EOR task instead; bit 2 runs the EOR task. **[infer]** `ir0`/`ir1` are the
tile position and the event bits; the template is used verbatim, so their exact meaning only
matters if it misbehaves.

`setup_pixevent_regs` (`0x1c810`) then sets `EDMPixelPDSExec = address & ~0xf`
(absolute, like `EVENT_OTHER_PDS_EXEC`), `EDMPixelPDSData = 0x50 >> 4 = 5` (rows of four
dwords) and `EDMPixelPDSInfo = (1 × 4 + 15) >> 4 = 1` (one attribute). This settles the
former open question on `EVENT_PIXEL_PDS_INFO`.

#### 4.2.5 PBE state

`set_pbe_state` (`0x1c3c0`) builds `pbestate_words` = 5 words (`0x14` bytes at
pass data `+0x7c`, zeroed at `0x125c7`). The layout matches `PSP2 sgxdefs.h`
`EURASIA_PIXELBE1S*`/`2S*`/`2SB` for SGX545:

| word | fields | value |
|---|---|---|
| 0 emit1 src0 | `XMIN` [12:0], `YMIN` [28:16] | `x0 \| y0 << 16` |
| 1 emit1 src1 | `XMAX` [12:0], `YMAX` [28:16], inclusive | `(x1 − 1) \| (y1 − 1) << 16` |
| 2 emit2 src0 | `COUNT` [15:8], `LINESTRIDE` [27:16] | `0x8000 \| (stride_px / 2 − 1) << 16` |
| 3 emit2 src1 | `FBADDR` [31:2] | surface address `& ~3` |
| 4 sideband | `PACKMODE` [5:0], `TILERELATIVE` [9] | `0x200 \| packmode` |

Word 2 also holds `ROTATE` [29:28] and `SCALE` [31:30], and the sideband `SRCSEL` [8:6],
`DITHER` [10], `GAMMACORRECT` [12] and `MEMLAYOUT` [13]; all are 0 for this fill.

`COUNT` = `0x80` = 32 × 16 pixels / 4 pipes (`ta_pds_state_pipe_count`). The stride
comes from `0x1be00`: for a linear destination (`memlayout_stride` 3 or
`OUT_LINEAR` 5) it is `|i32StrideInBytes| / bpp` and **must be even**. `MEMLAYOUT` is 1 only
for layouts 0 (twiddled) and 7, which also require the rectangle to cover the surface.
`ARGB8888` (format 20) has table entry `0x23d30`: 1 pass, 32 bpp, pack mode 0
(`pixelbe_2sb_pack_mode_a8r8g8b8`). The address is the surface base, not the tile:
`TILERELATIVE` makes the PBE add the tile offset **[infer from the name and the
`tile_x`/`tile_y` fields of the EOT `DOUTU`]**.

For 64 × 32 at `A` with a 256-byte stride: `00000000 001f003f 001f8000 A 00000200`.

#### 4.2.6 The command

The fill sets these `SGXMKIF_TRANSFERCMD` words; everything else is 0:

| offset | field | value | set by |
|---|---|---|---|
| `0x000` | `ISPBgObjTag` | `(bg − ctx[0x30]) >> 4` | `0x1c8d6` |
| `0x004` | `ISPBgObj` | `0x200` | `0x1c870` |
| `0x008` | `ISPBgObjDepth` | `0x3f800000` | `0x1c877` |
| `0x00c` | `ISPRender` | 3 (`FASTSCALE`, scan `TL2BR`) | `0x1c869` |
| `0x010` | `ISPRgnBase` | 0 | not written |
| `0x014` | `ISPIPFMisc` | `0x100` (`PROCESSEMPTY`) | `0x1c87e` |
| `0x018` | `Bif3DReqBase` | `ctx[0x30] & 0xfff00000` | `0x1c898` |
| `0x01c` | `3DAAMode` | `0x80000000` | `0x1c8c0` |
| `0x020` | `ISPMultiSampCtl` | `0x88` | `0x1c8c7` |
| `0x024`–`0x02c` | `EDMPixelPDSExec/Data/Info` | address `& ~0xf`, 5, 1 | `0x1c8e4`–`0x1c904` |
| `0x030`, `0x034` | `ISPRenderBox1/2` | tile box, §2.4 | `0x1d310` |
| `0x038` | `PixelBE` | `0x80` | `0x1c8f3` |
| `0x03c` | `PDSExecBase` | `ctx[0x2c]` | `0x1c8e1` |
| `0x0a4` | `ui32Size` | `(0x1df + 8 × updates) & ~0x3f` = `0x1c0` | `0x18c67` |
| `0x0a8` | `ui32Flags` | 0 (bit 2 / 3 only with TA / 3D sync) | `0x17344` |
| `0x0ac`… | `shared_state` | the fence status (§1.3) | `0x18cea` |
| `0x190` | `load_fir_coefficients` | 0 | `0x12b2e` |
| `0x194` | `num_pixel_partitions` | `0x07c00000` | `0x17366` |
| `0x198` | `fence_id` | context fence counter `n` (status value `n + 1`) | `0x17371` |
| `0x19c` | `num_updates` | 0 | — |

The library writes the programs straight into CPU-mapped buffers and uses **no
`updates`**. Its buffer allocator (`0x10de0`) moves an allocation to the next 4 KiB page
when it would straddle one.

#### 4.2.7 Host recipe: 64 × 32 ARGB8888

It reuses the §4.1 context, CCB, CCB control and scratch words, with `ctx.sPDDevPAddr` =
the kernel page directory. Everything below is mapped into that directory (`gma_sgx_map`),
so it does not matter which directory list the EDM gives the 3D requestors (§3.1 step 6).

1. **Map and fill these buffers**, writing them through the same uncached or
   write-combined mapping as the boot buffers, with a write barrier before the kick:

   | address | heap | bytes | contents |
   |---|---|---|---|
   | `0xE4000000` | PDSPIXEL (`PDS_EXEC_BASE`) | `0x18` | PDS primary |
   | `0xE4000020` | PDSPIXEL | `0x18` | PDS secondary |
   | `0xE4000040` | PDSPIXEL | `0xc4` | pixel-event PDS program |
   | `0xF4000000` | PIXELSHADER (`USE_CODE_BASE_0`) | `0x10` | fill shader |
   | `0xF4000010` | PIXELSHADER | `0x10` | USE secondary |
   | `0xF4000020` | PIXELSHADER | `0x38` | EOT program |
   | `0xF4000060` | PIXELSHADER | `0x18` | EOR program |
   | `0xF0130000` | KERNEL_DATA | `0x44` | background object |
   | `0xF0200000` | KERNEL_DATA | `0x3000` | destination: 2 pages + 1 guard page |

   Each group fits in one page, and no item straddles a page.
2. **`DOUTU` words:** fill `U0 = 0x00000000`, USE secondary `0x00000020`, EOT `0x01000040`,
   EOR `0x010000c0`; `U1` = 4, 4, `0x00800000`, `0x00c00000`; `U2` = 0.
3. **PDS primary** = `00000000 00000004 00000000 00000000 07000305 af000000`; **PDS
   secondary** = `00000020 00000004 00000000 00000000 07000305 af000000`.
4. **Pixel-event program** = the template with dwords 0–5 = `01000040 00800000 00000000
   01000040 010000c0 00c00000` and dword 12 = `00800000`.
5. **Fill shader** for colour `C`: `00000000 fa440700`, then the `USSE_LIMM` split of `C` on
   `0xfca40001`. **USE secondary** = the template. **EOR** as in §4.2.4.
6. **EOT** with PBE state `00000000 001f003f 001f8000 f0200000 00000200`:

   ```
   00000000 fa440700 00000000 fca00000 003f003f fca00000 005f8000 fca00000
   00600000 fca3c010 00200080 fb200100 00208180 fb240040
   ```

   (`refdisasm545`: `mov.skipinv r3, #0xF0200000`, `emitpix2.end.freep #0, r2, r3, r0,
   #0x00000200`.)
7. **Background object** = the 17 dwords of §4.2.3; `Bif3DReqBase = 0xF0100000`, so
   `ISPBgObjTag = 0x00003000`.
8. **Destination:** fill all three pages with a sentinel such as `0xdeadbeef`.
9. **Command** at the CCB write offset: §4.2.6 with `ISPBgObjTag 0x3000`, `ISPBgObj 0x200`,
   `ISPBgObjDepth 0x3f800000`, `ISPRender 3`, `ISPRgnBase 0`, `ISPIPFMisc 0x100`,
   `Bif3DReqBase 0xF0100000`, `3DAAMode 0x80000000`, `ISPMultiSampCtl 0x88`,
   `EDMPixelPDSExec 0xE4000040`, `…Data 5`, `…Info 1`, `ISPRenderBox1 0`, `ISPRenderBox2
   0x00010001`, `PixelBE 0x80`, `PDSExecBase 0xE4000000`, `DoublePixelPartitions 0`,
   `ui32Flags 0`, no syncs, `ui32NumStatusVals 1` with `{F, n + 1}`,
   `num_pixel_partitions 0x07c00000`, `fence_id n`, `num_updates 0` and
   `ui32Size 0x1c0`. Advance the CCB control write offset by `0x1c0`.
10. **Kick** as in §4.1 step 5. Because new pages were mapped, send cache control
    `INVAL_BIF_PT | INVAL_BIF_PD | INVAL_DATA` (`0xb`) **[infer]**. The EDM itself
    invalidates the PDS, USE and TSP caches before every transfer (§3.3 step 5).
11. **Expect:** all 2048 dwords of the first two destination pages equal `C` **[infer: pack
    mode 0 stores the `o0` dword unchanged, so `0xAARRGGBB` lands as that dword]**, and the
    guard page still holds the sentinel. Also `F = n + 1`, the CCB read offset advanced by
    `0x1c0`, `ctx.ui32Count = 0`, one `SW_EVENT`, and HWPerf `TRANSFER_START` (type 2) and
    `TRANSFER_END` (`0x10000002`) records. On failure, read `BIF_FAULT` (requestor and
    address), `HostCtl.ui32AssertFail` and whether `sa9` bit 2 stays set (render not
    finished).

`tools/fwcutter/fwload.py` builds steps 2–6 and the fixed command words of step 9 for any
layout from a format v2 firmware file (`fwload.fill()`, §4.3); `test_fwload.py` checks every
word above for this layout and the `refdisasm545` / `pds-dis` text of the patched programs.
The background object (step 7) and the rest of the command stay driver code.

#### 4.2.8 Remaining uncertainties, ranked

1. **Fast-scale semantics.** That `ISP_RGN_BASE` is unused, and that the zero words 3 and
   5–8 of the background object are harmless, rest on the closed code alone. The vertex
   order (top-left, top-right, bottom-left) is likewise unobserved on hardware.
2. **Which directory list the pixel requestors use.** The PDS, USE, ISP, TSP and PBE fetches
   are assumed to follow `BIF_BANK0.INDEX_3D`. Mapping everything in the kernel directory
   hides the answer, and a separate client directory would expose it.
3. **Host-write visibility** of programs and the background object on Cedarview (write
   combining versus cache flushes). The EDM invalidates only the GPU-side caches.
4. **Colour encoding.** Pack mode 0 with `o0 = 0xAARRGGBB`, and the PBE `COUNT` of `0x80`.
5. **Pixel-event inputs.** The meaning of `ir0`/`ir1` and of the `douta` attribute; the
   template is used verbatim.
6. **`DOUTU` layout** (`EXE`/`CBASE` versus an 18-bit `EXE`): sidestepped by keeping code in
   the first 512 KiB of the PIXELSHADER heap.
7. **The page-straddle rule** of the closed allocator: whether hardware needs it. Avoided by
   construction.
8. **The EDM render path itself** (`0xa2f8` start, end-of-render event, completion at
   `0x1461`) has never run; only the dummy path is confirmed.

### 4.3 Where the programs come from: format v2 of the firmware file

Nothing in §4.2 needs a capture any more. The closed bytes a fill uses are seven templates:
five `.rodata` ranges and the immediates of two code generators. The rest is our own
reconstruction (FACT): field layouts, patch rules and the fixed register values. This is now
**format v2** of `sgx545_ukernel.fw`, specified in `sgx545_fw_format.md` §9:
`fwcutter.py cut <libsrv_init.so> <libsrv_um.so>` carves the templates from the user's
`libsrv_um.so.1.7.862890` (SHA-256 `2b2c0986…11988a44`, 196152 bytes, same tarball), checks
their hashes, and adds `TQ_META` (heap, alignment, temps, PDS data size and attributes,
no-straddle flag), `TQ_PATCHES` (colour, PBE words and `DOUTU` words into the templates) and
the fixed command words of §4.2.6. `fwload.fill()` and the C loader's `sgx_fw_tq_build()` /
`sgx_fw_tq_cmd()` produce the patched programs for a placement and a fill.

Format §9.8 lists where it departs from the proposal that stood here: one `TQ_PATCHES`
section, the `mov` destination registers as constant patches instead of a `DEST_REG`
encoding, no `PDS_PTR` kind (the background object stays driver code), and the command words
as a `VALUES` list. Cutting and disassembling the seven templates confirmed §4.2.4 as written.

A harness capture of a real `SGXQueueTransfer` FILL (a mock bridge under
`harness/ukernel-extract/fakedrm.c`) remains the independent oracle for the reconstruction,
but it no longer blocks Stage B.

## 5. Open questions, by priority

1. **The §4.2.8 list**, above all whether a FASTSCALE render with the §4.2.3 background
   object renders on hardware. That is the Stage B test itself.
2. **The second dummy (§4.1):** a destination sync that is not ready should keep the
   command queued until `PROCESS_QUEUES`.
3. **`ui32Priority`**: confirm that only 0 is safe (§3.5).
4. **The `ldad.fetchN` offset rule** (§3): confirm against `refdisasm` and fix or document
   it in `tools/usse-dis` and the r2 plugin, since other handlers' offset maps depend on it.
5. **The other request bits** (`0x80000`) and the `TA3DCtl` dwords 49–57 used here; the
   §13.8 request table should be updated with `0x20000` = TA work and `0x40000` = 3D-pipe
   work.
6. **Copies and other formats.** The same machinery with PDS primary `single_source`, the
   TAG state and USE program 0 gives the source copy (`PVR2DBlt`). The format table at
   `0x23240` (`0x8c` bytes per `PVRSRV_PIXEL_FORMAT`) gives the PBE and TAG words for other
   formats.
