# SGX545 Hardware / ABI Spec (reconstructed) — v0.9

> Reconstructed from the GPL DDK **1.7.862890**, cross-verified across two independent
> copies, with architecture context from `sgx_tbdr_primer.md`. This is a *living*
> reconstruction, not a vendor document. Every non-obvious claim cites its source file.

**Scope:** core identity, feature set, MMIO register map shape, MMU/BIF memory model, the
reset/init skeleton (§1–5), the **command-submit (kick) ABI** (§6), and the **microkernel
(EDM) boot & init handshake** (§7) — now including, new in v0.4, **binary RE corroboration of
`srvinit`** from the closed `libsrv_um.so` (§7.4) and a **firmware/microcode map** (§7.5),
the **measured hardware baseline** (§8), **per-register bitfields for the bring-up-critical
registers** (§9), and — new in v0.6 — the **ioctl bridge surface** (§10) and **Windows/EMGD
driver binaries as RE cross-check sources** (§11) — and, new in v0.8, the **captured SGX545
boot payload and the ukernel's EDM structure** (§13): handler map, init scripts, firmware
register map and, new in v0.9, the PDS entry programs and the `sa` register binding. Still
deferred: the TA/transfer command structures (§13.11).

Confidence tags: **[DDK]** pristine GPL source; **[RE]** read from stripped closed-binary
disassembly (radare2; names from retained dynamic symbols); **[CAP]** captured from the
closed SGX545 `srvinit` running under the harness (exact for this build); **[infer]**
architectural inference.

## Provenance & confidence

- **Primary source:** `reference/sgx545-ce/src/` — Intel's DDK 1.7.862890, imported verbatim
  as the repo's first commit (pristine). License GPL-2.0.
- **Corroboration:** `reference/linux_openpvrsgx/.../1.7.862890/pvr/` — second independent
  copy. The **`services4/srvkm/hwdefs/` register headers are byte-identical** between the two
  (the 43 files that differ are all `env/linux/*` kernel-compat glue and the CE5300 system
  port — none are hardware definitions). This gives high confidence the register/feature
  definitions below are the unmodified vendor values.
- **Empirical check (pending):** SGX535-reMESA's captured command streams
  (`reference/SGX535-reMESA/docs/`) — to validate the *dynamic* ABI (kick/command) in a later
  revision. 535 ≠ 545, so differences must be reconciled, not assumed.
- Confidence tags below: **[DDK]** = read directly from pristine source (high);
  **[infer]** = architectural inference (verify before relying).

Citations use paths relative to `reference/sgx545-ce/src/`.

---

## 1. Core identity

| Property | Value | Source |
|---|---|---|
| Friendly name | `SGX545` | `services4/srvkm/hwdefs/sgxfeaturedefs.h:119` |
| Core ID constant | `SGX_CORE_ID_545` | `…/sgxfeaturedefs.h:120` |
| Family | Series 5XT, USSE ISA | primer §2 |
| Header selected by | `#if defined(SGX545)` → `sgx545defs.h` | `…/hwdefs/sgxdefs.h:58` |

The live silicon reports identity via two registers (read these during hardware inventory to
confirm the exact revision in *our* laptop):

| Register | Offset | Purpose |
|---|---|---|
| `EUR_CR_CORE_ID` | `0x001C` | core identity |
| `EUR_CR_CORE_REVISION` | `0x0020` | silicon revision (major/minor/maintenance) |

Source: `…/hwdefs/sgx545defs.h:181,188`. **[DDK]**

## 2. Feature set (authoritative, from the DDK)

Full `#if defined(SGX545)` block, `…/hwdefs/sgxfeaturedefs.h:118–146`. **[DDK]**

| Feature macro | Value | Meaning |
|---|---|---|
| `SGX_FEATURE_ADDRESS_SPACE_SIZE` | 32 | full 32-bit (4 GB) GPU virtual address space |
| `SGX_FEATURE_NUM_USE_PIPES` | 4 | **4 USSE shader pipes** |
| `SGX_FEATURE_NUM_PDS_PIPES` | 2 | 2 Programmable Data Sequencer pipes |
| `SGX_FEATURE_BIF_NUM_DIRLISTS` | 16 | 16 MMU page-directory bases (→ 16 mem contexts) |
| `SGX_FEATURE_MULTIPLE_MEM_CONTEXTS` | — | hardware multi-context MMU |
| `SGX_FEATURE_MONOLITHIC_UKERNEL` | — | single monolithic microkernel (EDM firmware) |
| `SGX_FEATURE_USE_NO_INSTRUCTION_PAIRING` | — | USSE ISA: no paired-instruction slotting |
| `SGX_FEATURE_USE_UNLIMITED_PHASES` | — | USSE: unlimited shader phases |
| `SGX_FEATURE_VOLUME_TEXTURES` | — | 3D/volume texture support |
| `SGX_FEATURE_TEXTURESTRIDE_EXTENSION` | — | strided textures |
| `SGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS` | — | PDS data layout detail |
| `SGX_FEATURE_ZLS_EXTERNALZ` | — | external Z load/store |
| `SGX_FEATURE_MAX_TA_RENDER_TARGETS` | 512 | max render targets per TA (tiling) pass |
| `SGX_FEATURE_NATIVE_BACKWARD_BLIT` | — | backward blit in hardware |
| `SGX_FEATURE_WRITEBACK_DCU` | — | write-back data-cache unit |
| `SGX_FEATURE_BIF_WIDE_TILING_AND_4K_ADDRESS` | — | wide tiling + 4K addressing in BIF |
| `SGX_FEATURE_HOST_ALLOC_FROM_DPM` | — | host allocation from the DPM pool |
| `SGX_FEATURE_SECONDARY_REQUIRES_USE_KICK` | — | secondary requires a USE kick |
| `SGX_FEATURE_MULTI_EVENT_KICK` | — | multiple event kicks |
| `SGX_FEATURE_EDM_VERTEX_PDSADDR_FULL_RANGE` | — | full-range vertex PDS addr from EDM |
| `SGX_FEATURE_AUTOCLOCKGATING` | — | automatic clock gating |

**Notably absent** (vs siblings): no `SGX_FEATURE_2D_HARDWARE` (present on 535/543/554 — 545
has **no dedicated 2D block**), no `SGX_FEATURE_36BIT_MMU` (plain 32-bit MMU), no
`SGX_FEATURE_SYSTEM_CACHE`/mneme. These absences shape what the driver must do in software.

> Errata note: `sgxfeaturedefs.h:186–219` conditionally *undefines* features when specific
> `FIX_HW_BRN_*` are set (e.g. BRN_27266 → drop 36-bit MMU; BRN_27456 → drop wide tiling;
> BRN_31620 → drop multi-context). Which BRNs apply to *our* revision depends on
> `EUR_CR_CORE_REVISION` — a key hardware-inventory question. See `…/hwdefs/sgxerrata.h`.

## 3. MMIO register map

Registers are 32-bit, accessed at offsets from the SGX register BAR (`pvRegsBaseKM` in the
DDK; the physical base comes from the Cedarview PCI device — confirm BAR during inventory).

- **443** distinct `EUR_CR_*` register offsets defined `[DDK]` (`sgx545defs.h`, offset-macro
  count). Each has an associated family of `_MASK`/`_SHIFT`/`_clear_mask` bitfield macros.

**Functional blocks** (by relative bitfield-macro density — a proxy for complexity):

| Block | Role | Notes |
|---|---|---|
| `EVENT` | host⇄GPU event/IRQ status, enable, clear | largest block; two banks (`*2` variants) |
| `BIF` | Bus Interface Unit = the **MMU front-end** | dir-list bases, tiling, fault status |
| `USE` | USSE shader engine control | 4 pipes |
| `CLKGATECTL/…OVR/STATUS` | clock gating control & status | auto-clock-gating core |
| `PDS` | Programmable Data Sequencer | 2 pipes |
| `CORE` | core id/revision/config | identity |
| `SOFT_RESET` | per-subsystem reset bits | 0x0080 |
| `DESIGNER`, `TIMER` | designer id, timers | small |

**Key control registers** (offset — all `[DDK]`, `sgx545defs.h`):

| Register | Offset | Role |
|---|---|---|
| `EUR_CR_CLKGATECTL` | `0x0000` | clock gating control |
| `EUR_CR_CLKGATECTL2` | `0x0004` | clock gating control 2 |
| `EUR_CR_CLKGATESTATUS` | `0x0008` | clock gating status |
| `EUR_CR_CLKGATECTLOVR` | `0x000C` | clock gating override |
| `EUR_CR_CORE_ID` | `0x001C` | core identity |
| `EUR_CR_CORE_REVISION` | `0x0020` | silicon revision |
| `EUR_CR_SOFT_RESET` | `0x0080` | soft reset bits |
| `EUR_CR_EVENT_HOST_ENABLE2` | `0x0110` | event enable (bank 2) |
| `EUR_CR_EVENT_HOST_CLEAR2` | `0x0114` | event clear (bank 2) |
| `EUR_CR_EVENT_STATUS2` | `0x0118` | event status (bank 2) |
| `EUR_CR_EVENT_STATUS` | `0x012C` | event status |
| `EUR_CR_EVENT_HOST_ENABLE` | `0x0130` | event enable |
| `EUR_CR_EVENT_HOST_CLEAR` | `0x0134` | event clear |
| `EUR_CR_BIF_CTRL` | `0x0C00` | BIF/MMU control (pause, invalidate) |
| `EUR_CR_BIF_DIR_LIST_BASE0` | `0x0C84` | MMU page-directory base, context 0 |
| `EUR_CR_BIF_DIR_LIST_BASE1..15` | `0x0C38..0x0C70` | page-directory bases, contexts 1–15 |

## 4. Memory model — MMU & BIF

