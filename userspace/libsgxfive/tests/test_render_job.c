// SPDX-License-Identifier: MIT
/*
 * Render contexts and render jobs against the mock (design/sgx545_render_uapi.md
 * §11): what DEV_QUERY reports, the kernel's rules on contexts, commands and
 * reserved heaps, the triangle rendered by the mock's interpreter and checked
 * pixel by pixel, a faulting render losing its context, transfers keeping
 * PDS_EXEC_BASE at the PDS_PIXEL heap base, and transfers and renders
 * interleaved in one VM.
 */
#include <errno.h>
#include <string.h>

#include "sgxfive_mock.h"
#include "sgxfive_render.h"
#include "sgxfive_xfer.h"
#include "tap.h"

#define NSEC 1000000000LL

struct env {
	struct sgxfive_mock *mock;
	struct sgxfive_dev *dev;
	struct sgxfive_vm *vm;
	uint32_t ctx;
	struct sgxfive_render *r;
};

static void env_init(struct env *e)
{
	const struct sgxfive_mock_config cfg = { .layout = SGXFIVE_MOCK_LAYOUT_REV1 };

	memset(e, 0, sizeof(*e));
	e->mock = sgxfive_mock_create(&cfg);
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(e->mock), NULL, &e->dev), 0);
	CHECK_EQ(sgxfive_vm_create(e->dev, &e->vm), 0);
	CHECK_EQ(sgxfive_render_ctx_create(e->vm, SGXFIVE_TRI_MAX_W, SGXFIVE_TRI_MAX_H, &e->ctx),
		 0);
	CHECK_EQ(sgxfive_render_create(e->vm, e->ctx, &e->r), 0);
}

static void env_fini(struct env *e)
{
	sgxfive_render_destroy(e->r);
	sgxfive_ctx_destroy(e->dev, e->ctx);
	sgxfive_vm_destroy(e->vm);
	sgxfive_dev_close(e->dev);
	sgxfive_mock_destroy(e->mock);
}

static void query(void)
{
	struct env e;
	const struct sgxfive_info *info;
	const struct drm_sgxfive_heap *h;

	env_init(&e);
	info = sgxfive_dev_info(e.dev);
	CHECK(info->job.job_types & DRM_SGXFIVE_JOB_TYPE_RENDER_BIT);
	CHECK(info->job.job_types & DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT);
	CHECK_EQ(info->job.render_cmd_size, DRM_SGXFIVE_RENDER_CMD_SIZE_V1);
	CHECK_EQ(info->job.render_pb_pages, 0x422);
	CHECK_EQ(info->job.render_max_width, 4096);
	CHECK_EQ(info->job.render_max_height, 4096);
	CHECK_EQ(info->num_heaps, DRM_SGXFIVE_HEAP_COUNT);
	h = &info->heaps[DRM_SGXFIVE_HEAP_PARAMETERS];
	CHECK_EQ(h->base, 0x10000000);
	CHECK(h->flags & DRM_SGXFIVE_HEAP_RESERVED);
	h = &info->heaps[DRM_SGXFIVE_HEAP_TA_DATA];
	CHECK_EQ(h->base, 0x20000000);
	CHECK(h->flags & DRM_SGXFIVE_HEAP_RESERVED);
	env_fini(&e);
}

/* The triangle at several sizes: every pixel, the guard area, and twice in a row. */
static void triangle(void)
{
	static const uint32_t sizes[][2] = {
		{ 64, 64 }, { 128, 64 }, { 256, 128 }, { 32, 16 }, { 128, 192 }, { 256, 256 },
		{ 100, 60 }, { 1024, 768 }, { 32, 16 },
	};
	struct sgxfive_tri_counts n;
	struct sgxfive_mock_stats st;
	struct sgxfive_tri_params p;
	struct env e;

	env_init(&e);
	for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		sgxfive_tri_default(&p, sizes[i][0], sizes[i][1]);
		CHECK_EQ(sgxfive_render_tri(e.r, &p, 0, 0), 0);
		CHECK_EQ(sgxfive_render_wait(e.r, NSEC), 0);
		CHECK_EQ(sgxfive_tri_check(&p, sgxfive_render_fb(e.r), &n), 0);
		CHECK(n.tri > 0 && n.bg > 0);
		CHECK_EQ(n.other, 0);
		CHECK_EQ(sgxfive_render_guard_dirty(e.r, &p), 0);
	}
	/* Another colour on the same context. */
	sgxfive_tri_default(&p, 64, 64);
	p.colour = 0xffff0000;
	p.bg_colour = 0xff000080;
	CHECK_EQ(sgxfive_render_tri(e.r, &p, 0, 0), 0);
	CHECK_EQ(sgxfive_render_wait(e.r, NSEC), 0);
	CHECK_EQ(sgxfive_tri_check(&p, sgxfive_render_fb(e.r), &n), 0);
	sgxfive_mock_get_stats(e.mock, &st);
	CHECK_EQ(st.renders_run, 10);
	CHECK_EQ(st.faults, 0);
	env_fini(&e);
}

