// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the pure parts of sgxfive jobs: turning the UAPI transfer
 * register block into the firmware's SGXMKIF_TRANSFERCMD and a render
 * command into a CMDTA and render details, their checks, a render context's
 * addresses, and the client CCB a context is rebuilt with after a recovery
 * reset.
 */
#include <kunit/test.h>
#include <linux/dma-fence.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_render.h"
#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

static u32 dw(const u8 *p, unsigned int off)
{
	return p[off] | p[off + 1] << 8 | p[off + 2] << 16 | (u32)p[off + 3] << 24;
}

/* A fill's register block in the REV1 layout (design/sgx545_transfer.md §4.2). */
static void fill_regs(struct drm_sgxfive_transfer_cmd *c)
{
	memset(c, 0, sizeof(*c));
	c->isp_bgobjtag = 0x3000;
	c->isp_bgobj = 0x200;
	c->isp_bgobjdepth = 0x3f800000;
	c->isp_render = 3;
	c->isp_ipfmisc = 0x100;
	c->bif_3d_req_base = 0x3e100000;
	c->aa_mode_3d = 0x80000000;
	c->isp_multisamplectl = 0x88;
	c->event_pixel_pds_exec = 0x2e000040;
	c->event_pixel_pds_data = 5;
	c->event_pixel_pds_info = 1;
	c->isp_render_box2 = 0x00010001;
	c->pixelbe = 0x80;
	c->pds_exec_base = 0x2e000000;
}

static void sgxfive_job_test_build(struct kunit *test)
{
	struct drm_sgxfive_transfer_cmd c;
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int off, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	fill_regs(&c);
	c.load_flags = DRM_SGXFIVE_TRANSFER_LOAD_FIR;
	c.csc0_coeff[0] = 0x11;			/* lands at 0x44 */
	c.fir_hfilter[8] = 0x22;		/* lands at 0xa0 */
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgxfive_job_build(cmd, &c, false, 0x32400200, 7),
			0x1c0u);

	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0x3000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x018), 0x3e100000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x024), 0x2e000040u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x03c), 0x2e000000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x040), 0u);	/* double pixel partitions */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x044), 0x11u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0a0), 0x22u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_SIZE), 0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_STATUS), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 7u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x188), 0u);	/* load_yuv_coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x18c), 0u);	/* load_luma_key_coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x190), 1u);	/* load_fir_coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x194), 0x07c00000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x198), 7u);	/* fence id */
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_UPDATES), 0u);
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4)
		nonzero += dw(cmd, off) != 0;
	/* 14 register words + 2 coefficients + size, status x3, FIR, parts, id */
	KUNIT_EXPECT_EQ(test, nonzero, 23u);
}

static void sgxfive_job_test_dummy(struct kunit *test)
{
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_EXPECT_EQ(test, sgxfive_job_build(cmd, NULL, true, 0x32400200, 3),
			0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), (u32)SGX_XFER_FLAG_DUMMY);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 3u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x194), 0u);
}

/* Addresses the pixel pipe fetches or executes must be in a user heap. */
static void sgxfive_job_test_check(struct kunit *test)
{
	struct drm_sgxfive_transfer_cmd c;

	fill_regs(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), 0);

	c.pds_exec_base = 0x30000000;		/* the reserved firmware code */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	/* Render jobs rely on PDS_EXEC_BASE = the PDS_PIXEL heap base. */
	c.pds_exec_base = 0x2e001000;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.event_pixel_pds_exec = 0x32000000;	/* the reserved firmware data */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.bif_3d_req_base = 0x3e180000;		/* not 1 MiB aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.isp_rgn_base = 0x10000000;		/* outside every heap */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.double_pixel_partitions = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.load_flags = 1u << 3;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
}

