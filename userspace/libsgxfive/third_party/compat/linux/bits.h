/* SPDX-License-Identifier: MIT */
/* Non-Linux build shim: drm_mode.h needs only __GENMASK from <linux/bits.h>. */
#ifndef SGXFIVE_COMPAT_LINUX_BITS_H
#define SGXFIVE_COMPAT_LINUX_BITS_H
#define __GENMASK(h, l) (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (8 * sizeof(long) - 1 - (h))))
#endif
