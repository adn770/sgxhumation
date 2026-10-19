# SGX545 Hardware / ABI Spec (reconstructed) — v0.3

> Reconstructed from the GPL DDK **1.7.862890**, cross-verified across two independent
> copies, with architecture context from `sgx_tbdr_primer.md`. This is a *living*
> reconstruction, not a vendor document. Every non-obvious claim cites its source file.

**Scope:** core identity, feature set, MMIO register map shape, MMU/BIF memory model, the
reset/init skeleton (§1–5), the **command-submit (kick) ABI** (§6), and — new in v0.3 — the
**microkernel (EDM) boot & init handshake** (§7). Deferred to later revisions: full
per-register bitfield tables, the ioctl bridge surface, and the USSE/PDS instruction
encodings.

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

## 8. Open questions (drive the hardware inventory)

1. **Exact `EUR_CR_CORE_REVISION`** of our Cedarview silicon → which `FIX_HW_BRN_*` errata
   (and thus which feature *undefs* in §2) apply.
2. **SGX register BAR** base/size on the Cedarview PCI device (and whether `gma500` or
   nothing currently claims it).
3. **Stolen/GTT memory** size and where the microkernel/parameter-buffer live.
4. Does the CE5300 system glue (`services4/system/ce5300/`, present only in `sgx545-ce`)
   match Cedarview's integration, or do we need a new system layer?
5. **(critical path, from §7.1)** Can we reuse the archived `cedarview-graphics-drivers`
   `srvinit` + microkernel blob to produce the init scripts / EDM task regs / ukernel image —
   and does that blob's target revision match our silicon? If not, the scripts must be
   reconstructed from hardware capture.

## 9. Next extraction targets (spec v0.4)

- Per-register bitfield tables for `SOFT_RESET`, `BIF_CTRL`, `CLKGATECTL`, `EVENT_STATUS`,
  `EVENT_KICK*`.
- The **ioctl bridge** surface: `services4/srvkm/bridged/sgx/bridged_sgx_bridge.c` (which
  `SGX_CCB_KICK` and friends cross), to map the full userspace→kernel entry points.
- **Dissect the archived `cedarview-graphics-drivers` blob** (§8 Q5): locate `srvinit`, the
  ukernel image, and confirm whether its init scripts/EDM task regs are reusable for our
  silicon — the critical-path question from §7.1.
- Cross-check §6/§7 (kick, CCB, init handshake) against SGX535-reMESA captured streams
  (`reference/SGX535-reMESA/docs/hardware-evidence`, `docs/phase8`), reconciling 535↔545.
- Diff `sgx545defs.h` across DDK 1.7 → 1.17 (`reference/linux_openpvrsgx`) to separate
  core-invariant registers from version churn.
