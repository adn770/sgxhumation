// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The sgxfive error state. A recovery reset that is about to reset captures,
 * under the device lock and before it touches the hardware, a snapshot of
 * why (the detector, the suspect job, the guilty context), the registers and
 * firmware control blocks sgxfive_dump() shows, and each context's unfinished
 * jobs with their saved firmware commands. Once the reset is done the
 * snapshot goes to devcoredump on the gma500 PCI device, which shows it as
 * /sys/class/devcoredump/devcdN/data; the text is formatted on each read
 * with drm_coredump_printer, so nothing large is allocated at capture.
 *
 * Capture runs in the recovery paths, one of them the drm_sched timeout, so
 * it allocates with GFP_NOWAIT and does without a dump when that fails.
 * devcoredump keeps one dump per device until it is read and written to (or
 * for five minutes); while one is pending no new one is allocated.
 */
#include <linux/devcoredump.h>
#include <linux/dma-fence.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/utsname.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "sgxfive_coredump.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

static void sgxfive_print_words(struct drm_printer *p, const char *name,
				const u32 *w, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i += 8)
		drm_printf(p, "%s[%2u]: %*ph\n", name, i,
			   (int)(min(8u, n - i) * 4), &w[i]);
}

void sgxfive_devstate_print(struct drm_printer *p, const struct sgxfive_devstate *d)
{
	unsigned int i;

	drm_printf(p, "loaded %d live %d sw_events %d bif_faults %d ccb_sent %llu deferred %u\n",
		   d->loaded, d->live, d->sw_events, d->bif_faults, d->ccb_sent,
		   d->ndeferred);
	for (i = 0; i < SGXFIVE_DUMP_NREGS; i++)
		drm_printf(p, "reg %#05x = %#010x\n", d->regs[i].reg, d->regs[i].val);
	if (!d->loaded)
		return;
	sgxfive_print_words(p, "hostctl", d->hostctl, SGXFIVE_HC_DWORDS);
	sgxfive_print_words(p, "ta3dctl", d->ta3dctl, SGXFIVE_TA3D_CTL_DWORDS);
	sgxfive_print_words(p, "ccbctl", d->ccbctl, 2);
}

/* Under ctx's fence_lock: its unfinished jobs, as many as there is room for. */
static void sgxfive_coredump_jobs(struct sgxfive_coredump *cd,
				  struct sgxfive_cd_ctx *c, struct sgxfive_ctx *ctx)
{
	struct sgxfive_fence *f;

	c->first_job = cd->njobs;
	list_for_each_entry(f, &ctx->pending, link) {
		struct sgxfive_cd_job *j;

		c->npending++;
		if (cd->njobs == SGXFIVE_CD_MAX_JOBS)
			continue;
		j = &cd->jobs[cd->njobs++];
		c->njobs++;
		j->seqno = f->base.seqno;
		j->order = f->order;
		j->size = f->size;
		memcpy(j->cmd, f->cmd, min_t(u32, f->size, SGXFIVE_CD_CMD_BYTES));
	}
}

static void sgxfive_coredump_ctx(struct sgxfive_coredump *cd, struct sgxfive_ctx *ctx)
{
	const u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	struct sgxfive_cd_ctx *c;
	unsigned long flags;

	cd->nctxs_seen++;
	if (cd->nctxs == SGXFIVE_CD_MAX_CTXS)
		return;
	c = &cd->ctxs[cd->nctxs++];
	c->fence_context = ctx->fence_context;
	if (ctx->client) {
		c->pid = ctx->client->pid;
		strscpy(c->comm, ctx->client->comm);
	}
	c->lost = READ_ONCE(ctx->lost);
	c->done = READ_ONCE(*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF));
	c->woff = READ_ONCE(ctl[0]);
	c->roff = READ_ONCE(ctl[1]);
	spin_lock_irqsave(&ctx->fence_lock, flags);
	sgxfive_coredump_jobs(cd, c, ctx);
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
}

/*
 * Under the device's ctx_lock, before the guilty context's jobs are failed:
 * every context on ctxs, the guilty one (if any) first.
 */
void sgxfive_coredump_ctxs(struct sgxfive_coredump *cd, struct list_head *ctxs,
			   struct sgxfive_ctx *guilty)
{
	struct sgxfive_ctx *ctx;

	cd->guilty = -1;
	if (guilty) {
		cd->guilty = cd->nctxs;
		sgxfive_coredump_ctx(cd, guilty);
	}
	list_for_each_entry(ctx, ctxs, link)
		if (ctx != guilty)
			sgxfive_coredump_ctx(cd, ctx);
}

static const char *sgxfive_hang_name(enum sgxfive_hang hang)
{
	switch (hang) {
	case SGXFIVE_HANG_FAULT:
		return "bif-fault";
	case SGXFIVE_HANG_TIMEOUT:
		return "job-timeout";
	default:
		return "manual";
	}
}

static const char *sgxfive_comm(const struct sgxfive_cd_ctx *c)
{
	return c->comm[0] ? c->comm : "?";
}

/* The non-zero 32-byte lines of a job's command. */
static void sgxfive_print_cmd(struct drm_printer *p, const struct sgxfive_cd_job *j)
{
	u32 n = min_t(u32, j->size, SGXFIVE_CD_CMD_BYTES), off;

	for (off = 0; off < n; off += 32) {
		u32 len = min(32u, n - off);

		if (memchr_inv(j->cmd + off, 0, len))
			drm_printf(p, "    cmd[%#05x]: %*ph\n", off, (int)len, j->cmd + off);
	}
}

