# IGT tests for sgxfive

[igt-gpu-tools](https://gitlab.freedesktop.org/drm/igt-gpu-tools) tests for the sgxfive render
node (phase P8 of `design/sgx545_render_uapi.md` §8), kept as patches on top of IGT plus a
mirror of the files they add, the way `kernel/` keeps the kernel work. The branch, `sgxfive`,
lives in an IGT clone on the build host (`$SGX_IGT_TREE` below).

| path | what |
|------|------|
| `patches/` | `git format-patch` of the branch (`lib:` and `tests/sgxfive:` commits) |
| `lib/igt_sgxfive.[ch]` | raw-ioctl helpers; `__igt_*` return `-errno`, the others assert |
| `lib/sgxfive/` | a copy of libsgxfive's ioctl layer, transfer and render engines and programs |
| `tests/sgxfive/` | `sgxfive_{dev_query,gate,gem,vm,ctx,submit,transfer,render}.c`, `meson.build` |
| `sync-libsgxfive.sh` | refreshes `lib/sgxfive/`; libsgxfive's `make check` fails if it is stale |

The library patch also adds `include/drm-uapi/sgxfive_drm.h` (a copy of `kernel/uapi/`) and
`DRIVER_SGXFIVE`, which matches the DRM driver name `gma500` while sgxfive is hosted there;
`igt_sgxfive_open()` skips on a gma500 device that does not answer DEV_QUERY, and when there is
no render node.

The render node is opt-in: the laptop must boot with `gma500_gfx.sgx_render=1`
(`kernel/scripts/boot-test-kernel.sh` does), or every test but `sgxfive_gate` skips with "no
sgxfive render node: boot with gma500_gfx.sgx_render=1". `sgxfive_gate` opens the primary
node (run it as root) and checks whichever state the gate is in, from
`/sys/module/gma500_gfx/parameters/sgx_render`: `primary-dev-query` always; with the gate off
`off-no-render-node` and `off-primary-eperm` (all nine other ioctls `-EPERM` on card0); with it
on `on-render-node` and `on-primary-allowed`. The other state's subtests skip.

Most tests need no GPU programs: queries, BOs, VM rules, contexts, and SUBMIT_JOBS with
dummy transfers and syncobjs. `sgxfive_transfer` runs real fills and copies through the
vendored engine (every destination dword checked against a sentinel) and the isolation rules:
separate VMs, faults into another VM's address and into a read-only mapping, the faulting
context lost while others survive, a queued innocent job re-run by the recovery, the per-file
fault budget, and the devcoredump error state a recovered fault leaves (`fault-devcoredump`).
A faulting job's wait returns `-EIO`, and `fault-fence-errors` reads the fence statuses
through sync_files (`igt_sgxfive_fence_status()`): `-EIO` for the faulting fill, `-ECANCELED`
for a fill queued behind it on the same context, signalled for another context's fill
(`design/sgx545_render_uapi.md` R15).
Each isolation subtest opens its own file, since three faults in one file ban it. The tests
adopt IGT's fd with `sgxfive_dev_adopt()`.

`sgxfive_render` draws the first triangle through render contexts with the vendored render
engine (our own assembled programs): context limits and slot reuse, the reserved render heaps,
the `-EINVAL` rules of render commands (tiling words that are not the target's layout
included), the triangle at 21 render-target sizes from 32x16 to the 4096x4096 the kernel
reports (dynamic subtests) with every pixel checked against a CPU reference, targets of
changing sizes on one context, repeated renders, several contexts
and VMs queued together, renders interleaved with transfers and chained to them through
syncobjs, destroying a busy context, a faulting render recovered with its context lost and an
error state (the render fails with `-EIO`, one queued behind it with `-ECANCELED`), a
transfer from another file faulting while an older render waits for the 3D pipe
(only the transfer's context may be lost: blame by the fault's page directory), and
transfers' fixed `PDS_EXEC_BASE`. Render contexts state the largest target they take (patch
0012): `max-size-bounds`, `max-size-exceeded` and the dynamic `max-size-small` check the
bounds, the refusal of larger targets and exact renders on small contexts (patch 0013), and
`sgxfive_ctx@bad-max-size` that a transfer context states no size.

## Build and run

IGT is built for i686 in the Arch Linux 32 image (`tools/arch32/`), as the laptop's userland:

```sh
# on the build host; SGX_IGT_TREE is the absolute path of the IGT clone
# once: meson setup in the container
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$SGX_IGT_TREE":/igt -w /igt \
	sgxhumation/arch32:igt meson setup build32 -Doverlay=disabled -Dchamelium=disabled \
	-Dvalgrind=disabled -Dman=disabled -Ddocs=disabled -Dtestplan=disabled -Dsphinx=disabled \
	-Dxe_driver=disabled -Dvmtb=disabled -Dlibdrm_drivers= -Drunner=enabled -Doping=disabled
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$SGX_IGT_TREE":/igt -w /igt \
	sgxhumation/arch32:igt ninja -C build32 tests/sgxfive/sgxfive_submit   # or all of them
```

