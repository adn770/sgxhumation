# SGX545 host boot sequence — spec for the gma500 port

This is the host side of the first firmware boot on our Cedarview laptop, from "SGX island
powered" to "`HostCtl.ui32InitStatus` bit 0 = 1". It lists every register access and every
control-block write in order, with each `#if` resolved for this build. It is the spec for
`sgx_boot.c` in gma500 (plan milestone M3). The firmware side of the same boot is in
`sgx545_hw_spec.md` §13.7.

Conventions:

- **Sources.** `CDV` = Intel's GPL Cedarview kernel,
  `reference/preserved/cedarview-gfx-blob/extracted/cdv-gfx-drivers-1.0.3_bee/src/`
  `cedarview-kernel-v1.0.3_bee.patch`, paths under `drivers/staging/cdv/pvr/services4/`.
  Device files are cited as `sgxreset.c:SGXReset` (in `srvkm/devices/sgx/`), system files as
  `system/unified/sysconfig.c:…`. `gma500` = mainline v7.3 `drivers/gpu/drm/gma500/`.
- **The other DDK trees agree.** `sgxreset.c`, `sgxinit.c` and `sgxpower.c` in
  `reference/linux_openpvrsgx/.../pvrsgx/1.7.862890` are identical to CDV after stripping
  trailing whitespace. `reference/sgx545-ce` differs only in the CE5310 revision-exception pair
  of `SGXDevInitCompatCheck`, its system layer (`system/ce5300/`) and its `SGX_CORE_REV`
  (1013, so it has `FIX_HW_BRN_31939`).
- **Ops.** `W` register write, `R` register read, `P` poll, `M` write to shared memory,
  `D` delay. Register offsets are from the SGX base (BAR0 + `0x80000`).
- Inferences are marked **[infer]**. Everything else is read from the source.

## 0. Configuration, resolved

Compile flags come from the CDV top-level `Makefile`: `-DSGX545 -DSUPPORT_SGX545
-DSGX_CORE_REV=10131`, `SUPPORT_HW_RECOVERY`, `PVR_SECURE_HANDLES`, `SUPPORT_SGX_HWPERF`,
`SUPPORT_PERCONTEXT_PB`, `SYS_USING_INTERRUPTS`, `DISABLE_PM`, `SUPPORT_SGX_NEW_STATUS_VALS`.
There is no `PDUMP`, `EMULATOR`, `NO_HARDWARE`, `SUPPORT_ACTIVE_POWER_MANAGEMENT` or
`SGX_DYNAMIC_TIMING_INFO`. This matches build options `0x60218` (`sgx_options.h`: bits 3, 4,
9, 17, 18; bit 15, APM, is clear).

Device features and errata (`hwdefs/sgxfeaturedefs.h`, `hwdefs/sgxerrata.h`,
`hwdefs/sgx545defs.h`, `hwdefs/sgxmmu.h`, `include/sgx_mkif_km.h`):

| item | value for this build |
|------|----------------------|
| errata (rev 10131) | only `FIX_HW_BRN_SAMPLE_CACHE` |
| `SGX_FEATURE_MULTIPLE_MEM_CONTEXTS` | on, with 16 directory lists |
| `SGX_BIF_DIR_LIST_INDEX_EDM` | **15** (`SGX_FEATURE_BIF_NUM_DIRLISTS - 1`) |
| `SGX_FEATURE_MULTI_EVENT_KICK` | on |
| `_MP`, `_36BIT_MMU`, `_SYSTEM_CACHE` | off |
| `_2D_HARDWARE`, `_DATA_BREAKPOINTS` | off |
| `EUR_CR_POWER` | **not defined** on 545 |
| `EUR_CR_BIF_CTRL_INVAL`, `EUR_CR_BIF_MEM_ARB_CONFIG` | not defined |
| `SOFT_RESET` bits | BIF 0, DPM 1, TA 2, USE 3, ISP 4, TSP 5, nothing else |
| BRN 23944, 23410, 28011, 28889, 31939, 23281 | off |
| `SGX_MMU_PDE_ADDR_ALIGNSHIFT` | 0: the PD address is written as-is |

System constants (`system/unified/sysinfo.h`, `system/unified/sysconfig.c`):

| item | value |
|------|-------|
| `MAX_HW_TIME_US`, `WAIT_TRY_COUNT` | 500000, 10000, so polls are 500 ms in 50 µs steps |
| `SYS_SGX_CLOCK_SPEED` | 400000000 (a constant) |
| `SYS_SGX_PDS_TIMER_FREQ` (uKernel tick) | 1000 Hz |
| `SYS_SGX_HWRECOVERY_TIMEOUT_FREQ` | 100 Hz |
| `bEnableActivePM` | false (`SysInitialise`) |

