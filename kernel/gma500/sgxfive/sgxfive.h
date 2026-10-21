/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sgxfive: the PowerVR SGX (Series 5) GPU driver, today hosted by gma500 for
 * Cedarview's SGX545. This is the whole interface between the two: the host
 * describes the hardware it owns (struct sgxfive_host) and forwards the SGX
 * interrupt; the GPU side touches nothing else of the host. It is shaped so
 * that it can become an auxiliary-bus device later.
 */
#ifndef _SGXFIVE_H_
#define _SGXFIVE_H_

#include <linux/types.h>

struct dentry;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct sgxfive;

struct sgxfive_host {
	struct drm_device *drm;	/* messages and debugfs */
	void __iomem *regs;	/* the SGX register block */
	u32 reset_pd;		/* the host's PD, kept in BIF directory list 0 */
	u32 host_bridge_rev;	/* PCI 00:00.0 revision: selects the quirks */
};

struct sgxfive *sgxfive_create(const struct sgxfive_host *host);
void sgxfive_destroy(struct sgxfive *sgx);
bool sgxfive_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2);
void sgxfive_debugfs_init(struct sgxfive *sgx, struct dentry *root);

/*
 * The render-node interface: the host routes its sgxfive ioctls here. With
 * the render node off (trusted users only), the host lets through only what
 * sgxfive_ioctl_allowed() accepts and returns -EPERM for the rest.
 */
bool sgxfive_ioctl_allowed(bool render, unsigned int nr);
int sgxfive_ioctl(struct sgxfive *sgx, unsigned int nr, void *data,
		  struct drm_file *file);
struct drm_gem_object *sgxfive_gem_create_object(struct drm_device *drm,
						 size_t size);
int sgxfive_open(struct sgxfive *sgx, struct drm_file *file);
void sgxfive_postclose(struct sgxfive *sgx, struct drm_file *file);

#endif
