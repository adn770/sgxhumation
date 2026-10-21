// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the sgxfive error state (sgxfive_coredump.c): what a
 * recovery captures of the contexts and their unfinished jobs, its bounds,
 * and the text devcoredump reads, whole or in pieces.
 */
#include <kunit/test.h>
#include <linux/dma-fence.h>
#include <linux/module.h>
#include <linux/string.h>

#include <drm/drm_print.h>

#include "sgx_xfer.h"
#include "sgxfive_coredump.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

static struct sgxfive_ctx *fake_ctx(struct kunit *test, struct list_head *list,
				    struct sgxfive_client *client)
{
	struct sgxfive_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->mem.va = kunit_kzalloc(test, SGXFIVE_CTX_MEM_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->mem.va);
	ctx->fence_context = dma_fence_context_alloc(1);
	ctx->client = client;
	spin_lock_init(&ctx->fence_lock);
	INIT_LIST_HEAD(&ctx->pending);
	list_add_tail(&ctx->link, list);
	return ctx;
}

/* Job seqno of ctx, given to the firmware as order; fill marks its command. */
static struct dma_fence *fake_job(struct kunit *test, struct sgxfive_ctx *ctx,
				  u32 seqno, u64 order, u8 fill)
{
	struct sgxfive_fence *hw = sgxfive_fence_alloc();

	KUNIT_ASSERT_NOT_NULL(test, hw);
	memset(hw->cmd, 0, SGX_XFER_CMD_SIZE);
	hw->cmd[0] = fill;
	hw->cmd[0x40] = fill;		/* the line at 0x40; 0x20 stays zero */
	return sgxfive_fence_init(hw, ctx, seqno, order, 0x1c0);
}

static void put_jobs(struct sgxfive_ctx *ctx, struct dma_fence **f, int n)
{
	int i;

	sgxfive_fence_fail_all(ctx, -EIO);
	for (i = 0; i < n; i++)
		dma_fence_put(f[i]);
}

static struct sgxfive_coredump *new_dump(struct kunit *test)
{
	struct sgxfive_coredump *cd = kunit_kzalloc(test, sizeof(*cd), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cd);
	cd->hang = SGXFIVE_HANG_FAULT;
	cd->suspect = 11;
	cd->guilty = -1;
	return cd;
}

/* The whole text, as one read from offset 0 would return it. */
static char *dump_text(struct kunit *test, struct sgxfive_coredump *cd,
		       ssize_t *len)
{
	char *buf = kunit_kzalloc(test, SGXFIVE_COREDUMP_MAX + 1, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	*len = sgxfive_coredump_read(buf, 0, SGXFIVE_COREDUMP_MAX, cd, sizeof(*cd));
	KUNIT_ASSERT_GT(test, *len, 0);
	KUNIT_ASSERT_LT(test, *len, SGXFIVE_COREDUMP_MAX);
	return buf;
}

/*
 * The guilty context comes first with its unfinished jobs and their command
 * words, then the others; signalled jobs are not in it.
 */
static void sgxfive_coredump_test_capture(struct kunit *test)
{
	struct sgxfive_client client = { .pid = 4242 };
	struct sgxfive_coredump *cd = new_dump(test);
	struct sgxfive_ctx *a, *b;
	struct dma_fence *fa[2], *fb[3];
	LIST_HEAD(list);
	int i;

	strscpy(client.comm, "faulter");
	a = fake_ctx(test, &list, NULL);
	b = fake_ctx(test, &list, &client);
	fa[0] = fake_job(test, a, 1, 10, 0xa1);
	fa[1] = fake_job(test, a, 2, 12, 0xa2);
	for (i = 0; i < 3; i++)
		fb[i] = fake_job(test, b, 5 + i, 11 + 2 * i, 0xb0 + i);
	*(u32 *)sgxfive_ctx_cpu(b, SGXFIVE_CTX_FENCE_OFF) = 4;
	*(u32 *)sgxfive_ctx_cpu(b, SGXFIVE_CTX_CTL_OFF) = 0x540;
	*(u32 *)sgxfive_ctx_cpu(b, SGXFIVE_CTX_CTL_OFF + 4) = 0x1c0;

	sgxfive_coredump_ctxs(cd, &list, b);

	KUNIT_EXPECT_EQ(test, cd->nctxs, 2);
	KUNIT_EXPECT_EQ(test, cd->nctxs_seen, 2);
	KUNIT_EXPECT_EQ(test, cd->guilty, 0);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].fence_context, b->fence_context);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].pid, 4242);
	KUNIT_EXPECT_STREQ(test, cd->ctxs[0].comm, "faulter");
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].done, 4);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].woff, 0x540);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].roff, 0x1c0);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].npending, 3);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].njobs, 3);
	KUNIT_EXPECT_EQ(test, cd->ctxs[1].fence_context, a->fence_context);
	KUNIT_EXPECT_EQ(test, cd->ctxs[1].pid, 0);
	KUNIT_EXPECT_EQ(test, cd->ctxs[1].first_job, 3);
	KUNIT_EXPECT_EQ(test, cd->ctxs[1].njobs, 2);
	KUNIT_EXPECT_EQ(test, cd->njobs, 5);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].seqno, 5);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].order, 11);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].size, 0x1c0);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].cmd[0], 0xb0);
	KUNIT_EXPECT_EQ(test, cd->jobs[2].cmd[0x40], 0xb2);
	KUNIT_EXPECT_EQ(test, cd->jobs[4].seqno, 2);

	/* Signalled jobs are no longer pending. */
	*(u32 *)sgxfive_ctx_cpu(a, SGXFIVE_CTX_FENCE_OFF) = 1;
	sgxfive_fence_signal(a);
	cd = new_dump(test);
	sgxfive_coredump_ctxs(cd, &list, NULL);
	KUNIT_EXPECT_EQ(test, cd->guilty, -1);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].fence_context, a->fence_context);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].npending, 1);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].seqno, 2);

	put_jobs(a, fa, 2);
	put_jobs(b, fb, 3);
}