/* The first triangle's render command (design/sgx545_3d.md §14.1). */
static void tri_cmd(struct drm_sgxfive_render_cmd *c)
{
	memset(c, 0, sizeof(*c));
	c->vdm_ctrl_stream_base = 0x3e001000;
	c->te_aa = 0x20000000;
	c->te_mtile1 = 0x01004004;
	c->te_mtile2 = 0x01004004;
	c->te_screen = 0x3001;
	c->te_mtile_stride = 0x10;
	c->mte_ctrl = 0xa2;
	c->mte_wcompare = 0x358637bd;
	c->mte_wclamp = 0x358637bd;
	c->mte_screen = 0x003f003f;
	c->mte_multisamplectl = 0x88;
	c->pixelbe = 0x80;
	c->isp_ipfmisc = 0x000f0000;
	c->aa_mode_3d = 0x80000000;
	c->isp_bgobjdepth = 0x3f800000;
	c->isp_bgobj = 0x300;
	c->isp_multisamplectl = 0x88;
	c->tsp_config = 0x30;
	c->event_pixel_pds_exec = 0x2e001080;
	c->event_pixel_pds_data = 5;
	c->event_pixel_pds_info = 1;
	c->isp_fpuctrl = 1;
	c->isp_depthsort = 0x100;
	c->bgobj_words[0] = 0x04000104;
	c->bgobj_words[1] = 0x00030000;
	c->bgobj_words[2] = 0x04000102;
}

/*
 * CREATE_CONTEXT's arguments: a known type, no flags, the one priority, zero
 * padding; a render context states its largest target, 1 to 4096 each way
 * (0 is no size), a transfer context leaves both at 0.
 */
static void sgxfive_job_test_ctx_args(struct kunit *test)
{
	const struct drm_sgxfive_create_context_args xfer = {
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER, .vm_context_handle = 1,
	};
	const struct drm_sgxfive_create_context_args render = {
		.type = DRM_SGXFIVE_CTX_TYPE_RENDER, .vm_context_handle = 1,
		.max_width = 1024, .max_height = 600,
	};
	struct drm_sgxfive_create_context_args a;

	KUNIT_EXPECT_EQ(test, sizeof(a), (size_t)32);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&xfer), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&render), 0);
	a = xfer;
	a.max_width = 64;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.max_height = 64;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a._padding_1c = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.type = 2;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.flags = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.priority = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	/* Render: the older 24-byte struct reads as 0 x 0, which is refused. */
	a = render;
	a.max_width = 0;
	a.max_height = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_width = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_height = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_width = 4097;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_height = 4097;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_width = 4096;
	a.max_height = 4096;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), 0);
	a.max_width = 1;
	a.max_height = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), 0);
	a._padding_1c = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
}

/*
 * The checks on a render command: the addresses the TA and the pixel pipe
 * fetch from or run code at must be in the client's heaps (render VMs hold
 * the firmware alias), aligned; the render target fits the kernel's buffers.
 */
static void sgxfive_job_test_render_check(struct kunit *test)
{
	struct drm_sgxfive_render_cmd c;

	tri_cmd(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), 0);
	c.vdm_ctrl_stream_base = 0x32000000;	/* the reserved firmware data */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.vdm_ctrl_stream_base = 0x10001000;	/* a render context's PB */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.vdm_ctrl_stream_base = 0x3e001002;	/* not 4-byte aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.vdm_ctrl_stream_base = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	tri_cmd(&c);
	c.event_pixel_pds_exec = 0x30000000;	/* the reserved firmware code */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.event_pixel_pds_exec = 0x2e001088;	/* not 16-byte aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	tri_cmd(&c);
	c.mte_screen = 0x01ff01ff;		/* 512 x 512 */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	/* The context's maximum: 64 x 64 fits 64 x 64, not 63 or 32 across. */
	tri_cmd(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 64, 64), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 63, 64), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 64, 32), -EINVAL);
	tri_cmd(&c);
	c.isp_bgobj = 0x400;
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
}

/*
 * A render context's addresses: its firmware structures in its kernel memory
 * (struct sgxfive_ctx's page after the TA CCB), its slots in the reserved
 * PARAMETERS and TA_DATA heaps.
 */
