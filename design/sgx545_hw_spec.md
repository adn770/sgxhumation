# SGX545 Hardware / ABI Spec (reconstructed) — v0.1

> Reconstructed from the GPL DDK **1.7.862890**, cross-verified across two independent
> copies, with architecture context from `sgx_tbdr_primer.md`. This is a *living*
> reconstruction, not a vendor document. Every non-obvious claim cites its source file.

**Scope of v0.1:** core identity, feature set, MMIO register map shape, MMU/BIF memory
model, and the reset/init skeleton. Deferred to later revisions: full per-register bitfield
tables, the command-submit (kick) data structures, the microkernel (EDM) interface, and the
USSE/PDS instruction encodings.

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

## 6. Open questions (drive the hardware inventory)

1. **Exact `EUR_CR_CORE_REVISION`** of our Cedarview silicon → which `FIX_HW_BRN_*` errata
   (and thus which feature *undefs* in §2) apply.
2. **SGX register BAR** base/size on the Cedarview PCI device (and whether `gma500` or
   nothing currently claims it).
3. **Stolen/GTT memory** size and where the microkernel/parameter-buffer live.
4. Does the CE5300 system glue (`services4/system/ce5300/`, present only in `sgx545-ce`)
   match Cedarview's integration, or do we need a new system layer?

## 7. Next extraction targets (spec v0.2)

- Per-register bitfield tables for `SOFT_RESET`, `BIF_CTRL`, `CLKGATECTL`, `EVENT_*`.
- The **kick / command-submit** structures: `services4/srvkm/devices/sgx/sgxkick.c` +
  `bridged/sgx/bridged_sgx_bridge.c` (the host→GPU command ABI).
- Microkernel (EDM) load & handshake: `sgxinit.c` firmware path + `sgx/` ukernel headers.
- Cross-check the kick/command shapes against SGX535-reMESA captured streams
  (`reference/SGX535-reMESA/docs/hardware-evidence`, `docs/phase8`), reconciling 535↔545.
- Diff `sgx545defs.h` across DDK 1.7 → 1.17 (`reference/linux_openpvrsgx`) to separate
  core-invariant registers from version churn.