Source: `…/hwdefs/sgxmmu.h` **[DDK]**, plus BIF registers above.

- **Two-level page table, 4 KB pages.** VA split: **10 / 10 / 12** bits.
  - Page: `SGX_MMU_PAGE_SHIFT = 12` → 4 KB pages.
  - Page Directory: `SGX_MMU_PD_SHIFT = 10` → 1024 PDEs, mask `0xFFC00000`.
  - Page Table: `SGX_MMU_PT_SHIFT = 10` → 1024 PTEs, mask `0x003FF000`.
- **PDE** (`sgxmmu.h:45–52`): bit0 `VALID`; bits1–3 encode page size
  (4K/16K/64K/256K/1M/4M); addr mask `0xFFFFF000` (or `0xFFFFFF00`<<4 if `36BIT_MMU`, which
  545 does **not** have).
- **PTE** (`sgxmmu.h:65–69`): `VALID(0x1)`, `WRITEONLY(0x2)`, `READONLY(0x4)`,
  `CACHECONSISTENT(0x8)`, `EDMPROTECT(0x10)`; addr mask `0xFFFFF000`.
- **BIF** is the MMU front-end. With `BIF_NUM_DIRLISTS = 16` and `MULTIPLE_MEM_CONTEXTS`, up
  to **16 independent page directories** (`EUR_CR_BIF_DIR_LIST_BASE0..15`) → 16 concurrent
  memory contexts. **[DDK]**
- **Tiling:** `EUR_CR_BIF_TILE0..n` program tiled address ranges (min/max address, stride);
  the "wide tiling + 4K address" feature. See `sgxinit.c:1476–1529`. **[DDK]**

## 5. Reset / init skeleton

Extracted from `services4/srvkm/devices/sgx/sgxinit.c` **[DDK]** (high-level; exact bit
values deferred to v0.2):

1. **Map registers** at the SGX BAR → `pvRegsBaseKM`.
2. **Soft reset** via `EUR_CR_SOFT_RESET` (per-subsystem reset bits), then release.
3. **Clock gating** configured via `EUR_CR_CLKGATECTL*` (core auto-clock-gates).
4. **BIF/MMU bring-up:** program the page directory base(s) into
   `EUR_CR_BIF_DIR_LIST_BASE0..15`; configure tiling ranges via `EUR_CR_BIF_TILE*`.
5. **MMU invalidate** sequence (`sgxinit.c:1289–1303`): read `EUR_CR_BIF_CTRL`, set the
   `PAUSE` bit, write `EUR_CR_BIF_CTRL_INVAL` with `PTE_MASK`, then restore `BIF_CTRL`.
6. **Events/IRQ:** enable host events via `EUR_CR_EVENT_HOST_ENABLE[2]`; service/clear via
   `EUR_CR_EVENT_STATUS[2]` / `EUR_CR_EVENT_HOST_CLEAR[2]`.
7. **Load & start the microkernel** (EDM firmware) — monolithic ukernel; interface deferred.
8. Debug dump of state reads the above registers (`sgxinit.c:1031–1038`) — a useful map of
   the registers the vendor itself considers load-bearing.

## 6. Command submission — the kick ABI

The host⇄GPU interface is a **producer/consumer ring in GPU-mapped memory, rung by a
register doorbell**. Every workload — render (TA/3D), transfer/blit, power, cleanup — reaches
the microkernel this way. All **[DDK]**.

### 6.1 The command word — `SGXMKIF_COMMAND` (32 bytes)

`services4/include/sgx_mkif_km.h:49`:

```c
typedef struct {
    IMG_UINT32 ui32ServiceAddress;  // which microkernel routine handles this command
    IMG_UINT32 ui32CacheControl;    // cache-maintenance flags for this command
    IMG_UINT32 ui32Data[6];         // payload: device-virtual pointers to shared structs
} SGXMKIF_COMMAND;                  // 8 x u32 = 32 bytes
```

### 6.2 The ring — kernel CCB

- `PVRSRV_SGX_KERNEL_CCB` = `SGXMKIF_COMMAND asCommands[256]` → a **256-entry ring, 8 KB**
  (`sgx_mkif_km.h:57`).
- `PVRSRV_SGX_CCB_CTL` = `{ ui32WriteOffset, ui32ReadOffset }` (`:63`). Host advances
  `WriteOffset`; the microkernel advances `ReadOffset`. Both wrap **mod 256**.
- `SGXMKIF_HOST_CTL` (`:70`) is the shared handshake block: init/power/cleanup status,
  interrupt flags, ukernel timer clock, lockup counters — how the ukernel reports back.

### 6.3 Command types — `SGXMKIF_CMD_TYPE` (`services4/include/sgxinfo.h:50`)

| Val | Type | Val | Type |
|---|---|---|---|
| 0 | `TA` (render) | 6 | `GETMISCINFO` |
| 1 | `TRANSFER` | 7 | `PROCESS_QUEUES` |
| 2 | `2D` *(unused on 545 — no 2D HW)* | 8 | `DATABREAKPOINT` |
| 3 | `POWER` | 9 | `SETHWPERFSTATUS` |
| 4 | `CONTEXTSUSPEND` | 10 | `FLUSHPDCACHE` |
| 5 | `CLEANUP` | — | (`MAX` = 11) |

### 6.4 Userspace kick payload — `SGX_CCB_KICK` (`sgxinfo.h:239`)

Wraps one `SGXMKIF_COMMAND sCommand` plus the handles the kernel must resolve: CCB kernel
meminfo, destination sync objects (`ui32NumDstSyncObjects` + `pahDstSyncHandles`), TA/3D
status-value sync handles, and `bFirstKickOrResume`. The userspace driver fills this and
ioctls down; `SGXDoKickKM` (`services4/srvkm/devices/sgx/sgxkick.c:43`) validates the handles
and resolves them to device-virtual addresses before enqueuing.

### 6.5 Render-command shared data — `SGXMKIF_CMDTA_SHARED` (`sgx_mkif_km.h:115`)

A TA(render) command's `ui32Data[]` points at this structure: TA/3D status-val counts, the
TA and 3D transfer-queue sync objects (read/write "ops pending" counts + "ops complete"
device virtual addresses), source syncs, the **TA→3D dependency** sync object, and
status-info arrays. The blit analogue is `SGXMKIF_TRANSFERCMD_SHARED` (`:168`).

### 6.6 The doorbell — enqueue & kick

`SGXScheduleCCBCommand` (`services4/srvkm/devices/sgx/sgxutils.c`, ~:380–506):

1. Write the `SGXMKIF_COMMAND` into the CCB at the current `WriteOffset`.
2. `*pui32WriteOffset = (*pui32WriteOffset + 1) & 255;` — advance, wrap mod 256 (`:436`).
3. `*pui32KernelCCBEventKicker = (… + 1) & 0xFF;` — bump the event kicker (`:483`).
4. `OSWriteMemoryBarrier();` — publish the command + offset before the kick (`:485`).
5. **Ring the doorbell:**
   `OSWriteHWReg(pvRegsBaseKM, EUR_CR_EVENT_KICK, EUR_CR_EVENT_KICK_NOW_MASK);` (`:501`) —
   writing bit0 (`NOW`, `0x1`) of **`EUR_CR_EVENT_KICK` @ `0x0AC8`** (`sgx545defs.h:754`)
   tells the microkernel to drain the ring. (`EUR_CR_EVENT_KICK2` @ `0x0AE8` is used instead
   on system-cache + `FIX_HW_BRN_26620` builds — not our config.)
6. `OSMemoryBarrier();` (`:506`).

**In one sentence:** submitting work = append a 32-byte command (which references its sync and
shared structures by device-virtual address), advance the write offset, and poke one register
bit; the microkernel consumes from `ReadOffset` and reports completion via `SGXMKIF_HOST_CTL`
and the EVENT registers.

> Empirical validation target: this exact enqueue→kick pattern (CCB writes, write-offset
> advance, `EVENT_KICK` poke) should be directly visible in SGX535-reMESA's captured
> command streams (`reference/SGX535-reMESA/docs/hardware-evidence`, `docs/phase8`). Confirm
> the ring size, command-word layout, and KICK offset match on the 535 before assuming 545
> parity.

## 7. Microkernel (EDM) boot & init handshake

The SGX545 is driven by a microkernel — the **EDM** (Event-Driven Microkernel) — running on
the USSE. The host boots it, then waits for an in-memory "I'm alive" flag. All **[DDK]**,
source `services4/srvkm/devices/sgx/sgxinit.c`.

### 7.1 The boot sequence is *data, not code* — the init script

The register-level boot is a **replayable script**, not hard-coded C. `SGX_INIT_COMMAND` is a
tiny bytecode run by `SGXRunScript` (`sgxinit.c:242`):

| Opcode | Effect |
|---|---|
| `SGX_INIT_OP_WRITE_HW_REG` | `OSWriteHWReg(regs, offset, value)` — write one register |
| `SGX_INIT_OP_PDUMP_HW_REG` | (pdump builds only) record a register |
| `SGX_INIT_OP_HALT` | end of script → return OK |
| `SGX_INIT_OP_ILLEGAL` | error / unknown |

Three scripts live in `psDevInfo->sScripts`: `asInitCommandsPart1`, `asInitCommandsPart2`,
`asDeinitCommands` (each ≤ `SGX_MAX_INIT_COMMANDS`). **They are built by the userspace
`srvinit`**, not the kernel, and handed down via `SGXGetInfoForSrvinitKM` /
`SGX_BRIDGE_INFO_FOR_SRVINIT` (`sgxinit.c:591`). The microkernel image is likewise loaded by
srvinit, and the EDM task registers (`ui32EDMTaskReg0/1`, `sgxinit.c:214`) are computed there.

