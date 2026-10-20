/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SGX545 microkernel bring-up for Cedarview (GMA3600/3650).
 */
#ifndef _GMA500_SGX_BOOT_H_
#define _GMA500_SGX_BOOT_H_

struct drm_device;
struct drm_minor;

void gma_sgx_debugfs_init(struct drm_minor *minor);
void gma_sgx_fini(struct drm_device *dev);

#endif
