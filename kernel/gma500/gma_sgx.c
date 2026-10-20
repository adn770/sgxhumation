// SPDX-License-Identifier: GPL-2.0-only
/*
 * gma500's side of the sgxfive GPU driver: describe the SGX545 of a Cedarview
 * device to it and forward the SGX interrupt. The SGX is created with the
 * DRM minor's debugfs, where its bring-up controls live for now.
 */
#include <drm/drm_file.h>

#include "gma_sgx.h"
#include "mmu.h"
#include "psb_drv.h"
#include "sgxfive/sgxfive.h"

void gma_sgx_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct sgxfive_host host = {
		.drm = dev,
		.regs = dev_priv->sgx_reg,
	};

	if (!IS_CDV(dev) || dev_priv->sgx)
		return;
	host.reset_pd = page_to_phys(psb_mmu_get_default_pd(dev_priv->mmu)->p);
	dev_priv->sgx = sgxfive_create(&host);
	if (dev_priv->sgx)
		sgxfive_debugfs_init(dev_priv->sgx, minor->debugfs_root);
}

void gma_sgx_fini(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	sgxfive_destroy(dev_priv->sgx);
	dev_priv->sgx = NULL;
}

/* Called from gma_sgx_interrupt; true when the event belongs to the GPU. */
bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	return dev_priv->sgx && sgxfive_irq(dev_priv->sgx, stat_1, stat_2);
}