/* The kernel's rules on render contexts, commands and the reserved heaps. */
static void rules(void)
{
	struct drm_sgxfive_render_cmd cmd;
	struct sgxfive_tri_params p;
	struct sgxfive_bo *bo = NULL;
	uint32_t xctx, ctxs[32], n = 0, t;
	struct env e;
	int ret;

	env_init(&e);
	sgxfive_tri_default(&p, 64, 64);
	CHECK_EQ(sgxfive_render_tri(e.r, &p, 0, 0), 0);
	CHECK_EQ(sgxfive_render_wait(e.r, NSEC), 0);
	cmd = *sgxfive_render_last_cmd(e.r);

	/* Job and context types must match. */
	CHECK_EQ(sgxfive_ctx_create(e.vm, &xctx), 0);
	CHECK_EQ(sgxfive_submit_render(e.dev, xctx, &cmd, 0, 0), -EINVAL);
	CHECK_EQ(sgxfive_submit_transfer(e.dev, e.ctx, DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY,
					 NULL, 0, 0), -EINVAL);
	/* Fetch addresses in user heaps, aligned; the target within the limit. */
	cmd.vdm_ctrl_stream_base = 0x32000000;
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.vdm_ctrl_stream_base += 2;
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.event_pixel_pds_exec = 0x10001000;	/* the PARAMETERS heap */
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.mte_screen = 511 | 63 << 16;	/* tiling words not of this size */
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.te_mtile2 = 0x02008008;
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.mte_screen = 4096 | 63 << 16;	/* 4097 across */
	sgxfive_tiling_cmd(&cmd, 4097, 64);
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.isp_bgobj = 0x1300;
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -EINVAL);
	/* No render flags yet; a short command. */
	{
		struct drm_sgxfive_job job = {
			.type = DRM_SGXFIVE_JOB_TYPE_RENDER, .context_handle = e.ctx,
			.flags = 1, .cmd_stream = (uintptr_t)sgxfive_render_last_cmd(e.r),
			.cmd_stream_len = DRM_SGXFIVE_RENDER_CMD_SIZE_V1,
		};

		CHECK_EQ(sgxfive_submit(e.dev, &job, 1, NULL), -EINVAL);
		job.flags = 0;
		job.cmd_stream_len = DRM_SGXFIVE_RENDER_CMD_SIZE_V1 - 8;
		CHECK_EQ(sgxfive_submit(e.dev, &job, 1, NULL), -EINVAL);
	}
	/* The reserved render heaps cannot be mapped. */
	CHECK_EQ(sgxfive_bo_create(e.dev, 4096, 0, &bo), 0);
	CHECK_EQ(sgxfive_vm_map(e.vm, 0x10000000, bo, 0, 4096, 0), -EINVAL);
	CHECK_EQ(sgxfive_vm_map(e.vm, 0x20000000, bo, 0, 4096, 0), -EINVAL);
	sgxfive_bo_destroy(bo);
	/* 32 render contexts per VM: this one and 31 more, then -ENOSPC. */
	while (n < 32 && !(ret = sgxfive_render_ctx_create(e.vm, 64, 64, &ctxs[n])))
		n++;
	CHECK_EQ(n, 31);
	CHECK_EQ(ret, -ENOSPC);
	CHECK_EQ(sgxfive_ctx_destroy(e.dev, ctxs[0]), 0);
	CHECK_EQ(sgxfive_render_ctx_create(e.vm, 64, 64, &t), 0);
	ctxs[0] = t;
	for (uint32_t i = 0; i < n; i++)
		sgxfive_ctx_destroy(e.dev, ctxs[i]);
	/* An unknown context type. */
	CHECK_EQ(sgxfive_ctx_create_type(e.vm, 2, &t), -EINVAL);
	/*
	 * A render context states its largest target, 1 to 4096 each way: none
	 * (what a caller of the older 24-byte struct sends), 0 or 4097 is refused.
	 */
	CHECK_EQ(sgxfive_ctx_create_type(e.vm, DRM_SGXFIVE_CTX_TYPE_RENDER, &t), -EINVAL);
	CHECK_EQ(sgxfive_render_ctx_create(e.vm, 0, 64, &t), -EINVAL);
	CHECK_EQ(sgxfive_render_ctx_create(e.vm, 64, 0, &t), -EINVAL);
	CHECK_EQ(sgxfive_render_ctx_create(e.vm, 4097, 64, &t), -EINVAL);
	CHECK_EQ(sgxfive_render_ctx_create(e.vm, 64, 4097, &t), -EINVAL);
	sgxfive_ctx_destroy(e.dev, xctx);
	env_fini(&e);
}