> **Feasibility implication (important).** The GPL kernel driver is largely core-agnostic
> *plumbing*: it replays register writes and manages memory/sync. The part that actually
> encodes the SGX545 boot — the init scripts, the EDM task-register values, and the
> microkernel binary — comes from the **closed userspace `srvinit`** (shipped in the
> `cedarview-graphics-drivers` blob). So Goal-2 ("load the DDK on hardware") is two problems:
> (a) build & load the GPL kernel module — tractable; (b) obtain or reconstruct the init
> scripts + ukernel image — the real blocker. Options: reuse the archived Cedarview blob's
> srvinit, or reconstruct the scripts from reMESA-style hardware capture. This is the single
> most important finding for the project's critical path.
>
> **Resolved (v0.8):** both halves are now in hand without hardware — the harness runs the
> archived blob's srvinit to completion, yielding the ukernel image (§12) *and* the complete
> init payload: scripts, EDM task registers, kick addresses (§13).

### 7.2 `SGXInitialise` — the full boot (`sgxinit.c:286`)

1. `SGXInitClocks` — program clock gating.
2. Run **init script part 1** (`asInitCommandsPart1`) — pre-reset register config.
3. `SGXReset(...)` — soft-reset the core (first boot / HW recovery).
4. Write `EUR_CR_POWER = 0` (non-SGX531 path).
5. Zero the kernel-CCB event kicker (`*pui32KernelCCBEventKicker = 0`).
6. Run **init script part 2** (`asInitCommandsPart2`) — programs EDM task regs / ukernel entry.
7. Stamp `HOST_CTL.ui32HostClock`; clear `HOST_CTL.ui32InitStatus = 0`.
8. Memory barrier, then **kick** the microkernel: poke `EUR_CR_EVENT_KICK2` `NOW`
   (multi-event-kick path on 545) — else bump kicker + poke `EUR_CR_EVENT_KICK`
   (`sgxinit.c:373–384`).
9. **Handshake:** `PollForValueKM(&HOST_CTL.ui32InitStatus, PVRSRV_USSE_EDM_INIT_COMPLETE, …)`
   (`sgxinit.c:413`) — block until the ukernel writes `EDM_INIT_COMPLETE` into the shared
   host-control block. Timeout → `SGXDumpDebugInfo` + `PVRSRV_ERROR_RETRY`.

When this returns OK the microkernel is live and the §6 CCB ring is operational.

### 7.3 Pipeline event bits — `EUR_CR_EVENT_STATUS`

The host's view of GPU progress and faults (decoded in the `sgxinit.c:907` debug dump); these
map directly onto the TBDR pipeline from the primer:

`MASTER_INTERRUPT`, `TIMER`, `TA_FINISHED`, `TA_TERMINATE`, `TA_DPM_FAULT`,
`DPM_OUT_OF_MEMORY_ZLS`, `DPM_TA_MEM_FREE`, `DPM_INITEND`, `ISP_END_TILE`,
`ISP_VISIBILITY_FAIL`, `ISP2_ZLS_CSW_FINISHED`, `PIXELBE_END_RENDER`, `BREAKPOINT`,
`SW_EVENT`.

(DPM = Display-list/Parameter Manager feeding the tile pipeline; ISP = hidden-surface stage;
PIXELBE = pixel back-end, i.e. tile flush. `TA_*` = the geometry/tiling front-end.)

### 7.4 `srvinit` internals — empirical (from `libsrv_um.so`, radare2) **[RE]**

Static RE of the preserved closed userspace (DDK 1.7.862890, `i386 ELF`;
`reference/preserved/cedarview-gfx-blob/.../usr/lib/`) corroborates §7.1 and adds detail:

- **Entry points.** `libsrv_init.so` is a thin wrapper: exported `SrvInit` → imports
  `SGXGetInfoForSrvInit` + `SGXDevInitPart2`, both defined in **`libsrv_um.so`** (exported at
  `0x9f90` / `0xa060`). These mirror the GPL kernel's `SGXGetInfoForSrvinitKM` /
  `SGXDevInitPart2` — i.e. the two sides of the §7.1 userspace⇄kernel init handshake.
- **`SGXDevInitPart2` (939 B @ `0xa060`)** allocates a **~2 KB init block**
  (`PVRSRVCallocUserModeMem(0x814)`), then marshals many fields of a device/context struct
  (`esi` + `0x78`, `0x7c`, `0xa8`–`0xbc`, …) into that block (`ebp` + `0x768`, `0x78c`,
  `0x79c`, `0x7a0`, …), interleaving ~16 calls to a helper at `0x74a0` with two fixed-table
  copies via `PVRSRVMemCopy` (`0x2c` = 44 B, `0x1b0` = 432 B). The populated block is handed
  to the kernel. This is the **userspace construction of the `SGX_BRIDGE_INFO_FOR_SRVINIT`
  payload** (init scripts + EDM task regs + heap config) that the kernel consumes — §7.1
  confirmed from the binary.
- **USSE microkernel: location not yet confirmed** (corrects the first v0.4 draft). It is
  *not* a standalone `.fw` file, and *not* in the GPL kernel — the kernel references the
  ukernel only via status/signature/timer fields, never as a binary array. So it is embedded
  in the closed userspace, most likely `libsrv_um.so`. The `.rodata` strings `"USSE code"` /
  `"PDS code"` / `"NumVertexPartitions"` are **heap / app-hint label strings, not the image**
  (an earlier draft wrongly read them as the ukernel). Locating it (v0.5) needs a data-flow
  trace, not a string match: follow the routine that creates the SGX *kernel-code heap* and
  the large upload/`PVRSRVMemCopy` that fills it, or scan `.rodata`/`.data` for the largest
  non-string contiguous blob and test-disassemble it as USSE with reMESA/sgx540 tooling.

> Methodology note: `add ebx, 0x…` sequences in these PIC binaries are GOT-base setup, not
> data-table references — don't mistake them for register tables.

### 7.5 Firmware / microcode map — three distinct paths **[DDK/RE]**

"Is the microkernel part of linux-firmware?" resolves the platform into three separate
firmware stories, which map cleanly onto the project goals:

| Engine | Microcode/firmware | Load path | In linux-firmware? | Goal |
|---|---|---|---|---|
| Cedarview display controller | none | gma500 KMS, no fw | — | Goal 1 (display) |
| **SGX545 3D** (USSE EDM ukernel) | embedded in closed userspace libs | **userspace upload** `pvrsrvctl`→`SrvInit`→`SGXDevInitPart2` (§7.1/§7.4) | **No** | Goal 2 (3D) |
| Video **MSVDX/VXD** decode + **Topaz** encode | `.bin` blobs | kernel `request_firmware()` (`imgv/psb_msvdxinit.c`) from `/lib/firmware` | partial (Poulsbo/Medfield-era) | Goal 1 (VA-API) |

- The PowerVR blob that landed in **linux-firmware.git (~Linux 6.8) is for Rogue (Series 6+)**
  — a different ISA — and does **not** apply to our Series-5 SGX545.
- The SGX **3D microkernel is *not* a `request_firmware` blob**; it is uploaded from the
  closed userspace. Confirmed three ways: `openpvrsgx` uses `request_firmware` only for MSVDX
  (`imgv/psb_msvdxinit.c`), never SGX; our RE of `libsrv_um` (§7.4); and IMG's own statement
  that "the SGX microkernel is uploaded by `pvrsrvctl`." This is exactly why preserving the
  Cedarview userspace blob (which embeds it) is the Goal-2 critical path.
- The Cedarview package **does** ship `/lib/firmware` video blobs — `msvdx_fw*.bin` (VXD
  decode), `topaz*_fw.bin` (encode) — now preserved under
  `reference/preserved/.../cedarview-vaapi-v1.0.3_bee/lib/firmware/`. These are the Goal-1
  video path and are the kind of blob that legitimately belongs in `/lib/firmware`.

## 8. Hardware baseline — measured on the target (2026-10-19)

Target: laptop `user@laptop` — Atom **N2600** @ 1.6 GHz (4 threads), 2 GB RAM,
**i686**, Ubuntu 18.04 LTS, kernel **4.15.0-20-generic**.

These measurements are from Ubuntu. Since 2026-10-20 the laptop dual-boots, with Arch Linux 32
(kernel 6.19) as the default and Ubuntu still available. Nothing below has been re-measured on
Arch yet, including whether 6.19's `STRICT_DEVMEM` settings allow the `/dev/mem` reads. The
address is now `laptop` for both systems.

| Item | Measured | Source |
|---|---|---|
| GPU | Cedarview, PCI **`8086:0be1` rev 09** @ `00:02.0` (subsys `8086:1999`) | `lspci -nnk` |
| Driver bound | **`gma500`** (`gma500_gfx`) — display/KMS only; `/dev/dri/card0`, `psbdrmfb` | `lsmod` |
| 3D | **llvmpipe software** (Mesa 18.0) — no HW accel | `glxinfo` |
| Register BAR | **`0x80000000`, 1 MB, non-prefetchable, IRQ 25**; I/O `0x7100`[8] | `lspci -v` |
| **SGX register base** | **`0x80080000`** = BAR + `SGX_REGS_OFFSET` (`0x80000` CDV; `0x40000` Poulsbo) | DDK `services4/system/unified/sysconfig.{h,c}` |
| `/dev/mem` | present → direct register reads possible | — |
| On-device build | gcc + `/lib/modules/4.15.0-20-generic/build` + internet → can build `pvrsrvkm` here | — |

Derived physical register addresses (for `/dev/mem`):
- `EUR_CR_CORE_ID` (0x1C) → **`0x8008001C`**
- `EUR_CR_CORE_REVISION` (0x20) → **`0x80080020`**

