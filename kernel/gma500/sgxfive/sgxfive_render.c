// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sgxfive render contexts (design/sgx545_render_uapi.md §11). A render
 * context is a firmware HW render context with its TA client CCB, one RT data
 * set and one parameter buffer. The firmware structures live in the
 * context's kernel memory, which the EDM reads through its own page
 * directory and, while it loads the parameter buffer, through the context's
 * (so the VM aliases it read-only). The memory the TA, DPM and 3D pipe write
 * lives in the VM itself, in the context's slots of the reserved PARAMETERS
 * and TA_DATA heaps: the PB pages, their page table, the scratch primitive
 * block, the background object and the RT data's buffers.
 *
 * Each job is one whole scene, built at submission into its hardware fence
 * (sgxfive_render_build): the CMDTA and the render details. One render job
 * per context is in the firmware at a time (sgxfive_sched.c), so running a
 * job may rewrite the render details and the background object in place.
 */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>

#include <drm/drm_print.h>

#include "sgx_mmu.h"
#include "sgx_render.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

/* The parameter slot is mapped from its PB; the CPU sees from the table on. */
#define PARAM_PAGES	((SGX_RENDER_PARAM_SIZE - SGX_RENDER_PARAM_PB) >> PAGE_SHIFT)
#define CPU_FIRST	((SGX_RENDER_PARAM_PT - SGX_RENDER_PARAM_PB) >> PAGE_SHIFT)
#define CPU_PAGES	(PARAM_PAGES - CPU_FIRST)
#define TD_PAGES	(SGX_RENDER_TD_SIZE >> PAGE_SHIFT)

struct sgxfive_render {
	struct sgxfive_ctx *ctx;
	unsigned int slot;
	struct sgx_render_addrs a;
	struct page *param[PARAM_PAGES];
	struct page *td[TD_PAGES];
	u8 *cpu;			/* page table, scratch, background */
	u8 *tdva;			/* the RT data's buffers */
	bool mapped;			/* in the VM */
	struct sgxfive_deferred release;
};

static u32 *rctx(struct sgxfive_ctx *ctx, u32 off)
{
	return sgxfive_ctx_cpu(ctx, off);
}

static u32 *render_bg(struct sgxfive_render *r)
{
	return (u32 *)(r->cpu + SGX_RENDER_PARAM_BG - SGX_RENDER_PARAM_PT);
}

static void free_pages_wc(struct page **pages, unsigned int n)
{
	unsigned int i;

	if (pages[0])
		set_pages_array_wb(pages, n);
	for (i = 0; i < n; i++)
		if (pages[i])
			__free_page(pages[i]);
}

/* Zeroed lowmem pages (below 4 GiB), set write-combined like the firmware's. */
static int alloc_pages_wc(struct page **pages, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[i]) {
			while (i--) {
				__free_page(pages[i]);
				pages[i] = NULL;
			}
			return -ENOMEM;
		}
	}
	set_pages_array_wc(pages, n);
	return 0;
}

/*
 * The context's firmware structures as at creation (and after a reset): a
 * new HW render context, an empty TA CCB, a free RT data, the PB with its
 * initial free list and the host-built page table, zeroed tiling data.
 */
static void sgxfive_render_init_mem(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;
	u32 *ctl = rctx(ctx, SGXFIVE_CTX_CTL_OFF);

	sgx_render_ctx(rctx(ctx, SGXFIVE_CTX_HW_OFF), &r->a);
	ctl[0] = 0;
	ctl[1] = 0;
	sgx_render_rtds(rctx(ctx, SGXFIVE_RCTX_RTDS_OFF), &r->a);
	sgx_render_pb(rctx(ctx, SGXFIVE_RCTX_PBDESC_OFF),
		      rctx(ctx, SGXFIVE_RCTX_PBBLOCK_OFF), (u32 *)r->cpu, &r->a);
	*rctx(ctx, SGXFIVE_RCTX_ACCESS_OFF) = 0;
	*rctx(ctx, SGXFIVE_RCTX_PENDING_OFF) = 0;
	*rctx(ctx, SGXFIVE_RCTX_RTSTATUS_OFF) = 0;
	*rctx(ctx, SGXFIVE_CTX_TA_OFF) = *rctx(ctx, SGXFIVE_CTX_FENCE_OFF);
	memset(r->tdva, 0, SGX_RENDER_TD_SIZE);
	ctx->woff = 0;
}

