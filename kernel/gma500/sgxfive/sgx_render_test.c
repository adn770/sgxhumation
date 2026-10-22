// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_render.c: the kernel-owned side of a TA + 3D job. Field
 * placement is checked against the dword offsets design/sgx545_3d.md §4-§12
 * cites (CMDTA dwN, rd dwN, rtdata dwN, ctx dwN, pb dwN). The fixture is the
 * first triangle as it rendered on the hardware (§14): its addresses (the
 * debugfs test's REV1 layout) and its register words, so the values below are
 * the ones the laptop ran.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_render.h"

#define KD	0x32152000u	/* the debugfs test's kernel-data page */

/* The TA-data layout for 4096 x 4096: the fixed offsets patches 51-52 ran. */
#define TD_4096	{ .tail = 0, .control = 0x40000, .otpm = 0x41000, .state = 0x42000, \
		  .rgn = 0x43000, .zls = 0xa3000, .size = 0x103000 }

static const struct sgx_render_addrs addrs = {
	.pd_phys = 0x039ea000, .param_base = 0x10000000, .tadata = 0x20001000,
	.td = TD_4096,
	.ctx = KD + 0x100, .ccb = 0x32140000, .ccb_ctl = KD + 0x000,
	.rtds = KD + 0x200, .pbdesc = KD + 0x300, .pbblock = KD + 0x340,
	.rd = KD + 0x800, .ta_status = KD + 0x400, .td_status = KD + 0x404,
	.access = KD + 0x408, .pending = KD + 0x40c, .rt_status = KD + 0x410,
	.bgpri = KD + 0x414,
};

/* The first triangle's register words (§14.1, §6, §7, S.6). */
static const struct drm_sgxfive_render_cmd tri = {
	.vdm_ctrl_stream_base = 0x3e001000, .te_aa = 0x20000000,
	.te_mtile1 = 0x01004004, .te_mtile2 = 0x01004004, .te_screen = 0x3001,
	.te_mtile_stride = 0x10, .mte_ctrl = 0xa2, .mte_wcompare = 0x358637bd,
	.mte_wclamp = 0x358637bd, .mte_screen = 0x003f003f,
	.mte_multisamplectl = 0x88, .pixelbe = 0x80, .isp_ipfmisc = 0x000f0000,
	.aa_mode_3d = 0x80000000, .isp_bgobjdepth = 0x3f800000, .isp_bgobj = 0x300,
	.isp_multisamplectl = 0x88, .tsp_config = 0x30,
	.event_pixel_pds_exec = 0x2e001080, .event_pixel_pds_data = 5,
	.event_pixel_pds_info = 1, .isp_fpuctrl = 1, .isp_depthsort = 0x100,
	.bgobj_words = { 0x04000104, 0x00030000, 0x04000102 },
};

static u32 dw(const void *p, unsigned int off)
{
	const u8 *b = p;

	return b[off] | b[off + 1] << 8 | b[off + 2] << 16 | (u32)b[off + 3] << 24;
}

/* Every offset the EDM trace names (§12.2-§12.4) lands on our field. */
static void sgx_render_test_offsets(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_VPART, 4 * 5);	/* CMDTA dw5 */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_RD, 4 * 14);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_DSTSYNC, 4 * 15);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_RTDATA, 4 * 17);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_SH_TATQ, 4 * 25);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_TAREGS, 4 * 142);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_3DREGUPD, 4 * 170);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_SIZE, SGX_RENDER_CMD_3DREGUPD + 8);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CMD_STRIDE, ALIGN(SGX_RENDER_CMD_SIZE, 64));
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RD_TQSYNC, 4 * 57);	/* rd dw57 */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RD_NEXTRT, 4 * 66);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RD_BGPRI_LIST, 4 * 69);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RD_TESTATE, 4 * 73);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RD_ACCESS, 4 * 52);	/* after 52 registers */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RD_DEPTHSORT, 4 * 51);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RT_RD, 4 * 1);		/* rtdata dw1 */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RT_STATE, 4 * 5);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RT_CONTROL, 4 * 6);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RT_OTPM, 4 * 7);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RT_COMMON, 4 * 13);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RTDS_RT, 0x18);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_RTDS_SIZE, 0x18 + 0x38);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CTX_TACOUNT, 4 * 5);	/* ctx dw5 */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CTX_PBDESC, 4 * 6);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CTX_PARTIAL, 4 * 8);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_CTX_COMPLETE, 4 * 10);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_PB_ALLOC, 4 * 5);	/* pb dw5 */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_PB_TA_THRESH, 4 * 10);
	/* The parameter slot's items do not overlap and fit 8 MiB. */
	KUNIT_EXPECT_EQ(test, SGX_RENDER_PARAM_PB + SGX_RENDER_PB_PAGES * 4096,
			SGX_RENDER_PARAM_PT);
	KUNIT_EXPECT_EQ(test, SGX_RENDER_PARAM_PT + SGX_RENDER_PT_BYTES,
			SGX_RENDER_PARAM_PRIM);
	KUNIT_EXPECT_LE(test, SGX_RENDER_PARAM_BG + 4096, SGX_RENDER_PARAM_SIZE);
	KUNIT_EXPECT_LE(test, SGX_RENDER_PARAM_SIZE, SZ_8M);
	/* The UAPI command: version 1 and the struct agree. */
	KUNIT_EXPECT_EQ(test, sizeof(struct drm_sgxfive_render_cmd),
			(size_t)DRM_SGXFIVE_RENDER_CMD_SIZE_V1);
}