static void sgxfive_job_test_render_addrs(struct kunit *test)
{
	struct sgx_render_addrs a;
	struct sgx_render_tadata td;
	const u32 m = 0x32500000, pg = m + SGXFIVE_CTX_PAGE_OFF;

	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 1024, 600), 0);
	sgxfive_render_addrs(&a, m, 0x01234000, 3, &td);
	KUNIT_EXPECT_EQ(test, a.pd_phys, 0x01234000u);
	KUNIT_EXPECT_EQ(test, a.param_base, 0x10000000u + 3 * SZ_8M);
	KUNIT_EXPECT_EQ(test, a.tadata, 0x20000000u + 3 * SZ_2M);
	/* The TA data of the context's maximum, inside its slot. */
	KUNIT_EXPECT_EQ(test, a.td.rgn, td.rgn);
	KUNIT_EXPECT_EQ(test, a.td.zls, td.zls);
	KUNIT_EXPECT_EQ(test, a.td.size, 0x11000u);
	KUNIT_EXPECT_LE(test, SGX_RENDER_TD_MAX_SIZE, SGXFIVE_RENDER_TD_SLOT);
	KUNIT_EXPECT_EQ(test, a.ccb, m + SGXFIVE_CTX_CCB_OFF);
	KUNIT_EXPECT_EQ(test, a.ctx, m + SGXFIVE_CTX_HW_OFF);
	KUNIT_EXPECT_EQ(test, a.ccb_ctl, m + SGXFIVE_CTX_CTL_OFF);
	KUNIT_EXPECT_EQ(test, a.td_status, m + SGXFIVE_CTX_FENCE_OFF);
	KUNIT_EXPECT_EQ(test, a.ta_status, m + SGXFIVE_CTX_TA_OFF);
	KUNIT_EXPECT_EQ(test, a.rtds, m + SGXFIVE_RCTX_RTDS_OFF);
	KUNIT_EXPECT_EQ(test, a.pbdesc, m + SGXFIVE_RCTX_PBDESC_OFF);
	KUNIT_EXPECT_EQ(test, a.pbblock, m + SGXFIVE_RCTX_PBBLOCK_OFF);
	KUNIT_EXPECT_EQ(test, a.rd, m + SGXFIVE_RCTX_RD_OFF);
	/* Everything fits the context's one page, without overlaps. */
	KUNIT_EXPECT_LE(test, SGXFIVE_CTX_HW_OFF + SGX_RENDER_CTX_SIZE,
			SGXFIVE_CTX_CTL_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_BGPRI_OFF + 4, SGXFIVE_RCTX_RTDS_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_RTDS_OFF + SGX_RENDER_RTDS_SIZE,
			SGXFIVE_RCTX_PBDESC_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_PBDESC_OFF + SGX_RENDER_PBDESC_SIZE,
			SGXFIVE_RCTX_PBBLOCK_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_PBBLOCK_OFF + SGX_RENDER_PBBLOCK_SIZE,
			SGXFIVE_RCTX_RD_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_RD_OFF + SGX_RENDER_RD_SIZE, pg + PAGE_SIZE - m);
	KUNIT_EXPECT_EQ(test, a.access, m + SGXFIVE_RCTX_ACCESS_OFF);
	KUNIT_EXPECT_EQ(test, a.bgpri, m + SGXFIVE_RCTX_BGPRI_OFF);
	/* The TA CCB has a command's room past its 64 KiB. */
	KUNIT_EXPECT_GE(test, SGXFIVE_CTX_PAGE_OFF,
			SGX_RENDER_CCB_SIZE + SGX_RENDER_CMD_STRIDE);
}

/*
 * A render job's firmware image, kept in its hardware fence: the CMDTA (one
 * whole scene, the seqno as frame and as both status values) and, after it,
 * the render details.
 */
static void sgxfive_job_test_render_build(struct kunit *test)
{
	struct drm_sgxfive_render_cmd c;
	struct sgx_render_addrs a;
	struct sgx_render_tadata td;
	u8 *img = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	u32 *rd = kunit_kzalloc(test, SGX_RENDER_RD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, img);
	KUNIT_ASSERT_NOT_NULL(test, rd);
	KUNIT_EXPECT_LE(test, SGXFIVE_RENDER_IMG_RD + SGX_RENDER_RD_SIZE,
			SGX_XFER_CMD_SIZE);
	tri_cmd(&c);
	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 64, 64), 0);
	sgxfive_render_addrs(&a, 0x32500000, 0x01234000, 0, &td);
	memset(img, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgxfive_render_build(img, &a, &c, 9),
			(u32)SGX_RENDER_CMD_STRIDE);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_FLAGS), 0x73u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_RFLAGS), 0x400u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_FRAME), 9u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_TA_STATUS), a.ta_status);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_TA_STATUS + 4), 9u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_3D_STATUS), a.td_status);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_3D_STATUS + 4), 9u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_TAREGS + SGX_RENDER_TA_TEPSG),
			0x400000u);
	sgx_render_rd(rd, &a, &c);
	KUNIT_EXPECT_EQ(test, memcmp(img + SGXFIVE_RENDER_IMG_RD, rd, SGX_RENDER_RD_SIZE),
			0);
}

