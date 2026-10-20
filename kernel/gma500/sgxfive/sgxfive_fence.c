// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware fences of sgxfive contexts. Each context numbers its jobs; the
 * firmware writes a finished job's number to the context's fence word before
 * it raises SW_EVENT (design/sgx545_transfer.md §3.1 step 8), so on every
 * SW_EVENT the interrupt handler signals each context's pending fences up to
 * the number in its fence word. Contexts run their jobs in order, so the
 * word only moves forward.
 */
#include <linux/dma-fence.h>
#include <linux/slab.h>

#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

struct sgxfive_fence {
	struct dma_fence base;
	struct list_head link;		/* in the context's pending list */
};

bool sgxfive_seqno_passed(u32 done, u32 seqno)
{
	return (s32)(done - seqno) >= 0;
}

static const char *sgxfive_fence_driver_name(struct dma_fence *f)
{
	return "sgxfive";
}

static const char *sgxfive_fence_timeline_name(struct dma_fence *f)
{
	return "transfer";
}

static const struct dma_fence_ops sgxfive_fence_ops = {
	.get_driver_name = sgxfive_fence_driver_name,
	.get_timeline_name = sgxfive_fence_timeline_name,
};

/* A fence for the context's job number seqno, pending until it completes. */
struct dma_fence *sgxfive_fence_create(struct sgxfive_ctx *ctx, u32 seqno)
{
	struct sgxfive_fence *f = kzalloc_obj(*f);
	unsigned long flags;

	if (!f)
		return ERR_PTR(-ENOMEM);
	dma_fence_init(&f->base, &sgxfive_fence_ops, &ctx->fence_lock,
		       ctx->fence_context, seqno);
	spin_lock_irqsave(&ctx->fence_lock, flags);
	list_add_tail(&f->link, &ctx->pending);
	dma_fence_get(&f->base);	/* the pending list's reference */
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	return &f->base;
}

/* Signal the context's fences the firmware has reached; error fails all. */
static void sgxfive_fence_retire(struct sgxfive_ctx *ctx, int error)
{
	u32 done = READ_ONCE(*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF));
	struct sgxfive_fence *f, *next;
	unsigned long flags;

	spin_lock_irqsave(&ctx->fence_lock, flags);
	list_for_each_entry_safe(f, next, &ctx->pending, link) {
		if (!error && !sgxfive_seqno_passed(done, f->base.seqno))
			break;		/* in order: the rest are later */
		if (error)
			dma_fence_set_error(&f->base, error);
		dma_fence_signal_locked(&f->base);
		list_del(&f->link);
		dma_fence_put(&f->base);
	}
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
}

void sgxfive_fence_fail_all(struct sgxfive_ctx *ctx, int error)
{
	sgxfive_fence_retire(ctx, error);
}

/* From the SW_EVENT interrupt: every live context. */
void sgxfive_fences_irq(struct sgxfive *sgx)
{
	struct sgxfive_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&sgx->ctx_lock, flags);
	list_for_each_entry(ctx, &sgx->ctx_list, link)
		sgxfive_fence_retire(ctx, 0);
	spin_unlock_irqrestore(&sgx->ctx_lock, flags);
}
