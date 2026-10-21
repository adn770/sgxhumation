// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Hardware fences of sgxfive contexts. Each context numbers its jobs; the
 * firmware writes a finished job's number to the context's fence word before
 * it raises SW_EVENT (design/sgx545_transfer.md §3.1 step 8), so on every
 * SW_EVENT the interrupt handler signals each context's pending fences up to
 * the number in its fence word. Contexts run their jobs in order, so the
 * word only moves forward.
 * Fences keep their job's command for sgxfive_ctx_rewind (sgxfive_ctx.h).
 */
#include <linux/dma-fence.h>
#include <linux/slab.h>

#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

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

/* At submission; the job's command is built into the fence's cmd. */
struct sgxfive_fence *sgxfive_fence_alloc(void)
{
	struct sgxfive_fence *f = kzalloc_obj(*f);

	return f;
}

/*
 * When the job runs: the fence for the context's job number seqno, pending
 * until it completes. order is the job's place among all jobs given to the
 * firmware and size the space its command takes in the client CCB.
 */
struct dma_fence *sgxfive_fence_init(struct sgxfive_fence *f,
				     struct sgxfive_ctx *ctx, u32 seqno,
				     u64 order, u32 size)
{
	unsigned long flags;

	f->order = order;
	f->size = size;
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

void sgxfive_fence_signal(struct sgxfive_ctx *ctx)
{
	sgxfive_fence_retire(ctx, 0);
}

void sgxfive_fence_fail_all(struct sgxfive_ctx *ctx, int error)
{
	sgxfive_fence_retire(ctx, error);
}

/* The order of the context's oldest unfinished job; false if it has none. */
bool sgxfive_ctx_oldest(struct sgxfive_ctx *ctx, u64 *order)
{
	struct sgxfive_fence *f;
	unsigned long flags;

	spin_lock_irqsave(&ctx->fence_lock, flags);
	f = list_first_entry_or_null(&ctx->pending, struct sgxfive_fence, link);
	if (f)
		*order = f->order;
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	return f;
}

/*
 * With the firmware stopped: rewrite the client CCB from offset 0 with the
 * commands of the context's unfinished jobs, in order, and set the fence word
 * to the job before the first of them. Returns how many there are.
 */
u32 sgxfive_ctx_rewind(struct sgxfive_ctx *ctx)
{
	u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	u8 *ccb = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF);
	struct sgxfive_fence *f;
	unsigned long flags;
	u32 n = 0, woff = 0;

	spin_lock_irqsave(&ctx->fence_lock, flags);
	list_for_each_entry(f, &ctx->pending, link) {
		if (!n++)
			WRITE_ONCE(*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF),
				   (u32)f->base.seqno - 1);
		memcpy(ccb + woff, f->cmd, SGX_XFER_CMD_SIZE);
		woff = (woff + f->size) & (SGX_XFER_CCB_SIZE - 1);
	}
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	ctx->woff = woff;
	WRITE_ONCE(ctl[0], woff);	/* host write offset */
	WRITE_ONCE(ctl[1], 0);		/* firmware read offset */
	return n;
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
