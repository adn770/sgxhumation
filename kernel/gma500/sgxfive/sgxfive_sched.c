// SPDX-License-Identifier: GPL-2.0-only
/*
 * Job scheduling for sgxfive. One drm_sched scheduler serves the device and
 * each context has an entity on it, so a context's jobs reach the firmware in
 * submission order. Running a job copies its firmware command into the
 * context's client CCB, advances the write offset, creates its hardware fence
 * and kicks the firmware's transfer queue for the context. All of it happens
 * under the device lock, so a recovery reset never sees half a submission.
 */
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>

#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

#define SGXFIVE_JOB_TIMEOUT_MS	500
/* 64 commands of 0x1c0 bytes stay well inside one context's 64 KiB CCB. */
#define SGXFIVE_SCHED_CREDITS	64

static struct sgxfive_job *to_sgxfive_job(struct drm_sched_job *j)
{
	return container_of(j, struct sgxfive_job, base);
}

static struct dma_fence *sgxfive_run_job(struct drm_sched_job *sched_job)
{
	struct sgxfive_job *job = to_sgxfive_job(sched_job);
	struct sgxfive_ctx *ctx = job->ctx;
	struct sgxfive *sgx = ctx->sgx;
	u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	struct dma_fence *fence;
	int ret;

	mutex_lock(&sgx->lock);
	if (READ_ONCE(ctx->lost)) {
		fence = ERR_PTR(-ECANCELED);
		goto out;
	}
	fence = sgxfive_fence_create(ctx, job->seqno, ++sgx->kicks, job->cmd,
				     job->size);
	if (IS_ERR(fence))
		goto out;
	memcpy(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF + ctx->woff), job->cmd,
	       SGX_XFER_CMD_SIZE);
	ctx->woff = (ctx->woff + job->size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], ctx->woff);		/* host write offset */
	ret = sgxfive_kick_transfer(sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
	if (ret) {
		sgxfive_fence_fail_all(ctx, ret);
		dma_fence_put(fence);
		fence = ERR_PTR(ret);
	}
out:
	mutex_unlock(&sgx->lock);
	return fence;
}

/* The order of the oldest job the firmware has not finished, or U64_MAX. */
static u64 sgxfive_oldest(struct sgxfive *sgx)
{
	struct sgxfive_ctx *ctx;
	u64 oldest = U64_MAX, order;

	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry(ctx, &sgx->ctx_list, link)
		if (sgxfive_ctx_oldest(ctx, &order))
			oldest = min(oldest, order);
	spin_unlock_irq(&sgx->ctx_lock);
	return oldest;
}

/*
 * A job ran past its timeout. The firmware runs one transfer at a time, so a
 * job may only be waiting behind other contexts' work: the GPU hangs when the
 * oldest unfinished job is the same as at the previous timeout. Then the
 * recovery reset fails that job's context and re-runs every other context's
 * unfinished jobs. Either way the job stays with the scheduler, which frees it
 * once its fence signals (failed, if it was guilty), so this always reports
 * NO_HANG: the reset is done here, without stopping the scheduler.
 */
static enum drm_gpu_sched_stat sgxfive_timedout_job(struct drm_sched_job *sched_job)
{
	struct sgxfive *sgx = to_sgxfive_job(sched_job)->ctx->sgx;
	u64 oldest = sgxfive_oldest(sgx);

	if (oldest == U64_MAX || oldest != sgx->hang_oldest) {
		sgx->hang_oldest = oldest;	/* progress since the last look */
		return DRM_GPU_SCHED_STAT_NO_HANG;
	}
	drm_err(sgx->dev, "sgxfive: transfer job %llu hung, recovering\n", oldest);
	sgxfive_recover(sgx, true);
	sgx->hang_oldest = U64_MAX;
	return DRM_GPU_SCHED_STAT_NO_HANG;
}

static void sgxfive_free_job(struct drm_sched_job *sched_job)
{
	struct sgxfive_job *job = to_sgxfive_job(sched_job);

	drm_sched_job_cleanup(sched_job);
	sgxfive_ctx_put(job->ctx);
	kfree(job);
}

static const struct drm_sched_backend_ops sgxfive_sched_ops = {
	.run_job = sgxfive_run_job,
	.timedout_job = sgxfive_timedout_job,
	.free_job = sgxfive_free_job,
};

int sgxfive_sched_init(struct sgxfive *sgx)
{
	const struct drm_sched_init_args args = {
		.ops = &sgxfive_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = SGXFIVE_SCHED_CREDITS,
		.timeout = msecs_to_jiffies(SGXFIVE_JOB_TIMEOUT_MS),
		.name = "sgxfive",
		.dev = sgx->dev->dev,
	};

	return drm_sched_init(&sgx->sched, &args);
}

void sgxfive_sched_fini(struct sgxfive *sgx)
{
	drm_sched_fini(&sgx->sched);
}

int sgxfive_ctx_entity_init(struct sgxfive_ctx *ctx)
{
	struct drm_gpu_scheduler *sched = &ctx->sgx->sched;

	return drm_sched_entity_init(&ctx->entity, DRM_SCHED_PRIORITY_NORMAL,
				     &sched, 1, NULL);
}