/* At most SGXFIVE_CD_MAX_JOBS commands and SGXFIVE_CD_MAX_CTXS contexts. */
static void sgxfive_coredump_test_bounds(struct kunit *test)
{
	struct sgxfive_coredump *cd = new_dump(test);
	struct sgxfive_ctx *ctx[SGXFIVE_CD_MAX_CTXS + 2];
	struct dma_fence *f[SGXFIVE_CD_MAX_CTXS + 2][2];
	unsigned int i, n = ARRAY_SIZE(ctx);
	char *text;
	ssize_t len;
	LIST_HEAD(list);

	for (i = 0; i < n; i++) {
		ctx[i] = fake_ctx(test, &list, NULL);
		f[i][0] = fake_job(test, ctx[i], 1, 2 * i, 0x11);
		f[i][1] = fake_job(test, ctx[i], 2, 2 * i + 1, 0x22);
	}
	sgxfive_coredump_ctxs(cd, &list, ctx[n - 1]);

	KUNIT_EXPECT_EQ(test, cd->nctxs, SGXFIVE_CD_MAX_CTXS);
	KUNIT_EXPECT_EQ(test, cd->nctxs_seen, n);
	KUNIT_EXPECT_EQ(test, cd->guilty, 0);	/* kept although it is last */
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].fence_context, ctx[n - 1]->fence_context);
	KUNIT_EXPECT_EQ(test, cd->njobs, SGXFIVE_CD_MAX_JOBS);
	KUNIT_EXPECT_EQ(test, cd->ctxs[SGXFIVE_CD_MAX_CTXS - 1].npending, 2);
	KUNIT_EXPECT_EQ(test, cd->ctxs[SGXFIVE_CD_MAX_CTXS - 1].njobs, 0);

	text = dump_text(test, cd, &len);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "contexts: 10 (8 saved)\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "  2 jobs not saved\n"));

	for (i = 0; i < n; i++)
		put_jobs(ctx[i], f[i], 2);
}

/* The header lines, the device state and each job's non-zero command lines. */
static void sgxfive_coredump_test_print(struct kunit *test)
{
	struct sgxfive_client client = { .pid = 77 };
	struct sgxfive_coredump *cd = new_dump(test);
	struct sgxfive_ctx *a;
	struct dma_fence *f[1];
	LIST_HEAD(list);
	ssize_t len;
	char *text;

	strscpy(client.comm, "igt");
	a = fake_ctx(test, &list, &client);
	f[0] = fake_job(test, a, 3, 11, 0x5a);
	sgxfive_coredump_ctxs(cd, &list, a);
	cd->time.tv_sec = 1700000000;
	cd->time.tv_nsec = 5;
	cd->uptime_ns = 12ULL * NSEC_PER_SEC + 345000;
	cd->bif_int_stat = 0x1;
	cd->bif_fault = 0x3e100000;
	cd->result = 0;
	cd->dev.loaded = true;
	cd->dev.live = true;
	cd->dev.regs[0].reg = 0xc08;
	cd->dev.regs[0].val = 0x3e100000;
	cd->dev.hostctl[0] = 0x1;

	text = dump_text(test, cd, &len);
	KUNIT_EXPECT_EQ(test, strncmp(text, "**** sgxfive error state ****\n", 30), 0);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\ntime: 1700000000.000000005\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nuptime: 12.000345000\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\ndetector: bif-fault\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nsuspect: 11\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nrecovery: ok\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nBIF_INT_STAT: 0x00000001\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nBIF_FAULT: 0x3e100000\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nguilty: pid 77 comm igt\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nloaded 1 live 1 "));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nreg 0xc08 = 0x3e100000\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nhostctl[ 0]: 01 00 00 00 "));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\ncontexts: 1 (1 saved)\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, " guilty pid 77 comm igt done 0 "));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\n  job 3 order 11 size 0x1c0\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\n    cmd[0x000]: 5a 00 "));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\n    cmd[0x040]: 5a 00 "));
	KUNIT_EXPECT_NULL(test, strstr(text, "cmd[0x020]"));	/* all zero */
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\n**** end ****\n"));

	/* A manual reset blames no one; a failed one says so. */
	cd->hang = SGXFIVE_HANG_MANUAL;
	cd->suspect = U64_MAX;
	cd->guilty = -1;
	cd->result = -ETIMEDOUT;
	text = dump_text(test, cd, &len);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\ndetector: manual\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nsuspect: none\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nguilty: none\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nrecovery: failed -110\n"));
	cd->hang = SGXFIVE_HANG_TIMEOUT;
	text = dump_text(test, cd, &len);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\ndetector: job-timeout\n"));

	put_jobs(a, f, 1);
}

