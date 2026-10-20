# Third-party headers

- `drm/drm.h`, `drm/drm_mode.h`: Linux v7.3 `include/uapi/drm/`, MIT
  (`SPDX-License-Identifier: MIT` in each file). The `__user` annotations are stripped, as
  `scripts/headers_install.sh` does, so they build outside the kernel tree. Vendored so the
  library and its mock build on any host (including macOS) without libdrm or kernel headers.
- `compat/linux/`: our own two-line shims for non-Linux hosts, only on the include path when
  `uname -s` is not `Linux`.