static void sgx_render_test_ctx(struct kunit *test)
{
	u32 *ctx = kunit_kzalloc(test, SGX_RENDER_CTX_SIZE, GFP_KERNEL);
	unsigned int off;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	memset(ctx, 0xa5, SGX_RENDER_CTX_SIZE);
	sgx_render_ctx(ctx, &addrs);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x00), 1u);		/* new context */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x04), 0u);		/* priority 0 */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x08), 0x039ea000u);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x0c), 0x32140000u);	/* TA CCB */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x10), KD + 0x000);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x18), KD + 0x300);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x1c), 0x10463000u);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x14), 0u);		/* the EDM's */
	for (off = 0x20; off < SGX_RENDER_CTX_SIZE; off += 4)
		KUNIT_EXPECT_EQ_MSG(test, dw(ctx, off), 0u, "offset %#x", off);
}

/* §12.4: the capture's descriptor, block and page table for 0x422 pages. */
static void sgx_render_test_pb(struct kunit *test)
{
	u32 *desc = kunit_kzalloc(test, SGX_RENDER_PBDESC_SIZE, GFP_KERNEL);
	u32 *block = kunit_kzalloc(test, SGX_RENDER_PBBLOCK_SIZE, GFP_KERNEL);
	u32 *pt = kunit_kzalloc(test, SGX_RENDER_PT_BYTES, GFP_KERNEL);
	static const u32 want[] = {
		0, 0x422, 0x04220001, 0x04210000, 0x10423000, 0, 0x04220001,
		0x04210000, 0, 0, 0x432, 0x400, 0x400, 0x300,
	};
	unsigned int i, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, pt);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_ASSERT_NOT_NULL(test, block);
	memset(pt, 0xa5, SGX_RENDER_PT_BYTES);
	sgx_render_pb(desc, block, pt, &addrs, SGX_RENDER_PB_PAGES);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, desc[i], want[i], "dword %u", i);
	KUNIT_EXPECT_EQ(test, desc[14], KD + 0x340);
	KUNIT_EXPECT_EQ(test, block[0], 0x422u);
	KUNIT_EXPECT_EQ(test, block[1], 0x04220001u);
	KUNIT_EXPECT_EQ(test, block[2], KD + 0x300);
	KUNIT_EXPECT_EQ(test, block[3], 0u);
	KUNIT_EXPECT_EQ(test, pt[0], 0u);
	KUNIT_EXPECT_EQ(test, pt[1], 0xffff0002u);
	KUNIT_EXPECT_EQ(test, pt[2], 0x00010003u);
	KUNIT_EXPECT_EQ(test, pt[0x421], 0x04200422u);
	KUNIT_EXPECT_EQ(test, pt[0x422], 0x0421ffffu);
	for (i = 0; i < SGX_RENDER_PT_BYTES / 4; i++)
		nonzero += pt[i] != 0;
	KUNIT_EXPECT_EQ(test, nonzero, 0x422u);
}

/*
 * The closed driver's small parameter buffer (harness track3d.txt, PBSize
 * hint): 0x62 pages, thresholds TA = pages + 0x10, ZLS = global =
 * pages - 0x22, PDS = global - 0x100 or none; the page table chains only
 * those pages.
 */
static void sgx_render_test_pb_small(struct kunit *test)
{
	u32 *desc = kunit_kzalloc(test, SGX_RENDER_PBDESC_SIZE, GFP_KERNEL);
	u32 *block = kunit_kzalloc(test, SGX_RENDER_PBBLOCK_SIZE, GFP_KERNEL);
	u32 *pt = kunit_kzalloc(test, SGX_RENDER_PT_BYTES, GFP_KERNEL);
	static const u32 want[] = {
		0, 0x62, 0x00620001, 0x00610000, 0x10423000, 0, 0x00620001,
		0x00610000, 0, 0, 0x72, 0x40, 0x40, 0,
	};
	unsigned int i, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, pt);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_ASSERT_NOT_NULL(test, block);
	memset(pt, 0xa5, SGX_RENDER_PT_BYTES);
	sgx_render_pb(desc, block, pt, &addrs, 0x62);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, desc[i], want[i], "dword %u", i);
	KUNIT_EXPECT_EQ(test, block[0], 0x62u);
	KUNIT_EXPECT_EQ(test, block[1], 0x00620001u);
	KUNIT_EXPECT_EQ(test, pt[1], 0xffff0002u);
	KUNIT_EXPECT_EQ(test, pt[0x62], 0x0061ffffu);
	for (i = 0; i < SGX_RENDER_PT_BYTES / 4; i++)
		nonzero += pt[i] != 0;
	KUNIT_EXPECT_EQ(test, nonzero, 0x62u);
}

/* A page count is taken from the smallest with a ZLS threshold to the slot's. */
static void sgx_render_test_pb_pages(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(SGX_RENDER_PB_PAGES), 0x422u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0x62), 0x62u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(SGX_RENDER_PB_MIN_PAGES), 0x23u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0x22), 0x422u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0), 0x422u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0x423), 0x422u);
}