/* Reads in small pieces at increasing offsets give the same text. */
static void sgxfive_coredump_test_read(struct kunit *test)
{
	struct sgxfive_coredump *cd = new_dump(test);
	struct sgxfive_ctx *a;
	struct dma_fence *f[2];
	char *whole, *pieces;
	ssize_t len, off = 0, n;
	LIST_HEAD(list);

	a = fake_ctx(test, &list, NULL);
	f[0] = fake_job(test, a, 1, 1, 0x33);
	f[1] = fake_job(test, a, 2, 2, 0x44);
	sgxfive_coredump_ctxs(cd, &list, a);
	whole = dump_text(test, cd, &len);
	pieces = kunit_kzalloc(test, len + 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pieces);
	do {
		n = sgxfive_coredump_read(pieces + off, off, 37, cd, sizeof(*cd));
		KUNIT_ASSERT_GE(test, n, 0);
		KUNIT_ASSERT_LE(test, n, 37);
		off += n;
	} while (n && off <= len);
	KUNIT_EXPECT_EQ(test, off, len);
	KUNIT_EXPECT_EQ(test, memcmp(whole, pieces, len), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_coredump_read(pieces, len, 37, cd, sizeof(*cd)), 0);

	put_jobs(a, f, 2);
}

/*
 * A render context: its type and TA status word are in the dump, and its
 * job's whole CMDTA (0x2c0 bytes, more than a transfer's) is saved.
 */
static void sgxfive_coredump_test_render(struct kunit *test)
{
	struct sgxfive_coredump *cd = new_dump(test);
	struct sgxfive_fence *hw = sgxfive_fence_alloc();
	struct sgxfive_ctx *r;
	struct dma_fence *f;
	LIST_HEAD(list);
	ssize_t len;
	char *text;

	KUNIT_ASSERT_NOT_NULL(test, hw);
	r = fake_ctx(test, &list, NULL);
	r->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	memset(hw->cmd, 0, SGX_XFER_CMD_SIZE);
	hw->cmd[0x2a0] = 0x77;
	f = sgxfive_fence_init(hw, r, 3, 5, 0x2c0);
	*(u32 *)sgxfive_ctx_cpu(r, SGXFIVE_CTX_FENCE_OFF) = 2;
	*(u32 *)sgxfive_ctx_cpu(r, SGXFIVE_CTX_TA_OFF) = 3;

	sgxfive_coredump_ctxs(cd, &list, r);
	KUNIT_EXPECT_TRUE(test, cd->ctxs[0].render);
	KUNIT_EXPECT_EQ(test, cd->ctxs[0].ta_done, 3);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].size, 0x2c0);
	KUNIT_EXPECT_EQ(test, cd->jobs[0].cmd[0x2a0], 0x77);
	text = dump_text(test, cd, &len);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "render ta 3 done 2"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "cmd[0x2a0]"));
	put_jobs(r, &f, 1);
}

static struct kunit_case sgxfive_coredump_test_cases[] = {
	KUNIT_CASE(sgxfive_coredump_test_capture),
	KUNIT_CASE(sgxfive_coredump_test_bounds),
	KUNIT_CASE(sgxfive_coredump_test_print),
	KUNIT_CASE(sgxfive_coredump_test_read),
	KUNIT_CASE(sgxfive_coredump_test_render),
	{}
};

static struct kunit_suite sgxfive_coredump_test_suite = {
	.name = "gma500_sgxfive_coredump",
	.test_cases = sgxfive_coredump_test_cases,
};

kunit_test_suite(sgxfive_coredump_test_suite);