`SGXResetSleep` is `SGXWaitClocks(100)` (`sgxreset.c:SGXResetSleep`). That is
`OSWaitus(1 + 100 * 1000000 / 400000000)`, which is **1 µs** in integer arithmetic
(`sgxutils.c:SGXWaitClocks`). It is written `D` below.

## (a) Ordered register-level sequence

### How the DDK gets there

1. `DevInitSGXPart1` creates the kernel memory context (`BM_CreateContext`), whose page
   directory address becomes `sKernelPDDevPAddr`. It then calls `MMU_BIFResetPDAlloc`
   (`mmu.c`), which takes 3 contiguous pages: the **BIF-reset PD** (zeroed), the BIF-reset
   PT (zeroed) and a dummy page filled with `0xDB`.
2. `SGXGetInfoForSrvinitKM` hands `sKernelPDDevPAddr` to srvinit, which stores it in TA3DCtl
   dword 23 (`sa23`, spec §13.9). CPU and device physical addresses are identical on CDV
   (`sysconfig.c:SysCpuPAddrToDevPAddr`).
3. srvinit loads the ukernel, PDS programs and control blocks (HostCtl comes back all zero in
   the capture), then calls `DevInitSGXPart2KM`.
4. `DevInitSGXPart2KM` (`sgxinit.c`) sets `HostCtl.ui32PowerStatus |= 0x20` (`POWMAN_NO_WORK`)
   and zeroes the kernel CCB (`0x2000` bytes), the CCB control (8 bytes) and the event-kicker
   word (4 bytes). It registers the SGX power device with default state **OFF**.
