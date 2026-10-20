# IGT tests for sgxfive

[igt-gpu-tools](https://gitlab.freedesktop.org/drm/igt-gpu-tools) tests for the sgxfive render
node (plan phase P6), kept as patches on top of IGT plus a mirror of the files they add, the
way `kernel/` keeps the kernel work. The branch lives in an IGT clone on cachix
(`~/src/igt-gpu-tools`, branch `sgxfive`).

| path | what |
|------|------|
| `patches/` | `git format-patch` of the branch (`lib:` and `tests/sgxfive:` commits) |
| `lib/igt_sgxfive.[ch]` | raw-ioctl helpers; `__igt_*` return `-errno`, the others assert |
| `tests/sgxfive/` | `sgxfive_dev_query`, `_gem`, `_vm`, `_ctx`, `_submit` and their `meson.build` |

The library patch also adds `include/drm-uapi/sgxfive_drm.h` (a copy of `kernel/uapi/`) and
`DRIVER_SGXFIVE`, which matches the DRM driver name `gma500` while sgxfive is hosted there;
`igt_sgxfive_open()` skips on a gma500 device that does not answer DEV_QUERY.

These tests need no GPU programs: queries, BOs, VM rules, contexts, and SUBMIT_JOBS with
dummy transfers and syncobjs. Fill, copy and isolation (P6b) need the assembled programs and
the transfer builders of `libsgxfive`, to be brought into IGT's `lib/` (all MIT, all ours).

## Build and run

IGT is built for i686 in the Arch Linux 32 image (`tools/arch32/`), as the laptop's userland:

```sh
# on cachix, once: meson setup in the container
docker run --rm --user 1000:1000 -e HOME=/tmp -v ~/src/igt-gpu-tools:/igt -w /igt \
	sgxhumation/arch32:igt meson setup build32 -Doverlay=disabled -Dchamelium=disabled \
	-Dvalgrind=disabled -Dman=disabled -Ddocs=disabled -Dtestplan=disabled -Dsphinx=disabled \
	-Dxe_driver=disabled -Dvmtb=disabled -Dlibdrm_drivers= -Drunner=enabled -Doping=disabled
docker run --rm --user 1000:1000 -e HOME=/tmp -v ~/src/igt-gpu-tools:/igt -w /igt \
	sgxhumation/arch32:igt ninja -C build32 tests/sgxfive/sgxfive_submit   # or all of them
```

On the laptop the tests run from a directory holding them and `lib/libigt.so.0`
(`LD_LIBRARY_PATH=. ./sgxfive_submit`); they need `cairo libdrm libpciaccess pixman libunwind`
from pacman.

## Results (2026-10-21, 7.3.0-sgx with lockdep)

All 55 subtests pass, three rounds in a row: `dev_query` 8, `gem` 7, `vm` 12, `ctx` 8,
`submit` 20. Getting there fixed four kernel bugs the mock had hidden: SUBMIT_JOBS was
`_IOW`, so the failing job's index never reached userspace; CREATE_BO did not report the
rounded size; failed GEM lookups returned `-EINVAL` instead of `-ENOENT`; and destroying a
VM left stale BIF state, which made the next process's first jobs fault and stall
(`design/sgx545_render_uapi.md` §3.3).
