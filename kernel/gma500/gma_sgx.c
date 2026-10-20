// SPDX-License-Identifier: GPL-2.0-only
/*
 * gma500's side of the sgxfive GPU driver: describe the SGX545 of a Cedarview
 * device to it, forward the SGX interrupt, and route the sgxfive render-node
 * ioctls to it.
 */
#include <linux/pci.h>

#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/sgxfive_drm.h>

#include "gma_sgx.h"
#include "mmu.h"
#include "psb_drv.h"
#include "sgxfive/sgxfive.h"

/* After psb_mmu_driver_init: the reset sequence keeps gma500's PD in list 0. */
void gma_sgx_init(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct sgxfive_host host = {
		.drm = dev,
		.regs = dev_priv->sgx_reg,
	};
	struct pci_dev *mch;

	if (!IS_CDV(dev) || dev_priv->sgx)
		return;
	host.reset_pd = page_to_phys(psb_mmu_get_default_pd(dev_priv->mmu)->p);
	mch = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (mch) {
		host.host_bridge_rev = mch->revision;
		pci_dev_put(mch);
	}
	dev_priv->sgx = sgxfive_create(&host);
}

void gma_sgx_fini(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	sgxfive_destroy(dev_priv->sgx);
	dev_priv->sgx = NULL;
}

void gma_sgx_debugfs_init(struct drm_minor *minor)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(minor->dev);

	if (dev_priv->sgx)
		sgxfive_debugfs_init(dev_priv->sgx, minor->debugfs_root);
}

/* Called from gma_sgx_interrupt; true when the event belongs to the GPU. */
bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	return dev_priv->sgx && sgxfive_irq(dev_priv->sgx, stat_1, stat_2);
}

static int gma_sgx_ioctl(struct drm_device *dev, unsigned int nr, void *data,
			 struct drm_file *file)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	if (!dev_priv->sgx)
		return -ENODEV;
	return sgxfive_ioctl(dev_priv->sgx, nr, data, file);
}

#define GMA_SGX_IOCTL(name)						\
static int gma_sgx_ioctl_##name(struct drm_device *dev, void *data,	\
				struct drm_file *file)			\
{									\
	return gma_sgx_ioctl(dev, DRM_SGXFIVE_##name, data, file);	\
}

GMA_SGX_IOCTL(DEV_QUERY)
GMA_SGX_IOCTL(CREATE_BO)
GMA_SGX_IOCTL(GET_BO_MMAP_OFFSET)
GMA_SGX_IOCTL(CREATE_VM_CONTEXT)
GMA_SGX_IOCTL(DESTROY_VM_CONTEXT)
GMA_SGX_IOCTL(VM_MAP)
GMA_SGX_IOCTL(VM_UNMAP)
GMA_SGX_IOCTL(CREATE_CONTEXT)
GMA_SGX_IOCTL(DESTROY_CONTEXT)
GMA_SGX_IOCTL(SUBMIT_JOBS)

#define GMA_SGX_IOCTL_DEF(name)						\
	DRM_IOCTL_DEF_DRV(SGXFIVE_##name, gma_sgx_ioctl_##name,		\
			  DRM_RENDER_ALLOW)

/* gma500 has no ioctls of its own, so sgxfive's are the driver's. */
const struct drm_ioctl_desc gma_sgx_ioctls[GMA_SGX_NUM_IOCTLS] = {
	GMA_SGX_IOCTL_DEF(DEV_QUERY),
	GMA_SGX_IOCTL_DEF(CREATE_BO),
	GMA_SGX_IOCTL_DEF(GET_BO_MMAP_OFFSET),
	GMA_SGX_IOCTL_DEF(CREATE_VM_CONTEXT),
	GMA_SGX_IOCTL_DEF(DESTROY_VM_CONTEXT),
	GMA_SGX_IOCTL_DEF(VM_MAP),
	GMA_SGX_IOCTL_DEF(VM_UNMAP),
	GMA_SGX_IOCTL_DEF(CREATE_CONTEXT),
	GMA_SGX_IOCTL_DEF(DESTROY_CONTEXT),
	GMA_SGX_IOCTL_DEF(SUBMIT_JOBS),
};

static_assert(DRM_SGXFIVE_SUBMIT_JOBS + 1 == GMA_SGX_NUM_IOCTLS);

/* gma500's own GEM objects do not use this hook; sgxfive's shmem BOs do. */
struct drm_gem_object *gma_sgx_gem_create_object(struct drm_device *dev,
						 size_t size)
{
	return sgxfive_gem_create_object(dev, size);
}
