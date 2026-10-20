/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SGX545 microkernel bring-up for Cedarview (GMA3600/3650).
 */
#ifndef _GMA500_SGX_BOOT_H_
#define _GMA500_SGX_BOOT_H_

#include <linux/types.h>

struct drm_device;
struct drm_minor;

#if defined(CONFIG_DEBUG_FS)
void gma_sgx_debugfs_init(struct drm_minor *minor);
void gma_sgx_fini(struct drm_device *dev);
bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2);
#else
static inline void gma_sgx_fini(struct drm_device *dev) { }
static inline bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2)
{
	return false;
}
#endif

#endif
