/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sgxfive transfer contexts (sgxfive_ctx.c).
 */
#ifndef _SGXFIVE_CTX_H_
#define _SGXFIVE_CTX_H_

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <drm/drm_mm.h>

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
};

static inline void *sgxfive_ctx_cpu(struct sgxfive_ctx *ctx, u32 off)
{
	return (u8 *)ctx->mem.va + off;
}

bool sgxfive_seqno_passed(u32 done, u32 seqno);
struct dma_fence *sgxfive_fence_create(struct sgxfive_ctx *ctx, u32 seqno);
void sgxfive_fence_fail_all(struct sgxfive_ctx *ctx, int error);

struct sgxfive_ctx *sgxfive_ctx_get(struct sgxfive_file *f, u32 handle);
void sgxfive_ctx_put(struct sgxfive_ctx *ctx);
void sgxfive_ctx_close_all(struct sgxfive_file *f);
int sgxfive_ioctl_create_ctx(struct sgxfive *sgx, void *data,
			     struct drm_file *file);
int sgxfive_ioctl_destroy_ctx(struct sgxfive *sgx, void *data,
			      struct drm_file *file);

#endif