**Significance:** this one machine is a complete Goal-2 workbench — native i386 DDK userspace,
on-device kernel-module build, and raw register access via `/dev/mem`.

### Remaining open

1. ~~**`CORE_REVISION` value**~~: read on 2026-10-20, see §8.1.
2. **Stolen/GTT size** — not captured (`dmesg` restricted); get via `sudo dmesg` / `/sys`.
3. **Driver ownership conflict (Goal-2):** `gma500` currently owns `00:02.0`. The closed
   Cedarview stack *replaced* `gma500` with its own `psb`+`pvrsrvkm`. Bring-up must therefore
   either coexist (gma500 display + pvrsrvkm driving SGX via the shared BAR) or swap to the
   Cedarview kernel — to be decided.
4. **(critical path, §7.1)** Reuse vs reconstruct the ukernel + init scripts from the preserved
   closed userspace (`libsrv_init`/`libsrv_um`).

### 8.1 Identity, measured on Arch Linux 32 (2026-10-20)

Kernel 6.19.11 with `STRICT_DEVMEM` and `IO_STRICT_DEVMEM` on. `gma500` binds `00:02.0` and
brings up `/dev/dri/card0` and fbdev, with no power-management timeout. It does not claim the
register BAR in `/proc/iomem`, so `/dev/mem` reads work despite `IO_STRICT_DEVMEM`. The SGX
island was powered with gma500 alone.

| register | value | decode |
|----------|-------|--------|
| `CORE_ID` (`0x1c`) | `0x01150000` | ID `0x0115`, config 0 |
| `CORE_REVISION` (`0x20`) | `0x0001000d` | **1.0.13** (`SGX_CORE_REV` 1013 / 10131) |
| `CLKGATECTL`, `SOFT_RESET` | `0`, `0` | idle, no block held in reset |
| host bridge `00:00.0` | `8086:0bf1` rev **03** | the early Cedarview stepping |

- **The silicon matches the closed ukernel.** Its GETMISCINFO handler (`0xb98`) reports
  `CoreRevSW = 0x1000d` (1.0.13), `CoreIdSW = 0x221` (545), DDK 1.7.17 build 862890 and
  build options `0x60218`. The kernel's revision check passes without the exception pair the
  CE5310 port (silicon 1.0.14) needs.
- **The ukernel was built without the BRN31939 HostCtl field.** With `SUPPORT_HW_RECOVERY`
  (build option bit 3), a 1013 build adds `ui32BRN31939Mem` and makes HostCtl 0x48. The ukernel
  states 0x44, so it matches a 10131 build (which reports as 1.0.13 and carries only
  `FIX_HW_BRN_SAMPLE_CACHE`). The 17-dword layout in `ccb.py` stands.
- **The sample-cache workaround applies.** Intel's Cedarview kernel enables it when the host
  bridge revision is below 4 (`PCIInitDev`), and ours is 3.

System-layer note: our target follows the DDK **unified/CDV** path (`SGX_REGS_OFFSET=0x80000`
within the shared `00:02.0` BAR), **not** the CE5300 path (`0x00000`, SGX on its own function).

## 9. Per-register bitfields — bring-up-critical registers **[DDK]**

All from `sgx545defs.h`. On the target the register base is `0x80080000` (§8), so a register
at offset `X` is at physical `0x80080000 + X`.

### 9.1 `CORE_ID` (0x1C) & `CORE_REVISION` (0x20) — identity (the first HW read)

- `CORE_ID`: `ID` [31:16], `CONFIG` [15:0].
- `CORE_REVISION`: `DESIGNER` [31:24] | `MAJOR` [23:16] | `MINOR` [15:8] | `MAINTENANCE` [7:0].

Decode recipe for the u32 read at phys `0x80080020`:
`major=(v>>16)&0xFF; minor=(v>>8)&0xFF; maint=v&0xFF; designer=(v>>24)&0xFF`.
The DDK encodes a revision decimally as `MAJOR MINOR MAINT` (e.g. 1.2.1 → `121`). `sgxerrata.h`
enumerates SGX545 revisions (1.1.0, 1.1.1, 1.2.0/1/5, 1.3.0, …), each enabling specific
`FIX_HW_BRN_*`. So this single read selects which errata — and thus which feature *undefs*
(§2) — apply to our silicon.

### 9.2 `SOFT_RESET` (0x80) — per-block reset (1 bit each; 1 = hold, 0 = release)

| bit | field | block |
|---|---|---|
| 0 | `BIF_RESET` | Bus Interface / MMU |
| 1 | `DPM_RESET` | Display-list/Parameter Manager |
| 2 | `TA_RESET` | Tile Accelerator (geometry/tiling) |
| 3 | `USE_RESET` | USSE (shaders) |
| 4 | `ISP_RESET` | Image Synthesis Processor (HSR) |
| 5 | `TSP_RESET` | Texture & Shading Processor |

Used by `SGXReset` (§7.2 step 3). The bits enumerate the TBDR pipeline blocks (primer §4).

### 9.3 `CLKGATECTL` (0x00) — per-unit clock gating (2 bits/unit)

`ISP`[1:0], `ISP2`[3:2], `TSP`[5:4], `TE`[7:6], `MTE`[9:8], `DPM`[11:10], `VDM`[13:12],
`PDS0`[15:14]; `AUTO_MAN_REG`[24] = 1 selects automatic clock gating (SGX545 has
`SGX_FEATURE_AUTOCLOCKGATING`). Each 2-bit field sets that unit's gating mode.

### 9.4 `BIF_CTRL` (0x0C00) — MMU control

`NOREORDER`[0], `PAUSE`[1], `FLUSH`[2], `INVALDC`[3] (invalidate directory cache),
`CLEAR_FAULT`[4]; per-requestor MMU bypass: `CACHE`[8], `VDM`[9], `TE`[10], `PBE`[11],
`TSPP`[12], `ISP`[13], `USE`[14]. The MMU-invalidate sequence (§5 step 5 / §7) = set `PAUSE`,
write the invalidate (`BIF_CTRL_INVAL` alias with the PTE mask), then restore `BIF_CTRL`.

### 9.5 `EVENT_KICK` (0x0AC8) — the doorbell

`NOW`[0]: writing 1 signals the microkernel to drain the CCB (§6.6).

### 9.6 Unit glossary (recurs across SOFT_RESET / CLKGATECTL / BIF bypass)

`BIF` bus-interface+MMU · `DPM` display-list/parameter manager · `TA` tile accelerator ·
`VDM` vertex DMA · `TE`/`MTE` tiling engine / macro-tiling engine · `PDS` programmable data
sequencer · `USE` USSE shader core · `ISP` hidden-surface processor · `TSP`/`TSPP` texture &
shading (+ pre-processor) · `PBE` pixel back-end (tile flush).

## 10. Userspace⇄kernel ioctl bridge surface **[DDK]**

Userspace reaches the kernel via `PVRSRV_IOWR` ioctls dispatched in `services4/srvkm/bridged/`.
SGX calls are numbered from `PVRSRV_BRIDGE_SGX_CMD_BASE` (after the generic PVRSRV commands);
**33** SGX commands total (`sgx_bridge.h`). Each has a paired `PVRSRV_BRIDGE_IN_*` /
`PVRSRV_BRIDGE_OUT_*` struct defining its argument ABI. Key commands:

| Command | # | Purpose |
|---|---|---|
| `GETCLIENTINFO` | base+0 | handshake: client obtains kernel/device info |
| `DOKICK` | base+3 | **submit a render/compute kick** — carries `SGX_CCB_KICK` (§6.4); the hot path onto the CCB ring (§6) |
| `2DQUERYBLTSCOMPLETE` | base+9 | 2D blit completion query |
| `SUBMITTRANSFER` | base+13 | submit a transfer-queue (blit) command |
| `GETMISCINFO` | base+14 | misc HW/ukernel state |
| `REGISTER_HW_RENDER_CONTEXT` | base+20 | register a render context |
| `UNREGISTER_HW_RENDER_CONTEXT` | base+22 | tear down a render context |
| `SET_RENDER_CONTEXT_PRIORITY` | base+30 | scheduling priority |
| `SET_TRANSFER_CONTEXT_PRIORITY` | base+31 | scheduling priority |

(plus `SGXGetInfoForSrvinit` and the shared-PB-desc / context-alloc calls). This is the surface
the closed userspace (`libsrv_um`, §7.4) — or the Windows UMD, §11 — drives; `DOKICK` is the
command-submit hot path.

## 11. Windows & EMGD driver binaries — RE cross-check sources **[RE]**

Preserved under `reference/preserved/windows-drivers/` (archive.org item `intel-gma-drivers-win`,
checksummed). Same SGX545 silicon, different OS/build — complementary to the Linux closed
userspace (§7.4):

