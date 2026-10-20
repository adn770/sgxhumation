# SGX545 render node — kernel/userspace interface (draft 1)

This is the design for the long-term userspace interface to the SGX545 in Cedarview
(GMA3600/3650): a minimal DRM render node with modern primitives (GEM objects, per-file GPU
VMs managed with `drm_gpuvm`, `drm_sched` job queues, `drm_syncobj` / `dma_fence`
synchronisation), shaped like the upstream PowerVR Rogue driver (`drivers/gpu/drm/imagination`,
`include/uapi/drm/pvr_drm.h`) but scaled down to what the SGX firmware actually offers. A future
Mesa driver is the intended real consumer; a small test library comes first.

It replaces nothing yet. The current implementation (`kernel/gma500/sgx_boot.c`) is
debugfs-driven: it loads `sgx545_ukernel.fw`, boots the EDM, runs kernel-CCB commands and
transfer-queue tests, all from kernel code. This document says what of that becomes kernel
policy, what moves behind the UAPI, and what userspace must supply.

Conventions:

- **Sources.** `spec` = `sgx545_hw_spec.md`, `boot` = `sgx545_boot_sequence.md`, `xfer` =
  `sgx545_transfer.md`, `fwfmt` = `sgx545_fw_format.md`. `pvr` = mainline v7.3
  `drivers/gpu/drm/imagination/` and `include/uapi/drm/pvr_drm.h` (GPL-2.0/MIT; summarised,
  not copied). `DDK` = the GPL DDK 1.7.862890 (`reference/sgx545-ce`), read only to learn
  what SGX userspace historically needed.
- **[HW]** marks findings measured on the laptop, **[infer]** inferences, **[decide]** points
  that need the user's decision (collected in §9).
- Device virtual addresses (GPU VAs) are written as in the other documents: the harness
  layout the current code uses (`0xE8000000` KERNEL_CODE, `0xF0000000` KERNEL_DATA, …). The
  final layout is an open choice (§3.5); the interface does not bake it in.

## 1. Goals, non-goals and upstream requirements

### 1.1 Goals

1. A **render node** (`/dev/dri/renderD*`) that lets an unprivileged process allocate GPU
   memory, build its own GPU address space, and submit transfer (blit/fill/copy) jobs now and
   TA/3D render jobs later, with explicit synchronisation through syncobjs.
2. **Kernel owns the firmware.** Only the kernel touches the kernel CCB, HostCtl, TA3DCtl,
   the HW contexts, the client CCBs and the page directories. Userspace never sees a
   firmware address or a physical address.
3. **Isolation where the hardware allows it**: one page directory per VM, the minimum of
   firmware memory aliased into it, read-only where possible, and a clear statement (§6) of
   what remains unprotected until the hardware probes are done.
4. **Shaped like `pvr_drm.h`**, so the Mesa side can borrow the Rogue winsys structure
   (`src/imagination/vulkan/winsys/pvrsrvkm` vs `.../powervr`): DEV_QUERY with heaps,
   CREATE_BO, VM context + VM_MAP/VM_UNMAP, CREATE_CONTEXT, SUBMIT_JOBS with sync ops.
5. **Incremental**: every phase (§8) is useful on its own and testable on the laptop.

### 1.2 Non-goals

- Running the closed DDK userspace (`libsrv_um`, `libGLESv2`). Its bridge (`spec` §10,
  `PVRSRV_BRIDGE_SGX_*`) hands userspace device-virtual pointers into shared kernel heaps,
  user-built HW contexts and raw sync-object counters; none of that survives a security
  review. The UAPI here is not a compatibility layer.
- Exposing firmware structures verbatim. `SGXMKIF_TRANSFERCMD` is built by the kernel from a
  UAPI register block (§4.4); the MKIF layout stays internal and may change with the firmware
  build.
- 2D core support (SGX545 has none, `spec` §13.1), multi-core, 36-bit addressing, Vulkan
  sparse binding, user-mode queues.
- Context priorities other than "normal" (the EDM only scans priority 0, `xfer` §3.5).

### 1.3 Upstream requirements and how we meet them