/* A render whose stream is mapped nowhere faults: the context is lost. */
static void fault(void)
{
	struct drm_sgxfive_render_cmd cmd;
	struct sgxfive_mock_stats st;
	struct sgxfive_tri_params p;
	struct sgxfive_tri_counts n;
	struct sgxfive_render *r2;
	uint32_t ctx2, sync;
	struct env e;

	env_init(&e);
	sgxfive_tri_default(&p, 64, 64);
	CHECK_EQ(sgxfive_render_tri(e.r, &p, 0, 0), 0);
	CHECK_EQ(sgxfive_render_wait(e.r, NSEC), 0);
	cmd = *sgxfive_render_last_cmd(e.r);
	cmd.vdm_ctrl_stream_base = 0x7e000000;	/* GENERAL, unmapped */
	CHECK_EQ(sgxfive_syncobj_create(e.dev, false, &sync), 0);
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, sync), 0);
	sgxfive_mock_get_stats(e.mock, &st);
	CHECK_EQ(st.faults, 1);
	CHECK_EQ(st.last_fault_addr, 0x7e000000);
	CHECK_EQ(sgxfive_submit_render(e.dev, e.ctx, &cmd, 0, 0), -ECANCELED);
	/* Another render context of the file still renders. */
	CHECK_EQ(sgxfive_render_ctx_create(e.vm, 64, 64, &ctx2), 0);
	CHECK_EQ(sgxfive_render_create(e.vm, ctx2, &r2), 0);
	CHECK_EQ(sgxfive_render_tri(r2, &p, 0, 0), 0);
	CHECK_EQ(sgxfive_render_wait(r2, NSEC), 0);
	CHECK_EQ(sgxfive_tri_check(&p, sgxfive_render_fb(r2), &n), 0);
	sgxfive_render_destroy(r2);
	sgxfive_ctx_destroy(e.dev, ctx2);
	sgxfive_syncobj_destroy(e.dev, sync);
	env_fini(&e);
}

/*
 * Transfers keep PDS_EXEC_BASE at the PDS_PIXEL heap base: the kernel refuses
 * another value, and a second engine in the VM (whose PDS page is not the
 * first) still fills correctly; fills and renders interleave in one VM.
 */
static void transfers(void)
{
	const struct sgxfive_rect rect = { 4, 2, 40, 18 };
	struct sgxfive_prog_source *src = sgxfive_prog_asm_create();
	struct drm_sgxfive_transfer_cmd xcmd;
	struct sgxfive_xfer *x1, *x2;
	struct sgxfive_tri_params p;
	struct sgxfive_tri_counts n;
	struct sgxfive_bo *bo = NULL;
	struct sgxfive_xfer_layout l;
	uint32_t xctx, *dst = NULL, bad = 0;
	uint64_t addr;
	struct env e;

	env_init(&e);
	CHECK_EQ(sgxfive_ctx_create(e.vm, &xctx), 0);
	CHECK_EQ(sgxfive_xfer_create(e.vm, xctx, src, &x1), 0);
	CHECK_EQ(sgxfive_xfer_create(e.vm, xctx, src, &x2), 0);
	sgxfive_xfer_get_layout(x2, &l);
	CHECK(l.pds_page != 0x2e000000);
	CHECK_EQ(sgxfive_bo_create(e.dev, 4 * 4096, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, &bo), 0);
	CHECK_EQ(sgxfive_bo_mmap(bo, (void **)&dst), 0);
	CHECK_EQ(sgxfive_vm_alloc(e.vm, DRM_SGXFIVE_HEAP_GENERAL, 4 * 4096, 4096, &addr), 0);
	CHECK_EQ(sgxfive_vm_map(e.vm, addr, bo, 0, 4 * 4096, 0), 0);
	sgxfive_tri_default(&p, 64, 64);
	for (int round = 0; round < 2; round++) {
		const struct sgxfive_surface s = { addr, 64, 64, 256 };
		struct sgxfive_xfer *x = round ? x2 : x1;
		uint32_t colour = round ? 0xff123456 : 0xff654321;

		memset(dst, 0, 4 * 4096);
		CHECK_EQ(sgxfive_xfer_fill(x, &s, &rect, colour, 0), 0);
		CHECK_EQ(sgxfive_render_tri(e.r, &p, 0, 0), 0);
		CHECK_EQ(sgxfive_xfer_wait(x, NSEC), 0);
		CHECK_EQ(sgxfive_render_wait(e.r, NSEC), 0);
		CHECK_EQ(sgxfive_xfer_last_cmd(x)->pds_exec_base, 0x2e000000);
		for (uint32_t y = 0; y < 64; y++)
			for (uint32_t xx = 0; xx < 64; xx++) {
				bool in = xx >= rect.x0 && xx < rect.x1 && y >= rect.y0 &&
					  y < rect.y1;

				bad += dst[y * 64 + xx] != (in ? colour : 0);
			}
		CHECK_EQ(bad, 0);
		CHECK_EQ(sgxfive_tri_check(&p, sgxfive_render_fb(e.r), &n), 0);
	}
	/* Any other PDS_EXEC_BASE is refused. */
	xcmd = *sgxfive_xfer_last_cmd(x2);
	xcmd.pds_exec_base = (uint32_t)l.pds_page;
	CHECK_EQ(sgxfive_submit_transfer(e.dev, xctx, 0, &xcmd, 0, 0), -EINVAL);
	sgxfive_xfer_destroy(x1);
	sgxfive_xfer_destroy(x2);
	sgxfive_vm_unmap(e.vm, addr, 4 * 4096);
	sgxfive_bo_destroy(bo);
	sgxfive_prog_source_destroy(src);
	sgxfive_ctx_destroy(e.dev, xctx);
	env_fini(&e);
}

