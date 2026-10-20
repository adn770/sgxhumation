// SPDX-License-Identifier: GPL-2.0-only
/*
 * SUBMIT_JOBS for sgxfive transfer contexts. A call is validated as a whole
 * before anything is queued; then each job is armed on its context's entity
 * in order, so a job may wait on a syncobj that an earlier job of the same
 * call signals.
 */
#include <linux/dma-fence-chain.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>
#include <drm/gpu_scheduler.h>

#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

#define SGXFIVE_MAX_JOBS	16
#define SGXFIVE_MAX_SYNC_OPS	16

struct sgxfive_submit {
	struct drm_sgxfive_job desc;
	struct drm_sgxfive_transfer_cmd regs;
	struct drm_sgxfive_sync_op ops[SGXFIVE_MAX_SYNC_OPS];
	struct sgxfive_job *job;
	struct dma_fence_chain *chains[SGXFIVE_MAX_SYNC_OPS];
};

/* Copy count elements of stride bytes; a longer stride's tail must be zero. */
static int copy_array(void *dst, size_t size, const struct drm_sgxfive_obj_array *a,
		      u32 max)
{
	u32 i;

	if (a->count > max || (a->count && a->stride < size))
		return -EINVAL;
	for (i = 0; i < a->count; i++) {
		const u8 __user *src = u64_to_user_ptr(a->array) + (u64)i * a->stride;
		u32 j;

		if (copy_from_user((u8 *)dst + i * size, src, size))
			return -EFAULT;
		for (j = size; j < a->stride; j++) {
			u8 b;

			if (get_user(b, src + j))
				return -EFAULT;
			if (b)
				return -EINVAL;
		}
	}
	return 0;
}

static int sgxfive_submit_check(struct sgxfive_submit *s, struct sgxfive_ctx *ctx)
{
	const struct drm_sgxfive_job *d = &s->desc;
	bool dummy = d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY;
	u32 len = d->cmd_stream_len, i;
	int ret;

	if (d->type != DRM_SGXFIVE_JOB_TYPE_TRANSFER ||
	    (d->flags & ~DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_FLAGS_MASK) ||
	    d->updates.count || d->updates.stride || d->updates.array)
		return -EINVAL;
	if (READ_ONCE(ctx->lost))
		return -ECANCELED;
	if (dummy) {
		if (len || d->cmd_stream)
			return -EINVAL;
	} else {
		if (len < sizeof(s->regs) || len > PAGE_SIZE || d->cmd_stream % 8)
			return -EINVAL;
		ret = copy_array(&s->regs, sizeof(s->regs),
				 &(struct drm_sgxfive_obj_array){ len, 1, d->cmd_stream },
				 1);
		if (ret)
			return ret;
		ret = sgxfive_job_check(&s->regs);
		if (ret)
			return ret;
	}
	ret = copy_array(s->ops, sizeof(s->ops[0]), &d->sync_ops,
			 SGXFIVE_MAX_SYNC_OPS);
	if (ret)
		return ret;
	for (i = 0; i < d->sync_ops.count; i++) {
		u32 type = s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK;

		if ((s->ops[i].flags & ~DRM_SGXFIVE_SYNC_OP_FLAGS_MASK) ||
		    type > DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ ||
		    (type == DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ &&
		     s->ops[i].value))
			return -EINVAL;
		if (type == DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ &&
		    (s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL)) {
			s->chains[i] = dma_fence_chain_alloc();
			if (!s->chains[i])
				return -ENOMEM;
		}
	}
	return 0;
}

/* Queue one validated job: dependencies, arming, its command, its signals. */
static int sgxfive_submit_queue(struct sgxfive_submit *s, struct sgxfive_ctx *ctx,
				struct drm_file *file)
{
	const struct drm_sgxfive_job *d = &s->desc;
	struct sgxfive_job *job = s->job;
	struct dma_fence *done;
	u32 i;
	int ret;