- **`win7_sgx545_8.0.4.1.1096`** (Intel GMA 3600, Win7 32-bit; sha256 `41d5403b…`). All
  **PE32 i386**: `igdkmd32.sys` = kernel miniport, `igdumd32.dll` = user-mode D3D driver,
  `igdogl32.dll` = OpenGL, `igddxva32.dll` = video. Deeper RE (radare2) findings:
  - **Build = DDK ~1.8 WDDM** (`wddm_ddk … 1.8.INTELWIN7.BETA3@780347`, PDB `igdkmd32.pdb`) —
    a *different* build/version than our Cedarview Linux DDK 1.7.862890 (same SGX545 silicon,
    different OS stack). `SGXInit` validates driver↔device DDK revision.
  - **Generic multi-core SGX KMD**: strings enumerate SGX520→554, **including SGX545** — one
    driver, core selected at runtime; applies to our chip.
  - **The microkernel is embedded here** (unlike the stripped Linux libs): `"SGX Microkernel
    assert fail: 0x%08X"`, the `sgx_options.h` driver↔ukernel build-options check, and the
    USSE-codegen strings (`"Error generating main/SA-update/subroutine USSE code"`) confirm
    both the ukernel blob (in `igdkmd32.sys`) and the USSE shader compiler (in
    `igdumd32.dll`/`igdogl32.dll`) are present.
  - **185 named `_PVRSRV*KM` / `_DevInitSGXPart2KM` / SGX symbols** in `igdkmd32.sys`
    (saved to `debug/win_kmd_sgx_symbols.txt`) — a labeled map that overlays onto the stripped
    Linux `libsrv_um`/`libsrv_init` (§7.4). (The UMD exports only WDDM DDI entry points, so its
    internals stay opaque — but the KMD is well-labeled.)
  - These named functions map 1:1 onto our GPL kernel source (great for labeling the kernel
    side). **But the ukernel is *not* a static blob in the KMD** (corrects an earlier
    optimism): disassembly shows `DevInitSGXPart2KM` *receives* the init block via arguments
    (`arg_ch`/`arg_10h`) — the ukernel is **uploaded from usermode**, exactly as on Linux
    (§7.1). `fcn.00020426` (which holds the "Microkernel assert" string) is the **debug-dump**
    routine (`SGXDumpDebugInfo`), not the loader. So the ukernel binary sits in the **stripped
    usermode `igdumd32.dll`** — the Windows twin of the stripped `libsrv_um.so`, equally
    unlabeled. The Windows driver's real wins are the labeled *kernel* services map + confirmed
    presence of ukernel/compiler, **not** an easy ukernel carve.
- **`EMGD_CDV_1.15.1_GC_3278`** (Windows EMGD, **Cedarview**; sha256 `b6eab159…`) — a proprietary
  Intel "SEA" self-extractor whose 77.5 MB `.setup` payload no standard tool unpacks. The only
  Cedarview EMGD we hold; **non-critical** (we already have the Cedarview DDK 1.7 blob + GPL
  `sgx545-ce` + the Win7 GMA3600 driver), so not worth the Wine/carver effort now.

**EMGD for Linux** — `reference/emgd-linux/` (git clone of `EMGD-Community/intel-binaries-linux`,
surfaced via the MeeGo/Moblin lead). ⚠ **Built for SGX535, not SGX545**: the extracted
`libemgdsrv_um.so` carries `-DSGX535 -DSGX_CORE_REV=121 -DSUPPORT_SGX535`,
`binary_pc_i686_poulsbo_…` (Poulsbo / Tunnel Creek). EMGD v1.18 / PVR DDK 1.5.15.3226. So it is
a **sibling-core source**, not a Cedarview one — but it is the **exact SGX535 rev121 that
`SGX535-reMESA` targets**, with GPL kernel source + Linux-native extractable userspace +
official Intel PDFs. Use it as the reMESA cross-reference and to study the EMGD userspace
structure; for SGX545 specifics, the Cedarview sources (DDK 1.7 blob, `sgx545-ce`, Win7 driver)
remain primary.

**RE strategy:** use the Windows KMD's named symbols to label the equivalent stripped routines
in `libsrv_um.so`, accelerating the §7.4/§9/ukernel work. USSE shader codegen lives in
`igdumd32.dll`/`igdogl32.dll` (Windows) and `libglslcompiler.so` (Linux) — cross-readable.

## 12. Next extraction targets (spec v0.7)

- **Extract the USSE microkernel image — DONE ✅** via the mock-and-intercept harness
  (`harness/ukernel-extract/`, output + provenance in `extracted/MANIFEST.md`). `SrvInit` runs to
  completion (returns 0) under a fake `libdrm` on cachix and uploads the ukernel into a harness-
  owned buffer. Result: the **53096-byte (0xCF68) SGX545 USSE microkernel**, a static blob in
  **`libsrv_init.so` @ file offset `0x2080`** (not `libsrv_um`), plus 5 auxiliary USSE programs at
  offsets `0x1ea0`–`0x2058`. The loader patches **54 bytes (18 × 24-bit address fields)** into the
  template at init. Key facts the extraction nailed down, now spec-grade:
  - **Bridge command numbering**: the closed lib is built **PDUMP- and OVERLAY-disabled**, so
    `PVRSRV_BRIDGE_SGX_CMD_BASE = 83` and `INITSRV_CONNECT = 67` (bridge-ID low byte = absolute
    index). srvinit path: `CONNECT(12) INITSRV_CONNECT(67) ENUM(0) ACQUIRE(1) SGXINFO_FOR_SRVINIT(98)`
    then per buffer `ALLOC_DEVICEMEM(6) MHANDLE_TO_MMAP_DATA(11) mmap2`.
  - **CPU mapping path**: `libsrv_um` imports `syscall`+`munmap` but **not `mmap`** — device memory
    is mapped via raw `syscall(SYS_mmap2)` on the drm fd at the offset returned by
    `MHANDLE_TO_MMAP_DATA`.
  - **`SGXINFO_FOR_SRVINIT` OUT** = `u32 eError; u32 sPDDevPAddr; PVRSRV_HEAP_INFO asHeapInfo[32]`
    (776 B). srvinit requires heap IDs 0..6 present (`GENERAL,TADATA,KERNEL_CODE,KERNEL_DATA,
    PIXELSHADER,VERTEXSHADER,PDSPIXEL_CODEDATA`); the ukernel is allocated from **KERNEL_CODE
    (ID 2)**.
  - **`PVRSRV_CLIENT_MEM_INFO`** field offsets (confirmed empirically): `pvLinAddr`@0, …,
    `uAllocSize`@24, `hMappingInfo`@44, `hKernelMemInfo`@48; **`IMG_CPU_PHYADDR` is 64-bit**.
  - ~~Runtime capture on the 18.04 laptop~~ stays blocked (kernel 2.6.25–3.0.32 vs 4.15) — moot now.
- **Disassemble the ukernel + decode the init block — DONE ✅** (§13): clean-room USSE decoder
  `tools/usse-dis/` (99.9% of the ukernel, 0 wrong vs Imagination's own `ref_usse_disasm`, which
  `tools/usse-dis/refdisasm/` builds from PVR_PSP2 as the oracle) and the `usse` radare2 plugin;
  EDM entry points, init scripts and the firmware register map decoded in §13.
- **Next on the ukernel** (§13.8): the EDM PDS program and its `sa` register binding, the 18
  address-patch sites, per-handler command payloads, and the 5 auxiliary KERNEL_CODE programs.
- **Version-diff `sgx545defs.h`** across DDK `1.5.15` (`emgd-linux`), `1.7` (`sgx545-ce`) and
  `→1.17` (`linux_openpvrsgx`) — the register headers are core-agnostic so this is valid even
  though `emgd-linux` is an SGX535 *build*. Use `emgd-linux` + its PDFs mainly as the
  **SGX535-reMESA cross-reference** (same rev121 core).
- Cross-check §6/§7/§9/§10 against SGX535-reMESA captured streams + the `emgd-linux` SGX535
  rev121 build (`reference/SGX535-reMESA/docs/hardware-evidence`, `docs/phase8`), reconciling
  535↔545.

---

## 13. Captured SGX545 boot payload & ukernel EDM structure **[CAP/RE]**

§7 describes the boot *mechanism* from the DDK. This section records the actual **SGX545
content**, captured by running the closed `srvinit` to completion under the harness and
decoded with `harness/ukernel-extract/initinfo.py` (tested: `test_initinfo.py`). The
2068-byte IN buffer of `SGX_DEVINITPART2` is the complete `SGX_BRIDGE_INIT_INFO`
(`extracted/sgx545_initinfo.bin`). Its `#ifdef`-dependent layout is pinned empirically: every
meminfo-handle field lands on a harness allocation token, and the field table consumes the
buffer exactly.

The ukernel analysis uses the `usse` radare2 plugin (register profile + ESIL). The generated
script labels the handlers, the boot path and all 604 firmware register-access sites:

```sh
radare2 -a usse -b 32 -i harness/ukernel-extract/sgx545_ukernel.r2 \
        harness/ukernel-extract/extracted/sgx545_ukernel_template.bin
```

### 13.1 Build configuration (inferred from the layout) **[CAP]**

- **No `SGX_FEATURE_2D_HARDWARE`.** `SGX_MISCINFO_STRUCT_SIZES` carries 13 fields (the two
  `2DCMD` sizes are absent) and the 2D host-kick address is 0. 2D work must go through the
  TRANSFER queue — relevant to any 2D-acceleration route.
- `SUPPORT_SGX_HWPERF` on (a HWPerf circular-buffer handle is present); no `SGX_FEATURE_MP`;
  no PTLA write-back or BRN-workaround handles. Client build options = `0x60218`.

### 13.2 Control blocks **[CAP]**

All live in KERNEL_DATA (`0xF0000000`); the code buffers live in KERNEL_CODE (`0xE8000000`).

| init-info field | alloc | size | notes |
|---|---|---|---|
| `hKernelCCBMemInfo` | #0 | `0x2000` | kernel CCB: 256 × `SGXMKIF_COMMAND` (`0x20`) |
| `hKernelCCBCtlMemInfo` | #1 | `0x8` | CCB control: write offset +0 (host), read offset +4 (EDM) |
| `hKernelCCBEventKickerMemInfo` | #2 | `0x4` | event-kicker word |
| `hKernelSGXTA3DCtlMemInfo` | #3 | `0xec` | TA/3D control |
| `hKernelSGXHostCtlMemInfo` | #4 | `0x44` | host control (= `sizeof SGXMKIF_HOST_CTL`) |
| `hKernelSGXMiscMemInfo` | #5 | `0x54` | misc info |
| `hKernelHWPerfCBMemInfo` | #6 | `0x640c` | HWPerf circular buffer |
| `hKernelTASigBufferMemInfo` | #7 | `0x620` | TA signature buffer |
| `hKernel3DSigBufferMemInfo` | #8 | `0x2088` | 3D signature buffer |
| `asInitMemHandles[0..5]` | #9–14 | — | KERNEL_CODE: ukernel (`0xCF68`) + 5 aux programs |

