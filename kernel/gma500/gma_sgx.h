/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gma500's side of the sgxfive GPU driver (Cedarview only).
 */
#ifndef _GMA500_GMA_SGX_H_
#define _GMA500_GMA_SGX_H_

#include <linux/types.h>

#include <drm/drm_ioctl.h>

struct drm_device;
struct drm_minor;

void gma_sgx_init(struct drm_device *dev);
void gma_sgx_fini(struct drm_device *dev);
void gma_sgx_debugfs_init(struct drm_minor *minor);
bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2);

#define GMA_SGX_NUM_IOCTLS	10	/* DRM_SGXFIVE_DEV_QUERY..SUBMIT_JOBS */
extern const struct drm_ioctl_desc gma_sgx_ioctls[GMA_SGX_NUM_IOCTLS];

struct drm_gem_object *gma_sgx_gem_create_object(struct drm_device *dev,
						 size_t size);

#endif
