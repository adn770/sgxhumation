/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sgxfive transfer contexts (sgxfive_ctx.c).
 */
#ifndef _SGXFIVE_CTX_H_
#define _SGXFIVE_CTX_H_

#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include <drm/drm_mm.h>
#include <drm/gpu_scheduler.h>

#include "sgx_xfer.h"
#include "sgxfive_priv.h"

/* A context's kernel memory: the client CCB, then one page of control data. */
#define SGXFIVE_CTX_CCB_OFF	0
#define SGXFIVE_CTX_PAGE_OFF	ALIGN(SGX_XFER_CCB_SIZE + SGX_XFER_CMD_SIZE, PAGE_SIZE)
#define SGXFIVE_CTX_HW_OFF	(SGXFIVE_CTX_PAGE_OFF + 0x000)	/* HW context */
#define SGXFIVE_CTX_CTL_OFF	(SGXFIVE_CTX_PAGE_OFF + 0x100)	/* CCB control */
#define SGXFIVE_CTX_FENCE_OFF	(SGXFIVE_CTX_PAGE_OFF + 0x200)	/* last seqno */
#define SGXFIVE_CTX_MEM_SIZE	(SGXFIVE_CTX_PAGE_OFF + PAGE_SIZE)

struct sgxfive_ctx {
	struct kref ref;
	struct sgxfive *sgx;
	struct sgxfive_vm *vm;		/* holds a reference */
	struct drm_mm_node node;	/* in the kernel data heap */
	struct sgxfive_buf mem;

	/* Hardware fences (sgxfive_fence.c). */
	u64 fence_context;
	spinlock_t fence_lock;		/* the fences and the pending list */
	struct list_head pending;	/* fences not yet signalled, in order */
	struct list_head link;		/* in the device's context list */

	/* Job queue (sgxfive_sched.c). */
	struct drm_sched_entity entity;
	struct mutex submit_lock;	/* job numbers in queueing order */
	u32 woff;			/* client CCB write offset */
	u32 seqno;			/* last job number handed out */
	bool lost;			/* a job of it hung: no more jobs */
	u32 replay;			/* jobs to re-run after a reset */
};

/*
 * A job's hardware fence (sgxfive_fence.c). It keeps the job's firmware
 * command until it signals, so that after a recovery reset the context's
 * client CCB can be rebuilt with the commands the firmware has not finished.
 */
struct sgxfive_fence {
	struct dma_fence base;
	struct list_head link;		/* in the context's pending list */
	u64 order;			/* when the firmware got it, device-wide */
	u32 size;			/* in the client CCB */
	u8 cmd[SGX_XFER_CMD_SIZE];
};

/*
 * One transfer job. Its hardware fence is allocated at submission, with the
 * firmware command built into it, so that running the job allocates nothing.
 */
struct sgxfive_job {
	struct drm_sched_job base;
	struct sgxfive_ctx *ctx;	/* holds a reference */
	struct sgxfive_fence *hw;	/* until the job runs */
	u32 seqno;
	u32 size;			/* in the client CCB */
};

static inline void *sgxfive_ctx_cpu(struct sgxfive_ctx *ctx, u32 off)
{
	return (u8 *)ctx->mem.va + off;
}

bool sgxfive_seqno_passed(u32 done, u32 seqno);
struct sgxfive_fence *sgxfive_fence_alloc(void);
struct dma_fence *sgxfive_fence_init(struct sgxfive_fence *f,
				     struct sgxfive_ctx *ctx, u32 seqno,
				     u64 order, u32 size);
void sgxfive_fence_signal(struct sgxfive_ctx *ctx);
void sgxfive_fence_fail_all(struct sgxfive_ctx *ctx, int error);
bool sgxfive_ctx_oldest(struct sgxfive_ctx *ctx, u64 *order);
u32 sgxfive_ctx_rewind(struct sgxfive_ctx *ctx);

int sgxfive_ctx_entity_init(struct sgxfive_ctx *ctx);
int sgxfive_ioctl_submit(struct sgxfive *sgx, void *data, struct drm_file *file);

struct sgxfive_ctx *sgxfive_ctx_get(struct sgxfive_file *f, u32 handle);
void sgxfive_ctx_put(struct sgxfive_ctx *ctx);
void sgxfive_ctx_close_all(struct sgxfive_file *f);
int sgxfive_ioctl_create_ctx(struct sgxfive *sgx, void *data,
			     struct drm_file *file);
int sgxfive_ioctl_destroy_ctx(struct sgxfive *sgx, void *data,
			      struct drm_file *file);

#endif