static void sgx_render_test_rtds(struct kunit *test)
{
	u32 *r = kunit_kzalloc(test, SGX_RENDER_RTDS_SIZE, GFP_KERNEL);
	const u8 *rt;

	KUNIT_ASSERT_NOT_NULL(test, r);
	memset(r, 0xa5, SGX_RENDER_RTDS_SIZE);
	sgx_render_rtds(r, &addrs, tri.mte_screen);
	rt = (const u8 *)r + SGX_RENDER_RTDS_RT;
	KUNIT_EXPECT_EQ(test, dw(r, 0x00), KD + 0x40c);		/* pending */
	KUNIT_EXPECT_EQ(test, dw(r, 0x04), 0u);
	KUNIT_EXPECT_EQ(test, dw(r, 0x08), 8u);			/* MTileWidth */
	KUNIT_EXPECT_EQ(test, dw(r, 0x0c), 0u);
	KUNIT_EXPECT_EQ(test, dw(r, 0x10), 0u);
	KUNIT_EXPECT_EQ(test, dw(r, 0x14), 1u);			/* one RT data */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x00), KD + 0x100);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x04), 0u);		/* the EDM's */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x08), 1u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x0c), 0xc0u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x10), 0x10464000u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x14), 0x20043000u);	/* DPM state */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x18), 0x20041000u);	/* DPM control */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x1c), 0x20042000u);	/* OTPM */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x20), 0x20001000u);	/* tail pointers */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x24), 0x100u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x28), 1u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x2c), 0u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x30), KD + 0x410);	/* RT status */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x34), 0x10u);		/* free */
}

/*
 * A larger target changes the RT data set's macrotile width and the RT
 * data's tile blocks, region array and tail sizes, and nothing else: the
 * EDM's words (complete count, CommonStatus) stay as they are.
 */
static void sgx_render_test_rtds_resize(struct kunit *test)
{
	u32 *r = kunit_kzalloc(test, SGX_RENDER_RTDS_SIZE, GFP_KERNEL);
	u32 *ref = kunit_kzalloc(test, SGX_RENDER_RTDS_SIZE, GFP_KERNEL);
	unsigned int off;

	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_ASSERT_NOT_NULL(test, ref);
	sgx_render_rtds(r, &addrs, tri.mte_screen);
	r[1] = 5;					/* complete count */
	r[(SGX_RENDER_RTDS_RT + SGX_RENDER_RT_COMMON) / 4] = 0x30;
	memcpy(ref, r, SGX_RENDER_RTDS_SIZE);
	sgx_render_rtds_resize(r, 1023 | 767 << 16);	/* 1024 x 768 */
	KUNIT_EXPECT_EQ(test, r[SGX_RENDER_RTDS_MTILEW / 4], 0x20u);
	KUNIT_EXPECT_EQ(test, r[(SGX_RENDER_RTDS_RT + SGX_RENDER_RT_BLOCKS) / 4], 6u);
	KUNIT_EXPECT_EQ(test, r[(SGX_RENDER_RTDS_RT + SGX_RENDER_RT_RGNARRAY) / 4], 0x480u);
	KUNIT_EXPECT_EQ(test, r[(SGX_RENDER_RTDS_RT + SGX_RENDER_RT_TAILSIZE) / 4], 0x4000u);
	for (off = 0; off < SGX_RENDER_RTDS_SIZE; off += 4) {
		if (off == SGX_RENDER_RTDS_MTILEW ||
		    off == SGX_RENDER_RTDS_RT + SGX_RENDER_RT_BLOCKS ||
		    off == SGX_RENDER_RTDS_RT + SGX_RENDER_RT_RGNARRAY ||
		    off == SGX_RENDER_RTDS_RT + SGX_RENDER_RT_TAILSIZE)
			continue;
		KUNIT_EXPECT_EQ_MSG(test, r[off / 4], ref[off / 4], "offset %#x", off);
	}
	sgx_render_rtds_resize(r, tri.mte_screen);	/* back to 64 x 64 */
	KUNIT_EXPECT_EQ(test, memcmp(r, ref, SGX_RENDER_RTDS_SIZE), 0);
}

/*
 * The tiling layout of a render target, as the closed driver chose it for
 * each size (harness size=WxH captures, design/sgx545_3d.md §14.6): TE_SCREEN,
 * TE_MTILE1/2, TE_MTILE, the RT data set's macrotile width, the RT data's
 * tile blocks per macrotile, region array and tail sizes, and the size of
 * its region-header buffers. Four macrotiles across and down from 640 x 480
 * pixels on, two below.
 */