5. So `PVRSRVFinaliseSystem` (state `DEFAULT` = OFF) changes nothing. The first CCB command
   powers the device on: the `GETMISCINFO` from `SGXDevInitCompatCheck` goes through
   `sgxutils.c:SGXScheduleCCBCommandKM`, which calls `PVRSRVSetDevicePowerStateKM(ON)`. That
   runs `SysDevicePostPowerState` (a no-op for SGX on CDV: "we don't power off SGX on
   Cedartrail"), then `sgxpower.c:SGXPostPowerState`, which is phase B below.

### Phase B — `SGXPostPowerState(ON, from OFF)`

Source: `sgxpower.c:SGXPostPowerState`, which calls `SGXUpdateTimingInfo` (B2–B5),
`SGXInitialise` (B6) and `SGXStartTimer` (B7).

| # | op | target | value |
|---|----|--------|-------|
| B1 | M | HostCtl `+0x04` `ui32PowerStatus` | `0` |
| B2 | — | host timer `SGXOSTimer`, created disabled | period `1000*50/1000` = **50 ms** |
| B3 | M | HostCtl `+0x14` `ui32HWRecoverySampleRate` | `1000/100` = **10** |
| B4 | M | HostCtl `+0x18` `ui32uKernelTimerClock` | `400000000/1000` = **400000** (`0x61a80`) |
| B5 | M | HostCtl `+0x1c` `ui32ActivePowManSampleRate` | **0** (APM off) |
| B6 | — | `SGXInitialise(psDevInfo, bHardwareRecovery = FALSE)` | phase C |
| B7 | — | enable the 50 ms timer | — |

### Phase C — `SGXInitialise`, registers and memory in order

Source: `sgxinit.c:SGXInitialise`; C1–C2 are `sgxreset.c:SGXInitClocks`, C3–C4 are
`sgxinit.c:SGXRunScript` on script part 1.

| # | op | reg | name | value |
|---|----|-----|------|-------|
| C1 | W | `0x000` | `CLKGATECTL` | `0x0000a6aa` (init info `ui32ClkGateCtl`) |
| C2 | W | `0x004` | `CLKGATECTL2` | `0x00aaaaaa` (init info `ui32ClkGateCtl2`) |
| C3 | W | `0xa58` | `EVENT_PDS_ENABLE` | `0` |
| C4 | W | `0xacc` | `EVENT_TIMER` | `0` |
| C5 | — | | `ui32NumResets++` | then `SGXReset(bHWR = bFirstTime \|\| FALSE)` |

`bFirstTime` is a function-static that starts true, so **the first boot takes the
hardware-recovery branch of `SGXReset`** (rows R8–R15). Later power-ons skip it.

`SGXReset`, non-MP variant. Source: `sgxreset.c`; R1 and R10 are `SGXResetSoftReset`,
R3–R7 `SGXResetInitBIFContexts`, R12–R14 and R18–R20 `SGXResetInvalDC`, R16–R17
`SGXResetSetupBIFContexts`, the delays `SGXResetSleep`.

| # | op | reg | name | value |
|---|----|-----|------|-------|
| R1 | W | `0x080` | `SOFT_RESET` | `0x3f` (all six blocks, BIF included) |
| R2 | D | | | 1 µs |
| R3 | W | `0xc00` | `BIF_CTRL` | `0` |
| R4 | W | `0xc74` | `BIF_BANK_SET` | `0` |
| R5 | W | `0xc78` | `BIF_BANK0` | `0` |
| R6 | W | `0xc84` | `BIF_DIR_LIST_BASE0` | `0` |
| R7 | W | `0xc38`…`0xc70` | `BIF_DIR_LIST_BASE1`…`15` | `0`, 15 writes, ascending |
| R8 | W | `0xc84` | `BIF_DIR_LIST_BASE0` | BIF-reset PD physical address |
| R9 | D | | | 1 µs |
| R10 | W | `0x080` | `SOFT_RESET` | `0x3e` (release BIF only) |
| R11 | D | | | 1 µs |
| R12 | W | `0xc00` | `BIF_CTRL` | `0x8` (`INVALDC`) |
| R13 | W | `0xc00` | `BIF_CTRL` | `0` |
| R14 | D | | | 1 µs; no `BIF_MEM_REQ_STAT` poll with multiple contexts |
| R15 | R | `0xc04` | `BIF_INT_STAT` | `& 0xffff` (`FAULT_REQ`): 0 → R16, else loop F |
| R16 | W | `0xc78` | `BIF_BANK0` | **`0xf`** (`INDEX_EDM` = 15; TA, host, 3D, 2D = 0) |
| R17 | W | `0xc70` | `BIF_DIR_LIST_BASE15` | **kernel PD physical address** (`>> 0`) |
| R18 | W | `0xc00` | `BIF_CTRL` | `0x8` |
| R19 | W | `0xc00` | `BIF_CTRL` | `0` |
| R20 | D | | | 1 µs |
| R21 | W | `0x080` | `SOFT_RESET` | `0` |
| R22 | D | | | 1 µs |

On a power-on that is not the first (and not HWR), R8–R15 become: `W SOFT_RESET = 0x3e`,
`D 1 µs`. No `BIF_DIR_LIST_BASE0` write happens then, so it stays 0 from R6.

Fault loop F, run only while R15 sees a fault. Each pass maps the faulting page to the dummy
page, resets, invalidates, and unmaps it again:

| # | op | target | value |
|---|----|--------|-------|
| F1 | R | `0xc08` `BIF_FAULT` | `addr`; `pd = addr >> 22`, `pt = (addr & 0x3ff000) >> 12` |
| F2 | W | `0x080` `SOFT_RESET` | `0x3f` |
| F3 | M | reset PD `[pd]`; reset PT `[pt]` | `PT_phys \| 1` (4 KB, valid); `dummy_phys \| 1` |
| F4 | R, W | `0x12c` `EVENT_STATUS` → `0x134` `EVENT_HOST_CLEAR` | the value read |
| F5 | R, W | `0x118` `EVENT_STATUS2` → `0x114` `EVENT_HOST_CLEAR2` | the value read |
| F6 | D | | 1 µs |
| F7 | W, D | `0x080` `SOFT_RESET` | `0x3e`, then 1 µs |
| F8 | W, W, D | `0xc00` `BIF_CTRL` | `0x8`, `0`, then 1 µs |
| F9 | M | reset PD `[pd]`; reset PT `[pt]` | `0`, `0`; back to R15 |

After `SGXReset` returns, `SGXInitialise` continues:

| # | op | target | value |
|---|----|--------|-------|
| C6 | — | `EUR_CR_POWER` | **no write**: the register does not exist on 545 |
| C7 | M | event-kicker word | `0` |
| C8 | W×38 | script part 2 | spec §13.5, in that order, values relocated by the `.fw` loader |
| C9 | M | HostCtl `+0x34` `ui32HostClock` | `OSClockus()` = `jiffies * (1000000 / HZ)`, u32 |
| C10 | M | HostCtl `+0x00` `ui32InitStatus` | `0` |
| C11 | — | `OSWriteMemoryBarrier()` | `wmb` |
| C12 | W | `0xae8` `EVENT_KICK2` | **`0x1`** (`NOW`); the kicker word is **not** bumped |
| C13 | — | `OSMemoryBarrier()` | `mb` |
| C14 | P | HostCtl `+0x00` | `(v & 1) == 1`; timeout **500 ms**, busy-wait every 50 µs |
| C15 | — | CCB control | assert `ReadOffset == WriteOffset` (both 0) |

C12 is the `SGX_FEATURE_MULTI_EVENT_KICK` branch; C14 is `PollForValueKM`.

On timeout `SGXInitialise` calls `SGXDumpDebugInfo(psDevInfo, IMG_FALSE)` and returns
`PVRSRV_ERROR_RETRY` (`FIX_HW_BRN_23281` is off, so the dump happens and there is no retry
loop). With `bDumpSGXRegs = FALSE` the dump only covers memory:
`HostCtl.ui32AssertFail` (logged, then cleared to 0), all 17 HostCtl dwords and all TA3DCtl
dwords. The register half of the dump (enabled by HWR) reads `CORE_ID`, `CORE_REVISION`,
`EVENT_STATUS`, `EVENT_STATUS2`, `BIF_CTRL`, `BIF_BANK0`, `BIF_INT_STAT`, `BIF_FAULT`,
`BIF_MEM_REQ_STAT` and `CLKGATECTL`. Our M3 timeout dump should read both halves.

### What the sequence leaves behind

- `BIF_BANK0 = 0xf`: the EDM requestor uses directory list **15**, the others use list 0.
- `BIF_DIR_LIST_BASE15` = kernel PD, which must be the same PD as TA3DCtl `sa23`.
- `BIF_DIR_LIST_BASE0` = the BIF-reset PD (all zero) after the first boot, 0 after a later
  power-on. Lists 1–14 are 0. Per-context PDs are set up later, not during boot
  **[infer]**: the ukernel has no statically resolvable `DIR_LIST_BASE` write, but it does
  write `BIF_BANK0`/`BANK_SET` and makes register-indirect stores (spec §13.6).
- `BIF_CTRL = 0`: no MMU bypass, no pause.
- The DCU, USE code-base, PDS and event registers are as script part 2 left them, and the
  event→PDS dispatch is armed (`EVENT_PDS_ENABLE = 0x2204b00f`).

### Teardown, for reference (`sgxpower.c:SGXPrePowerState`, new state OFF)

Disable the timer; submit `SGXMKIF_CMD_POWER` with `ui32Data[1] = 1` (POWEROFF); poll
`HostCtl.ui32PowerStatus` for bit 3 (`POWEROFF_COMPLETE`), 500 ms. Poll `CLKGATESTATUS`
(`0x008`) `& 0xffffffdf == 0`, 500 ms. Then run the deinit script (9 writes, spec §13.5).
IDLE is the same with command 2 and status bit 2, without the deinit script.

## (b) HostCtl and the other control blocks

`SGXMKIF_HOST_CTL` (`include/sgx_mkif_km.h`), 17 dwords, `0x44` bytes. srvinit allocates
it zeroed. The last column lists every host write in CDV; all other writes are the EDM's.

| dw | off | field | host writes |
|----|-----|-------|-------------|
| 0 | `0x00` | `ui32InitStatus` | `0` just before the kick (C10); polled for bit 0 |
| 1 | `0x04` | `ui32PowerStatus` | `\|= 0x20` in Part2KM; `0` at power-on (B1) |
| 2 | `0x08` | `ui32CleanupStatus` | `&= ~1` after each cleanup command |
| 3 | `0x0c` | `ui32uKernelDetectedLockups` | never (read by misc info) |
| 4 | `0x10` | `ui32HostDetectedLockups` | `++` on a lockup seen by `SGXOSTimer` |
| 5 | `0x14` | `ui32HWRecoverySampleRate` | **10** (B3) |
| 6 | `0x18` | `ui32uKernelTimerClock` | **400000** (B4) |
| 7 | `0x1c` | `ui32ActivePowManSampleRate` | **0** (B5) |
| 8 | `0x20` | `ui32InterruptFlags` | never; the EDM sets `HWR` = 1, `ACTIVE_POWER` = 2 |
| 9 | `0x24` | `ui32InterruptClearFlags` | `\|= 1` in HWR; `\|= 2` / `&= ~2` for active power |
| 10 | `0x28` | `ui32BPSetClearSignal` | `0` (breakpoint misc-info path only) |
| 11 | `0x2c` | `ui32NumActivePowerEvents` | `++` after an active power-down |
| 12 | `0x30` | `ui32TimeWraps` | never |
| 13 | `0x34` | `ui32HostClock` | `OSClockus()` before the kick of every boot (C9) |
| 14 | `0x38` | `ui32AssertFail` | `0` after logging it in the debug dump |
| 15 | `0x3c` | `ui32PerfGroup` | set by the `SET_HWPERF_STATUS` misc-info request |
| 16 | `0x40` | `ui32OpenCLDelayCount` | decremented or zeroed by `SGXOSTimer` |

Sources: `sgxinit.c`, `sgxpower.c`, `sgxutils.c` (`SGXCleanupRequest`,
`SGXTestActivePowerEvent`). The EDM programs `EVENT_TIMER` from dword 6 with bit 24 set
(spec §13.7).

Before the first kick the host has written exactly: `PowerStatus` (`0x20`, then 0),
`HWRecoverySampleRate` = 10, `uKernelTimerClock` = 400000, `ActivePowManSampleRate` = 0,
`HostClock` and `InitStatus` = 0. All other dwords are 0.

The other control blocks at the kick:

| block | state at the kick |
|-------|-------------------|
| kernel CCB (`0x2000`) | all zero (Part2KM) |
| CCB control (8 B) | write offset 0 (host), read offset 0 (EDM) |
| event kicker (4 B) | 0 (zeroed in Part2KM and at C7); `EVENT_KICKER` holds its address |
| TA3DCtl (`0xec`) | srvinit's content; `sa23` = the kernel PD (= `DIR_LIST_BASE15`) |
| Misc, HWPerf CB, TA/3D signature | srvinit's content (zero) |

### Where the clock comes from

Nothing reads the SGX clock from the hardware. `SysInitialise` stores the constant
`SYS_SGX_CLOCK_SPEED = 400000000` in `gsSGXDeviceMap.sTimingInfo`. `SGXUpdateTimingInfo`
reads it from there, because `SGX_DYNAMIC_TIMING_INFO` is off. DDK 1.14's
`system/cedarview_linux/sysinfo.h` uses the same 400 MHz. The value sets only two things:
`ui32uKernelTimerClock`, the EDM tick in core clocks (1 ms at 400 MHz), and the
`SGXWaitClocks` delays (1 µs). If the N2600's SGX clock is not exactly 400 MHz, only the
tick period changes **[infer]**.

The first command after boot carries `ui32CacheControl = 0x2` (`INVAL_BIF_PD`): it comes from
the init info's `ui32CacheControl` (`sgxinit.c:InitDevInfo`), and `SGXScheduleCCBCommand`
copies it into the next command and clears it. This matters for M4.

## (c) Interrupt protocol

**Line.** The SGX `MASTER_INTERRUPT` reaches the CPU as bit 18 (`_PSB_IRQ_SGX_FLAG`) of the
display block's `PSB_INT_IDENTITY_R` (`0x20a4`). It is enabled in `PSB_INT_ENABLE_R` and
unmasked in `PSB_INT_MASK_R`. Intel's handler (`drv/psb_irq.c:psb_irq_handler`) calls
`SYSPVRServiceSGXInterrupt` → `SGX_ISRHandler`, schedules the MISR if that returns true, and
acks `PSB_INT_IDENTITY_R` afterwards. Which SGX events assert the line is set by
`EVENT_HOST_ENABLE`. Script part 2 sets it to `0x4000`, so only **`SW_EVENT`** (bit 14)
interrupts the host. `EVENT_HOST_ENABLE2` = 0.

**LISR — `sgxinit.c:SGX_ISRHandler`:**

1. `R 0x12c EVENT_STATUS`, `R 0x130 EVENT_HOST_ENABLE`; `status &= enable`.
2. If `status & 0x4000` (`SW_EVENT`): `W 0x134 EVENT_HOST_CLEAR = 0x80004000`
   (`SW_EVENT | MASTER_INTERRUPT`), then `W 0x114 EVENT_HOST_CLEAR2 = 0`. Return handled.
3. Otherwise write nothing and return not handled. `EVENT_STATUS2` is not read
   (`SGX_FEATURE_DATA_BREAKPOINTS` is off).

The host never clears any other event bit. The EDM acknowledges its own events: it writes
`EVENT_HOST_CLEAR` at 34 sites and `EVENT_HOST_CLEAR2` at 4 (`sgx545_ukernel.r2`).

**MISR — `SGX_MISRHandler`, then `common/pvrsrv.c:PVRSRVMISR`:**

1. If `InterruptFlags & 1` (HWR) and not `InterruptClearFlags & 1`: `HWRecoveryResetSGX`.
   It sets `InterruptClearFlags |= 1`, dumps the debug info (registers included), runs
   `SGXInitialise(TRUE)` (the full phase C again, with the HWR reset branch), then
   `SGXScheduleProcessQueuesKM` and `PVRSRVProcessQueues`.
2. If `bReProcessDeviceCommandComplete` (a command was deferred from LISR context):
   `SGXScheduleProcessQueuesKM`. It sends `PROCESS_QUEUES` unless `PowerStatus` has
   `NO_WORK`.
3. `SGXTestActivePowerEvent`: if `InterruptFlags & 2` and not `InterruptClearFlags & 2`, it
   sets `InterruptClearFlags |= 2` and powers the device OFF. On `RETRY` it clears the bit
   again. This cannot fire here: with `ActivePowManSampleRate = 0` the EDM never requests it
   **[infer]**.
4. `PVRSRVProcessQueues`, then signal the global event object (waiters re-check their sync
   objects).

The host only ever sets bits in `InterruptClearFlags`. Clearing `InterruptFlags` and
`InterruptClearFlags` after the handshake is the EDM's job **[infer]**: no host code clears
them.

**Host watchdog — `SGXOSTimer`, every 50 ms while powered.** It reads
`USE0_SERV_EVENT` (`0xb14`) XOR `USE1_SERV_EVENT` (`0xb20`), the EDM task registers from the
init info. Three equal readings in a row, with no reset in between, count as a lockup:
`HostDetectedLockups++`, then `HWRecoveryResetSGX`. A live EDM must change these counters
at least every 150 ms; the 1 ms EDM timer tick should do that **[infer]**. This also makes
a cheap liveness check right after M3.

**Mainline gma500 today (`psb_irq.c`).** `gma_irq_handler` reads `EVENT_STATUS` and
`EVENT_STATUS2` **unmasked** whenever bit 18 is set, and `gma_sgx_interrupt` then:

- reads `0xe04` if `EVENT_STATUS` bit 27 is set (SGX535 `2D_BLIT_STATUS`; bit 27 and `0xe04`
  are undefined on 545);
- if `EVENT_STATUS2` bit 4 is set (535 `BIF_REQUESTER_FAULT`, undefined on 545), reads
  `BIF_INT_STAT`/`BIF_FAULT` and logs them, decoded with 535 field names;
- **writes `EVENT_HOST_CLEAR = EVENT_STATUS` and `EVENT_HOST_CLEAR2 = EVENT_STATUS2`**, so
  every pending event bit is cleared, then reads `EVENT_HOST_CLEAR2` back.

The last step races the EDM: it can clear TA, 3D, DPM, timer or cache-invalidate events the
EDM has not consumed yet, and the EDM also polls some of them (spec §13.10, `INVAL_DATA`).
`gma_irq_preinstall` writes `EVENT_HOST_ENABLE = 0`. `gma_irq_postinstall` writes
`EVENT_HOST_ENABLE2 = 0x10` and `EVENT_HOST_ENABLE = 0x08000000`, both undefined bits on
545. Script part 2 overwrites all three. See (e) for the required changes.

## (d) `FIX_HW_BRN_SAMPLE_CACHE`

`system/unified/sysconfig.c:PCIInitDev` sets `need_sample_cache_workaround` when the
`00:00.0` host bridge revision is below 4. Ours is rev 3 (spec §8.1), so it applies. The
compile-time define only adds `SGX_TEXTURE_HEAP_ID = 14` (`include4/sgxapi_km.h`), and
`SGX_MAX_HEAP_ID` stays 15. **The runtime flag changes the device heap layout and nothing
else**: no register, script or HostCtl value depends on it. `sgxinit.c:SGXRegisterDevice`:

- reports 14 heaps instead of 15 when the flag is false;
- uses the `*_REV1` bases from `devices/sgx/sgxconfig.h` (SGX545 block) for every heap;
- adds a **Texture** heap (ID 14) at `0xf0000000`, size `0x0ffff000`, per-context, WC.

| heap | 545 default | 545 `REV1` (our silicon) | harness capture |
|------|-------------|--------------------------|-----------------|
| General | `0x10000000` | `0x3e000000` (to `0xefffefff`) | `0x08000000` |
| 3D parameters (per-context) | `0xd2000000` | `0x10000000` | — |
| TA data | `0xe2000000` | `0x20000000` | `0xd0000000` |
| Sync info | `0xef000000` | `0x2d000000` | `0xe0000000` |
| PDS pixel code/data | `0xf0000000` | `0x2e000000` | `0xe4000000` |
| **Kernel code** | `0xf2000000` | **`0x30000000`** | `0xe8000000` |
| PDS vertex code/data | `0xf2400000` | `0x30400000` | `0xec000000` |
| **Kernel data** | `0xf4000000` | **`0x32000000`** | `0xf0000000` |
| Pixel shader | `0xf9000000` | `0x37000000` | `0xf4000000` |
| Vertex shader | `0xfe000000` | `0x3c000000` | `0xfc000000` |
| Texture | — | `0xf0000000` | — |

On this silicon the workaround moves everything except textures below `0x3e000000` and gives
textures a 256 MB region of their own. That suggests the erratum is about textures sharing
address bits with other data in the sample cache **[infer]**.

Consequences:

- **Nothing needs doing for M3.** The boot touches only the kernel code/data, PDS and shader
  heaps, at whatever addresses the `.fw` loader is given (`layout_moved` proves relocation).
- **Recommendation: use the `REV1` layout** for the device address map. It is what Intel's
  kernel actually ran on rev-3 host bridges. Its boot heaps (`0x2e000000`–`0x3dffffff`) also
  stay clear of gma500's default-PD mappings (see (e)).
- The harness mock reported the generic 32-bit table (`fakedrm.c` `HEAP_BASE`), not Intel's
  SGX545 table. Spec §13.5's remark that this "is also what the real kernel driver reports"
  does not hold for the Cedarview kernel. The boot does not depend on it.
- Once userspace runs (beyond M4), the closed GL driver can ask for heap ID 14, so the host
  must offer the Texture heap at `0xf0000000` **[infer]**.

## (e) gma500 v7.3 interplay and required changes

What gma500 does to the SGX, and when. All writes marked "probe" happen before our debugfs
trigger, so `SGXReset` and script part 2 redo or override them:

| when | gma500 action | effect on 545 | conflict |
|------|---------------|---------------|----------|
| probe | `cdv_init_pm`: APM `GFX` = ON, wait for D0 | powers the island | needed |
| probe | `psb_mmu_driver_init`: `BIF_CTRL` `CLEAR_FAULT` pulse | redone by reset | no |
| probe | `psb_do_init`: `BANK0 = BANK1 = 0`; `BIF_CTRL &= ~0x1ff00` | redone by reset | no |
| probe | `psb_spank`: `SOFT_RESET = 0x7f`, 1 ms, `0` | bits 0–5 + a spare bit | no |
| probe | `psb_spank`, `psb_do_init`: `W 0xc88 = gatt_start` | 535 `TWOD_REQ_BASE`, undefined | no |
| probe | `psb_mmu_set_pd_context(default, 0)`: `DIR_LIST_BASE0` | zeroed by R6 | item 2 |
| probe | `psb_mmu_set_pd_context(pf_pd, 1)`: `W 0xc3c` | `DIR_LIST_BASE2`, zeroed by R7 | no |
| probe | `PDS_EXEC_BASE = 0x20000000`, `BIF_3D_REQ_BASE = 0x30000000` | overwritten | no |
| probe, resume | `gma_irq_pre/postinstall`: `HOST_ENABLE`, `_ENABLE2` | override part 2 | **yes** |
| SGX IRQ | `gma_sgx_interrupt`: clears all `EVENT_STATUS`/`STATUS2` bits | eats events | **yes** |
| GEM pin/unpin | `psb_mmu_flush`: read-modify-write of `BIF_CTRL` | races the EDM | **yes** |
| sleep | `gma_power_suspend`: `cdv_power_down` (island D3), PCI D3hot | state lost | **yes** |
| unload | `psb_mmu_free_pagedir`, `psb_mmu_driver_takedown` | PD freed | **yes** |

Files: `cdv_device.c`, `mmu.c`, `psb_drv.c`, `psb_irq.c`, `power.c`. The `BIF_CTRL` race is
**[infer]**: the EDM writes `BIF_CTRL` at 4 sites, and gma500's flush is not atomic with them.
`psb_mmu_free_pagedir` writes `0` to `0xc84 + 4 * hw_context`, that is `0xc84` and `0xc88`.

Runtime PM is not a risk. `gma_power_init` does `pm_runtime_get()` and nothing puts it back
until unload, so the device never runtime-suspends. Runtime PM is also forbidden by default
for PCI. Only system sleep and unload power the island down. Intel's driver never power-gates
the SGX island either (`sysconfig.c:SysDevicePrePowerState`).

gma500's SGX MMU mappings do not sit at `0xe0000000`. `mmu_gatt_start = 0xe0000000` only
feeds `gatt_free_offset`. Stolen memory and pinned GEM objects are mapped into the default
PD at **`gatt_start` + offset** (`psb_drv.c:psb_driver_load`, `gem.c:psb_gem_pin`), where
`gatt_start` is BAR2. When CDV does not report BAR2, `gtt.c:psb_gtt_init` falls back to
`0x40000000` + 128 MB. Our heaps must stay out of that range if they share the default PD.

Required changes, in the order M3 needs them:

1. **SGX interrupt (before the kick).** Give `gma_sgx_interrupt` the `SGX_ISRHandler`
   semantics while the firmware is up: if `status & enable & 0x4000`, write
   `EVENT_HOST_CLEAR = 0x80004000` and `EVENT_HOST_CLEAR2 = 0`, then queue the MISR work.
   Never write the raw status back. Report BIF faults by reading `BIF_INT_STAT` (`0xc04`,
   `FAULT_REQ` = bits 15:0, `FAULT_TYPE` = 18:16) and `BIF_FAULT`, without clearing.
   For the very first boot, masking bit 18 in `PSB_INT_MASK_R` is an acceptable stopgap,
   since the boot handshake is polled.
2. **BIF directory lists.** The EDM translates through **`DIR_LIST_BASE15`**, selected by
   `BANK0 = 0xf`, not `DIR_LIST_BASE0`. The plan's "keep gma500's PD in `DIR_LIST_BASE0`"
   is therefore not what the EDM uses. Write the EDM's PD to `0xc70` (R17), and put the same
   address in TA3DCtl `sa23`. For `DIR_LIST_BASE0` (R8), either implement the 3-page
   BIF-reset PD as the DDK does, or restore gma500's default PD after the reset. Writing the
   kernel PD to `DIR_LIST_BASE0` is the DDK's own `FIX_HW_BRN_28011` variant, so the second
   choice is a configuration the DDK supports.
3. **Which PD.** Prefer a private kernel PD (as `DevInitSGXPart1` does) over gma500's
   default PD. Unload then cannot free it under the EDM, and gma500's GEM mappings are not
   visible to the firmware **[infer]**. If the default PD is used, the heap layout must avoid
   `[gatt_start, gatt_start + aperture)`. PDE format is `PT_phys | 1` and PTE format is
   `page_phys | flags` with `1` valid, `2` write-only, `4` read-only, `8` cache-consistent,
   `0x10` EDM-protect; gma500's PTE bits are the same.
4. **No IRQ reinstall while live.** `gma_irq_preinstall`/`_postinstall` must not run while
   the firmware is up. They only run at probe and resume, and after resume the firmware must
   be booted again anyway.
5. **Suspend and unload.** In `gma_power_suspend` and before `psb_mmu_driver_takedown`, stop
   the firmware: the POWEROFF command and deinit script from (a), or at least the deinit
   script and `SOFT_RESET = 0x3f`. Mark it not booted; the debugfs trigger boots it again.
6. **`BIF_CTRL` ownership.** The DDK writes `BIF_CTRL` only inside `SGXReset`. Later MMU
   invalidations go through the next command's `ui32CacheControl` (`mmu.c` sets
   `INVAL_BIF_PD`/`PT`/`SL` in `psDevInfo->ui32CacheControl`). For M3, avoid gma500 GEM pins
   while the firmware runs: no modesets, no cursor or framebuffer changes. In the longer
   term, route gma500's SGX-MMU flushes through the CCB the same way.
