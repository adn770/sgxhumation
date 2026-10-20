// SPDX-License-Identifier: GPL-2.0-only
/*
 * sgxfive transfer contexts. Each one is a firmware HW transfer context with
 * its own client CCB, in kernel memory the EDM reads through its own page
 * directory, bound for its lifetime to one client VM whose directory the
 * firmware gives the context's 3D work (design/sgx545_transfer.md §2.2).
 */
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>

#include "sgx_xfer.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

static void sgxfive_ctx_release(struct kref *ref)
{
	struct sgxfive_ctx *ctx = container_of(ref, struct sgxfive_ctx, ref);
	struct sgxfive *sgx = ctx->sgx;
	unsigned long flags;

	spin_lock_irqsave(&sgx->ctx_lock, flags);
	list_del(&ctx->link);
	spin_unlock_irqrestore(&sgx->ctx_lock, flags);
	sgxfive_fence_fail_all(ctx, -ECANCELED);	/* none should be left */
	sgxfive_kmem_free(sgx, &ctx->node, &ctx->mem);
	sgxfive_vm_put(ctx->vm);
	mutex_destroy(&ctx->submit_lock);
	kfree(ctx);
}

void sgxfive_ctx_put(struct sgxfive_ctx *ctx)
{
	kref_put(&ctx->ref, sgxfive_ctx_release);
}

struct sgxfive_ctx *sgxfive_ctx_get(struct sgxfive_file *f, u32 handle)
{
	struct sgxfive_ctx *ctx;

	xa_lock(&f->ctxs);
	ctx = xa_load(&f->ctxs, handle);
	if (ctx)
		kref_get(&ctx->ref);
	xa_unlock(&f->ctxs);
	return ctx;
}

int sgxfive_ioctl_create_ctx(struct sgxfive *sgx, void *data,
			     struct drm_file *file)
{
	struct drm_sgxfive_create_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_ctx *ctx;
	int ret;

	if (a->type != DRM_SGXFIVE_CTX_TYPE_TRANSFER || a->flags ||
	    a->priority != DRM_SGXFIVE_CTX_PRIORITY_NORMAL || a->_padding_14)
		return -EINVAL;
	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->vm = sgxfive_vm_get(f, a->vm_context_handle);
	if (!ctx->vm) {
		kfree(ctx);
		return -EINVAL;
	}
	ret = sgxfive_ensure_live(sgx);
	if (!ret)
		ret = sgxfive_kmem_alloc(sgx, &ctx->node, &ctx->mem,
					 SGXFIVE_CTX_MEM_SIZE);
	if (ret) {
		sgxfive_vm_put(ctx->vm);
		kfree(ctx);
		return ret;
	}
	ctx->sgx = sgx;
	kref_init(&ctx->ref);
	mutex_init(&ctx->submit_lock);
	ctx->fence_context = dma_fence_context_alloc(1);
	spin_lock_init(&ctx->fence_lock);
	INIT_LIST_HEAD(&ctx->pending);
	/* Priority 0, the client CCB and its control block, the client PD. */
	sgx_xfer_ctx_init(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_HW_OFF),
			  sgxfive_vm_pd_phys(ctx->vm),
			  ctx->mem.addr + SGXFIVE_CTX_CCB_OFF,
			  ctx->mem.addr + SGXFIVE_CTX_CTL_OFF);
	spin_lock_irq(&sgx->ctx_lock);
	list_add_tail(&ctx->link, &sgx->ctx_list);
	spin_unlock_irq(&sgx->ctx_lock);
	ret = sgxfive_ctx_entity_init(ctx);
	if (ret) {
		sgxfive_ctx_put(ctx);
		return ret;
	}
	ret = xa_alloc(&f->ctxs, &a->handle, ctx, xa_limit_32b, GFP_KERNEL);
	if (ret) {
		drm_sched_entity_destroy(&ctx->entity);
		sgxfive_ctx_put(ctx);
	}
	return ret;
}

int sgxfive_ioctl_destroy_ctx(struct sgxfive *sgx, void *data,
			      struct drm_file *file)
{
	struct drm_sgxfive_destroy_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_ctx *ctx;

	if (a->_padding_4)
		return -EINVAL;
	ctx = xa_erase(&f->ctxs, a->handle);
	if (!ctx)
		return -EINVAL;
	drm_sched_entity_destroy(&ctx->entity);	/* waits for its jobs */
	sgxfive_ctx_put(ctx);
	return 0;
}

/* On close: contexts go before the VMs they hold. */
void sgxfive_ctx_close_all(struct sgxfive_file *f)
{
	struct sgxfive_ctx *ctx;
	unsigned long i;

	xa_for_each(&f->ctxs, i, ctx) {
		drm_sched_entity_destroy(&ctx->entity);
		sgxfive_ctx_put(ctx);
	}
	xa_destroy(&f->ctxs);
}
