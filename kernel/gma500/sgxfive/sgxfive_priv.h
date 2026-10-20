/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sgxfive internals shared by the device code (sgxfive.c) and the render-node
 * interface (sgxfive_drv.c).
 */
#ifndef _SGXFIVE_PRIV_H_
#define _SGXFIVE_PRIV_H_

#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include <drm/drm_mm.h>
#include <drm/gpu_scheduler.h>
#include <drm/sgxfive_drm.h>

#include "sgx_fw.h"

struct drm_device;
struct drm_file;
struct drm_gem_object;
struct firmware;
struct sgx_mmu_pd;

struct sgxfive_buf {
	struct page **pages;
	unsigned int npages;
	bool wc;		/* pages are set write-combined */
	void *va;
	u32 addr;		/* device virtual, once mapped */
	struct sgx_mmu_pd *pd;	/* the page directory it is mapped in */
	struct debugfs_blob_wrapper blob;
};

struct sgxfive {
	struct drm_device *dev;		/* for messages and debugfs */
	void __iomem *regs;		/* the SGX register block */
	u32 reset_pd;			/* the display side's PD, in list 0 */
	struct mutex lock;		/* serialises load and teardown */
	const struct firmware *blob;
	struct sgx_fw fw;
	struct sgx_fw_layout layout;
	struct sgx_mmu_pd *pd;		/* the EDM's page directory */
	struct sgxfive_buf buf[SGX_FW_MAX_BUFFERS];
	struct sgxfive_buf xbuf[10];	/* transfer-queue test area, XB_* */
	u32 xfer_woff;			/* client CCB write offset */
	struct sgx_mmu_pd *cpd;		/* a client page directory */
	bool alias_ro;			/* map the EDM aliases read-only in it */
	bool no_alias;			/* ...or not at all (the Q1 probe) */
	struct sgxfive_buf cbuf[2];
	u32 xfer_runs;
	bool loaded;
	bool live;			/* EDM booted: the IRQ events are ours */

	/* What DEV_QUERY reports. */
	u32 core_id, core_revision;	/* read at probe */
	u32 host_bridge_rev;
	bool sw_valid;			/* the firmware's own values, below */
	u32 core_id_sw, core_revision_sw, ddk_version;
	u32 heap_start;			/* first buffer's offset in each heap */
	u32 cache_control;		/* SGXMKIF_CC_* for the next command */
	atomic_t sw_events;		/* EVENT_STATUS.SW_EVENT interrupts */
	wait_queue_head_t event_wq;	/* woken on each SW_EVENT */
	atomic_t bif_faults;

	/* Kernel memory for render-node contexts (sgxfive_kmem_alloc). */
	struct drm_mm kmem;		/* offsets in the kernel data heap */
	unsigned int kmem_users;	/* live allocations: no reload */

	/* Render-node contexts, walked by the SW_EVENT interrupt. */
	spinlock_t ctx_lock;
	struct list_head ctx_list;
	struct drm_gpu_scheduler sched;	/* render-node jobs */
	u64 kicks;			/* jobs given to the firmware, under lock */
	u64 hang_oldest;		/* oldest job at the last timeout */
};

struct sgxfive_vm;

/* Per-open-file state of the render node. */
struct sgxfive_file {
	struct xarray vms;		/* handle -> struct sgxfive_vm */
	struct xarray ctxs;		/* handle -> struct sgxfive_ctx */
};

struct sgxfive_vm *sgxfive_vm_get(struct sgxfive_file *f, u32 handle);
void sgxfive_vm_put(struct sgxfive_vm *vm);
u32 sgxfive_vm_pd_phys(const struct sgxfive_vm *vm);

/* The client heap layout (sgxfive_drv.c). */
extern const struct drm_sgxfive_heap sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_COUNT];

bool sgxfive_is_bo(struct drm_gem_object *obj);
void sgxfive_need_bif_inval(struct sgxfive *sgx);
int sgxfive_kmem_alloc(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b, u32 size);
void sgxfive_kmem_free(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b);
int sgxfive_ensure_live(struct sgxfive *sgx);
void sgxfive_fences_irq(struct sgxfive *sgx);
int sgxfive_kick_transfer(struct sgxfive *sgx, u32 hw_ctx_addr);
int sgxfive_recover(struct sgxfive *sgx, bool blame);
int sgxfive_sched_init(struct sgxfive *sgx);
void sgxfive_sched_fini(struct sgxfive *sgx);

u32 sgxfive_job_build(u8 *cmd, const struct drm_sgxfive_transfer_cmd *regs,
		      bool dummy, u32 status_addr, u32 value);
int sgxfive_job_check(const struct drm_sgxfive_transfer_cmd *regs);

int sgxfive_vm_check_range(const struct drm_sgxfive_heap *heaps, unsigned int n,
			   u64 addr, u64 size);
int sgxfive_ioctl_create_vm(struct sgxfive *sgx, void *data,
			    struct drm_file *file);
int sgxfive_ioctl_destroy_vm(struct sgxfive *sgx, void *data,
			     struct drm_file *file);
int sgxfive_ioctl_vm_map(struct sgxfive *sgx, void *data, struct drm_file *file);
int sgxfive_ioctl_vm_unmap(struct sgxfive *sgx, void *data,
			   struct drm_file *file);

#endif