static void sgx_render_test_layout(struct kunit *test)
{
	/* w, h; TE_SCREEN, TE_MTILE1, TE_MTILE2, TE_MTILE; the RT data words; buffers */
	static const struct {
		u16 w, h;
		u32 te_screen, mtile1, mtile2, stride;
		u32 mtilew, blocks, rgnarray, rgn, tail;
	} cap[] = {
		{ 64, 64, 0x3001, 0x1004004, 0x1004004, 0x10,
		  0x8, 0x1, 0xc0, 0x300, 0x100 },
		{ 1, 1, 0x0, 0x1004004, 0x1004004, 0x10,
		  0x8, 0x1, 0xc0, 0x300, 0x100 },
		{ 100, 60, 0x3003, 0x1004004, 0x1004004, 0x10,
		  0x8, 0x1, 0xc0, 0x300, 0x100 },
		{ 64, 144, 0x8001, 0x1004004, 0x2008008, 0x20,
		  0x8, 0x2, 0x180, 0x600, 0x400 },
		{ 256, 128, 0x7007, 0x1004004, 0x1004004, 0x10,
		  0x8, 0x1, 0xc0, 0x300, 0x100 },
		{ 128, 192, 0xb003, 0x1004004, 0x2008008, 0x20,
		  0x8, 0x2, 0x180, 0x600, 0x400 },
		{ 256, 256, 0xf007, 0x1004004, 0x2008008, 0x20,
		  0x8, 0x2, 0x180, 0x600, 0x400 },
		{ 512, 16, 0xf, 0x2008008, 0x1004004, 0x20,
		  0x10, 0x2, 0x180, 0x600, 0x400 },
		{ 32, 512, 0x1f000, 0x1004004, 0x4010010, 0x40,
		  0x8, 0x4, 0x300, 0xc00, 0x1000 },
		{ 512, 512, 0x1f00f, 0x2008008, 0x4010010, 0x80,
		  0x10, 0x8, 0x600, 0x1800, 0x1000 },
		{ 640, 475, 0x1d013, 0x300c00c, 0x4010010, 0xc0,
		  0x18, 0xc, 0x900, 0x2400, 0x1000 },
		{ 640, 480, 0x1d013, 0x82010018, 0x2010018, 0x40,
		  0x20, 0x4, 0x300, 0x3000, 0x1000 },
		{ 333, 777, 0x3000a, 0x2008008, 0x701c01c, 0xe0,
		  0x10, 0xe, 0xa80, 0x2a00, 0x4000 },
		{ 1024, 768, 0x2f01f, 0x82010018, 0x3018024, 0x60,
		  0x20, 0x6, 0x480, 0x4800, 0x4000 },
		{ 1366, 768, 0x2f02a, 0x83018024, 0x3018024, 0x90,
		  0x30, 0x9, 0x6c0, 0x6c00, 0x4000 },
		{ 1280, 800, 0x31027, 0x83018024, 0x4020030, 0xc0,
		  0x30, 0xc, 0x900, 0x9000, 0x4000 },
		{ 2049, 2049, 0x80040, 0x8502803c, 0x904806c, 0x2d0,
		  0x50, 0x2d, 0x21c0, 0x21c00, 0x40000 },
		{ 4096, 2048, 0x7f07f, 0x88040060, 0x8040060, 0x400,
		  0x80, 0x40, 0x3000, 0x30000, 0x10000 },
		{ 64, 4096, 0xff001, 0x1004004, 0x20080080, 0x200,
		  0x8, 0x20, 0x1800, 0x6000, 0x40000 },
		{ 4096, 74, 0x407f, 0x10040040, 0x1004004, 0x100,
		  0x80, 0x10, 0xc00, 0x3000, 0x10000 },
		{ 4096, 75, 0x407f, 0x88040060, 0x100800c, 0x80,
		  0x80, 0x8, 0x600, 0x6000, 0x10000 },
		{ 4096, 4096, 0xff07f, 0x88040060, 0x100800c0, 0x800,
		  0x80, 0x80, 0x6000, 0x60000, 0x40000 },
	};
	struct sgx_render_layout l;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cap); i++) {
		const unsigned int w = cap[i].w, h = cap[i].h;

		memset(&l, 0xa5, sizeof(l));
		sgx_render_layout(&l, w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.te_screen, cap[i].te_screen, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.te_mtile1, cap[i].mtile1, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.te_mtile2, cap[i].mtile2, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.te_mtile_stride, cap[i].stride, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.mtile_width, cap[i].mtilew, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.blocks_per_mt, cap[i].blocks, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.rgn_array_size, cap[i].rgnarray, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.rgn_size, cap[i].rgn, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, l.tail_size, cap[i].tail, "%ux%u", w, h);
	}
}

/*
 * A render context's TA data, for the largest target it takes: the tail
 * pointers, the DPM control, OTPM and DPM state pages and the two
 * region-header buffers, each rounded up to whole pages and packed from the
 * slot base in that order. For 4096 x 4096 they are the fixed offsets the
 * laptop ran before contexts stated a maximum.
 */