### 13.3 MKIF structure sizes the ukernel was built against **[CAP]**

`CMDTA 0x2b0`, `CMDTA_SHARED 0x1e0`, `TRANSFERCMD 0x4c8`, `TRANSFERCMD_SHARED 0xdc`,
`3DREGISTERS 0xd0`, `HWPBDESC 0x3c`, `HWRENDERCONTEXT 0x44`, `HWRENDERDETAILS 0x1ec`,
`HWRTDATA 0x38`, `HWRTDATASET 0x50`, `HWTRANSFERCONTEXT 0xb4`, `HOST_CTL 0x44`, `COMMAND 0x20`.
A host-side reimplementation must lay these structures out to exactly these sizes.

### 13.4 EDM handler map — `aui32HostKickAddr[]` **[CAP/RE]**

The KMD copies entry *n* into each queued command's `ui32ServiceAddress`
(`sgxutils.c:374`), and the EDM branches to it. The values are **ukernel instruction
indices** (×8 = byte offset) from the EDM code base (`USE_CODE_BASE_15` = `0xE8000000`,
§13.5).

| `SGXMKIF_CMD_*` | insn | byte | opening instructions |
|---|---|---|---|
| TA | `0x11d` | `0x8e8` | `mov r1, r7; ldad r2, [r1,+#5]` — largest handler (11 blocks) |
| TRANSFER | `0x103` | `0x818` | `mov r1, r7; ldad r2, [r1,+#5]` — same prologue as TA |
| 2D | — | — | no 2D core (§13.1) |
| POWER | `0xd4` | `0x6a0` | tests `r7 == 3` |
| CONTEXTSUSPEND | `0xeb` | `0x758` | `mov r0, r6; ldad r1, [r0,+#0]` |
| CLEANUP | `0x13a` | `0x9d0` | tests `r6 == 1` |
| GETMISCINFO | `0x173` | `0xb98` | `ldr r1, #8; ldr r3, #7` → reads `CORE_REVISION`, `CORE_ID` |
| PROCESS_QUEUES | `0x156` | `0xab0` | one-instruction trampoline: `ba 0x161` |
| DATABREAKPOINT | — | — | not built |
| SETHWPERFSTATUS | `0x157` | `0xab8` | `ldad r0, [sa0,+#43]; mov r1, #0xFFFFFFFF` |
| FLUSHPDCACHE | — | — | not built |

Calling convention **[infer]**: `r7`/`r6` carry the command's parameter words (TA and
TRANSFER dereference `r7` as a pointer to command data); control blocks are addressed through
`sa` (secondary-attribute) registers, e.g. `[sa0,+#43]`. `GETMISCINFO` is how the host learns
the core ID and revision through the firmware — complementary to the direct `/dev/mem` read
of §8.

### 13.5 Init scripts — the SGX545 register boot sequence **[CAP]**

**Part 1** (before reset, 2 writes): `EVENT_PDS_ENABLE = 0`, `EVENT_TIMER = 0`.

**Part 2** (after reset, 38 writes, in order; names drop the `EUR_CR_` prefix):