static void sgxfive_render_unmap(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;

	if (!r->mapped)
		return;
	sgxfive_vm_unmap_pages(ctx->vm, r->a.param_base + SGX_RENDER_PARAM_PB,
			       PARAM_PAGES);
	sgxfive_vm_unmap_pages(ctx->vm, r->a.tadata, TD_PAGES);
	sgxfive_vm_unmap_pages(ctx->vm, ctx->mem.addr, ctx->mem.npages);
	r->mapped = false;
}

static int sgxfive_render_map(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;
	int ret;

	r->mapped = true;
	ret = sgxfive_vm_map_pages(ctx->vm, r->a.param_base + SGX_RENDER_PARAM_PB,
				   r->param, PARAM_PAGES, 0);
	if (!ret)
		ret = sgxfive_vm_map_pages(ctx->vm, r->a.tadata, r->td, TD_PAGES, 0);
	/* The EDM reads the PB descriptor here through the context's directory. */
	if (!ret)
		ret = sgxfive_vm_map_pages(ctx->vm, ctx->mem.addr, ctx->mem.pages,
					   ctx->mem.npages, SGX_MMU_RO);
	if (ret)
		sgxfive_render_unmap(r);
	return ret;
}

/* Everything but the VM's slot, which the caller still holds. */
static void sgxfive_render_free(struct sgxfive_render *r)
{
	if (r->cpu)
		vunmap(r->cpu);
	if (r->tdva)
		vunmap(r->tdva);
	free_pages_wc(r->param, PARAM_PAGES);
	free_pages_wc(r->td, TD_PAGES);
	kfree(r);
}

/*
 * A new render context, its kernel memory allocated and its VM set: the slot
 * and the firmware alias in the VM, the slot's pages, the structures.
 */
int sgxfive_render_init(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = kzalloc_obj(*r);
	int ret;

	if (!r)
		return -ENOMEM;
	r->ctx = ctx;
	ret = sgxfive_vm_render_attach(ctx->vm, &r->slot);
	if (ret) {
		kfree(r);
		return ret;
	}
	sgxfive_render_addrs(&r->a, ctx->mem.addr, sgxfive_vm_pd_phys(ctx->vm), r->slot);
	ret = alloc_pages_wc(r->param, PARAM_PAGES);
	if (!ret)
		ret = alloc_pages_wc(r->td, TD_PAGES);
	if (!ret) {
		r->cpu = vmap(r->param + CPU_FIRST, CPU_PAGES, VM_MAP,
			      pgprot_writecombine(PAGE_KERNEL));
		r->tdva = vmap(r->td, TD_PAGES, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
		ret = r->cpu && r->tdva ? 0 : -ENOMEM;
	}
	if (!ret)
		ret = sgxfive_render_map(r);
	if (ret) {
		sgxfive_vm_render_detach(ctx->vm, r->slot);
		sgxfive_render_free(r);
		return ret;
	}
	sgxfive_render_init_mem(r);
	wmb(); /* the write-combined structures, before the first kick */
	ctx->render = r;
	return 0;
}

/* After the BIF has dropped what it cached of them: pages, memory, the rest. */
static void sgxfive_render_release(struct sgxfive_deferred *d)
{
	struct sgxfive_render *r = container_of(d, struct sgxfive_render, release);
	struct sgxfive_ctx *ctx = r->ctx;

	sgxfive_kmem_free(ctx->sgx, &ctx->node, &ctx->mem);
	sgxfive_vm_put(ctx->vm);
	sgxfive_client_put(ctx->client);
	mutex_destroy(&ctx->submit_lock);
	kfree(ctx);
	sgxfive_render_free(r);
}

/*
 * The last reference to a render context is gone and it is off the device's
 * list: the firmware drops its state (CLEANUP; a blame-free reset if it does
 * not answer), the VM its mappings, and the memory goes once no stale
 * translation can reach it. The slot is free at once: a stale translation
 * still lands on the pinned pages (design §3.3). Frees ctx.
 */
void sgxfive_render_fini(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = ctx->render;
	struct sgxfive *sgx = ctx->sgx;

	if (sgxfive_render_cleanup(sgx, r->a.rtds, r->a.ctx, r->a.pbdesc)) {
		drm_warn(sgx->dev, "sgxfive: render context cleanup failed, resetting\n");
		sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_MANUAL);
	}
	sgxfive_render_unmap(r);
	sgxfive_vm_render_detach(ctx->vm, r->slot);
	r->release.free = sgxfive_render_release;
	sgxfive_defer(sgx, &r->release);
}