static void sgx_render_test_tadata(struct kunit *test)
{
	static const struct {
		u16 w, h;
		struct sgx_render_tadata t;
	} want[] = {
		{ 4096, 4096, TD_4096 },
		{ 64, 64, { 0, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000 } },
		{ 1, 1, { 0, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000 } },
		/* the laptop's panel: n = 4, region headers 0x4800, tails 0x4000 */
		{ 1024, 600, { 0, 0x4000, 0x5000, 0x6000, 0x7000, 0xc000, 0x11000 } },
		/* not a power of two: region headers 0x2a00, tails 0x4000 */
		{ 333, 777, { 0, 0x4000, 0x5000, 0x6000, 0x7000, 0xa000, 0xd000 } },
		/* either side of the 640 x 480 threshold: 0x2400 and 0x3000 */
		{ 640, 475, { 0, 0x1000, 0x2000, 0x3000, 0x4000, 0x7000, 0xa000 } },
		{ 640, 480, { 0, 0x1000, 0x2000, 0x3000, 0x4000, 0x7000, 0xa000 } },
		{ 2048, 2048, { 0, 0x10000, 0x11000, 0x12000, 0x13000, 0x2b000, 0x43000 } },
		/* thin targets: the tail pointers cover the longer side squared */
		{ 4096, 1, { 0, 0x10000, 0x11000, 0x12000, 0x13000, 0x16000, 0x19000 } },
		{ 1, 4096, { 0, 0x40000, 0x41000, 0x42000, 0x43000, 0x49000, 0x4f000 } },
	};
	struct sgx_render_tadata t;
	struct sgx_render_layout l;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(want); i++) {
		const unsigned int w = want[i].w, h = want[i].h;

		memset(&t, 0xa5, sizeof(t));
		KUNIT_EXPECT_EQ_MSG(test, sgx_render_tadata_layout(&t, w, h), 0, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.tail, want[i].t.tail, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.control, want[i].t.control, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.otpm, want[i].t.otpm, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.state, want[i].t.state, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.rgn, want[i].t.rgn, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.zls, want[i].t.zls, "%ux%u", w, h);
		KUNIT_EXPECT_EQ_MSG(test, t.size, want[i].t.size, "%ux%u", w, h);
		/* Whole pages, in order, each buffer as large as the layout's. */
		sgx_render_layout(&l, w, h);
		KUNIT_EXPECT_EQ(test, t.size % 4096, 0);
		KUNIT_EXPECT_LE(test, t.tail + l.tail_size, t.control);
		KUNIT_EXPECT_EQ(test, t.control + 4096, t.otpm);
		KUNIT_EXPECT_EQ(test, t.otpm + 4096, t.state);
		KUNIT_EXPECT_EQ(test, t.state + 4096, t.rgn);
		KUNIT_EXPECT_LE(test, t.rgn + l.rgn_size, t.zls);
		KUNIT_EXPECT_LE(test, t.zls + l.rgn_size, t.size);
	}
	KUNIT_EXPECT_EQ(test, want[0].t.size, (u32)SGX_RENDER_TD_MAX_SIZE);
	/* Each dimension 1 to the device's 4096; 0 is no size. */
	KUNIT_EXPECT_EQ(test, sgx_render_tadata_layout(&t, 0, 64), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_tadata_layout(&t, 64, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_tadata_layout(&t, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_tadata_layout(&t, 4097, 64), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_tadata_layout(&t, 64, 4097), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_tadata_layout(&t, 0xffffffff, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_check_max(1, 1), 0);
	KUNIT_EXPECT_EQ(test, sgx_render_check_max(4096, 4096), 0);
	KUNIT_EXPECT_EQ(test, sgx_render_check_max(0, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_check_max(1, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_check_max(4097, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_check_max(1, 4097), -EINVAL);
}

/* Every target within w x h has a layout no larger than w x h's. */
static void covers(struct kunit *test, u32 mw, u32 mh, u32 step)
{
	struct sgx_render_layout big, l;
	u32 w, h, fails = 0;

	sgx_render_layout(&big, mw, mh);
	for (w = 1; w <= mw; w = w == mw ? mw + 1 : min(w + step, mw)) {
		for (h = 1; h <= mh; h = h == mh ? mh + 1 : min(h + step, mh)) {
			sgx_render_layout(&l, w, h);
			fails += l.rgn_size > big.rgn_size || l.tail_size > big.tail_size;
		}
		/* The tallest target under the 640 x 480 area at this width. */
		h = min(mh, (640 * 480 - 1) / w);
		if (h) {
			sgx_render_layout(&l, w, h);
			fails += l.rgn_size > big.rgn_size || l.tail_size > big.tail_size;
		}
	}
	KUNIT_EXPECT_EQ_MSG(test, fails, 0u, "max %ux%u", mw, mh);
}

/*
 * The largest target is the worst case of every target that fits it, so a
 * context's buffers sized for its maximum hold any job's layout: the region
 * headers and tail pointers grow with each dimension and from two
 * macrotiles to four (design/sgx545_render_uapi.md §11.4). Maxima on both
 * sides of the 640 x 480 threshold, non-powers of two, thin targets.
 */
static void sgx_render_test_tadata_covers(struct kunit *test)
{
	covers(test, 64, 64, 1);
	covers(test, 100, 60, 1);
	covers(test, 640, 480, 7);
	covers(test, 640, 479, 7);
	covers(test, 700, 500, 9);
	covers(test, 1024, 600, 13);
	covers(test, 333, 777, 11);
	covers(test, 4096, 75, 17);
	covers(test, 75, 4096, 17);
	covers(test, 4096, 4096, 61);
}

/*
 * The builders place the TA-data buffers where the context's layout says:
 * the tail pointers, the DPM pages and the region headers of a small
 * maximum sit closer to the slot base than for 4096 x 4096.
 */
static void sgx_render_test_tadata_addrs(struct kunit *test)
{
	struct sgx_render_addrs a = addrs;
	u32 *r = kunit_kzalloc(test, SGX_RENDER_RTDS_SIZE, GFP_KERNEL);
	u32 *rd = kunit_kzalloc(test, SGX_RENDER_RD_SIZE, GFP_KERNEL);
	u8 *cmd = kunit_kzalloc(test, SGX_RENDER_CMD_STRIDE, GFP_KERNEL);
	const struct sgx_render_kick k = { 0 };
	const u8 *rt;

	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_ASSERT_NOT_NULL(test, rd);
	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&a.td, 1024, 600), 0);
	sgx_render_rtds(r, &a, tri.mte_screen);
	rt = (const u8 *)r + SGX_RENDER_RTDS_RT;
	KUNIT_EXPECT_EQ(test, dw(rt, SGX_RENDER_RT_TAIL), 0x20001000u);
	KUNIT_EXPECT_EQ(test, dw(rt, SGX_RENDER_RT_CONTROL), 0x20005000u);
	KUNIT_EXPECT_EQ(test, dw(rt, SGX_RENDER_RT_OTPM), 0x20006000u);
	KUNIT_EXPECT_EQ(test, dw(rt, SGX_RENDER_RT_STATE), 0x20007000u);
	sgx_render_rd(rd, &a, &tri);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_RGNBASE), 0x20008000u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_ZLSEXTZ), 0x2000d000u);
	sgx_render_cmd(cmd, &a, &tri, &k);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_RENDER_CMD_TAREGS + SGX_RENDER_TA_PSGRGN),
			0x20008000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_RENDER_CMD_TAREGS + SGX_RENDER_TA_TPC),
			0x20001000u);
}