/* A fence is done once the context's fence word has reached its number. */
static void sgxfive_job_test_seqno(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, sgxfive_seqno_passed(5, 5));
	KUNIT_EXPECT_TRUE(test, sgxfive_seqno_passed(6, 5));
	KUNIT_EXPECT_FALSE(test, sgxfive_seqno_passed(4, 5));
	KUNIT_EXPECT_FALSE(test, sgxfive_seqno_passed(0, 1));	/* nothing yet */
	/* Across the 32-bit wrap. */
	KUNIT_EXPECT_TRUE(test, sgxfive_seqno_passed(2, 0xfffffffe));
	KUNIT_EXPECT_FALSE(test, sgxfive_seqno_passed(0xfffffffe, 2));
}

/* A context with its memory in a plain buffer, as far as fences need one. */
static struct sgxfive_ctx *fake_ctx(struct kunit *test)
{
	struct sgxfive_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->mem.va = kunit_kzalloc(test, SGXFIVE_CTX_MEM_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->mem.va);
	ctx->fence_context = dma_fence_context_alloc(1);
	spin_lock_init(&ctx->fence_lock);
	INIT_LIST_HEAD(&ctx->pending);
	return ctx;
}

/*
 * After a reset the client CCB holds the context's unfinished commands from
 * offset 0, in order, and the fence word the last job it finished.
 */
static void sgxfive_job_test_rewind(struct kunit *test)
{
	struct sgxfive_ctx *ctx = fake_ctx(test);
	u8 *ccb = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF);
	u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	u32 *done = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF);
	struct dma_fence *f[3];
	u8 *cmd[3];
	u64 order;
	int i;

	for (i = 0; i < 3; i++) {
		struct sgxfive_fence *hw = sgxfive_fence_alloc();

		KUNIT_ASSERT_NOT_NULL(test, hw);
		cmd[i] = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, cmd[i]);
		memset(cmd[i], 0x11 * (i + 1), SGX_XFER_CMD_SIZE);
		memcpy(hw->cmd, cmd[i], SGX_XFER_CMD_SIZE);
		f[i] = sgxfive_fence_init(hw, ctx, i + 1, 10 + i, 0x100);
	}
	KUNIT_EXPECT_TRUE(test, sgxfive_ctx_oldest(ctx, &order));
	KUNIT_EXPECT_EQ(test, order, 10);

	/* The first job completes. */
	*done = 1;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_TRUE(test, dma_fence_is_signaled(f[0]));
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	KUNIT_EXPECT_TRUE(test, sgxfive_ctx_oldest(ctx, &order));
	KUNIT_EXPECT_EQ(test, order, 11);

	/* Stale state from before the reset. */
	ctx->woff = 0x300;
	ctl[0] = 0x300;
	ctl[1] = 0x200;
	memset(ccb, 0xee, 0x400);
	*done = 0xdead;

	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(ctx), 2);
	KUNIT_EXPECT_EQ(test, ctx->woff, 0x200);
	KUNIT_EXPECT_EQ(test, ctl[0], 0x200);
	KUNIT_EXPECT_EQ(test, ctl[1], 0);
	KUNIT_EXPECT_EQ(test, *done, 1);
	KUNIT_EXPECT_EQ(test, memcmp(ccb, cmd[1], 0x100), 0);
	KUNIT_EXPECT_EQ(test, memcmp(ccb + 0x100, cmd[2], SGX_XFER_CMD_SIZE), 0);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[2]));

	/* A guilty context's jobs fail instead. */
	sgxfive_fence_fail_all(ctx, -EIO);
	for (i = 1; i < 3; i++) {
		KUNIT_EXPECT_TRUE(test, dma_fence_is_signaled(f[i]));
		KUNIT_EXPECT_EQ(test, f[i]->error, -EIO);
	}
	KUNIT_EXPECT_FALSE(test, sgxfive_ctx_oldest(ctx, &order));
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(ctx), 0);
	KUNIT_EXPECT_EQ(test, ctx->woff, 0);
	for (i = 0; i < 3; i++)
		dma_fence_put(f[i]);
}

/* A job of order on ctx, unfinished, its fence returned. */
static struct dma_fence *fake_job(struct kunit *test, struct sgxfive_ctx *ctx, u32 seqno,
				  u64 order)
{
	struct sgxfive_fence *hw = sgxfive_fence_alloc();

	if (!hw)
		return NULL;
	return sgxfive_fence_init(hw, ctx, seqno, order, SGX_XFER_CMD_SIZE);
}