/*
 * A context renders any target within the maximum it was created with, each
 * dimension on its own, and refuses a job for a larger one without losing
 * the context.
 */
static void max_size(void)
{
	static const uint32_t fits[][4] = {	/* max w, h; target w, h */
		{ 64, 64, 64, 64 }, { 64, 64, 32, 16 }, { 64, 64, 2, 2 },
		{ 1024, 600, 1024, 600 }, { 1024, 600, 600, 600 }, { 1024, 600, 640, 480 },
		{ 1024, 600, 640, 478 }, { 100, 60, 100, 60 },
	};
	static const uint32_t over[][4] = {
		{ 64, 64, 66, 64 }, { 64, 64, 64, 66 }, { 1024, 600, 600, 1024 },
		{ 1024, 600, 2048, 16 }, { 100, 60, 100, 62 },
	};
	struct sgxfive_tri_counts n;
	struct sgxfive_tri_params p;
	struct sgxfive_render *r;
	uint32_t ctx;
	struct env e;

	env_init(&e);
	for (unsigned int i = 0; i < sizeof(fits) / sizeof(fits[0]); i++) {
		CHECK_EQ(sgxfive_render_ctx_create(e.vm, fits[i][0], fits[i][1], &ctx), 0);
		CHECK_EQ(sgxfive_render_create(e.vm, ctx, &r), 0);
		sgxfive_tri_default(&p, fits[i][2], fits[i][3]);
		CHECK_EQ(sgxfive_render_tri(r, &p, 0, 0), 0);
		CHECK_EQ(sgxfive_render_wait(r, NSEC), 0);
		CHECK_EQ(sgxfive_tri_check(&p, sgxfive_render_fb(r), &n), 0);
		CHECK_EQ(n.other, 0);
		sgxfive_render_destroy(r);
		sgxfive_ctx_destroy(e.dev, ctx);
	}
	for (unsigned int i = 0; i < sizeof(over) / sizeof(over[0]); i++) {
		CHECK_EQ(sgxfive_render_ctx_create(e.vm, over[i][0], over[i][1], &ctx), 0);
		CHECK_EQ(sgxfive_render_create(e.vm, ctx, &r), 0);
		sgxfive_tri_default(&p, over[i][2], over[i][3]);
		CHECK_EQ(sgxfive_render_tri(r, &p, 0, 0), -EINVAL);
		/* Refused at submission: the context still renders what fits. */
		sgxfive_tri_default(&p, over[i][0], over[i][1]);
		CHECK_EQ(sgxfive_render_tri(r, &p, 0, 0), 0);
		CHECK_EQ(sgxfive_render_wait(r, NSEC), 0);
		CHECK_EQ(sgxfive_tri_check(&p, sgxfive_render_fb(r), &n), 0);
		sgxfive_render_destroy(r);
		sgxfive_ctx_destroy(e.dev, ctx);
	}
	env_fini(&e);
}

int main(void)
{
	query();
	triangle();
	rules();
	max_size();
	fault();
	transfers();
	return tap_done();
}
