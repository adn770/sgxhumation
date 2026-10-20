// SPDX-License-Identifier: GPL-2.0-only
/*
 * Job scheduling for sgxfive. One drm_sched scheduler serves the device and
 * each context has an entity on it, so a context's jobs reach the firmware in
 * submission order. Running a job copies its firmware command into the
 * context's client CCB, advances the write offset, creates its hardware fence
 * and kicks the firmware's transfer queue for the context.
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
	u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	struct dma_fence *fence;
	int ret;

	if (READ_ONCE(ctx->lost))
		return ERR_PTR(-ECANCELED);
	memcpy(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF + ctx->woff), job->cmd,
	       SGX_XFER_CMD_SIZE);
	ctx->woff = (ctx->woff + job->size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], ctx->woff);		/* host write offset */
	fence = sgxfive_fence_create(ctx, job->seqno);
	if (IS_ERR(fence))
		return fence;
	ret = sgxfive_kick_transfer(ctx->sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
	if (ret) {
		sgxfive_fence_fail_all(ctx, ret);
		dma_fence_put(fence);
		return ERR_PTR(ret);
	}
	return fence;
}

/*
 * A job ran past its timeout. Until firmware recovery exists (plan P5b),
 * the context is marked lost and its outstanding fences fail.
 */
static enum drm_gpu_sched_stat sgxfive_timedout_job(struct drm_sched_job *sched_job)
{
	struct sgxfive_ctx *ctx = to_sgxfive_job(sched_job)->ctx;

	drm_err(ctx->sgx->dev, "sgxfive: transfer job timed out, context lost\n");
	WRITE_ONCE(ctx->lost, true);
	sgxfive_fence_fail_all(ctx, -ETIMEDOUT);
	return DRM_GPU_SCHED_STAT_RESET;
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