/*
 * Whom a hang blames. A BIF fault names a page directory: the context to blame
 * is the one with the oldest unfinished job among the contexts on that
 * directory (one VM). Without a directory (the EDM's own list, a directory no
 * context uses, or one whose contexts have nothing unfinished) it is the owner
 * of the oldest unfinished job of all, as before.
 */
static void sgxfive_job_test_suspect(struct kunit *test)
{
	struct sgxfive_ctx *a = fake_ctx(test), *b = fake_ctx(test), *c = fake_ctx(test);
	struct dma_fence *f[4];
	LIST_HEAD(ctxs);
	u64 order;
	int i;

	a->pd_phys = 0x01234000;	/* a render context and ... */
	c->pd_phys = 0x01234000;	/* ... a transfer context in one VM */
	b->pd_phys = 0x02345000;	/* a transfer context in another */
	list_add_tail(&a->link, &ctxs);
	list_add_tail(&b->link, &ctxs);
	list_add_tail(&c->link, &ctxs);

	order = 1;
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_suspect(&ctxs, 0, &order));
	KUNIT_EXPECT_EQ(test, order, U64_MAX);
	f[0] = fake_job(test, a, 1, 10);	/* the render, queued first */
	f[1] = fake_job(test, b, 1, 11);	/* the faulting transfer */
	f[2] = fake_job(test, c, 1, 12);
	f[3] = fake_job(test, a, 2, 13);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_NOT_NULL(test, f[i]);

	/* By age: the render. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);
	/* By the fault's directory: the transfer. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x02345000, &order), b);
	KUNIT_EXPECT_EQ(test, order, 11);
	/* Two contexts on the directory: the older job of the two. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x01234000, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);
	/* A directory no context uses: by age. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x03456000, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);

	/* The job a suspect order names, while it is its context's oldest. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_of_job(&ctxs, 11), b);
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_of_job(&ctxs, 10), a);
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_of_job(&ctxs, 12), c);
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, 13));	/* a has 10 first */
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, 14));
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, U64_MAX));

	/* Once the transfer is failed (recovered), its directory has nothing left. */
	sgxfive_fence_fail_all(b, -EIO);
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, 11));
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x02345000, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);
	sgxfive_fence_fail_all(a, -EIO);
	sgxfive_fence_fail_all(c, -EIO);
	for (i = 0; i < 4; i++)
		dma_fence_put(f[i]);
}

/* Three faults within a minute ban the file; older faults do not count. */
static void sgxfive_job_test_budget(struct kunit *test)
{
	struct sgxfive_client c = { }, w = { };
	unsigned long t = 1000, u = ULONG_MAX - HZ;

	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&c, t));
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&c, t + 61 * HZ));
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&c, t + 62 * HZ));
	KUNIT_EXPECT_FALSE(test, c.banned);
	KUNIT_EXPECT_TRUE(test, sgxfive_client_fault(&c, t + 63 * HZ));
	KUNIT_EXPECT_TRUE(test, c.banned);

	/* Across the jiffies wrap. */
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&w, u));
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&w, u + HZ));
	KUNIT_EXPECT_TRUE(test, sgxfive_client_fault(&w, u + 2 * HZ));
}

static struct kunit_case sgxfive_job_test_cases[] = {
	KUNIT_CASE(sgxfive_job_test_build),
	KUNIT_CASE(sgxfive_job_test_dummy),
	KUNIT_CASE(sgxfive_job_test_check),
	KUNIT_CASE(sgxfive_job_test_ctx_args),
	KUNIT_CASE(sgxfive_job_test_render_check),
	KUNIT_CASE(sgxfive_job_test_render_addrs),
	KUNIT_CASE(sgxfive_job_test_render_build),
	KUNIT_CASE(sgxfive_job_test_seqno),
	KUNIT_CASE(sgxfive_job_test_rewind),
	KUNIT_CASE(sgxfive_job_test_suspect),
	KUNIT_CASE(sgxfive_job_test_budget),
	{}
};

static struct kunit_suite sgxfive_job_test_suite = {
	.name = "gma500_sgxfive_job",
	.test_cases = sgxfive_job_test_cases,
};

kunit_test_suite(sgxfive_job_test_suite);