/* §12.6 "render details": the first triangle's, as the laptop ran it. */
static void sgx_render_test_rd(struct kunit *test)
{
	u32 *rd = kunit_kzalloc(test, SGX_RENDER_RD_SIZE, GFP_KERNEL);
	static const struct { u16 off; u32 val; } want[] = {
		{ 0x000, 0x10000000 }, { 0x008, 0x80 }, { 0x00c, 0x000f0000 },
		{ 0x010, 0x80000000 }, { 0x020, 0x20044000 }, { 0x028, 0x200a4000 },
		{ 0x038, 0x80000000 }, { 0x03c, 0x80000000 }, { 0x040, 0x3f800000 },
		{ 0x044, 0x300 }, { 0x048, 0x46400 }, { 0x050, 0x88 }, { 0x058, 0x30 },
		{ 0x05c, 0x2e001080 }, { 0x060, 5 }, { 0x064, 1 }, { 0x068, 1 },
		{ 0x0cc, 0x100 }, { 0x0d0, 0x32152408 }, { 0x0d8, 0x32152200 },
		{ 0x0dc, 0x32152218 }, { 0x104, 1 }, { 0x114, 0x32152414 },
		{ 0x13c, 0x07c00000 },
	};
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, rd);
	memset(rd, 0xa5, SGX_RENDER_RD_SIZE);
	sgx_render_rd(rd, &addrs, &tri);
	for (i = 0; i < SGX_RENDER_RD_SIZE; i += 4) {
		u32 v = 0;

		for (j = 0; j < ARRAY_SIZE(want); j++)
			if (want[j].off == i)
				v = want[j].val;
		KUNIT_EXPECT_EQ_MSG(test, dw(rd, i), v, "offset %#x", i);
	}
}

/*
 * Each user 3D register lands in its own render-details word, and the
 * kernel's words do not move whatever the user's are.
 */
static void sgx_render_test_rd_user(struct kunit *test)
{
	u32 *rd = kunit_kzalloc(test, SGX_RENDER_RD_SIZE, GFP_KERNEL);
	struct drm_sgxfive_render_cmd c = tri;

	KUNIT_ASSERT_NOT_NULL(test, rd);
	c.pixelbe = 0x101;
	c.isp_ipfmisc = 0x102;
	c.aa_mode_3d = 0x103;
	c.isp_bgobjdepth = 0x104;
	c.isp_bgobj = 0x200;
	c.isp_multisamplectl = 0x106;
	c.tsp_config = 0x107;
	c.event_pixel_pds_exec = 0x2e002000;
	c.event_pixel_pds_data = 0x109;
	c.event_pixel_pds_info = 0x10a;
	c.isp_fpuctrl = 0x10b;
	c.isp_depthsort = 0x10c;
	sgx_render_rd(rd, &addrs, &c);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_PIXELBE), 0x101u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_IPFMISC), 0x102u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_AAMODE), 0x103u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_BGDEPTH), 0x104u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_BGOBJ), 0x200u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_MSCTL), 0x106u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_TSPCONFIG), 0x107u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_PIXEV_EXEC), 0x2e002000u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_PIXEV_DATA), 0x109u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_PIXEV_INFO), 0x10au);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_FPUCTRL), 0x10bu);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_DEPTHSORT), 0x10cu);
	/* Kernel-owned: parameter slot, region headers, background, no ZLS. */
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_REQBASE), 0x10000000u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_RGNBASE), 0x20044000u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_BGTAG), 0x46400u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_ZLSCTL), 0u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_SLOAD), 0x80000000u);
	KUNIT_EXPECT_EQ(test, dw(rd, SGX_RENDER_RD_NUMPIXPART), 0x07c00000u);
}