On the laptop the tests run from a directory holding them and `lib/libigt.so.0`
(`LD_LIBRARY_PATH=. ./sgxfive_submit`); they need `cairo libdrm libpciaccess pixman libunwind`
from pacman.

## Results (2026-10-22, failed jobs' fence errors)

Kernel series 0001–0054 with IGT patches 0014–0015, as root, with `gma500_gfx.sgx_render=1`,
two rounds: `transfer` passes 14 with `fault-fence-errors` (the faulting fill `-EIO`, the fill
behind it on its context `-ECANCELED`, another context's fill signalled, through sync_files
and through libsgxfive's waits), every faulting transfer's wait returns `-EIO` and the re-run
innocent job's 0; `render` passes 17, 46 with the dynamic ones, `fault-recovered` with the
faulting render's `-EIO` and `-ECANCELED` for the render queued behind it, and
`transfer-fault-blames-transfer` with the transfer's `-EIO` and both renders' success. The
others as before: `dev_query` 8, `gem` 7, `vm` 12, `ctx` 9, `submit` 20, `gate` 3 (2
skipped). Faults are recovered in 101–107 ms. On series 0001–0053 `fault-fence-errors` fails:
the fill behind the faulting one also got `-EIO`. With `gma500_gfx.sgx_render=0` every binary
but `sgxfive_gate` skips all its subtests (exit 77) and `sgxfive_gate` passes 3 and skips 2.

## Results (2026-10-22, render contexts sized for their largest target)

Kernel series 0001–0053 with IGT patches 0012–0013, as root, with `gma500_gfx.sgx_render=1`,
two rounds: all 17 `sgxfive_render` subtests pass, 46 counting the dynamic ones:
`max-size-bounds`, `max-size-exceeded`, `max-size-small` (eight maxima from 32x16 to 1366x768,
each rendering its largest target and smaller ones exactly), the 21 sizes of `triangle-sizes`
up to 4096x4096, `triangle-resize` and `transfer-fault-blames-transfer` included. `ctx` passes
9 with `bad-max-size`, and the others as before: `dev_query` 8, `gem` 7, `vm` 12, `submit` 20,
`transfer` 13, `gate` 3 (2 skipped). With `gma500_gfx.sgx_render=0` every binary but
`sgxfive_gate` skips all its subtests (exit 77) and `sgxfive_gate` passes 3 and skips 2.

## Results (2026-10-21, render targets up to 4096x4096, blame by fault directory)

Kernel series 0001–0052, as root, with `gma500_gfx.sgx_render=1`, two rounds: all 14
`sgxfive_render` subtests pass, 35 counting the 21 dynamic sizes of `triangle-sizes` (32x16 to
4096x4096; 4096x4096 takes 11 s, almost all of it the CPU's sentinel fill and pixel check),
`triangle-resize` and `transfer-fault-blames-transfer` included, and so do all the others:
`dev_query` 8, `gem` 7, `vm` 12, `ctx` 8, `submit` 20, `transfer` 13, `gate` 3 (2 skipped).
`fault-recovered` reads devcoredump, so it needs root. `transfer-fault-blames-transfer` fails
on series 0001–0051 (the waiting render is blamed and its pixels are never written). With
`gma500_gfx.sgx_render=0` every binary but `sgxfive_gate` skips all its subtests (exit 77) and
`sgxfive_gate` passes 3 and skips 2.

## Results (2026-10-21, render jobs)

With `gma500_gfx.sgx_render=1`, two rounds: all twelve `sgxfive_render` subtests pass (six of
them dynamic sizes from 32x16 to 256x128), and so do all the others: `dev_query` 8, `gem` 7,
`vm` 12, `ctx` 8, `submit` 20, `transfer` 13, `gate` 3 (2 skipped). A faulting render is
recovered in about 100 ms. With `gma500_gfx.sgx_render=0` the render tests skip with the
others and `sgxfive_gate` passes its gate-off subtests.

## Results (2026-10-21, the render-node gate)

With `gma500_gfx.sgx_render=1`, two rounds: all 68 subtests above pass, and `sgxfive_gate`
passes 3 and skips its 2 gate-off subtests; `sgxfive-test isolation` passes. With
`gma500_gfx.sgx_render=0`: `sgxfive_gate` passes 3 and skips the 2 gate-on subtests, the other
six binaries skip all their subtests (exit 77) with the message above.

## Results (2026-10-21, 7.3.0-sgx with lockdep)

All 68 subtests pass, three rounds in a row: `dev_query` 8, `gem` 7, `vm` 12, `ctx` 8,
`submit` 20, `transfer` 13 (a fault is recovered in about 60-130 ms). Getting there fixed
five kernel bugs the mock had hidden: SUBMIT_JOBS was `_IOW`, so the failing job's index
never reached userspace; CREATE_BO did not report the rounded size; failed GEM lookups
returned `-EINVAL` instead of `-ENOENT`; destroying a VM left stale BIF state, which made the
next process's first jobs fault and stall; and memory the GPU could still reach was freed
before the BIF flush (`design/sgx545_render_uapi.md` §3.3).
