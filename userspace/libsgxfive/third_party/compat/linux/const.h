/* SPDX-License-Identifier: MIT */
/* Non-Linux build shim for <linux/const.h>, as far as drm_mode.h needs it. */
#ifndef SGXFIVE_COMPAT_LINUX_CONST_H
#define SGXFIVE_COMPAT_LINUX_CONST_H
#define __KERNEL_DIV_ROUND_CLOSEST(x, d) (((x) + ((d) / 2)) / (d))
#endif