void sgxfive_coredump_print(struct drm_printer *p, const struct sgxfive_coredump *cd)
{
	u64 up = cd->uptime_ns;
	u32 up_ns = do_div(up, NSEC_PER_SEC);
	unsigned int i, k;

	drm_puts(p, "**** sgxfive error state ****\n");
	drm_printf(p, "kernel: %s\n", init_utsname()->release);
	drm_printf(p, "time: %lld.%09ld\n", (long long)cd->time.tv_sec,
		   cd->time.tv_nsec);
	drm_printf(p, "uptime: %llu.%09u\n", up, up_ns);
	drm_printf(p, "detector: %s\n", sgxfive_hang_name(cd->hang));
	if (cd->suspect == U64_MAX)
		drm_puts(p, "suspect: none\n");
	else
		drm_printf(p, "suspect: %llu\n", cd->suspect);
	if (cd->guilty < 0)
		drm_puts(p, "guilty: none\n");
	else
		drm_printf(p, "guilty: pid %d comm %s\n", cd->ctxs[cd->guilty].pid,
			   sgxfive_comm(&cd->ctxs[cd->guilty]));
	if (cd->result)
		drm_printf(p, "recovery: failed %d\n", cd->result);
	else
		drm_puts(p, "recovery: ok\n");
	drm_printf(p, "BIF_INT_STAT: %#010x\n", cd->bif_int_stat);
	drm_printf(p, "BIF_FAULT: %#010x\n", cd->bif_fault);

	drm_puts(p, "**** device ****\n");
	sgxfive_devstate_print(p, &cd->dev);

	drm_printf(p, "**** contexts ****\ncontexts: %u (%u saved)\n",
		   cd->nctxs_seen, cd->nctxs);
	for (i = 0; i < cd->nctxs; i++) {
		const struct sgxfive_cd_ctx *c = &cd->ctxs[i];

		drm_printf(p, "ctx %llu%s pid %d comm %s", c->fence_context,
			   (int)i == cd->guilty ? " guilty" : "", c->pid, sgxfive_comm(c));
		drm_printf(p, " done %u woff %#x roff %#x lost %d pending %u\n",
			   c->done, c->woff, c->roff, c->lost, c->npending);
		for (k = c->first_job; k < c->first_job + c->njobs; k++) {
			const struct sgxfive_cd_job *j = &cd->jobs[k];

			drm_printf(p, "  job %u order %llu size %#x\n", j->seqno,
				   j->order, j->size);
			sgxfive_print_cmd(p, j);
		}
		if (c->npending > c->njobs)
			drm_printf(p, "  %u jobs not saved\n", c->npending - c->njobs);
	}
	drm_puts(p, "**** end ****\n");
}

/* devcoredump's read: the text from offset, at most count bytes of it. */
ssize_t sgxfive_coredump_read(char *buf, loff_t offset, size_t count, void *data,
			      size_t datalen)
{
	struct drm_print_iterator iter = {
		.data = buf, .start = offset, .remain = count,
	};
	struct drm_printer p = drm_coredump_printer(&iter);

	sgxfive_coredump_print(&p, data);
	return count - iter.remain;
}

#define SGXFIVE_CD_PENDING	0	/* bit in sgx->coredump: one with devcoredump */

/*
 * A snapshot for a recovery that is about to reset, or NULL: without
 * devcoredump, while a dump is still pending, or when memory is short.
 */
struct sgxfive_coredump *sgxfive_coredump_alloc(struct sgxfive *sgx,
						enum sgxfive_hang hang, u64 suspect)
{
	struct sgxfive_coredump *cd;

	if (!IS_ENABLED(CONFIG_DEV_COREDUMP) ||
	    test_and_set_bit(SGXFIVE_CD_PENDING, &sgx->coredump))
		return NULL;
	cd = kzalloc_obj(*cd, GFP_NOWAIT | __GFP_NOWARN);
	if (!cd) {
		clear_bit(SGXFIVE_CD_PENDING, &sgx->coredump);
		return NULL;
	}
	cd->sgx = sgx;
	ktime_get_real_ts64(&cd->time);
	cd->uptime_ns = ktime_get_boottime_ns();
	cd->hang = hang;
	cd->suspect = suspect;
	cd->guilty = -1;
	return cd;
}

/* A snapshot that is not used, or devcoredump is done with it. */
static void sgxfive_coredump_free(void *data)
{
	struct sgxfive_coredump *cd = data;

	clear_bit(SGXFIVE_CD_PENDING, &cd->sgx->coredump);
	kfree(cd);
}

void sgxfive_coredump_discard(struct sgxfive_coredump *cd)
{
	if (cd)
		sgxfive_coredump_free(cd);
}

/* Outside the device lock: hand the snapshot to devcoredump. */
void sgxfive_coredump_emit(struct sgxfive_coredump *cd)
{
	if (!cd)
		return;
	drm_info(cd->sgx->dev, "sgxfive: error state captured in /sys/class/devcoredump\n");
	dev_coredumpm(cd->sgx->dev->dev, THIS_MODULE, cd, sizeof(*cd),
		      GFP_NOWAIT | __GFP_NOWARN, sgxfive_coredump_read,
		      sgxfive_coredump_free);
}

/* Teardown: free a dump still pending, so its callbacks run while sgx lives. */
void sgxfive_coredump_fini(struct sgxfive *sgx)
{
	dev_coredump_put(sgx->dev->dev);
}