/* At submission: the job's firmware image, into its hardware fence. */
void sgxfive_render_job_build(struct sgxfive_ctx *ctx, struct sgxfive_job *job)
{
	job->size = sgxfive_render_build(job->hw->cmd, &ctx->render->a, &job->rc,
					 job->seqno);
}

/*
 * Under the device lock, with the context's previous render finished: the
 * job's render details and background object in place, its CMDTA at the TA
 * CCB's write offset, one more scene in the RT data set's pending count (the
 * EDM counts the completed ones, and cleans the set up only when the two
 * agree), then the kick.
 */
int sgxfive_render_run(struct sgxfive_ctx *ctx, struct sgxfive_job *job,
		       const struct sgxfive_fence *hw)
{
	struct sgxfive_render *r = ctx->render;
	u32 *ctl = rctx(ctx, SGXFIVE_CTX_CTL_OFF);

	memcpy(rctx(ctx, SGXFIVE_RCTX_RD_OFF), hw->cmd + SGXFIVE_RENDER_IMG_RD,
	       SGX_RENDER_RD_SIZE);
	sgx_render_bgobj(render_bg(r), &job->rc);
	*rctx(ctx, SGXFIVE_RCTX_BGPRI_OFF) = job->rc.bgobj_words[2];
	*rctx(ctx, SGXFIVE_RCTX_PENDING_OFF) += 1;
	memcpy(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF + ctx->woff), hw->cmd,
	       SGX_RENDER_CMD_STRIDE);
	ctx->woff = (ctx->woff + job->size) & (SGX_RENDER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], ctx->woff);		/* host write offset */
	return sgxfive_kick_render(ctx->sgx, r->a.ctx);
}

/*
 * With the firmware stopped (recovery reset), under the context list's lock:
 * the structures start over and the unfinished job, if any, is written back
 * (its CMDTA into the TA CCB from offset 0, its render details and background
 * object from the copy in its fence). Returns how many jobs to re-run.
 */
u32 sgxfive_render_reset(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = ctx->render;
	struct drm_sgxfive_render_cmd c = { 0 };
	struct sgxfive_fence *f;
	unsigned long flags;
	u32 n, i;

	sgxfive_render_init_mem(r);
	n = sgxfive_ctx_rewind(ctx);
	*rctx(ctx, SGXFIVE_RCTX_PENDING_OFF) = n;	/* the RT data set starts over */
	spin_lock_irqsave(&ctx->fence_lock, flags);
	f = list_first_entry_or_null(&ctx->pending, struct sgxfive_fence, link);
	if (f) {
		memcpy(rctx(ctx, SGXFIVE_RCTX_RD_OFF), f->cmd + SGXFIVE_RENDER_IMG_RD,
		       SGX_RENDER_RD_SIZE);
		/* The background object's inputs, as the command carries them. */
		for (i = 0; i < 3; i++)
			memcpy(&c.bgobj_words[i], f->cmd + SGX_RENDER_CMD_SPEC + 4 * i, 4);
		memcpy(&c.mte_screen, f->cmd + SGX_RENDER_CMD_TAREGS +
		       SGX_RENDER_TA_MTESCREEN, 4);
		sgx_render_bgobj(render_bg(r), &c);
		*rctx(ctx, SGXFIVE_RCTX_BGPRI_OFF) = c.bgobj_words[2];
	}
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	return n;
}