7. **Cache attributes of the shared buffers.** The DDK maps kernel heaps write-combined
   (`PVRSRV_HAP_WRITECOMBINE`) and relies on `wmb()` before the kick (C11). Do the same
   (`set_pages_array_wc`, as `psb_gem_pin` does), or set PTE bit 3 if the pages stay cached.
   Either way, the C14 poll must use `READ_ONCE` on a mapping the SGX writes are visible
   through.
8. **Addresses below 4 GB.** No 36-bit MMU, so every PD, PT and data page must be below
   4 GB. That holds on this 2 GB i686 machine; `GFP_DMA32` is harmless.

## (f) Open questions

1. **Does the EDM raise `SW_EVENT` during boot?** If it does, unpatched gma500 clears all
   pending events while the EDM is starting up. After the first boot, check `EVENT_STATUS`
   and the IRQ count.
2. **Is the `REV1` layout right for our ukernel build?** Relocation is proven, but no
   capture has used the `REV1` bases yet. Run `fwload.py` with them before M3.
3. **Does a private PD versus gma500's default PD change anything** beyond the teardown
   coupling? The `BIF_CTRL` race in (e) item 6 exists either way.
4. **Who clears `InterruptFlags`/`InterruptClearFlags`?** No host code does. Observe it after
   the first HWR.
5. **The actual SGX clock of the N2600.** It is assumed to be 400 MHz. Measure the EDM tick
   against `jiffies` after boot, for example by sampling `USE0_SERV_EVENT` against time.
6. **gma500's `gatt_start` on the laptop.** Is BAR2 reported, or is it the `0x40000000`
   fallback? It matters only if the default PD is shared.
7. **Does the ukernel itself do anything for the sample-cache erratum?** No such code is
   known; heap placement is the only visible part of the workaround.
8. **BIF faults during the first boot (fault loop F).** None are expected with all buffers
   mapped. A fault there means a mapping is missing, so log `BIF_FAULT` in the loop.

Two corrections to `sgx545_hw_spec.md` follow from this document. §7.2 step 4 does not apply
to SGX545: there is no `EUR_CR_POWER` register. §7.2 step 3 is more than a soft reset: it
also sets up the BIF directory lists, with the EDM on list 15.