/* §12.6 "SGXMKIF_CMDTA": the single-kick form the laptop ran. */
static void sgx_render_test_cmd(struct kunit *test)
{
	static const struct sgx_render_kick k = {
		.flags = SGX_RENDER_TA_FIRST | SGX_RENDER_TA_LAST | SGX_RENDER_TA_INVAL,
		.render_flags = SGX_RENDER_SCENE, .te_psg = SGX_RENDER_TEPSG_END,
		.frame = 7, .ta_val = 0x7a01, .td_val = 0x3d01,
	};
	static const struct { u16 off; u32 val; } want[] = {
		{ 0x000, 0x2c0 }, { 0x004, 0x73 }, { 0x008, 0x04000104 },
		{ 0x00c, 0x00030000 }, { 0x010, 0x04000102 }, { 0x014, 0x50000000 },
		{ 0x018, 0x50000000 }, { 0x01c, 0x400 }, { 0x020, 7 },
		{ 0x024, 0x32152300 }, { 0x038, 0x32152800 }, { 0x040, 0x32152200 },
		{ 0x044, 0x32152218 },
		{ 0x058, 1 }, { 0x05c, 1 }, { 0x060, 1 },		/* READY, 1 + 1 */
		{ 0x118, 0x32152400 }, { 0x11c, 0x7a01 },		/* TA status */
		{ 0x218, 0x32152404 }, { 0x21c, 0x3d01 },		/* 3D status */
		{ 0x238, 0x20000000 }, { 0x23c, 0x01004004 }, { 0x240, 0x01004004 },
		{ 0x244, 0x3001 }, { 0x248, 0x10 }, { 0x24c, 0x400000 },
		{ 0x250, 0x3e001000 }, { 0x254, 0xa2 }, { 0x258, 0x20044000 },
		{ 0x25c, 0x20001000 }, { 0x260, 0x358637bd }, { 0x264, 0x358637bd },
		{ 0x268, 0x003f003f }, { 0x274, 0x10000000 }, { 0x2a4, 0x88 },
	};
	u8 *cmd = kunit_kzalloc(test, SGX_RENDER_CMD_STRIDE, GFP_KERNEL);
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	memset(cmd, 0xa5, SGX_RENDER_CMD_STRIDE);
	KUNIT_EXPECT_EQ(test, sgx_render_cmd(cmd, &addrs, &tri, &k),
			(u32)SGX_RENDER_CMD_STRIDE);
	for (i = 0; i < SGX_RENDER_CMD_STRIDE; i += 4) {
		u32 v = 0;

		for (j = 0; j < ARRAY_SIZE(want); j++)
			if (want[j].off == i)
				v = want[j].val;
		KUNIT_EXPECT_EQ_MSG(test, dw(cmd, i), v, "offset %#x", i);
	}
}

/* Each user TA register lands in its own hw_ta_registers word. */
static void sgx_render_test_cmd_user(struct kunit *test)
{
	struct sgx_render_kick k = { 0 };
	struct drm_sgxfive_render_cmd c = tri;
	u8 *cmd = kunit_kzalloc(test, SGX_RENDER_CMD_STRIDE, GFP_KERNEL);
	const unsigned int ta = SGX_RENDER_CMD_TAREGS;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	c.vdm_ctrl_stream_base = 0x3e00a000;
	c.te_aa = 0x201;
	c.te_mtile1 = 0x202;
	c.te_mtile2 = 0x203;
	c.te_screen = 0x7007;
	c.te_mtile_stride = 0x205;
	c.mte_ctrl = 0x206;
	c.mte_wcompare = 0x207;
	c.mte_wclamp = 0x208;
	c.mte_screen = 0x00ff00ff;
	c.mte_multisamplectl = 0x20a;
	sgx_render_cmd(cmd, &addrs, &c, &k);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_VDM), 0x3e00a000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_TEAA), 0x201u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_TEMTILE1), 0x202u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_TEMTILE2), 0x203u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_TESCREEN), 0x7007u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_TEMTILESTRIDE), 0x205u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_MTECTRL), 0x206u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_WCOMPARE), 0x207u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_WCLAMP), 0x208u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_MTESCREEN), 0x00ff00ffu);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_MTEMSCTRL), 0x20au);
	/* Kernel-owned: the slot, the region headers and tail pointers. */
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_REQBASE), 0x10000000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_PSGRGN), 0x20044000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, ta + SGX_RENDER_TA_TPC), 0x20001000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_RENDER_CMD_NUM3DREGS), 0u);
}

/* A job's kick: the whole scene in one, seqno as frame and status values. */
static void sgx_render_test_scene_kick(struct kunit *test)
{
	struct sgx_render_kick k;

	memset(&k, 0xa5, sizeof(k));
	sgx_render_scene_kick(&k, 42, 43);
	KUNIT_EXPECT_EQ(test, k.flags, 0x73u);
	KUNIT_EXPECT_EQ(test, k.render_flags, 0x400u);
	KUNIT_EXPECT_EQ(test, k.te_psg, 0x400000u);
	KUNIT_EXPECT_EQ(test, k.frame, 42u);
	KUNIT_EXPECT_EQ(test, k.ta_val, 43u);
	KUNIT_EXPECT_EQ(test, k.td_val, 43u);
}

/*
 * S.8: the user's three PDS words, then the whole render target as the
 * transfer queue's zero-layer rectangle (sgx_xfer.c's vertex packing).
 */
static void sgx_render_test_bgobj(struct kunit *test)
{
	u32 bg[17];
	struct drm_sgxfive_render_cmd c = tri;

	memset(bg, 0xa5, sizeof(bg));
	sgx_render_bgobj(bg, &c);
	KUNIT_EXPECT_EQ(test, bg[0], 0x04000104u);
	KUNIT_EXPECT_EQ(test, bg[1], 0x00030000u);
	KUNIT_EXPECT_EQ(test, bg[2], 0x04000102u);
	KUNIT_EXPECT_EQ(test, bg[3], 0u);
	KUNIT_EXPECT_EQ(test, bg[9], 0x00100000u);	/* (0, 0) */
	KUNIT_EXPECT_EQ(test, bg[11], 0x00000000u);
	KUNIT_EXPECT_EQ(test, bg[12], 0x10400010u);	/* x1 = 64, y0 */
	KUNIT_EXPECT_EQ(test, bg[14], 0x00104000u);	/* y1 = 64 */
	c.mte_screen = 255 | 127 << 16;			/* 256 x 128 */
	sgx_render_bgobj(bg, &c);
	KUNIT_EXPECT_EQ(test, bg[12], 0x11000010u);	/* x1 = 256 */
	KUNIT_EXPECT_EQ(test, bg[14], 0x00108000u);	/* y1 = 128 */
}