	ret = drm_sched_job_init(&job->base, &ctx->entity, 1, file,
				 file->client_id);
	if (ret)
		return ret;
	for (i = 0; i < d->sync_ops.count; i++) {
		if (s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL)
			continue;
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file,
							   s->ops[i].handle,
							   s->ops[i].value);
		if (ret) {
			drm_sched_job_cleanup(&job->base);
			return ret;
		}
	}

	drm_sched_job_arm(&job->base);
	job->ctx = ctx;
	job->seqno = ++ctx->seqno;
	job->size = sgxfive_job_build(job->cmd,
				      (d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY) ?
				      NULL : &s->regs,
				      d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY,
				      ctx->mem.addr + SGXFIVE_CTX_FENCE_OFF,
				      job->seqno);
	done = &job->base.s_fence->finished;
	for (i = 0; i < d->sync_ops.count; i++) {
		struct drm_syncobj *obj;

		if (!(s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL))
			continue;
		obj = drm_syncobj_find(file, s->ops[i].handle);
		if (!obj)
			continue;	/* checked again below: nothing to signal */
		if (s->chains[i]) {
			drm_syncobj_add_point(obj, s->chains[i], done,
					      s->ops[i].value);
			s->chains[i] = NULL;
		} else {
			drm_syncobj_replace_fence(obj, done);
		}
		drm_syncobj_put(obj);
	}
	kref_get(&ctx->ref);		/* the job's reference */
	drm_sched_entity_push_job(&job->base);
	s->job = NULL;
	return 0;
}

/* Every signal syncobj must exist before anything is queued. */
static int sgxfive_submit_signals_exist(struct sgxfive_submit *s,
					struct drm_file *file)
{
	u32 i;

	for (i = 0; i < s->desc.sync_ops.count; i++) {
		struct drm_syncobj *obj;

		if (!(s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL))
			continue;
		obj = drm_syncobj_find(file, s->ops[i].handle);
		if (!obj)
			return -ENOENT;
		drm_syncobj_put(obj);
	}
	return 0;
}

int sgxfive_ioctl_submit(struct sgxfive *sgx, void *data, struct drm_file *file)
{
	struct drm_sgxfive_submit_jobs_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_ctx *ctxs[SGXFIVE_MAX_JOBS] = { NULL };
	struct drm_sgxfive_job *descs;
	struct sgxfive_submit *subs;
	u32 n = a->jobs.count, i, j;
	int ret;

	if (!n || n > SGXFIVE_MAX_JOBS)
		return -EINVAL;
	descs = kcalloc(n, sizeof(*descs), GFP_KERNEL);
	subs = kcalloc(n, sizeof(*subs), GFP_KERNEL);
	ret = descs && subs ? copy_array(descs, sizeof(*descs), &a->jobs,
					 SGXFIVE_MAX_JOBS) : -ENOMEM;
	if (ret) {
		kfree(descs);
		kfree(subs);
		return ret;
	}
	for (i = 0; i < n; i++)
		subs[i].desc = descs[i];
	kfree(descs);

	for (i = 0; i < n && !ret; i++) {
		ctxs[i] = sgxfive_ctx_get(f, subs[i].desc.context_handle);
		ret = ctxs[i] ? sgxfive_submit_check(&subs[i], ctxs[i]) : -EINVAL;
		if (!ret)
			ret = sgxfive_submit_signals_exist(&subs[i], file);
		if (!ret) {
			subs[i].job = kzalloc_obj(*subs[i].job);
			ret = subs[i].job ? 0 : -ENOMEM;
		}
	}
	if (ret) {
		a->jobs.count = i ? i - 1 : 0;	/* the failing job */
		goto out;
	}

	for (i = 0; i < n; i++) {
		mutex_lock(&ctxs[i]->submit_lock);
		ret = sgxfive_submit_queue(&subs[i], ctxs[i], file);
		mutex_unlock(&ctxs[i]->submit_lock);
		if (ret) {
			a->jobs.count = i;
			break;
		}
	}
out:
	for (i = 0; i < n; i++) {
		kfree(subs[i].job);		/* NULL once queued */
		for (j = 0; j < SGXFIVE_MAX_SYNC_OPS; j++)
			dma_fence_chain_free(subs[i].chains[j]);
		if (ctxs[i])
			sgxfive_ctx_put(ctxs[i]);
	}
	kfree(subs);
	return ret;
}