| # | reg | name | value | note |
|---|------|------|-------|------|
| 1 | `0x0818` | `DCU_BYPASS` | `0x000007ff` |  |
| 2 | `0x0810` | `DCU_ICTRL` | `0x00000001` |  |
| 3 | `0x0814` | `DCU_PCTRL` | `0x0000ffff` |  |
| 4 | `0x0130` | `EVENT_HOST_ENABLE` | `0x00004000` |  |
| 5 | `0x0110` | `EVENT_HOST_ENABLE2` | `0x00000000` |  |
| 6 | `0x013c` | `PDS_CACHE_HOST_ENABLE` | `0x00000000` |  |
| 7 | `0x0a7c` | `USE_G0` | `0x00000000` |  |
| 8 | `0x0a80` | `USE_G1` | `0x00000000` |  |
| 9 | `0x0a74` | `DMS_CTRL` | `0x00208200` |  |
| 10 | `0x04e8` | `ISP_DEPTHSORT` | `0x00000100` |  |
| 11 | `0x0a78` | `PDS_VCB_PERF_CTRL` | `0x0000002c` |  |
| 12 | `0x0aac` | `USE_TMPREG` | `0x00000045` |  |
| 13 | `0x0abc` | `PDS_DMS_CTRL2` | `0x0000000e` |  |
| 14 | `0x0800` | `TSP_CONFIG` | `0x00000030` |  |
| 15 | `0x0804` | `CACHE_CTRL` | `0x0000ffff` |  |
| 16 | `0x0a00` | `USE_CTRL` | `0x0007c000` |  |
| 17 | `0x0a0c` | `USE_CODE_BASE_0` | `0x03e80000` | DM=PIXEL, code `0xf4000000` |
| 18 | `0x0a10` | `USE_CODE_BASE_1` | `0x03e81000` | DM=PIXEL, code `0xf4080000` |
| 19 | `0x0a14` | `USE_CODE_BASE_2` | `0x03e82000` | DM=PIXEL, code `0xf4100000` |
| 20 | `0x0a18` | `USE_CODE_BASE_3` | `0x03e83000` | DM=PIXEL, code `0xf4180000` |
| 21 | `0x0a1c` | `USE_CODE_BASE_4` | `0x03e84000` | DM=PIXEL, code `0xf4200000` |
| 22 | `0x0a20` | `USE_CODE_BASE_5` | `0x03e85000` | DM=PIXEL, code `0xf4280000` |
| 23 | `0x0a24` | `USE_CODE_BASE_6` | `0x03e86000` | DM=PIXEL, code `0xf4300000` |
| 24 | `0x0a28` | `USE_CODE_BASE_7` | `0x03e87000` | DM=PIXEL, code `0xf4380000` |
| 25 | `0x0a2c` | `USE_CODE_BASE_8` | `0x03e88000` | DM=PIXEL, code `0xf4400000` |
| 26 | `0x0a30` | `USE_CODE_BASE_9` | `0x03e89000` | DM=PIXEL, code `0xf4480000` |
| 27 | `0x0a34` | `USE_CODE_BASE_10` | `0x01f80000` | DM=VERTEX, code `0xfc000000` |
| 28 | `0x0a38` | `USE_CODE_BASE_11` | `0x01f81000` | DM=VERTEX, code `0xfc080000` |
| 29 | `0x0a3c` | `USE_CODE_BASE_12` | `0x01f82000` | DM=VERTEX, code `0xfc100000` |
| 30 | `0x0a40` | `USE_CODE_BASE_13` | `0x01f83000` | DM=VERTEX, code `0xfc180000` |
| 31 | `0x0a48` | `USE_CODE_BASE_15` | `0x07d00000` | DM=EDM, code `0xe8000000` |
| 32 | `0x0ab8` | `PDS_EXEC_BASE` | `0xe4000000` |  |
| 33 | `0x0b30` | `USE_MOE` | `0x00000100` |  |
| 34 | `0x0ac4` | `EVENT_KICKER` | `0xf0004000` | address of the event-kicker word (#2) |
| 35 | `0x0a68` | `EVENT_OTHER_PDS_EXEC` | `0xe800e000` | boot PDS program (#10), §13.8 |
| 36 | `0x0a6c` | `EVENT_OTHER_PDS_DATA` | `0x00000001` |  |
| 37 | `0x0a70` | `EVENT_OTHER_PDS_INFO` | `0x00000006` |  |
| 38 | `0x0a58` | `EVENT_PDS_ENABLE` | `0x2204b00f` |  |

**Deinit** (9 writes): clear `EVENT_PDS_ENABLE`, `EVENT_OTHER_PDS_EXEC/DATA/INFO`,
`USE_CODE_BASE_15` and `EVENT_HOST_ENABLE/ENABLE2`, then `CLKGATECTL = CLKGATECTL2 = 0`.

`USE_CODE_BASE_n` decodes as `ADDR[24:0]` in 128-byte units plus a data-master field
`DM[26:25]` (`sgx545defs.h`). With that decode, all three groups land exactly on heaps from
the heap map — an independent check on both the decode and the heap layout:

| registers | DM | code address | heap |
|---|---|---|---|
| `USE_CODE_BASE_0..9` | PIXEL | `0xF4000000 + n·0x80000` | PIXELSHADER |
| `USE_CODE_BASE_10..13` | VERTEX | `0xFC000000 + n·0x80000` | VERTEXSHADER |
| `USE_CODE_BASE_15` | **EDM** | **`0xE8000000`** | **KERNEL_CODE — the ukernel** |

Reading the sequence **[infer from order]**: cache/DCU and event configuration → shader code
bases → `PDS_EXEC_BASE = 0xE4000000` (PDSPIXEL heap) → `USE_MOE` → `EVENT_KICKER` = the
kicker word's address → the EDM's boot PDS program (`EVENT_OTHER_PDS_EXEC` = its absolute
address in bits [31:4]; `DATA` = data-segment size 1; `INFO` = 6, i.e. attribute size 1 with
`SD` set) → finally `EVENT_PDS_ENABLE = 0x2204b00f`, which arms event→PDS dispatch so that
the first kicked event starts the EDM.

The address-valued fields above come from a capture in which the harness gave every
allocation a unique, realistic device address (a per-heap bump allocator in `fakedrm.c`); an
earlier capture with all addresses 0 had left them looking like constants.

The mock's other replies were checked the same way:

- **`SGX_GETMISCINFO` has no effect.** srvinit asks for `DRIVER_SGXREV` (`eRequest = 2`) and the
  zero-mock answers core revision, core ID, DDK version/build, SW core ID/revision and build
  options all 0. Captures with two distinct, plausible feature sets (one with deliberately
  mismatched build options) are byte-identical to the zero-mock capture in the init info and all
  15 buffers. srvinit makes the same 57 bridge calls each time and succeeds. So no init-script
  value, `asInitDevData` entry or control-block field depends on the core revision or the DDK
  version the kernel reports. That includes `DMS_CTRL`, `USE_TMPREG` and `PDS_VCB_PERF_CTRL`.
  The request carries no clock speed (that is `eRequest = 0`, never sent).
  `test_initinfo.py` pins the invariance.
- **Heap bases are mock inputs.** `PDS_EXEC_BASE` (`0xE4000000`) and `asInitDevData[12..13]`
  (`0xE4000000`, `0xF4000000`) are the PDS and pixel-shader heap bases from
  `SGXINFO_FOR_SRVINIT`. The mock reports the DDK's compile-time `sgxconfig.h` table, which is
  also what the real kernel driver reports for this build.
- **The page-directory address is a mock input.** `sPDDevPAddr`, from the same reply, becomes
  TA3DCtl dword 23 (`sa23`, §13.9). A capture with it set to `0x2345000` changes that dword
  and nothing else. On hardware, it is the physical address of the EDM's page directory, which
  the host builds.
- Every other reply is a status word, a handle or the allocator's addresses (covered above).

Other EDM values from the init block:

- **EDM task registers** = `EUR_CR_USE0_SERV_EVENT` / `EUR_CR_USE1_SERV_EVENT`
  (`0xb14`/`0xb20`).
- **Clock gating**: `CLKGATECTL = 0xa6aa`, `CLKGATECTL2 = 0xaaaaaa`; idle is polled on
  `CLKGATESTATUS` (`0x8`) with mask `0xffffffdf`.
- **Cache control** = `0x2`. `asInitDevData[8..13]` = `2, 0, 0x300, 0x940, 0xE4000000,
  0xF4000000` — the PDS and pixel-shader heap bases among them; the rest is still open.

### 13.6 Firmware register map — MMIO from USSE code **[RE]**

The EDM reaches SGX registers with `ldr rD, #idx` / `str #idx, src`, where the immediate is a
**dword register index** (offset = idx × 4). Verified: **all 191 distinct indices in the
ukernel resolve to named SGX545 registers**, none unknown — 183 reads and 421 writes:

| block | regs | reads | writes |
|-------|------|-------|--------|
| clock / core id / reset / perf | 6 | 4 | 4 |
| events, PDS cache, timer | 8 | 32 | 52 |
| VDM / TE / MTE / GSG (tiling front end) | 34 | 13 | 41 |
| ISP / ZLS / 3D (raster back end) | 44 | 38 | 92 |
| DPM (parameter memory manager) | 39 | 55 | 93 |
| TSP / TPU / cache | 16 | 5 | 49 |
| USE / PDS / DMS | 37 | 24 | 72 |
| BIF (MMU banks, req bases) | 6 | 12 | 16 |
| emulation | 1 | 0 | 2 |

The firmware — not the host — programs the TA, 3D, DPM and BIF registers per job. It also
reads `CORE_ID`/`CORE_REVISION` and can write `SOFT_RESET`. For bring-up this means the host
side is small: replay the init script, then feed CCB commands. The per-register list (with
read/write counts) is regenerated by `tools/usse-dis/regtouch.py`, which runs on our
clean-room decoder's output.

### 13.7 Boot path **[CAP/RE]**

The EDM is never entered at instruction 0. Every entry goes through a PDS program, which
loads the secondary attributes and then issues a `doutu` naming a USSE instruction index
(§13.8). The boot chain, with byte offsets into the ukernel:

1. The init script's final writes point `EVENT_OTHER_PDS_EXEC` at **PDS #10** (`0xE800E000`)
   and enable event→PDS dispatch. The host then clears `HostCtl.ui32InitStatus`, sets
   `ui32HostClock` and writes **`EUR_CR_EVENT_KICK2`**. SGX545 has
   `SGX_FEATURE_MULTI_EVENT_KICK`, so the boot kick does not use `EVENT_KICK` and does not bump
   the kicker word. The kick runs PDS #10, which starts **`edm.task.boot`** (`0xd60`).
2. `edm.task.boot` uses `emitpds` to launch **PDS #11**, which DMAs the TA3DCtl block into the
   `sa` registers and enters `edm.task.pds11` (`0xdf0`). It then launches **PDS #12**.
3. **`edm.task.init_done`** (`0xe20`, entered via PDS #12) finishes the boot:
   - writes its two patched words to `EVENT_OTHER_PDS_EXEC` / `_DATA`, installing **PDS #13**
     (data size 6) as the steady-state event program;
   - programs `EVENT_TIMER` from `HostCtl.ui32uKernelTimerClock`, with bit 24 set;
   - writes `HostCtl.ui32InitStatus = 1` (`PVRSRV_USSE_EDM_INIT_COMPLETE`).

   The host's `SGXInitialise` polls exactly that bit, so **`InitStatus` bit 0 is the
   "firmware is up" signal** (`ccb.py`'s `boot_kick()` lists the host side).
4. From then on, every event runs PDS #13, and deferred work runs through PDS #14 (§13.8).

Instruction 0 is a dead stub. It writes `0xAD002600` to HostCtl dword 14, `ui32AssertFail`
(`[sa1,+#14]`), flushes and branches to `0xb9f8`, but no branch, call or PDS code word targets
it. So it is an assertion trap, not the cold entry an earlier revision of this section called
it.

The most-called routine, **`hwperf.log`** (`0xcbc8`, insn `0x1979`, 33 call sites), appends
an HWPerf record: entry = `[sa0,+#43]` (the HWPerf circular buffer) + 12 + write-offset × 100.

### 13.8 PDS entry programs **[CAP]**

The five auxiliary KERNEL_CODE allocations (#10–#14) are PDS programs. Each one is a data
segment followed by 32-bit PDS instructions. They were first disassembled with `refpds545`
(PVR_PSP2's `pdsdisasm`, used as a black-box oracle). They can now also be read with our
clean-room decoder, `tools/usse-dis/pds-dis`, or in radare2 as `-a pds` with the code mapped at
0. Their data banks were decoded with `harness/ukernel-extract/pdsprog.py`:

- The data segment is stored in rows of four dwords: `ds0[2r], ds0[2r+1], ds1[2r], ds1[2r+1]`.
- A USSE code-address word is `0x08000000 | (pc << 4) | 0xf`. `pc` counts instructions from
  the EDM code base, so the template's indices are relocated by the ukernel's offset in that
  base (`0x200` instructions on this build).
- The `emitpds` control word carries the data-segment size, in 4-dword units, at bits [27:23].
- `EVENT_OTHER_PDS_EXEC` holds the program address in bits [31:4]. `_DATA` holds the data
  size. `_INFO` holds attribute size [9:2], `SD` [1] and rotate [0].

| prog | started by | data | USSE entry |
|------|-----------|------|------------|
| #10 | init script (`EVENT_OTHER_PDS_EXEC`) | 1 | `edm.task.boot` `0xd60` |
| #11 | `emitpds` in the boot task | 2 | `edm.task.pds11` `0xdf0`, after DMAing TA3DCtl → `sa` |
| #12 | `emitpds` in the boot task | 1 | `edm.task.init_done` `0xe20` |
| #13 | `EVENT_OTHER_PDS_EXEC` (set by `init_done`) | 6 | event dispatcher, below |
| #14 | `emitpds` in `edm.request` | 3 | `edm.work.dpm_store` / `.dpm_clear` / `.csw_flush` |

**PDS #13** tests the pending event bits and branches to a USSE entry. If only
`DPM_INITEND|DPM_CONTROL_STORE|DPM_STATE_STORE` are pending, it runs `edm.event.dpm_store`
(`0x7050`). If only `DPM_CONTROL_CLEAR` is pending, it runs `edm.event.dpm_clear` (`0x2cd0`).
If only `ISP2_ZLS_CSW_FINISHED|OTPM_INV|TPC_FLUSH` are pending, it runs `edm.event.csw_flush`
(`0x5670`). Everything else goes to `edm.event.general` (`0x30`). Relative to the code base,
these are code words `0xe0a`, `0x59a`, `0xace` and `0x6`.

**PDS #14 runs deferred work.** `edm.request` (`0xbab0`) takes a request mask in `r16` and ORs
it into TA3DCtl dword 31 (`[sa0,+#31]`, the pending-request word). Only if that adds new bits
does it `emitpds` PDS #14, passing the mask as `ir0`. PDS #14 then starts one task by group:

| request bits | task | same work as |
|--------------|------|--------------|
| `0x0004000f` | `edm.work.dpm_store` `0x73a0` | `edm.event.dpm_store` |
| `0x00120080` | `edm.work.dpm_clear` `0x2ea0` | `edm.event.dpm_clear` |
| anything else | `edm.work.csw_flush` `0x5740` | `edm.event.csw_flush` |

Each task first clears the bits it was handed from the pending word, then runs the same code
paths as the matching PDS #13 fast path. `edm.event.general` posts requests when several
events arrive together:

| request bit | event (`EVENT_STATUS`, i.e. `pa1`) |
|-------------|-------------------------------------|
| `0x1` | `DPM_INITEND` |
| `0x2` | `DPM_STATE_STORE` |
| `0x8` | `DPM_CONTROL_STORE` |
| `0x30` | `ISP2_ZLS_CSW_FINISHED` / `OTPM_INV` |
| `0x40` | `TPC_FLUSH` |
| `0x80` | `DPM_CONTROL_CLEAR` |

Other code posts the remaining bits directly: `0x40` from three more places, and `0x80`,
`0x20000`, `0x40000` and `0x80000` from one each. Their sources are not decoded yet. The
tasks run their work as separate USSE tasks rather than inside the event program. The likely
reason is that an event task must stay short **[infer]**.

### 13.9 `sa` binding and the address-patch sites **[CAP/RE]**

The USSE code reads `sa0`–`sa2` as base pointers and never writes them. The values come from
TA3DCtl, which PDS #11 DMAs into the secondary attributes. The captured TA3DCtl gives:

| sa | points at |
|----|-----------|
| `sa0` | TA3DCtl itself, so `[sa0,+#k]` reads TA3DCtl dword k |
| `sa1` | HostCtl |
| `sa2` | kernel CCB control (`+#0` write offset, `+#1` read offset) |
| `sa22` | kernel CCB |
| `sa23` | page-directory physical address (a value, not a pointer) |
| `sa43` / `sa44` / `sa45` | HWPerf circular buffer / TA signature / 3D signature |

The other 51 of the first 59 dwords are 0 at boot; the EDM keeps state in some of them, for
example the HWPerf status in `sa19` and the pending-cleanup bits in `sa10` (§13.10). An earlier
revision of this table read `sa0` as "kernel CCB + 0x4000" and `sa43` as the CCB control. Both
were wrong: the first was a coincidence of the harness allocator's layout.

Once the harness gave each allocation a unique device address, all **18 template patch sites**
(§12) resolved to specific buffers. In the template they are `mov rN, #0xDEADBEEF`
placeholders, which the loader replaces with the following values:

- the `emitpds` control words and `address >> 4` of PDS #11, #12 and #14;
- the TA3DCtl device address (`0xd78`);
- PDS #13's address and data size, written to `EVENT_OTHER_PDS_EXEC/_DATA` (`0xe40`/`0xe50`).

The 18 three-byte sites are the two immediate halves of 9 `mov` instructions. A capture with
every buffer moved (`extracted/layout_moved/`) confirms each value's source. It also shows
that srvinit leaves the kick table alone when the ukernel moves within the heap, while the PDS
code words shift with it. `tools/fwcutter` carries these patches, the PDS data segments that
srvinit generates and the init scripts as a firmware file (`design/sgx545_fw_format.md`).

`harness/ukernel-extract/sgx545_labels.r2` records the patch sites as comments.
`make_r2.sh` combines that file with the `initinfo.py` and `regtouch.py` output to produce the
r2 project script for the ukernel.

### 13.10 Kernel CCB command protocol **[DDK/RE]**

A command is `SGXMKIF_COMMAND` (`sgx_mkif_km.h`), 32 bytes. The kernel CCB holds 256 of them:

| offset | field | |
|--------|-------|-|
| `+0` | `ui32ServiceAddress` | EDM handler: `aui32HostKickAddr[cmd]` (§13.4) |
| `+4` | `ui32CacheControl` | `SGXMKIF_CC_*` invalidations to do first |
| `+8` | `ui32Data[6]` | payload |

**Host side** (`sgxutils.c`, `SGXScheduleCCBCommand`):
1. Write the command at `CCB[WriteOffset]`.
2. Set `WriteOffset = (WriteOffset + 1) & 0xff`.
3. Set the event-kicker word to `(kicker + 1) & 0xff`.
4. Write `EUR_CR_EVENT_KICK` (`0x0ac8`) = 1. None of the SGX545 revisions (1013, 10131,
   1014) has BRN26620, so it uses `KICK`, not `KICK2`.

**EDM side** (`0x4c0`, insn 152 onward), when `pa1` bit 8 (the kick event) is set:
1. `r1 = [sa2,+#1]` (ReadOffset) and `r0 = [sa0,+#22] + r1 × 32` (the command's address).
2. Load dword 0 into `r4`, then dwords 1–7 into `r5`–`r11` with one `ldad.fetch7`.
3. Apply `r5`, the cache control:
   - bit 1 `INVAL_BIF_PD` and bit 0 `INVAL_BIF_PT` each call `0x158a` (`r16` = 1 or 0);
   - bit 3 `INVAL_DATA` sets `CACHE_CTRL` bit 28, polls `EVENT_STATUS` and acknowledges
     through `EVENT_HOST_CLEAR`;
   - bit 2 `INVAL_BIF_SL` is ignored.
4. Store `(ReadOffset + 1) & 0xff` back, then `mov pclink, r4; lapc` jumps to the handler.

So **every handler receives `ui32Data[0..5]` in `r6`–`r11`**. The handlers agree with the
kernel driver's payloads:

| cmd | handler | `ui32Data[]` (kernel) | EDM use |
|-----|---------|-----------------------|---------|
| POWER | `0xd4` | `[1]` = POWEROFF 1 / IDLE 2 / RESUME 3 | `r7` compared with 3, 1, 2 |
| CONTEXTSUSPEND | `0xeb` | `[0]` context, `[1]` SUSPEND 1 / RESUME 2 | `r0 = r6`; `r7` vs 2 |
| TRANSFER | `0x103` | `[1]` HW transfer context addr | `r1 = r7` |
| TA | `0x11d` | built by userspace (`libsrv_um`) | `r1 = r7`, so `[1]` again |
| CLEANUP | `0x13a` | `[0]` type RT 1…PB 5, `[1]` HW data | `sa10` bit type+1; `r7` → dw32 |
| PROCESS_QUEUES | `0x156` | none | `ba 0x161`, into the scheduler |
| SETHWPERFSTATUS | `0x157` | `[0]` new HWPerf status | bit 0 resets the buffer; rest → `sa19` |
| GETMISCINFO | `0x173` | `[1]` misc-info buffer addr | `r2 = r7` |

`DATABREAKPOINT`, `FLUSHPDCACHE` and `2D` have no handler in this build (kick address 0).
`edm.event.general` tests `sa19` bits 3 and 1 early on. Those are `MK_EXECUTION_ON` and
`GRAPHICS_ON`, so it checks whether to log to HWPerf.

**HostCtl** (`SGXMKIF_HOST_CTL`, `sa1`) has 17 dwords in this build. `SUPPORT_HW_RECOVERY`
(build-option bit 3) is on, and there are no EDM breakpoints. Dword 0 is `ui32InitStatus`,
1–2 the power and cleanup status, 3–5 the HW-recovery fields, 6 `ui32uKernelTimerClock`, 7 the
active-power sample rate, 8–9 the interrupt flags, 10–12 the breakpoint signal, power-event
count and time wraps, 13 `ui32HostClock`, 14 `ui32AssertFail`, 15 `ui32PerfGroup` and 16 the
OpenCL delay count. Size `0x44` matches the struct size srvinit reports.

`harness/ukernel-extract/ccb.py` builds and decodes commands and lists the host's submit
writes in order. It is the starting point for submitting commands on hardware.

### 13.10a First boot on hardware (2026-10-20) **[HW]**

The laptop booted the closed ukernel from our gma500 port (`kernel/patches/`, kernel 7.3), with
the sequence of `design/sgx545_boot_sequence.md` and the buffers built from `sgx545_ukernel.fw`
(all 15 checked byte for byte against `fwload.py` first):

- `HostCtl.ui32InitStatus` went to 1 **140 µs** after the `EVENT_KICK2` write. No BIF fault,
  no `AssertFail`, `CLKGATESTATUS = 0x20`.
- The EDM then ticks: HostCtl dword 12 and `USE0_SERV_EVENT` advance at **1001.6 Hz**. With
  `uKernelTimerClock = 400000` that puts the SGX clock at about **400 MHz**, the DDK's
  `SYS_SGX_CLOCK_SPEED`.
- The EDM translates through **BIF directory list 15** (`BANK0 = 0xf`): the first attempt
  faulted with `BIF_FAULT` CID 15.
- **The ukernel must start at the EDM code base.** With the ukernel at code base + `0x1000`
  (the harness layout) the EDM faulted on page `0xE8000000`: its branch targets and the CCB
  service addresses are absolute instruction indices. At the code base it booted.
- **The kernel CCB works as decoded in §13.10.** GETMISCINFO (first command with cache control
  `0x2`) is answered in about 140 µs. The EDM returns core revision `0x1000d`, core ID
  `0x01150000`, DDK `0x10711` build 862890, `CoreIdSW` 545, `CoreRevSW` `0x1000d`, build options
  `0x60218`, and the 13 MKIF struct sizes of §13.3, all as expected. Repeated commands and a
  stop/boot cycle (deinit script plus `SOFT_RESET = 0x3f`) work.
- **The transfer queue renders.** Dummy transfers, sync waits and a 64 × 32 solid fill run
  as `design/sgx545_transfer.md` §4 specifies; the fill is the first rendering by the SGX545
  under our driver.
- **Power commands work.** `SGXMKIF_CMD_POWER` with IDLE (2) sets `PowerStatus` bit 2 and with
  POWEROFF (1) bit 3, each in about 150 µs, after which the clocks gate. A command sent after
  IDLE wakes the EDM; after POWEROFF and the deinit script the firmware boots again.

### 13.11 Open questions

- **The TA and TRANSFER command structures** behind `ui32Data[1]`. Userspace builds them, so
  they need the closed `libsrv_um` or a capture from a client.
  TRANSFER is now documented in `sgx545_transfer.md` (layouts, EDM handler, first-test recipe).
- **The sources of the directly posted request bits** `0x20000`, `0x40000` and `0x80000`
  (§13.8), and what the shared write to register 77 means in each work path.
- **The rest of `asInitDevData`.** Entries outside [8..13] are still unexplained.
- **Host boot sequence:** see `sgx545_boot_sequence.md` (it also corrects §7.2 steps 3–4).