/*
 * The render target is within the device's 4096 x 4096 and its TE words are
 * the layout of its size (the kernel sized its buffers for that layout);
 * ISP_BGOBJ bits 8-9 only.
 */
static void sgx_render_test_check_size(struct kunit *test)
{
	static const u16 sizes[][2] = {
		{ 1, 1 }, { 64, 64 }, { 256, 128 }, { 128, 192 }, { 256, 256 },
		{ 1024, 768 }, { 1366, 768 }, { 333, 777 }, { 4096, 4096 },
	};
	struct drm_sgxfive_render_cmd c = tri;
	struct sgx_render_layout l;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), 0);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		sgx_render_layout(&l, sizes[i][0], sizes[i][1]);
		c = tri;
		c.mte_screen = (sizes[i][0] - 1) | (sizes[i][1] - 1) << 16;
		c.te_screen = l.te_screen;
		c.te_mtile1 = l.te_mtile1;
		c.te_mtile2 = l.te_mtile2;
		c.te_mtile_stride = l.te_mtile_stride;
		KUNIT_EXPECT_EQ_MSG(test, sgx_render_check_size(&c, 4096, 4096), 0, "%ux%u",
				    sizes[i][0], sizes[i][1]);
	}
	/* 4096 x 4096 from here on: one word off at a time. */
	c.te_screen ^= 1;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	c.te_screen ^= 1;
	c.te_mtile1 ^= 0x80000000;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	c.te_mtile1 ^= 0x80000000;
	c.te_mtile2 = 0x01004004;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	c.te_mtile2 = l.te_mtile2;
	c.te_mtile_stride = 0x10;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	c.te_mtile_stride = l.te_mtile_stride;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), 0);
	c.mte_screen = 4096 | 4095 << 16;		/* 4097 across */
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	c.mte_screen = 4095 | 4096 << 16;		/* 4097 down */
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	/* The 64 x 64 words with a taller target: what faulted on the laptop. */
	c = tri;
	c.mte_screen = 127 | 191 << 16;
	c.te_screen = 3 | 11 << 12;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
	c = tri;
	c.isp_bgobj = 0x100;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), 0);
	c.isp_bgobj = 0x301;
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), -EINVAL);
}

/* The triangle's command for a w x h target, with that target's TE words. */
static void with_size(struct drm_sgxfive_render_cmd *c, u32 w, u32 h)
{
	struct sgx_render_layout l;

	sgx_render_layout(&l, w, h);
	*c = tri;
	c->mte_screen = (w - 1) | (h - 1) << 16;
	c->te_screen = l.te_screen;
	c->te_mtile1 = l.te_mtile1;
	c->te_mtile2 = l.te_mtile2;
	c->te_mtile_stride = l.te_mtile_stride;
}

/*
 * A job's target must fit the maximum its context was created with, each
 * dimension on its own (the context's tiling buffers are that large).
 */
static void sgx_render_test_check_max_size(struct kunit *test)
{
	struct drm_sgxfive_render_cmd c;

	with_size(&c, 64, 64);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 64, 64), 0);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 63, 64), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 64, 63), -EINVAL);
	with_size(&c, 1, 1);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 1, 1), 0);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 64, 64), 0);
	with_size(&c, 65, 64);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 64, 64), -EINVAL);
	with_size(&c, 64, 65);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 64, 64), -EINVAL);
	/* Same area, other shape: does not fit. */
	with_size(&c, 600, 1024);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 1024, 600), -EINVAL);
	with_size(&c, 1024, 600);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 1024, 600), 0);
	with_size(&c, 600, 600);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 1024, 600), 0);
	/* Smaller in area, but one dimension over: rejected before the layout. */
	with_size(&c, 2000, 16);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 1024, 600), -EINVAL);
	with_size(&c, 4096, 4096);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4096), 0);
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c, 4096, 4095), -EINVAL);
}

static struct kunit_case sgx_render_test_cases[] = {
	KUNIT_CASE(sgx_render_test_offsets),
	KUNIT_CASE(sgx_render_test_ctx),
	KUNIT_CASE(sgx_render_test_pb),
	KUNIT_CASE(sgx_render_test_pb_small),
	KUNIT_CASE(sgx_render_test_pb_pages),
	KUNIT_CASE(sgx_render_test_rtds),
	KUNIT_CASE(sgx_render_test_rtds_resize),
	KUNIT_CASE(sgx_render_test_layout),
	KUNIT_CASE(sgx_render_test_tadata),
	KUNIT_CASE(sgx_render_test_tadata_covers),
	KUNIT_CASE(sgx_render_test_tadata_addrs),
	KUNIT_CASE(sgx_render_test_rd),
	KUNIT_CASE(sgx_render_test_rd_user),
	KUNIT_CASE(sgx_render_test_cmd),
	KUNIT_CASE(sgx_render_test_cmd_user),
	KUNIT_CASE(sgx_render_test_scene_kick),
	KUNIT_CASE(sgx_render_test_bgobj),
	KUNIT_CASE(sgx_render_test_check_size),
	KUNIT_CASE(sgx_render_test_check_max_size),
	{}
};

static struct kunit_suite sgx_render_test_suite = {
	.name = "gma500_sgx_render",
	.test_cases = sgx_render_test_cases,
};

kunit_test_suite(sgx_render_test_suite);
