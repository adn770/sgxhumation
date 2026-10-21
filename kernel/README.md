# Kernel side

The SGX545 bring-up lives in gma500, as a patch series on mainline `v7.3`. The kernel tree
(`$SGX_KERNEL_TREE`, branch `sgx545`) is on the build host (`$SGX_BUILD_HOST`); all kernel
builds happen there. The laptop under test is `$SGX_TARGET_HOST`.

| path | what |
|------|------|
| `patches/` | `git format-patch v7.3..sgx545`, the source of truth, re-exported after each commit |
| `sgx_fw/` | the firmware parser (also in the series) plus its userspace harness, `fwdump` |
| `gma500/` | working copies: `gma_sgx.[ch]` (gma500's side) and `sgxfive/` (the GPU driver; |
| | its `sgx_fw.*` are copied from `sgx_fw/`), edited here and copied into the tree |
| `uapi/` | working copy of `include/uapi/drm/sgxfive_drm.h` |
| `scripts/` | installing and booting test kernels on the laptop, the netconsole listener |

## Applying the series

```sh
git checkout -b sgx545 v7.3
git am /path/to/sgxhumation/kernel/patches/*.patch
```

The driver builds into `gma500_gfx` with `CONFIG_DRM_GMA500`; it selects `DRM_SCHED` and
`DRM_GEM_SHMEM_HELPER`. `CONFIG_DRM_GMA500_KUNIT_TEST` adds the KUnit suites, which run when
the module loads. The driver needs `sgx545_ukernel.fw` in the firmware search path; it is cut
from Intel's driver package by `tools/fwcutter` and is not redistributable.

## Status of the series

The series is a **log of the bring-up, not a submission**. The patches follow the order in
which things were found out: self-test hooks, "check that ..." experiments, and fixes to
earlier patches are kept as they happened. Please do not review it as an RFC. A series for
the list would be reorganised from scratch, and would deal with the points below first:

- The debugfs hooks in `sgxfive.c` (`load`, `boot`, `stop`, `reset`, `idle`, `xfer_*`, `tri*`) are
  bring-up self-tests, some of which drive the GPU. They belong in KUnit or IGT, or behind a
  debug Kconfig option.
- The boot steps log with `drm_info`; they should be `drm_dbg`.
- There is no `MODULE_FIRMWARE()` for the firmware name, and the name has no vendor
  subdirectory (such as `gma500/`) as linux-firmware expects.
- A few lines run past 80 columns, mostly in the uapi kerneldoc.
- The firmware has no redistribution licence, which blocks any upstream submission.

## Licences

- The files under `drivers/gpu/drm/gma500/sgxfive/` (the GPU driver, the firmware loader and
  their KUnit tests) are ours and are `GPL-2.0-only OR MIT`, as the imagination driver is.
  The uapi header `include/uapi/drm/sgxfive_drm.h` is
  `(GPL-2.0-only WITH Linux-syscall-note) OR MIT`, the usual DRM uapi licence.
- `gma_sgx.[ch]`, gma500's side, and the changes to existing gma500 files (`psb_drv.[ch]`,
  `psb_irq.c`, `Kconfig`, `Makefile`) are `GPL-2.0-only`, as gma500 is.
- The SPDX line of every file is what counts. The relicensing is the last patch of the
  series, so the earlier patches still show `GPL-2.0-only` on some sgxfive files.

## Sign-offs and AI assistance

The patches were written with an AI coding assistant and carry `Assisted-by: LLM`, as
`Documentation/process/coding-assistants.rst` asks. They have **no `Signed-off-by:`** on
purpose: under those rules only a human can certify the Developer Certificate of Origin, so
the author adds their sign-off (`git rebase --signoff v7.3`) after reviewing each patch. Until
then the series is not fit to be sent to a mailing list.

## Test cycle

`scripts/push-gma500.sh` builds gma500 on the build host with `LOCALVERSION=` (the installed
kernel is `7.3.0-sgx` and `LOCALVERSION_AUTO` is off, so the module keeps matching it),
installs it on the laptop and boots the test kernel once. Check the device buffers with
`tools/fwcutter/check_dump.py` on a copy of debugfs `dri/0/sgx/`.

The sgxfive render node is opt-in (`design/sgx545_render_uapi.md` §6.3, decision 8): gma500
creates it only with `gma500_gfx.sgx_render=1`, a read-only parameter for trusted users only.
Without it there is no `/dev/dri/renderD*` and card0 answers only DEV_QUERY (`-EPERM` for the
rest), so the render-node tests skip. `scripts/boot-test-kernel.sh` boots the test kernel with
`gma500_gfx.sgx_render=1`, read by the linux-sgx GRUB entries from grubenv (`sgx_cmdline`);
`boot-test-kernel.sh 0`, or `SGX_RENDER=0 scripts/push-gma500.sh`, boots it with the gate off.
`push-gma500.sh` copies both laptop scripts before booting.