From `Documentation/gpu/drm-uapi.rst` ("Open-Source Userspace Requirements" and "Testing and
validation"):

- **Open-source userspace, reviewed and ready to merge in its canonical project** (Mesa):
  met late, by a Mesa driver (§1.4). Until then the UAPI stays out of tree.
- **The userspace must be the real thing, not a test tool**: the test library is for
  development only and does not satisfy this.
- **Kernel UAPI merged to drm-next / drm-misc-next before the userspace lands**: the normal
  flow once Mesa is reviewed.
- **Documented UAPI**: kernel-doc on every struct and ioctl in the header, plus
  `Documentation/gpu/sgxfive/` (like `Documentation/gpu/imagination/`).
- **IGT tests where reasonable**: driver-specific tests in `igt-gpu-tools/tests/sgxfive/`,
  modelled on `tests/panfrost` and `tests/v3d`.
- **MIT-licensed UAPI header**: `include/uapi/drm/sgxfive_drm.h`, `SPDX-License-Identifier:
  (GPL-2.0-only WITH Linux-syscall-note) OR MIT` as `pvr_drm.h` (kept in this repo as
  `kernel/uapi/sgxfive_drm.h`).

The **firmware** is not covered by those rules but has its own problem: `sgx545_ukernel.fw`
is cut by the user from their own copy of the closed blob (`fwfmt`, the b43-fwcutter model),
so it cannot go into linux-firmware. b43 is the precedent that an upstream driver may depend on
user-extracted firmware. The driver must therefore work, minus the render node, when the file
is absent.

### 1.4 Consumers, in order

1. **`libsgxfive` + `sgxfive-test`** (this repo, `userspace/libsgxfive/`): a thin C wrapper
   over the ioctls and a test tool reproducing the debugfs tests (dummy, fill, copy,
   isolation) from userspace, unit-tested today against an in-process mock of the ioctl
   layer. It takes GPU programs from a pluggable *program source*. The default,
   `sgxfive_prog_asm_create()`, is our own transfer programs (`tools/usse-asm/progs/`,
   assembled into the library at generation time, with the `TQ_META`/`TQ_PATCHES` facts of
   `fwfmt` §9–§11): it equals the closed templates bit for bit and needs no file at all. A
   source reading the user's `.fw` would be a thin adapter but is no longer needed.
2. **IGT tests** using the same ioctls directly (no programs needed for most: queries, BOs,
   VM map/unmap, dummy transfers, syncobj behaviour, error paths). Fill/copy IGT tests need
   programs: they can use hand-assembled clean-room USSE/PDS words (see item 3).
3. **Mesa**: a Gallium driver for GLES2 (later GL 2.1/GLES 3) under `src/imagination/sgx/` or
   its own directory **[decide]**. The hard prerequisite is an **open USSE and PDS
   assembler/compiler**. Every program a transfer needs (fill shader, copy shader, USE
   secondary, EOT, EOR, pixel-event PDS, PDS primaries) is now our own source, assembled by
   our clean-room `usse-asm`/`pds-asm` (`tools/usse-asm`), so 2D needs no `libsrv_um` bytes;
   the shader compiler is what remains, and it gates upstreaming far more than the kernel
   does.

## 2. Device structure

### 2.1 Where the render node lives

Two options; both use the same internal split, so the choice can be made late, but it must
be made **before** the UAPI is merged, because the driver name in `DRM_IOCTL_VERSION` is what
Mesa's loader matches. **[decide]**

**A. Render node on gma500's `drm_device`** (what the current code implies). gma500 adds
`DRIVER_RENDER` and `DRIVER_GEM` render ioctls flagged `DRM_RENDER_ALLOW`; card0 and
renderD128 belong to one device, like i915 or nouveau.

- \+ Shared BAR, IRQ line (bit 18 of `PSB_INT_IDENTITY_R`, `boot` (c)), MMU driver
  (`psb_mmu_driver`), BIF registers and the display power island are naturally one owner.
- \+ Scanout of a rendered BO needs no PRIME import (same GEM object type; §3.6).
- − gma500 is an old, mostly-maintenance driver; reviewers may resist a large render stack
  inside it. The driver name stays `gma500`, which also covers Poulsbo/Oaktrail where there is
  no SGX545 (DEV_QUERY must then say "no render support", or the render node is not created).
- − Mesa would need a `gma500` entry; nothing ties that name to "SGX".

**B. A separate driver on the auxiliary bus.** gma500 creates an `auxiliary_device`
(`gma500.sgx`) on Cedarview and keeps the PCI function, KMS, the IRQ top half and the power
island. The SGX driver (`drivers/gpu/drm/sgxfive/`, its own render-only `drm_device`) binds
to it and gets a small ops table: register window, IRQ demux callback, the BIF/MMU interface
(§3.4) and power-island requests.

- \+ Mirrors Rogue on SoCs: render-only GPU device + separate KMS device, which Mesa's
  `kmsro`/`renderonly` model already handles; driver name `sgxfive` says what it is.
- \+ Keeps the bulk of the code out of gma500; could later serve other SGX5xx parts.
- − Needs an internal gma500 ↔ sgxfive interface (MMU, IRQ, power), PRIME import support in
  gma500 (it has none today), and care with module load order and unbind.

**Recommendation:** keep developing inside gma500 (option A mechanics), but structure the
code as if it were B: `sgx_dev.c` (firmware, kernel CCB, power, HWR) with no DRM file
knowledge, `sgx_vm.c`, `sgx_ctx.c`, `sgx_job.c`, `sgx_drv.c` (ioctls), and one narrow
`sgx_host_ops` interface to gma500 (registers, IRQ, MMU, power). Switching to B is then a
matter of moving files and registering an auxiliary driver. Choose B for the upstream
submission unless reviewers prefer A.

### 2.2 Firmware loading

- `request_firmware("sgx545_ukernel.fw")` (`MODULE_FIRMWARE` declared) **at probe**, with
  `firmware_request_nowarn` so a missing file logs one info line. Missing or invalid file:
  KMS works, no render node is registered (option B: the aux driver fails to bind with
  `-ENOENT`; option A: `DRIVER_RENDER` is cleared before `drm_dev_register`).
- The parsed file (`sgx_fw.c`, KUnit-tested) stays resident: the boot scripts and patch
  tables are needed again at every power-on and HWR. The kernel-code buffers (ukernel, PDS
  #10–#14) and control blocks are built once and kept in RAM.
- The TQ templates of format v2 (`fwfmt` §9) are **not used by the kernel** in this design:
  programs are userspace's job (§4.4). The kernel keeps parsing them only for the debugfs
  tests. A later firmware format can drop them from what the kernel requires.
- DEV_QUERY reports the DDK version/build, build options and the format version (§5) so
  userspace can refuse an unknown firmware build (the MKIF struct sizes are checked by the
  kernel at boot through GETMISCINFO, as `gma_sgx_miscinfo` does today).

### 2.3 Boot, power and runtime PM

Firmware states, kept by `sgx_dev`: `OFF` (blocks in reset), `ON` (EDM booted, accepting
commands), `IDLE` (EDM accepted `POWER/IDLE`, clocks gated). Proven on hardware: boot in
140 µs, IDLE and POWEROFF each in about 150 µs, wake from IDLE by any command, reboot after
POWEROFF + deinit (`spec` §13.10a).

- **Boot lazily**: the firmware boots on the first job submission (or first CREATE_CONTEXT),
  not at probe. Until then the SGX is untouched, which keeps KMS-only users unaffected.
- **Idle**: after no job has been pending for an autosuspend delay (100 ms default), send
  `POWER/IDLE`. The next job wakes it implicitly. This is the only "runtime PM" we need: the
  display power island stays on because gma500 holds its runtime-PM reference forever
  (`boot` (e)), and Intel's driver never power-gates the SGX island either. If gma500 ever
  gets real runtime PM, the SGX holds a reference while `ON`.
- **System suspend** (`gma_power_suspend`): stop accepting new jobs, drain or time out the
  schedulers, `POWER/POWEROFF`, deinit script, `SOFT_RESET = 0x3f`, state `OFF`. Required:
  `boot` (e) item 5 — gma500 powers the island down and the EDM state is lost.
- **Resume**: nothing eager. The next job reboots the firmware. Because suspend waited for
  the queues to drain, every HW context is reset to its initial image (count 0, list links 0,
  `NEWCONTEXT` set) and every client CCB to empty before the reboot, so the EDM's queue
  state cannot dangle.
- **Unload / unbind**: as suspend, then free all firmware buffers and PDs.
- The gma500 hazards of `boot` (e) stay required: no IRQ reinstall while live, the
  `SW_EVENT`-only acknowledge, and BIF flushes routed through CCB cache-control (§3.4).

### 2.4 Recovery (HWR)

Triggers:

1. **BIF fault** (`BIF_INT_STAT.FAULT_REQ`, today read only in `gma_sgx_irq`). A wedged
   render raises no `SW_EVENT`, and the init script enables no other host event, so a
   fault produces **no interrupt**: it must be found by polling `BIF_INT_STAT` from the
   50 ms watchdog (trigger 4) while jobs are in flight, or by enabling a BIF-fault event
   in `EVENT_HOST_ENABLE`/`_ENABLE2` if SGX545 has one (§7 Q2a). `BIF_FAULT.CID` is the
   directory-list index: 15 is the EDM itself, any other index maps to the PD the EDM put
   there, hence to a VM, hence to the context(s) with a job in flight on it. **[HW]** Both
   kinds of fault seen so far wedge the EDM until the firmware is reloaded: the EDM faulting
   on its own code through a client PD (`xfer_iso` without aliases), and a client render
   writing to a read-only alias (`xfer_ro`: the render never completes, no status write, no
   `SW_EVENT`). So a fault always means reset, and since only one transfer runs at a time,
   the faulting job is known: the one in flight on that CID.
2. **Job timeout** from `drm_sched` with no firmware progress (§4.6).
3. **Firmware-reported lockup**: `HostCtl.ui32InterruptFlags` bit 0 (HWR) with `SW_EVENT`
   (`boot` (c), MISR step 1).
4. **Host watchdog**: `USE0_SERV_EVENT ^ USE1_SERV_EVENT` unchanged for 3 × 50 ms while
   `ON` (`SGXOSTimer`); with a 1 kHz EDM tick this is a cheap liveness test.

Procedure (modelled on `HWRecoveryResetSGX` and `pvr_queue`'s timeout path):
`drm_sched_stop` on every scheduler → dump registers and HostCtl (devcoredump later) → stop
the firmware (POWEROFF is pointless on a wedged EDM: deinit + `SOFT_RESET = 0x3f`) → reset
every HW context and client CCB to empty → mark the **guilty** context (from the fault CID
or the timed-out job) as lost → reboot the firmware → `drm_sched_start`; pending jobs of
innocent contexts are re-run from scratch (their CCB words are rebuilt from the kernel's
saved copy of each job's command), the guilty context's jobs complete with `-EIO` /
`-ETIMEDOUT` and further submissions to it fail with `-ECANCELED` (like `pvr`). Reboot is
140 µs, so a reset is cheap; the expensive part is losing in-flight work.

A wedged EDM after a client-caused fault is a **denial of service** any client can trigger
(§6), **[HW]** confirmed with a fill into a read-only alias. Without HWR the GPU stays dead
until a manual reload, so recovery is not optional: it is part of the first phase that runs
untrusted jobs (P5, §8). Detection must not depend on the job completing — the wedged render
raises no `SW_EVENT` — so the watchdog polls for BIF faults (tens of milliseconds) and the
`drm_sched` timeout is the backstop. Recovery makes the DoS survivable (a 140 µs reboot
plus re-running innocent jobs); banning the guilty context, and after repeated offences the
whole file (a per-file fault budget, e.g. 3 resets per minute), bounds repeat offenders.

**As implemented (P5b).** Two detectors find a hang. A 50 ms watchdog, armed by each kick while
jobs are in flight, polls `BIF_INT_STAT.FAULT_REQ` and recovers when a fault is set and the
oldest unfinished job is still the same one at the next poll, since a fault alone is not always
a hang (§3.3 status); a faulting job's fence fails about 105 ms after submission (**[HW]**,
`sgxfive-test isolation` check `fault-recovered-quickly`). The `drm_sched` timeout is the
backstop for hangs without a fault: a timeout is a hang when the oldest job given to the
firmware, counted device-wide at `run_job`, is still unfinished and is the same as at the
previous timeout, so it recovers within two timeouts (about 1.1 s). Completions do not count as
progress, because a context's dummy jobs still complete while the 3D pipe is stalled. A detector
passes the job it suspects to the recovery, which does nothing if that job is no longer the
oldest, so the detector that loses a race blames no one. The guilty context is the owner of that
oldest job: it is marked lost and its unfinished jobs fail with `-EIO`. Each job's hardware
fence keeps a copy of its firmware command, so after the reset every other context's client CCB
is rewritten from offset 0 with its unfinished commands and the fence word set to the job before
them, and the context is kicked once the firmware is up. The handler does this itself, under the
device lock that `run_job` also takes for the CCB write and kick, and returns `NO_HANG`. The
scheduler never stops, no job is resubmitted through it (`drm_sched_resubmit_jobs` is
deprecated) and the timed-out job goes back on its pending list, to be freed when its fence
signals. Hardware fences are allocated with their jobs at submission, so `run_job` allocates
nothing. Lockdep (`PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`) stays quiet through the isolation tests
and their recoveries. The fault budget is per open file: a refcounted client object, shared with
the file's contexts because they can outlive it through their jobs, keeps the times of its last
three faults, and three within a minute ban the file. CREATE_CONTEXT and SUBMIT_JOBS on it then
fail with `-ECANCELED`, while jobs it already queued on contexts that are not lost still run
(**[HW]**, check `faulting-file-banned`). Still to come: an error state such as devcoredump.

## 3. Memory

### 3.1 GEM buffer objects

Use the **shmem GEM helpers** (`drm_gem_shmem_helper`), as `pvr` does, with these
constraints:

- **Pages below 4 GiB** (no 36-bit MMU, `boot` (e) item 8): on x86-64 kernels the shmem
  mapping gets `GFP_DMA32`. On i686 there is no DMA32 zone; with PAE and RAM above 4 GiB
  (possible on 4 GiB Cedarview boards, after the PCI hole) the driver must exclude highmem
  above 4 GiB, simplest by checking `page_to_phys` at pin time and failing the map with
  `-ENOMEM`. Our laptop (2 GiB) never hits it.
- **CPU mapping write-combined by default.** This is what works today: firmware buffers and
  the fill destination are `set_pages_array_wc` + WC `vmap`, with `wmb()` before the kick and
  `rmb()` after the `SW_EVENT` (**[HW]**, all fills and copies so far). User mmaps are
  `pgprot_writecombine` (`map_wc = true`); the linear-map attribute change follows
  `psb_gem_pin`'s `set_pages_array_wc` precedent, done once when pages are first pinned.
- **Cached CPU mappings** (`DRM_SGXFIVE_BO_CPU_CACHED`) only once the coherency question is
  answered: whether PTE bit 3 (`CACHECONSISTENT`) makes SGX accesses snoop on Cedarview
  (§7, Q5). Until then the flag is rejected with `-EINVAL`, so the bit is reserved.
- **No CPU access** unless asked: like `pvr`'s `DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS`, a
  BO is mmap-able only with `DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS`. This keeps the door open for
  BOs whose pages never need a WC linear-map change.
- GPU access attributes are **per mapping**, not per BO (§3.3): read-only is a VM_MAP flag.
- `GET_BO_MMAP_OFFSET` returns the fake offset for `mmap()` on the render node (the
  `pvr` ioctl; `DRM_IOCTL_MODE_MAP_DUMB` is KMS-only).

### 3.2 PRIME (dma-buf) and KMS

- **Export**: `drm_gem_prime_export` with the shmem sg-table; the KMS side imports it to
  scan out. With option A the same object type can be used by gma500's framebuffer code once
  `psb_framebuffer_create` accepts shmem objects (or render BOs are `psb_gem_object`s). With
  option B gma500 needs `gem_prime_import_sg_table` support, which it lacks today.
- **Import**: sg-table imports of system-memory dma-bufs are mapped like own BOs, page by
  page, after checking every page is below 4 GiB. gma500 buffers in **stolen memory** (the
  fbdev console) have no `struct page`; mapping them needs `psb_mmu_insert_pfn_sequence` and
  a pfn-range import path. Not needed for the first phases.
- **Implicit sync for scanout**: `drm_sched` finished fences are added to the BO's
  `dma_resv` for exported BOs only (shared VM resv otherwise, §3.3). gma500's page flip is
  legacy (non-atomic) and does not wait for fences; until it does
  (`drm_gem_plane_helper_prepare_fb`-style wait in `gma_crtc_page_flip`), userspace must
  wait on its out-syncobj before flipping. Mesa does this for render-only devices anyway.

### 3.3 Per-file GPU VMs with `drm_gpuvm`

**As implemented (P4, 2026-10-20):** the UAPI's exact-mapping rules (a map may not overlap, an
unmap removes exactly one whole mapping) need no splitting or merging, so the first
implementation tracks mappings with a `drm_mm` of reserved nodes per VM instead of
`drm_gpuvm`; each VM owns one `sgx_mmu` page directory, and a mapping pins its BO's shmem
pages. Moving to `drm_gpuvm` stays an option when sparse or partial remaps are needed.

As in `pvr`: a file creates **VM contexts** (`CREATE_VM_CONTEXT`); each is a `drm_gpuvm`
plus an SGX page directory. Contexts are created against one VM. Several VMs per file are
allowed (Mesa may want one per GL context) but one is the common case.

- `drm_gpuvm_init(..., start = 0, range = 1ULL << 32, reserve_offset, reserve_range, ...)`:
  the **reserved node is the kernel alias region** (§3.5), so `drm_gpuvm` itself rejects user
  mappings there. One shared `dma_resv` per VM (the `pvr` "dummy GEM" pattern), so a job
  attaches one fence instead of one per mapped BO.
- **VM_MAP / VM_UNMAP are synchronous** (`pvr` semantics, not an asynchronous VM_BIND
  queue): the map is in the page tables when the ioctl returns. Mappings must be 4 KiB
  aligned, inside one heap reported by DEV_QUERY, and must not overlap an existing mapping
  (`-EEXIST`); partial BO mappings are allowed (`offset`, `size`). Unmap removes a whole
  mapping by address and size, as in `pvr`. An asynchronous, syncobj-driven VM_BIND can be
  added later as a new ioctl if Mesa needs it; nothing here precludes it.
- **TLB invalidation** has no host-side register path that is safe while the EDM runs
  (`boot` (e) item 6: the DDK writes `BIF_CTRL` only during reset). Instead every map/unmap
  ORs `SGXMKIF_CC_INVAL_BIF_PT | _PD` (and `INVAL_DATA`) into the device's pending
  cache-control word, which the next kernel-CCB command carries (`spec` §13.10 step 3), as
  the DDK does. Consequence for **unmap**: the GPU may still translate through the stale TLB
  until that command has been *processed*. So pages of an unmapped range are **not released**
  (the `drm_gpuvm_bo` reference is kept) until the EDM's kernel-CCB read offset has passed a
  command carrying the invalidation; if no job follows, the unmap path sends a
  `PROCESS_QUEUES` with the cache-control bits to force it. The same rule frees page tables
  and, on VM destruction, the page directory itself (the EDM caches PD → directory-list
  assignments in `BIF_DIR_LIST_BASE0..14`, `xfer` §3.1 step 6; a freed and reused PD page
  must not match a stale entry, so `INVAL_BIF_PD` must have been processed first).
- **Status (2026-10-21): implemented.** Unmapped pages (still pinned), page tables and page
  directories go on a deferred list tagged with the next kernel-CCB command number, and that
  command carries `INVAL_BIF_PT | _PD`; a 10 ms reaper frees each item once the EDM's read
  offset has passed its command, and sends `PROCESS_QUEUES` itself when no other command
  follows. A halted firmware (BIF in reset) frees everything at once. The VA range is
  reusable immediately, since a stale translation still lands on the pinned pages. Before
  this, destroying a VM freed its PD at once and the next process's VM got the page back:
  the EDM ran its first jobs on stale BIF state and the tiling-engine and cache requestors
  faulted at page 0 (`BIF_INT_STAT` `0xd0400`, `0xd0100`), stalling the job (**[HW]**, IGT
  `sgxfive_submit`, repeated `sgxfive-test dummy`). Now: no such fault in IGT rounds and
  dummy chains, and the list drains to empty once idle (`dri/0/sgx/status`).
- **Unmapping under a running job** is the user's problem, as in `pvr`: their job faults,
  which triggers HWR (§2.4) and bans the context. The kernel guarantees only that no page is
  reused while the GPU can reach it.

### 3.4 Page-table management on `psb_mmu`

The SGX MMU is two-level, 4 KiB pages, 10/10/12 split; PDE = `PT_phys | 1`, PTE =
`page_phys | flags` with `1` valid, `2` write-only, `4` read-only, `8` cache-consistent,
`0x10` EDM-protect (`spec` §4); gma500's `psb_mmu` uses the same format and already supports
the RO/WO type bits (`psb_mmu_mask_pte`). Proposal:

- **One `psb_mmu_pd` per VM**, allocated with `psb_mmu_alloc_pd(mmu, 1, 0)` (trap faults,
  no dummy page, as `gma_sgx_client_pd` does) so stray accesses fault visibly instead of
  hitting a dummy page.
- **The kernel never programs `BIF_DIR_LIST_BASE*` for clients.** The PD's physical address
  goes into the HW context (`common.sPDDevPAddr`, offset `0x8`, `xfer` §1.2), and the EDM
  assigns a directory list (round-robin over 0–14, never 15) and sets `BIF_BANK0.INDEX_3D`
  per transfer (`xfer` §3.1 step 6, **[HW]** `xfer_iso`). 16 lists bound the number of
  *resident* VMs, not the number of VMs.
- **Shared page tables for the global alias.** The global part of the kernel alias (§3.5) is
  mapped once in the EDM's own PD with page tables preallocated for the whole range; each VM
  PD gets **copies of those PDEs**, pointing at the same PT pages (the DDK's
  `MMU_InsertHeap` for `SHARED_EXPORTED` heaps). Adding a firmware buffer later then needs no
  per-VM update. `psb_mmu` needs a small extension for this (a "borrowed PT" that
  `psb_mmu_free_pagedir` does not free) — today `gma_sgx_client_pd` duplicates the PTEs
  instead.
- **Per-VM private alias**: the client CCBs and HW contexts of *this VM's* contexts (§3.5)
  live in a separate range with per-VM page tables, so one process cannot read another's
  command stream.
- New `psb_mmu` entry points (internal, for `sgx_vm.c`): insert with explicit PTE flags
  (RO, EDM-protect, cache-consistent), the borrowed-PT PDE copy, and pfn-sequence insert for
  stolen-memory imports. Locking stays `psb_mmu_driver.sem` + `.lock`; `drm_gpuvm` ops run
  under the VM's mutex as in `pvr_vm.c`.
- **gma500's own `psb_mmu_flush`** (a read-modify-write of `BIF_CTRL` on GEM pin/unpin) must
  go through the same pending cache-control path while the firmware is `ON` (`boot` (e)
  item 6); with a separate SGX driver this is part of the MMU interface.

### 3.5 GPU VA layout (heaps)

The layout is **reported, not fixed in the UAPI** (DEV_QUERY `HEAP_INFO`, `pvr` style),
because two layouts are live candidates: the harness layout the current code uses and the
DDK's `REV1` layout for host bridges below rev 4, which our laptop has (`boot` (d)). Some heap
bases are baked into **global** registers by the init script and so are the same in every VM:
`USE_CODE_BASE_0..9` (pixel USSE code, ten 512 KiB windows), `USE_CODE_BASE_10..13` (vertex
code), `USE_CODE_BASE_15` (the EDM's code), `PDS_EXEC_BASE` (overwritten per transfer by the
command, `xfer` §3.3, but its window rules still apply).

Heaps a VM reports (harness layout values shown; `REV1` in brackets):

| id | heap | base [REV1] | size | notes |
|---|---|---|---|---|
| 0 | `GENERAL` | `0x1000` [`0x3e000000`] | to next heap | surfaces, BG objects, streams |
| 1 | `TEXTURE` | — [`0xf0000000`] | [`0x0ffff000`] | sample-cache quirk only |
| 2 | `PDS_PIXEL` | `0xe4000000` [`0x2e000000`] | 32 MiB | `PDS_EXEC_BASE` points here |
| 3 | `PDS_VERTEX` | `0xec000000` [`0x30400000`] | — | TA work, later |
| 4 | `USSE_PIXEL` | `0xf4000000` [`0x37000000`] | 5 MiB | `USE_CODE_BASE_0..9` windows |
| 5 | `USSE_VERTEX` | `0xfc000000` [`0x3c000000`] | 2 MiB | `USE_CODE_BASE_10..13` |
| 6 | `KERNEL_CODE` | `0xe8000000` [`0x30000000`] | [4 MiB] | **alias; never mappable** |
| 7 | `KERNEL_DATA` | `0xf0000000` [`0x32000000`] | [80 MiB] | **alias; never mappable** |

The kernel heaps are reported (flag `DRM_SGXFIVE_HEAP_RESERVED`) so userspace can avoid them
and tests can probe them. In the harness layout they were contiguous (`0xe8000000` to
`0xf3ffffff`) and one `KERNEL` heap would have done; in the `REV1` layout (§9.1a decision 5)
`PDS_VERTEX` (`0x30400000`) sits between kernel code and kernel data, so they are two heaps.
VM_MAP accepts only addresses inside one non-reserved heap, so the kernel heaps need no
`drm_gpuvm` reserved node of their own.

**What the alias must contain.** **[HW]** While applying a transfer's `updates`, the EDM
points `BIF_BANK0.INDEX_EDM` at the *context's* directory list and keeps executing (`xfer`
§4.2, `xfer_iso`): without the ukernel mapped in the client PD it faulted on its own code
(`BIF_FAULT` CID 2, page `0xe8009000`) and wedged. During that window every EDM memory access
— code fetch, the command's update list, and any `sa`-relative load or store by an EDM task
that happens to run then (the 1 kHz timer task writes HostCtl; two USE pipes serve EDM
events) — translates through the client PD. Hence the DDK maps its kernel heaps into every
memory context, read-write.

**[HW] Read-only aliases suffice** (`xfer` §4.2, `dri/0/sgx/alias_ro`, `xfer_ro`): with
every EDM alias mapped read-only in the client directory, the isolation test (fill plus one
`updates` entry through the client PD) still passes, so the EDM only *reads* through the
aliases. And a client fill aimed at a read-only alias **faults** (`BIF_FAULT` page
`0xf0200000`, CID 1 = the client's directory list) and writes nothing: the PTE `READONLY`
bit is enforced for the pixel-side requestors, so the kernel buffers are protected from
client rendering. Our proposal, with every part read-only:

- **Global code**, per device: the ukernel and PDS #10–#14. Read-only (PTE RO) **[HW]**.
- **Global data**, per device: kernel CCB and its control, kicker word, TA3DCtl, HostCtl,
  misc info, HWPerf and signature buffers. Read-only **[HW]**.
- **Private data**, per VM: this VM's client CCBs, CCB controls, HW contexts and kernel
  fence words. Read-only **[HW]**; the EDM presumably writes them through its own PD
  (list 15).

The read-only result was measured with a one-entry update list, i.e. a short window. Since
the UAPI never lets userspace send `updates` (§4.4), the window stays at least that short
in practice. A stress run with a long list while EDM timer tasks run (§7 Q2) is still worth
doing: an EDM *write* inside the window (HostCtl, HWPerf) would fault and wedge the firmware,
though it could not corrupt anything.

What the alias still exposes is **read access**: a client's shaders can read the ukernel,
the control blocks and, through the global data, the kernel CCB. The private range keeps
other processes' CCBs and contexts out of view. Whether the alias is needed at all when a
command has **no `updates`** is §7 Q1: if the EDM skips the `BANK0` switch for an empty
update list, the alias could shrink to the private data or disappear, removing the read
exposure too.

**Address constraints.** GPU VAs are 32-bit. The UAPI carries them as `__u64` (as `pvr`
does, and for Mesa code sharing) and the kernel rejects anything at or above 4 GiB. Several
fields are narrower still: ISP-stream and background-object addresses are relative to
`BIF_3D_REQ_BASE` (a 1 MiB-aligned window, `xfer` §2.4), `EVENT_PIXEL_PDS_EXEC` takes bits
[31:4], `DOUTU` reaches code within the selected 512 KiB code-base window. Those are
userspace's to respect; the kernel validates only what protects other users (§4.5).

### 3.6 Kernel-owned GPU memory

Allocated from the kernel alias region by a simple range allocator (`drm_mm`), never visible
as GEM handles:

- firmware buffers (built at load, `fwfmt` §3);
- per transfer context: client CCB `0x10000 + 0x4c8` (the EDM masks the read offset with
  `0xffff`, `xfer` §3.5), CCB control (8 B), HW transfer context (`0xb4`, 32-byte aligned),
  one fence word; together ~68 KiB per context, placed in the owning VM's private alias range
  and in the EDM's PD;
- later per render context: HW render context, render details, per-context parameter
  buffer descriptors, RT data sets (`spec` §13.3 sizes).

## 4. Contexts and job submission

### 4.1 Objects

| UAPI object | kernel object | firmware object |
|---|---|---|
| VM context | `sgx_vm`: `drm_gpuvm` + PD | a page directory |
| `TRANSFER` context | `sgx_ctx`: scheduler + CCB | `HWTRANSFERCONTEXT` (`0xb4`) |
| `RENDER` context (later) | as above, TA and 3D queues | `HWRENDERCONTEXT` (`0x44`) + PB |
| render target set (later) | `sgx_rt` | `HWRTDATASET` / `HWRTDATA` (`pvr`: HWRT dataset) |
| parameter buffer (later) | `sgx_pb` | `HWPBDESC` (`pvr`: free list) |

### 4.2 Context create / destroy (transfer)

`CREATE_CONTEXT {type = TRANSFER, priority = NORMAL, vm_context_handle}`:

1. Allocate the CCB, CCB control, HW context and fence word in the VM's private alias range
   and in the EDM's PD (§3.6).
2. Fill the HW context **in the kernel** (`sgx_xfer_ctx_init`): flags `NEWCONTEXT`, priority
   0, `sPDDevPAddr` = the VM's PD, CCB and CCB-control addresses, everything else 0. PID
   (`+0xa8`) = the TGID, which only feeds HWPerf.
3. Create the `drm_sched` scheduler and entity (§4.6). Return a handle.

`DESTROY_CONTEXT`: tear down the entity (waits for or kills queued jobs), wait for the
context's last fence, send `SGXMKIF_CMD_CLEANUP` type `TC` (3) with the HW context address
and wait for `HostCtl.ui32CleanupStatus` (the DDK's `SGXCleanupRequest`; **not yet tried on
hardware**), then free the memory once the EDM's read offset has passed the cleanup command.
A context destroyed while the firmware is `OFF` needs no cleanup command.

### 4.3 SUBMIT_JOBS

Shaped like `DRM_IOCTL_PVR_SUBMIT_JOBS`: an array of jobs, each

- `type` — `TRANSFER` now; `TA`, `3D` later (`pvr`'s `GEOMETRY`/`FRAGMENT`);
- `context_handle`, `flags` (job-type specific);
- `cmd_stream`, `cmd_stream_len` — a type-specific, kernel-defined register block (§4.4);
- `sync_ops` — an array of `{handle, flags, value}`: wait or signal, binary or timeline
  syncobj (the `pvr` encoding, `DRM_PVR_SYNC_OP_FLAG_*`).

No BO list: residency is the VM (all mapped BOs are resident; nothing is evictable), and
synchronisation is explicit. The only implicit fences are on exported BOs (§3.2), attached
by walking the VM's external objects with `drm_gpuvm_exec_lock` — `pvr` does not even do
that, so this is optional and can wait for the KMS integration.

On error the ioctl reports the failing job index in `jobs.count` (the `pvr` convention) and
no job of the call is queued: all jobs are validated, armed and pushed together under one
`drm_exec` transaction.

### 4.4 What userspace supplies and what the kernel fills (transfer)

Userspace places its programs and data **in its own VM** (fill or copy shader, USE
secondary, EOT/EOR, pixel-event PDS, PDS primary/secondary, background object, source and
destination surfaces), then submits a `drm_sgxfive_transfer_cmd` (§10) holding only the
register words of `SGXMKIF_TRANSFERCMD.sHWRegs` (`xfer` §2.3) and the coefficient loads.

| `SGXMKIF_TRANSFERCMD` field | by | how |
|---|---|---|
| `sHWRegs` ISP/BIF/PDS/PBE (`0x000`–`0x043`) | user | copied after validation (§4.5) |
| CSC, luma-key, FIR (`0x044`–`0x0a3`) | user | copied; used only with its load flag |
| `ui32Size` | kernel | `(0x1a0 + 8 × updates + 0x3f) & ~0x3f` = `0x1c0` |
| `ui32Flags` | kernel | `DUMMYTRANSFER` (`0x20`) on request, nothing else |
| `shared_state` src/dst syncs | kernel | none: `drm_sched` resolves dependencies (§4.6) |
| `shared_state` TA/3D sync blocks | kernel | 0 until render contexts need TQ/3D ordering |
| `sCtlStatusInfo[0]` | kernel | `{context fence word, job seqno}`: completion |
| `sCtlStatusInfo[1..3]` | kernel | unused (0) |
| `load_flag*Coefficients` | user | from `load_flags` |
| `num_pixel_partitions` | kernel | `0x07c00000` (31, `xfer` §1.3) |
| `fence_id` | kernel | job seqno (loaded by the EDM, no use seen) |
| `num_updates`, `updates[]` | kernel | 0: **not exposed** (below) |

The kernel never sets `TATQ_SYNC`, `3DTQ_SYNC`, `KEEPPENDING` or `NOSYNCUPDATE`.

Why **no `updates`** in the UAPI: they are memory writes performed *by the EDM* through the
client's directory list while the EDM is executing — exactly the window that forces the
alias (§3.5). Userspace has no need for them (the closed library's fills use none, `xfer`
§4.2.6; with CPU-side dependency resolution there is nothing to patch at kick time), and
leaving them out may remove the alias requirement altogether (§7 Q1). If a later need
appears, they can come back as a validated list whose targets must lie inside the caller's
own mappings: the job struct reserves that list (`drm_sgxfive_job.updates`, §10), rejected
with `-EINVAL` while `JOB_INFO.max_updates` is 0.

The **command stream is versioned by length**, using `copy_struct_from_user` semantics:
a newer, longer struct is accepted if its extra tail is zero, an older shorter one is
zero-extended. Unknown `flags` bits and non-zero padding are `-EINVAL`.

### 4.5 Validation rules

The kernel cannot check that userspace's programs are correct; it checks what protects the
firmware and other clients:

1. **Addresses that make the 3D pipe fetch or execute** — `EVENT_PIXEL_PDS_EXEC`,
   `PDS_EXEC_BASE`, `BIF_3D_REQ_BASE`, and `ISP_RGN_BASE` when non-zero — must lie in a user
   heap of the VM (not in `KERNEL`), with the field's alignment. This matters only if the
   alias is present; it stops a client from pointing the pixel pipe at the ukernel or at
   PDS #13 in its alias.
2. **Register fields with global side effects** are masked to their documented bits:
   `ISP_RENDER` type in the allowed set (FASTSCALE and the normal types), `PIXELBE`,
   `ISP_IPFMISC`, `3D_AA_MODE`, `ISP_MULTISAMPLECTL` and `DOUBLE_PIXEL_PARTITIONS` (0 only
   until a use is known). `DMS_CTRL` is OR-ed with `num_pixel_partitions` by the EDM
   (`xfer` §3.3 step 1), which is why that word is kernel-owned.
3. **Render box** within the 4K × 4K tile range the register can express (no
   `cr_4kx4k_render` on SGX545).
4. **Sizes**: `cmd_stream_len` ≥ the v1 struct, ≤ one page; ≤ 64 sync ops per job, ≤ 64 jobs
   per call.
5. **Context state**: a lost (guilty) context fails with `-ECANCELED`; a context of another
   type than the job with `-EINVAL`.

What remains unvalidated is the content of user programs, which run with whatever privileges
USSE and PDS tasks have; that is §6's main open risk.

### 4.6 Scheduling with `drm_sched`

- **One `drm_gpu_scheduler` per context** with a single entity, like `pvr_queue`: the
  firmware arbitrates between contexts (round-robin within priority 0, transfers ahead of 3D
  renders, `xfer` §3.1 step 4), so the kernel does not. All schedulers share one ordered
  `submit_wq` for the device, so there is no thread per context.
- **Credits = CCB bytes**: `credit_limit = 0x10000`, each job costs its `ui32Size` (`0x1c0`
  for a transfer without updates, so 146 jobs in flight per context). `run_job` writes the
  command at the CCB write offset, advances the offset, then sends `SGXMKIF_CMD_TRANSFER`
  with `ui32Data[1]` = the HW context and the pending cache-control bits through the kernel
  CCB (the existing `gma_sgx_command`). The kernel CCB (256 × 32 B) is shared by all
  contexts and drained by the EDM within microseconds; a full kernel CCB is waited for
  briefly in `run_job`, not reported to userspace.
- **Dependencies are all resolved on the CPU**: wait sync ops and implicit fences become
  `drm_sched_job_add_dependency` entries, so a job reaches the firmware only when it is
  ready, and the firmware's own sync-object protocol (`xfer` §1.5) stays unused. A later
  optimisation can turn same-device fences into firmware syncs (as `pvr` does with UFOs) to
  save the CPU round trip; the UAPI does not change for that.
- **One transfer runs at a time** in the firmware (`xfer` §3.1 step 7). drm_sched does not
  need to model it, but it affects the **timeout**: a job's timer starts at `run_job`, while
  it may legitimately wait behind other contexts' work. `timedout_job` therefore first checks
  whether *any* context's fence word advanced since the last check; if so it returns
  `DRM_GPU_SCHED_STAT_NO_HANG` and re-arms; only a device with no progress goes to HWR
  (§2.4). Default timeout 500 ms, matching the DDK's `MAX_HW_TIME_US`.
- **Completion**: the EDM writes each `sCtlStatusInfo` value, then raises `SW_EVENT`
  (`xfer` §3.1 step 8; **[HW]** the status word is already written when the interrupt
  arrives). The hard IRQ acknowledges `SW_EVENT | MASTER_INTERRUPT` (as now) and wakes a
  threaded handler, which reads (`rmb`) the fence word of every context with jobs in flight
  and signals every job fence whose seqno is ≤ the value read. Per-context seqnos are
  monotonic because the EDM consumes each CCB in order.
- **Dummy jobs** (`DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY`): the EDM's `DUMMYTRANSFER` path
  runs queueing, completion writes and `SW_EVENT` without touching 3D state (**[HW]**). It
  gives userspace and IGT a cheap, program-free job for sync and scheduling tests, and gives
  Mesa a way to signal a syncobj in queue order.

### 4.7 `dma_fence` semantics

- A job's out-fence is the `drm_sched` finished fence; it signals after the EDM's status
  write for that job **and** after the job's memory writes are visible to the CPU: the EDM
  writes the status after the PBE's end-of-render (`PIXELBE_END_RENDER`), and our CPU
  mappings are WC/uncached, so a reader after the fence sees the data (**[HW]** for fills
  and copies via the `rmb` after the event). Cached mappings, when allowed, would add an
  explicit invalidate on the CPU side (`DRM_IOCTL_SGXFIVE_BO_SYNC`-style) or rely on snooping.
- Error fences: `-EIO` for a job lost in HWR, `-ETIMEDOUT` for the timed-out job,
  `-ECANCELED` for jobs flushed from a destroyed or lost context.
- Signal sync ops attach the finished fence to binary syncobjs or timeline points; userspace
  can export them as `sync_file`s for KMS or other devices.
- The fence context is per scheduler (per HW context), as `drm_sched` provides.

## 5. Queries (DEV_QUERY)

One extensible ioctl, `pvr` style: `{type, size, pointer}`; a NULL pointer returns the size;
a short `size` gets a truncated copy; arrays use `{stride, count, array}`.

- `GPU_INFO`: `CORE_ID` (`0x01150000`), `CORE_REVISION` (`0x0001000d`), the SW core id
  (545) and revision from GETMISCINFO, the host bridge revision, and quirk bits
  (`SAMPLE_CACHE` when the bridge is below rev 4).
- `FW_INFO`: DDK version (`1.7.17`), build (862890), build options (`0x60218`), `.fw` format
  version, and the firmware state (`OFF`/`ON`/`IDLE`) for debugging.
- `HEAP_INFO`: the heap table of §3.5, `{base, size, flags, page_size_log2}` per heap id.
- `JOB_INFO`: bitmask of supported job types (`TRANSFER` now), maximum sync ops and jobs per
  call, the transfer command-stream size (v1) and the update-list limit (0: none accepted).
- `RUNTIME_INFO`: `num_pixel_partitions` (31), tile size (32 × 16); render values later.

`GPU_INFO` is answered from registers read at probe plus the cached GETMISCINFO reply of the
last boot; it never boots the firmware by itself (if the firmware has not booted yet, the SW
fields are 0 and a flag says so).

## 6. Security analysis

### 6.1 What isolation rests on

1. **One page directory per VM**, assigned by the EDM per job. **[HW]** The pixel-side
   requestors (PDS, USE, ISP, TSP, PBE) and the `updates` writes translate through the
   context's directory: a fill into an address mapped only there lands, an update target
   mapped only there is written (`xfer` §4.2, `xfer_iso`).
2. **Read-only PTEs are enforced**, and the EDM only reads through its aliases. **[HW]** With
   all aliases read-only the isolation test passes, and a client fill aimed at an alias
   faults and writes nothing (`xfer_ro`). So the firmware's code and data cannot be
   modified by client rendering.
3. **Kernel-owned firmware structures**: HW contexts, CCBs, sync and fence words, the kernel
   CCB and all control blocks are allocated, written and validated only by the kernel.
4. **Command words reduced to rendering state** (§4.4, §4.5); no raw firmware pointers,
   no `updates`, no firmware sync objects from userspace.

### 6.2 What a malicious client cannot do (if §7 Q4 comes out as hoped)

- Address another process's memory: no mapping of it exists in its PD.
- Address physical memory: no PTE it controls; PD/PT pages are kernel memory not mapped in
  any VM.
- Write firmware code or data, or forge firmware commands: the kernel CCB, control blocks
  and its own client CCB appear in its PD only as read-only aliases, and a write to them
  faults instead of landing (**[HW]**, `xfer_ro`); the client CCB contents are written only
  by the kernel.
- Corrupt firmware state through registers it supplies: global side effects masked (§4.5).

### 6.3 What it can do today, or might

Each risk with its status and mitigation:

- **DoS by GPU fault** — certain, **[HW]**. Any client fault, e.g. a fill aimed at a
  read-only alias (`xfer_ro`) or at an unmapped page, wedges the EDM with no completion until
  the firmware is reloaded. Mitigation: mandatory HWR (§2.4) — fault detection by the
  watchdog, reset and reboot, re-run of innocent jobs, discard of the guilty context's jobs and a
  ban on that context; a per-file fault budget on top.
- **DoS by long jobs** — certain: one transfer at a time, no preemption. Mitigation: the
  500 ms timeout leads to HWR, so no job holds the GPU longer.
- **Read of the alias** (firmware code, control blocks, its own CCB) — while the alias
  exists. Mitigation: RO PTEs, private CCBs per VM (§3.5), and dropping the alias if Q1
  allows.
- **Write to the alias** — prevented, **[HW]**: with RO aliases the write faults and nothing
  is written. Mitigation: RO PTEs on every alias page; the fault itself is the DoS above.
- **USSE register reads** — certain, **[HW]** (§7 Q4). A client pixel shader reads any SGX
  register with `ldr rD, #idx`, the EDM's own MMIO instruction: `CORE_ID`, and
  `BIF_DIR_LIST_BASE0`, the physical address of a page directory. So a client learns
  physical addresses of kernel page tables and sees every other register's state, including
  the fault and event registers. Registers that clear on read, if SGX545 has any, would let
  it disturb the EDM as well. Mitigation: kernel-side program validation (the vc4
  precedent; it must cover programs the client can rewrite after submission, so it needs
  kernel-owned read-only copies), or restricting the render node to trusted users.
- **USSE register writes** — not seen, **[HW]**: a client `str #671` (`USE_G0`) neither
  reads back in the same shader nor reaches the host, while the host can write the register.
  Only one register was tried; a store landing in `BIF_DIR_LIST_BASE*` would hand over all
  memory, so this stays a residual risk until more registers are covered safely.
- **Stale temporaries** — certain, **[HW]**: temporaries a shader reads before writing hold
  what earlier tasks left, including a kernel-heap address from the firmware's own task
  (`0x32411000`, a context's HW-context address). So data leaks between clients and from the
  firmware. Mitigation: none in the kernel short of program validation (a shader that reads
  a temporary before writing it is rejected).
- **`DOUTU` into code base 15** — not exploitable as tried, **[HW]**: a client PDS primary
  kicking `CBASE` 15 at instruction 0 faults at page 0, not at the EDM code base
  (`0x30000000`), and the client directory never maps the kernel code heap anyway. The fault
  is recovered like any other.
- **Information leak through HWPerf and signature buffers** — low: not exposed by the UAPI,
  read-only alias at worst.
- **Stale TLB after unmap** — handled by the deferred page release (§3.3).

HWR now works and Q4 is answered (reads yes, the writes tried dropped). By decision 7
(§9.1a) the render node stays on by default with the register-read leak accepted for now; an
opt-in gate (a module parameter documented as "trusted users only") can follow later. The
DDK never offered isolation (its shared heaps are read-write in every context); with
read-only aliases and recovery this design already protects the firmware and other clients'
memory, which is strictly better than history, but the register reads keep it short of an
upstream default until a validator or the gate exists.

## 7. Hardware probes this design depends on

Each is a debugfs test in the style of `xfer_iso`, before the corresponding phase:

- **Q1. Does the EDM switch `BIF_BANK0.INDEX_EDM` to the client list when
  `num_updates = 0`? — answered no [HW]** (`dri/0/sgx/no_alias`, 2026-10-20). A
  client-PD fill with no updates, whose client directory maps only the job's programs,
  background object and destination (no alias at all, not even the context's CCB or HW
  context), completes correctly with no fault, three runs in a row; `BIF_BANK0 = 0x100f`
  (EDM on list 15, 3D on list 1). The EDM reads contexts, CCBs and its own code through its
  own directory; it enters a client's only to apply `updates`. **Consequence: client VMs
  carry no EDM alias** (the UAPI has no updates, decision 4). The reserved `KERNEL_CODE` /
  `KERNEL_DATA` heaps stay reserved and unmapped in client VMs, which also closes the
  PDS-pointer reach concern (a pointer into them faults instead of reaching firmware data);
  the residual risk is a faulting job, handled by recovery.
- **Q2. Can the global alias be read-only? — answered yes [HW]** (`alias_ro`, `xfer_ro`):
  all aliases read-only, isolation test passes, a client write to an alias faults and lands
  nowhere. Residual stress test: a long `updates` list (100 entries, debugfs only) run many
  times while the 1 kHz timer and other EDM tasks run, looking for a `BIF_FAULT` with CID =
  the client list (an EDM write inside the window).
- **Q3. What does PTE bit 4 (`EDMPROTECT`) do?** Less urgent now that RO works. The DDK sets
  it on HW contexts in kernel data (`sgxutils.c`, `mmu.c`). Test: a fill whose destination
  page is EDM-protected (a fault would mean "EDM only"), and an `updates` target that is
  EDM-protected. `BIF_INT_STAT.FAULT_TYPE` [18:16] should distinguish permission faults;
  record it for the `xfer_ro` fault too.
- **Q2a. Can a wedged EDM be recovered without a full reload?** After the `xfer_ro` fault:
  does deinit + `SOFT_RESET = 0x3f` + the boot sequence (with the fault-loop F of `boot` (a)
  mapping the faulting page to a dummy, as the DDK's HWR reset does) bring it back, with the
  other contexts' state reset as §2.4 describes? How long does it take? Is the
  `BIF_INT_STAT` fault cleared by the reset? Does any `EVENT_STATUS`/`_STATUS2` bit latch
  on the fault, so it could interrupt the host instead of being polled? This is the core of
  HWR.
- **Q4. Can non-EDM USSE tasks access SGX registers?** **Answered (**[HW]**, 2026-10-21,
  `sgxfive-test q4`, client fills on the render node): reads yes, writes not seen.**
  `ldr r0, #7` returns `CORE_ID` (`0x01150000`) in every pixel, and `ldr r0, #801` returns
  `BIF_DIR_LIST_BASE0` exactly as the host reads it. `str #671` (`USE_G0`, which the host can
  write, low 8 bits) changes nothing, read back in the shader or by the host. Unwritten
  temporaries hold earlier tasks' data. A PDS kick with `CBASE` 15 faults at page 0. §6.3
  has the consequences. (The probe first showed nothing at all, because libsgxfive's
  assembled source patched DOUTU words from its own metadata instead of the engine's, so
  the probe shader ran without temporaries; fixed.)
- **Q5. Does PTE bit 3 (`CACHECONSISTENT`) give snooped access on Cedarview?** Test: a fill
  into a cached (WB) page with bit 3 set, read without a flush. Decides `BO_CPU_CACHED`.
- **Q6. Directory-list behaviour with more than 15 resident VMs**: 20 contexts on 20 VMs
  round-robin; check isolation still holds and the EDM never reuses list 15.
- **Q7. Through which list does the EDM do completion writes** (status words, sync complete
  words)? Test: a status target mapped only in the EDM's PD vs only in the client PD. Decides
  whether fence words need to be in the private alias at all.
- **Q8. `SGXMKIF_CMD_CLEANUP` (type TC)** semantics and `ui32CleanupStatus` handshake.

## 8. Phased implementation plan

Every phase ends with KUnit tests (`sgx_*_test.c`, `kunit.py` on cachix) for the pure code
and hardware tests on the laptop (`scripts/push-gma500.sh`, then the tool or IGT binary run
there).

- **P0 (done).** debugfs: load, boot, misc info, power, dummy/sync/fill/copy/isolation and
  read-only-alias transfers. KUnit: `sgx_fw`, `sgx_seq`, `sgx_xfer` builders. Laptop: the
  debugfs files.
- **P1.** Hardware probes Q1, Q2a (wedge recovery), Q4, Q7, Q3 as debugfs tests (Q2 is
  done). Laptop: the probes; results go into `xfer` and this document.
- **P2.** Refactor into `sgx_dev` (firmware, kernel CCB, cache-control accumulator, IRQ
  thread, power states, lazy boot, idle) and `sgx_host_ops`; debugfs stays on top. KUnit:
  kernel-CCB ring and wrap, cache-control accumulation, the state machine. Laptop:
  boot/idle/wake/stop loops, a suspend/resume cycle.
- **P3.** Render node (opt-in), `DEV_QUERY`, `CREATE_BO`, `GET_BO_MMAP_OFFSET`; the
  `libsgxfive` + `sgxfive-test` skeleton. KUnit: query size and truncation rules, BO flag
  validation. Laptop: `sgxfive-test query`, BO create/mmap/close, invalid-argument cases.
- **P4.** VM contexts and `VM_MAP`/`VM_UNMAP` on `drm_gpuvm` + `psb_mmu`: per-VM PD, shared
  global PTs, read-only private alias, deferred release. KUnit: VA validation against heaps,
  alias placement, PDE sharing, deferred-free ordering. Laptop: map/unmap stress, a map in
  `KERNEL` rejected, PD/PT leak checks through debugfs.
- **P5.** Transfer contexts, `SUBMIT_JOBS` with dummy jobs, syncobjs, `drm_sched`, the
  completion IRQ path, timeouts, context destroy with CLEANUP, and **HWR** (fault detection,
  reset and reboot, re-run of innocent jobs, ban of the guilty context). KUnit:
  command-word construction, validation masks, seqno handling, HWR bookkeeping (which jobs
  re-run, which fail). Laptop: dummy chains across contexts, wait/signal timelines, a
  1000-job soak, destroy with pending jobs, a forced reset with jobs queued on several
  contexts.
- **P6.** Real transfers from userspace: `sgxfive-test` builds fill and copy programs from the
  user's `.fw` templates. Laptop: the P0 fill/copy rectangle matrix from userspace,
  isolation (VM A cannot reach VM B), and a fill into the read-only alias → fault → HWR →
  `-ECANCELED` for that context while another context's jobs complete.
- **P7.** PRIME export, gma500 scanout of a rendered BO, a fence wait before the flip.
  Laptop: fill a BO, flip to it, check the screen.
- **P8.** IGT tests for P3–P7 in an `igt-gpu-tools` branch (`tests/sgxfive/`), run with
  `igt_runner` on the laptop.
- **P9.** Render contexts: TA/3D, parameter buffers, RT data sets (own design document
  first). Laptop: the first triangle.
- **P10.** Open USSE/PDS assembler, a Mesa Gallium driver, the upstream submission (device
  structure decided, documentation). Laptop: piglit and deqp-gles2 subsets.

P1 comes first on purpose: its results decide the alias (P4) and whether the render node can
ever be enabled by default.

## 9. Decisions for the user, and open questions

### 9.1 Decisions needed (most consequential first) — all taken, see §9.1a

1. **Device structure** (§2.1): render node on gma500's device, or an auxiliary-bus
   `pvrsgx` driver. Recommendation: develop in gma500 with the internal split, submit as B.
   Needed before any userspace depends on the driver name.
2. **Open userspace path** (§1.4): commit to writing a clean-room USSE/PDS assembler (and
   later a compiler) for Mesa. Without it there is no upstream UAPI; with only the test tool,
   the work stays a local kernel series.
3. **UAPI name** (§10): `pvrsgx` (`pvrsgx_drm.h`, `drm_pvrsgx_*`, `DRM_IOCTL_PVRSGX_*`) is
   proposed. `sgx` alone collides with Intel SGX (`arch/x86/kernel/cpu/sgx`, `/dev/sgx_*`) on
   the very x86 machines this targets; `pvr` is taken by Rogue. The out-of-tree OpenPVRSGX
   tree uses `pvrsgx` as a directory name for the DDK port, which is a minor clash.
4. **No `updates` in the UAPI** (§4.4): recommended; confirm.
5. **Heap layout**: harness layout (what the code runs today) or the DDK's `REV1` layout with
   a separate texture heap (`boot` (d) recommends `REV1` for our rev-3 host bridge). The UAPI
   is neutral; the firmware placement and tests are not.
6. **Opt-in policy** (§6.3): module parameter, default off, until the security probes pass.

### 9.1a Decisions taken (2026-10-20, 7 on 2026-10-21)

1. **Device structure: develop inside gma500, structured as the auxiliary-bus split (option
   C).** All GPU code lives in its own directory and module boundary behind an internal
   interface shaped like the auxiliary-bus split, and becomes a separate `pvrsgx`-style driver
   on the auxiliary bus before upstream submission. The GPU-side page-table (SGX MMU) code
   moves into the GPU part now, since every per-process address space depends on it; gma500
   keeps only the display GTT.
2. **Open userspace: commit to a clean-room USSE/PDS assembler now and a shader compiler
   later (option A).** The assembler is the inverse of our disassemblers, verified against
   the same reference tools; its first milestone is reproducing the transfer templates from
   our own source, so the closed `libsrv_um` is no longer needed for 2D. The compiler, as the
   back end of a Mesa driver (GLES for glamor and Wayland compositors), is an accepted
   long-term goal. The ukernel still comes from the user's library; replacing it is a separate
   question. Status: the first milestone is done. `libsgxfive` builds the transfer programs
   from our source by default, and `fwcutter cut <libsrv_init.so> --tq-from-source` writes
   them into the firmware file as origin `OURS` (`fwfmt` §11).
3. **Name: `sgxfive`** (SGX, PowerVR Series 5). It replaces the draft's `pvrsgx` throughout:
   `sgxfive_drm.h`, `struct drm_sgxfive_*`, `DRM_IOCTL_SGXFIVE_*`, the module and the DRM
   driver name, and Mesa's `src/sgxfive/`. "Five" is exact: SGX is Series 5 and 5XT, and
   Series 6 onwards is Rogue (upstream `imagination`). Spelled out, it stays distinct from
   Intel SGX in searches, unlike `sgx` or `sgx5`. No use of the name was found in v7.3 or in
   our reference trees. (§10 has used `sgxfive` since the header was written,
   `kernel/uapi/sgxfive_drm.h`.)
4. **`updates`: not in the first UAPI, with an extension point reserved (option C).** The
   kernel always submits zero updates. The submit structure reserves a flag (or extension
   slot) for an update list, rejected with `-EINVAL` today, so a validated list can be added
   later (e.g. for TA/3D) without breaking userspace. Follow-up probe (Q1): whether a job
   without updates ever runs the EDM through the client's directory; if not, client VMs need
   no EDM alias at all.
5. **GPU address layout: the DDK's `REV1` layout now, reported through DEV_QUERY (option B,
   structured as C).** It is what Intel's kernel ran on rev < 4 host bridges
   (`FIX_HW_BRN_SAMPLE_CACHE`, our silicon): everything below `0x3e000000` (kernel code
   `0x30000000`, kernel data `0x32000000`, PDS `0x2e000000`, pixel shaders `0x37000000`, ...)
   and a 256 MB texture heap at `0xf0000000` (`design/sgx545_boot_sequence.md` (d)). The
   laptop moves to it and the full hardware checklist is re-run. Userspace never hardcodes
   addresses: DEV_QUERY reports the heaps, so the generic layout for rev ≥ 4 steppings (or
   other SGX5 platforms) can be added once someone can test it. A follow-up probe may try to
   reproduce the sample-cache erratum under the generic layout.
6. **Enablement: the render node is on by default (option A).** No module parameter gates
   it. The items that option B would have used as gates become high-priority work instead,
   tracked in §8: hardware recovery (watchdog, reset, resubmission of innocent jobs, banning
   the guilty context), the Q4 probes (client USSE/PDS access to registers or EDM code, with a
   kernel validator if needed), the Q1 alias probe, and IGT-style tests. Until recovery
   lands, a faulting job wedges the GPU until the firmware is reloaded (accepted for now).
7. **After Q4: the render node stays on by default, and the register-read leak is accepted
   for now (option C, 2026-10-21).** Recovery has landed and memory isolation holds, but
   client shaders read every SGX register and see stale temporaries (§6.3, §7 Q4). The
   choice is to accept that information leak for the time being, as older GPU drivers did,
   rather than gate the node. Two remedies stay open: an opt-in gate, a module parameter
   documented as "trusted users only" (option A), can be added later without a UAPI change;
   a kernel program validator with kernel-owned read-only program copies (option B) is the
   long-term path once a Mesa compiler defines what it must accept. Either is required
   before upstream submission.

### 9.2 Open questions, ranked

1. **Q4 — USSE/PDS register access from client tasks.** **Answered** (§7): clients read
   every register and see stale temporaries; writes to the one register tried are dropped.
   Memory isolation still holds, but a client learns physical addresses and other clients'
   leftovers, so unprivileged access needs a program validator or stays off.
2. **Q2a — client-fault recovery**: **answered yes for the reset** (**[HW]**, `xfer`
   §4.2 isolation findings): halt without POWEROFF, clear the latched fault and events,
   rebuild the firmware buffers in place, reset every HW context and client CCB, boot.
   Innocent contexts' unfinished jobs are re-run correctly (**[HW]**, `sgxfive-test
   isolation` check `queued-innocent-job-reruns`).
3. **Q1 — alias needed without `updates`?** No longer a safety question (read-only aliases
   protect the firmware, **[HW]**), but it decides the VM layout's complexity and whether
   clients can read firmware memory.
4. **Q2 stress / Q3** — an EDM write inside the alias window under load, and the
   `EDMPROTECT` bit's meaning.
5. **TLB/PD invalidation guarantees**: that cache-control bits on the next command are
   sufficient after unmap and before a PD page is reused (P4 stress).
6. **Q7 — where completion writes translate**; decides fence-word placement.
7. **CLEANUP command (Q8)** and context teardown while the EDM still holds the context in a
   queue.
8. **Q5 — coherency**; decides cached BOs and CPU-side sync ioctls.
9. **gma500 KMS integration**: PRIME import (option B), fence waits in legacy page flip,
   stolen-memory imports.
10. **Render (TA/3D) objects**: PB sizing, per-context PB vs shared PB, RT data set ownership
    — separate design document after P6.
11. **Memory above 4 GiB on PAE i686** (§3.1): reject or allocate from lowmem.

## 10. UAPI header

The header is `kernel/uapi/sgxfive_drm.h` in this repository (destined for
`include/uapi/drm/sgxfive_drm.h`), with kernel-doc on every struct, field and ioctl. It is the
source of truth; the listing below is a condensed copy with the kernel-doc reduced to trailing
comments. Names follow `pvr_drm.h` where the concept is the same, so Mesa's Rogue winsys can be
used as a model. Every struct is a multiple of 8 bytes with explicit `_padding_<offset>`
members, so i686 and x86-64 share one layout (`userspace/libsgxfive/tests/test_uapi.c` checks
that every struct is the sum of its members). Padding must be zero, and every `flags` field has
a `*_FLAGS_MASK` outside which bits are rejected with `-EINVAL`.

```c
/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
#include "drm.h"

struct drm_sgxfive_obj_array {		/* arrays passed by pointer, as in pvr_drm.h */
	__u32 stride;			/* sizeof one element as userspace knows it */
	__u32 count;
	__u64 array;			/* user pointer */
};

#define DRM_SGXFIVE_DEV_QUERY		0x00	/* DRM_IOWR, dev_query */
#define DRM_SGXFIVE_CREATE_BO		0x01	/* DRM_IOWR, create_bo */
#define DRM_SGXFIVE_GET_BO_MMAP_OFFSET	0x02	/* DRM_IOWR, get_bo_mmap_offset */
#define DRM_SGXFIVE_CREATE_VM_CONTEXT	0x03	/* DRM_IOWR, create_vm_context */
#define DRM_SGXFIVE_DESTROY_VM_CONTEXT	0x04	/* DRM_IOW, destroy_vm_context */
#define DRM_SGXFIVE_VM_MAP		0x05	/* DRM_IOW, vm_map */
#define DRM_SGXFIVE_VM_UNMAP		0x06	/* DRM_IOW, vm_unmap */
#define DRM_SGXFIVE_CREATE_CONTEXT	0x07	/* DRM_IOWR, create_context */
#define DRM_SGXFIVE_DESTROY_CONTEXT	0x08	/* DRM_IOW, destroy_context */
#define DRM_SGXFIVE_SUBMIT_JOBS		0x09	/* DRM_IOW, submit_jobs */
/* 0x0a..0x0f reserved for render objects (parameter buffers, RT data sets). */
/* DRM_IOCTL_SGXFIVE_<name> = _mode(DRM_COMMAND_BASE + n, struct drm_sgxfive_<data>_args) */

/* ---- DEV_QUERY: {type, size, pointer}; NULL pointer = size only ---------------------- */

enum drm_sgxfive_dev_query {			/* append only */
	DRM_SGXFIVE_DEV_QUERY_GPU_INFO = 0,
	DRM_SGXFIVE_DEV_QUERY_FW_INFO,
	DRM_SGXFIVE_DEV_QUERY_HEAP_INFO,
	DRM_SGXFIVE_DEV_QUERY_JOB_INFO,
	DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO,
};

#define DRM_SGXFIVE_QUIRK_SAMPLE_CACHE	(1ULL << 0) /* bridge rev < 4: REV1 + TEXTURE */
#define DRM_SGXFIVE_GPU_INFO_SW_VALID	(1U << 0)   /* firmware has booted */

struct drm_sgxfive_dev_query_gpu_info {		/* 32 bytes */
	__u32 core_id;			/* EUR_CR_CORE_ID, 0x01150000 on SGX545 */
	__u32 core_revision;		/* EUR_CR_CORE_REVISION, e.g. 0x0001000d */
	__u32 core_id_sw;		/* from the firmware: 545; valid with SW_VALID */
	__u32 core_revision_sw;
	__u64 quirks;			/* DRM_SGXFIVE_QUIRK_* */
	__u32 host_bridge_rev;		/* 00:00.0 revision, selects the quirks */
	__u32 flags;			/* DRM_SGXFIVE_GPU_INFO_* */
};

enum drm_sgxfive_fw_state { DRM_SGXFIVE_FW_STATE_OFF, _ON, _IDLE };

struct drm_sgxfive_dev_query_fw_info {		/* 24 bytes */
	__u32 ddk_version;		/* major << 16 | minor << 8 | branch, 0x00010711 */
	__u32 ddk_build;		/* 862890 */
	__u32 build_options;		/* 0x60218 */
	__u32 fw_format;		/* sgx545_ukernel.fw format_version */
	__u32 state;			/* enum drm_sgxfive_fw_state; informational */
	__u32 _padding_14;
};

enum drm_sgxfive_heap_id {			/* every id reported; absent heap: size 0 */
	DRM_SGXFIVE_HEAP_GENERAL = 0,
	DRM_SGXFIVE_HEAP_TEXTURE,		/* size 0 unless QUIRK_SAMPLE_CACHE */
	DRM_SGXFIVE_HEAP_PDS_PIXEL,
	DRM_SGXFIVE_HEAP_PDS_VERTEX,
	DRM_SGXFIVE_HEAP_USSE_PIXEL,
	DRM_SGXFIVE_HEAP_USSE_VERTEX,
	DRM_SGXFIVE_HEAP_KERNEL_CODE,		/* reserved: never mappable */
	DRM_SGXFIVE_HEAP_KERNEL_DATA,		/* reserved: never mappable */
	DRM_SGXFIVE_HEAP_COUNT,
};

#define DRM_SGXFIVE_HEAP_RESERVED	(1U << 0)
#define DRM_SGXFIVE_HEAP_FIXED_WINDOWS	(1U << 1) /* base = a USE code base, 512 KiB windows */

struct drm_sgxfive_heap {			/* 24 bytes */
	__u64 base;
	__u64 size;			/* 0: heap absent on this device */
	__u32 flags;			/* DRM_SGXFIVE_HEAP_* */
	__u32 page_size_log2;		/* 12 */
};

struct drm_sgxfive_dev_query_heap_info {
	struct drm_sgxfive_obj_array heaps;	/* of struct drm_sgxfive_heap, by heap id */
};

#define DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT	(1ULL << 0)

struct drm_sgxfive_dev_query_job_info {		/* 24 bytes */
	__u64 job_types;		/* DRM_SGXFIVE_JOB_TYPE_*_BIT */
	__u32 max_jobs;			/* per SUBMIT_JOBS call */
	__u32 max_sync_ops;		/* per job */
	__u32 transfer_cmd_size;	/* largest drm_sgxfive_transfer_cmd known */
	__u32 max_updates;		/* entries in drm_sgxfive_job.updates; 0 today */
};

struct drm_sgxfive_dev_query_runtime_info {	/* 16 bytes */
	__u32 num_pixel_partitions;	/* 31 */
	__u32 tile_width;		/* 32 */
	__u32 tile_height;		/* 16 */
	__u32 _padding_c;
};

struct drm_sgxfive_dev_query_args {
	__u32 type;			/* enum drm_sgxfive_dev_query */
	__u32 size;			/* in: buffer size; out: bytes written/needed */
	__u64 pointer;			/* NULL: only report size */
};

/* ---- Buffer objects (released with DRM_IOCTL_GEM_CLOSE) ---------------------------- */

#define DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS	(1ULL << 0) /* mmap allowed, WC */
#define DRM_SGXFIVE_BO_CPU_CACHED	(1ULL << 1) /* reserved: -EINVAL for now */
#define DRM_SGXFIVE_BO_FLAGS_MASK	DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS

struct drm_sgxfive_create_bo_args {		/* 24 bytes */
	__u64 size;			/* in: bytes, non-zero, rounded up to 4 KiB */
	__u32 handle;			/* out */
	__u32 _padding_c;
	__u64 flags;			/* in: DRM_SGXFIVE_BO_* */
};

struct drm_sgxfive_get_bo_mmap_offset_args {	/* 16 bytes */
	__u32 handle;
	__u32 _padding_4;
	__u64 offset;			/* out: fake offset for mmap() */
};

/* ---- VM contexts --------------------------------------------------------------------- */

struct drm_sgxfive_create_vm_context_args { __u32 handle; __u32 _padding_4; };	/* out */
struct drm_sgxfive_destroy_vm_context_args { __u32 handle; __u32 _padding_4; };

#define DRM_SGXFIVE_VM_MAP_READ_ONLY	(1U << 0) /* SGX PTE READONLY */
#define DRM_SGXFIVE_VM_MAP_FLAGS_MASK	DRM_SGXFIVE_VM_MAP_READ_ONLY

struct drm_sgxfive_vm_map_args {		/* 40 bytes */
	__u32 vm_context_handle;
	__u32 flags;			/* DRM_SGXFIVE_VM_MAP_* */
	__u64 device_addr;		/* 4 KiB aligned, < 4 GiB, inside one user heap */
	__u32 handle;			/* BO */
	__u32 _padding_14;
	__u64 offset;			/* into the BO, 4 KiB aligned */
	__u64 size;			/* 4 KiB multiple; overlap = -EEXIST */
};

struct drm_sgxfive_vm_unmap_args {		/* 24 bytes */
	__u32 vm_context_handle;
	__u32 _padding_4;
	__u64 device_addr;		/* must match a whole mapping, else -ENOENT */
	__u64 size;
};

/* ---- Contexts ------------------------------------------------------------------------ */

enum drm_sgxfive_ctx_type { DRM_SGXFIVE_CTX_TYPE_TRANSFER = 0 /* RENDER later */ };
enum drm_sgxfive_ctx_priority { DRM_SGXFIVE_CTX_PRIORITY_NORMAL = 0 /* the only one */ };

struct drm_sgxfive_create_context_args {	/* 24 bytes */
	__u32 type;			/* enum drm_sgxfive_ctx_type */
	__u32 flags;			/* must be 0 */
	__s32 priority;			/* enum drm_sgxfive_ctx_priority */
	__u32 vm_context_handle;
	__u32 handle;			/* out */
	__u32 _padding_14;
};

struct drm_sgxfive_destroy_context_args { __u32 handle; __u32 _padding_4; };

/* ---- Job submission ------------------------------------------------------------------ */

/* Same encoding as pvr_drm.h's sync ops. */
#define DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK		0xf
#define DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ		0
#define DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ	1
#define DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL				(1U << 31)
#define DRM_SGXFIVE_SYNC_OP_FLAG_WAIT				0

struct drm_sgxfive_sync_op {			/* 16 bytes */
	__u32 handle;			/* syncobj */
	__u32 flags;
	__u64 value;			/* timeline point; 0 for binary */
};

/* Reserved extension point for the firmware's update list (updates, §9.1a decision 4). */
struct drm_sgxfive_mem_update {			/* 16 bytes */
	__u64 device_addr;		/* in the job's VM, 4-byte aligned */
	__u32 value;
	__u32 _padding_c;
};

enum drm_sgxfive_job_type { DRM_SGXFIVE_JOB_TYPE_TRANSFER = 0 /* TA, 3D later */ };

#define DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY	(1U << 0) /* no render, signal only */

#define DRM_SGXFIVE_TRANSFER_LOAD_YUV		(1U << 0)
#define DRM_SGXFIVE_TRANSFER_LOAD_LUMAKEY	(1U << 1)
#define DRM_SGXFIVE_TRANSFER_LOAD_FIR		(1U << 2)

/*
 * The register words of one transfer, by SGX register, in sHWRegs order. Addresses are
 * GPU virtual addresses in the context's VM, encoded as each register wants them.
 * Version 1; longer future versions keep this prefix, their unknown tail must be zero.
 */
struct drm_sgxfive_transfer_cmd {		/* 0xa8 bytes */
	__u32 isp_bgobjtag;		/* 0x000 ISP_BGOBJTAG */
	__u32 isp_bgobj;		/* 0x004 ISP_BGOBJ */
	__u32 isp_bgobjdepth;		/* 0x008 ISP_BGOBJDEPTH */
	__u32 isp_render;		/* 0x00c ISP_RENDER */
	__u32 isp_rgn_base;		/* 0x010 ISP_RGN_BASE */
	__u32 isp_ipfmisc;		/* 0x014 ISP_IPFMISC */
	__u32 bif_3d_req_base;		/* 0x018 BIF_3D_REQ_BASE, 1 MiB aligned */
	__u32 aa_mode_3d;		/* 0x01c 3D_AA_MODE */
	__u32 isp_multisamplectl;	/* 0x020 ISP_MULTISAMPLECTL */
	__u32 event_pixel_pds_exec;	/* 0x024 EVENT_PIXEL_PDS_EXEC */
	__u32 event_pixel_pds_data;	/* 0x028 EVENT_PIXEL_PDS_DATA */
	__u32 event_pixel_pds_info;	/* 0x02c EVENT_PIXEL_PDS_INFO */
	__u32 isp_render_box1;		/* 0x030 ISP_RENDBOX1 */
	__u32 isp_render_box2;		/* 0x034 ISP_RENDBOX2 */
	__u32 pixelbe;			/* 0x038 PIXELBE */
	__u32 pds_exec_base;		/* 0x03c PDS_EXEC_BASE */
	__u32 double_pixel_partitions;	/* 0x040 DOUBLE_PIXEL_PARTITIONS, 0 for now */
	__u32 load_flags;		/* 0x044 DRM_SGXFIVE_TRANSFER_LOAD_* */
	__u32 csc0_coeff[5];		/* 0x048 TPU_YUV00..04 */
	__u32 csc1_coeff[5];		/* 0x05c TPU_YUV10..14 */
	__u32 csc_scale;		/* 0x070 TPU_YUVSCALE */
	__u32 lumakey_coeff[3];		/* 0x074 TPU_LUMA0..2 */
	__u32 fir_htable;		/* 0x080 USE_FILTER_TABLE */
	__u32 fir_hfilter[9];		/* 0x084 USE_FILTER0/1/2 left/right/extra */
};

struct drm_sgxfive_job {			/* 56 bytes */
	__u32 type;			/* enum drm_sgxfive_job_type */
	__u32 context_handle;
	__u32 flags;			/* job-type specific */
	__u32 cmd_stream_len;		/* bytes, <= one page; 0 for a dummy transfer */
	__u64 cmd_stream;		/* user pointer, 8-byte aligned; 0 for a dummy */
	struct drm_sgxfive_obj_array sync_ops;	/* of struct drm_sgxfive_sync_op */
	struct drm_sgxfive_obj_array updates;	/* reserved: all zero while max_updates = 0 */
};

struct drm_sgxfive_submit_jobs_args {
	/* in: jobs; on error, count is set to the index of the failing job */
	struct drm_sgxfive_obj_array jobs;	/* of struct drm_sgxfive_job */
};
```

Notes on the header, and where it departs from draft 1 (which used the name `pvrsgx`):

- `drm_sgxfive_transfer_cmd` is `0xa8` bytes: the 41 `sHWRegs` words (`xfer` §2.3, `0x000` to
  `0x0a3`) in their firmware order with the load flags folded in at `0x044`. Draft 1 ended it
  with a `_pad` word, which made it `0xac` bytes, not a multiple of 8; the 42 words are already
  8-byte aligned, so the pad is gone. The kernel builds `SGXMKIF_TRANSFERCMD` from it; if a
  later firmware build changes the MKIF layout, only the kernel's builder changes.
- **`updates` extension point** (§9.1a decision 4): `drm_sgxfive_job.updates`, an array of
  `drm_sgxfive_mem_update`, must be all zero today (`-EINVAL` otherwise), and
  `JOB_INFO.max_updates` = 0 says so. A kernel that accepts a validated list raises
  `max_updates`; the job layout does not change.
- **Heaps** (§9.1a decision 5): the kernel reports the `REV1` layout through `HEAP_INFO` on our
  silicon. The reserved alias is two heaps, `KERNEL_CODE` and `KERNEL_DATA`, because `REV1`
  puts `PDS_VERTEX` between them (§3.5); draft 1 had one `KERNEL` heap.
- A dummy transfer carries no command stream (`cmd_stream` and `cmd_stream_len` zero), so IGT
  can submit one without building registers.
- The licence is the `pvr_drm.h` dual licence (GPL-2.0 WITH Linux-syscall-note, or MIT).
- No ioctl takes or returns a physical address, a firmware address or a kernel-alias
  address; `HEAP_INFO` reports the alias ranges only so userspace avoids them.
- The render ioctl numbers `0x0a`–`0x0f` are reserved for parameter buffers and RT data sets
  (`pvr`'s free lists and HWRT datasets); their design waits for P9.
