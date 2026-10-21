// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_tri.c: the first triangle's scene (its programs, MTE
 * state, VDM stream and geometry) and the register words it gives the shared
 * job builders (sgx_render.c, tested in sgx_render_test.c), against §12.6 and
 * §13 of design/sgx545_3d.md and the REV1 layout the debugfs test uses.
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgx_fw.h"
#include "sgx_render.h"
#include "sgx_tri.h"
#include "sgx_xfer.h"

static const struct sgx_tri_addrs addrs = {
	.pdsv = 0x30401000, .usev = 0x3c001000, .pdsp = 0x2e001000,
	.usep = 0x37001000, .gen = 0x3e001000, .fb = 0x3e010000,
	.pds_exec_base = 0x2e000000, .usep_base = 0x37000000,
	.usev_base = 0x3c000000,
};

static void sgx_tri_test_f32(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(1, 0), 0x3f800000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(1, 1), 0x3f000000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(-1, 1), 0xbf000000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(32, 0), 0x42000000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(-384, 9), 0xbf400000u);	/* -0.75 */
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(-192, 9), 0xbec00000u);	/* -0.375 */
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(0, 4), 0u);
}

/* DOUTU U0 (design/sgx545_transfer.md §4.2.4) and the pointer words (S.5). */
static void sgx_tri_test_words(struct kunit *test)
{
	/* The capture's vertex program: base 10, instruction 0x11b0. */
	KUNIT_EXPECT_EQ(test, sgx_tri_doutu(0xfc008d80, 0xfc000000, 10, 0), 0x11b0au);
	/* The transfer EOT task: 4 temporaries. */
	KUNIT_EXPECT_EQ(test, sgx_tri_doutu(0xf4000020, 0xf4000000, 0, 4), 0x01000040u);
	/* 16 data bytes = 1 row, offset 0x1040 from PDS_EXEC_BASE. */
	KUNIT_EXPECT_EQ(test, sgx_tri_pix_ptr(&addrs, 0x2e001040, 4), 0x04000104u);
	/* The capture's state copies: 12 dwords in one burst, 18 in 2 x 9. */
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(12), 0x02c0000bu);
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(18), 0x02000018u);
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(2), 0x00400001u);
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(4), 0x00c00003u);
}

static void sgx_tri_test_words_area(struct kunit *test)
{
	u32 w[SGX_TRI_W_COUNT];

	memset(w, 0xa5, sizeof(w));
	sgx_tri_words(w, &addrs);
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_TA], 0u);
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_3D], 0u);
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_RTSTATUS], 0u);
	/* The background's primary PDS pointer, for background dword 2. */
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_BGPRI], 0x04000102u);
}

/* S.2, S.9: the triangle's 18-word state block and the end of scene. */
static void sgx_tri_test_blocks(struct kunit *test)
{
	static const u32 want[SGX_TRI_BLOCK_WORDS] = {
		0xdf43, 0x01d00200, 0x10f0, 0x04000104, 0x00030001, 0x04000100,
		0x42000000, 0x42000000, 0x42000000, 0x42000000, 0x3f000000,
		0x3f000000, 0, 0x43000, 0x3727c5ac, 0x01555620, 0, 0,
	};
	u32 blk[SGX_TRI_BLOCK_WORDS], eos[SGX_TRI_EOS_WORDS];
	unsigned int i, n = 0;

	sgx_tri_block(blk, &addrs);
	for (i = 0; i < SGX_TRI_BLOCK_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, blk[i], want[i], "word %u", i);
	/* The presence word announces exactly the words that follow. */
	for (i = 0; i < 16; i++)
		if (blk[0] & BIT(i))
			n += i == 6 ? 3 : i == 7 ? 2 : i == 8 ? 6 : 1;
	KUNIT_EXPECT_EQ(test, n + 1, (unsigned int)SGX_TRI_BLOCK_WORDS);
	sgx_tri_eos(eos);
	KUNIT_EXPECT_EQ(test, eos[0], 0x2000u);
	KUNIT_EXPECT_EQ(test, eos[1], 0x00010001u);	/* 64 x 64: blocks 0-1 */
}

/* S.1: state, index list with its vertex task, end of scene, terminate. */
static void sgx_tri_test_vdm(struct kunit *test)
{
	static const u32 want[SGX_TRI_VDM_WORDS] = {
		0x43040104, 0x0a022405,			/* state: pds_mte, 5 rows */
		0x80c00003, 0x3e001200, 0xf0000000, 0x003fffff,
		0xf3040100, 0x01c00082,			/* vertex task: pds_vtx */
		0x63040108, 0x0a022401,			/* last task: end of scene */
		0xc0000000,
	};
	static const u32 want1[SGX_TRI_VDM1_WORDS] = {
		0x63040108, 0x0a022401, 0xc0000000,
	};
	u32 vdm[SGX_TRI_VDM_WORDS], vdm1[SGX_TRI_VDM1_WORDS];
	unsigned int i;

	sgx_tri_vdm(vdm, &addrs);
	for (i = 0; i < SGX_TRI_VDM_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, vdm[i], want[i], "word %u", i);
	sgx_tri_vdm1(vdm1, &addrs);
	for (i = 0; i < SGX_TRI_VDM1_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, vdm1[i], want1[i], "word %u", i);
}

static u64 qw(const u32 *p, unsigned int insn)
{
	return p[2 * insn] | (u64)p[2 * insn + 1] << 32;
}

/* The programs in place, with their patched fields. */
static void sgx_tri_test_scene(struct kunit *test)
{
	struct sgx_tri_cpu c = {
		.pdsv = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.usev = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.pdsp = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.usep = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.gen = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
	};
	const u32 *pv, *pp, *g;
	u64 eot;

	KUNIT_ASSERT_NOT_NULL(test, c.pdsv);
	KUNIT_ASSERT_NOT_NULL(test, c.usev);
	KUNIT_ASSERT_NOT_NULL(test, c.pdsp);
	KUNIT_ASSERT_NOT_NULL(test, c.usep);
	KUNIT_ASSERT_NOT_NULL(test, c.gen);
	sgx_tri_scene(&c, &addrs, 0xff00ff00, 0xff203040);

	/* pds_vtx: vertex buffer, doutd of 4 dwords, stride 16, U0. */
	pv = c.pdsv + SGX_TRI_PV_VTX / 4;
	KUNIT_EXPECT_EQ(test, pv[0], 0x3e001100u);
	KUNIT_EXPECT_EQ(test, pv[1], 3u);
	KUNIT_EXPECT_EQ(test, pv[2], 16u);
	KUNIT_EXPECT_EQ(test, pv[4], 0x200au);
	KUNIT_EXPECT_EQ(test, pv[8], 0x67800072u);	/* code follows */
	/* pds_mte for the block and for the end of scene. */
	pv = c.pdsv + SGX_TRI_PV_MTE / 4;
	KUNIT_EXPECT_EQ(test, pv[0], 0x30401100u);
	KUNIT_EXPECT_EQ(test, pv[1], 0x02000018u);
	KUNIT_EXPECT_EQ(test, pv[4], 0x210au);
	pv = c.pdsv + SGX_TRI_PV_EOS / 4;
	KUNIT_EXPECT_EQ(test, pv[0], 0x30401180u);
	KUNIT_EXPECT_EQ(test, pv[1], 0x00400001u);
	KUNIT_EXPECT_EQ(test, pv[4], 0x220au);
	KUNIT_EXPECT_EQ(test, c.pdsv[SGX_TRI_PV_BLOCK / 4], 0xdf43u);
	KUNIT_EXPECT_EQ(test, c.pdsv[SGX_TRI_PV_EOSBLOCK / 4], 0x2000u);
	/* use_vtx: the next phase is instruction 7 of the program at 0x1000. */
	KUNIT_EXPECT_EQ(test, qw(c.usev, 0), 0xfa44020000000207ull);
	KUNIT_EXPECT_EQ(test, qw(c.usev + SGX_TRI_UV_MTE18 / 4, 9),
			0xfb2f4000a0200000ull);		/* emitmtest.end */

	/* Pixel PDS: U0 words of the shaders, the pixel-event dispatch. */
	pp = c.pdsp;
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_PRIM / 4], 0x2000u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_PRIM / 4 + 1], 4u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_BGPRIM / 4], 0x2020u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_SEC / 4], 0x2040u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_SEC / 4 + 1], 4u);
	pp = c.pdsp + SGX_TRI_PP_PIXEV / 4;
	KUNIT_EXPECT_EQ(test, pp[0], 0x2060u);		/* PTOFF */
	KUNIT_EXPECT_EQ(test, pp[1], 0u);
	KUNIT_EXPECT_EQ(test, pp[3], 0x01002080u);	/* EOT, 4 temps */
	KUNIT_EXPECT_EQ(test, pp[4], 0x01002100u);	/* EOR, 1 temp */
	KUNIT_EXPECT_EQ(test, pp[5], 0x00c00000u);
	KUNIT_EXPECT_EQ(test, pp[12], 0x00800000u);
	KUNIT_EXPECT_EQ(test, pp[6], 8u);		/* template constants kept */

	/* The colours and the PBE words. */
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_FILL / 4, 1)),
			0xff00ff00u);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_BGFILL / 4, 1)),
			0xff203040u);
	KUNIT_EXPECT_EQ(test, qw(c.usep + SGX_TRI_UP_EOT / 4, 1), 0xfca0000000000000ull);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_EOT / 4, 2)),
			0x003f003fu);
	KUNIT_EXPECT_EQ(test, (qw(c.usep + SGX_TRI_UP_EOT / 4, 2) >> 21) & 0x7f, 1ull);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_EOT / 4, 3)),
			0x001f8000u);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_EOT / 4, 4)),
			0x3e010000u);
	KUNIT_EXPECT_EQ(test, (qw(c.usep + SGX_TRI_UP_EOT / 4, 4) >> 21) & 0x7f, 3ull);
	eot = qw(c.usep + SGX_TRI_UP_EOT / 4, 6);
	KUNIT_EXPECT_EQ(test, eot, sgx_fw_emitpix_sb_set(0xfb24000000208180ull, 0x200));

	/* Geometry: three clip-space vertices with w = 1, indices 0 1 2. */
	g = c.gen;
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 0], 0xbf400000u);	/* -0.75 */
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 1], 0xbf400000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 2], 0u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 3], 0x3f800000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 4], 0x3f400000u);	/* 0.75 */
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 5], 0xbec00000u);	/* -0.375 */
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 8], 0xbec00000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 9], 0x3f400000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_IDX / 4], 0x00010000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_IDX / 4 + 1] & 0xffff, 2u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VDM / 4 + 10], 0xc0000000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VDM1 / 4 + 2], 0xc0000000u);
}

/*
 * The register words the triangle gives the job builders: the captured TA
 * and 3D registers for 64 x 64 (§6, §7, S.6) with our pixel-event program and
 * background words. The kernel's background object from them is the one the
 * transfer builder makes from the programs' addresses (§14.1).
 */
static void sgx_tri_test_render_cmd(struct kunit *test)
{
	static const struct sgx_xfer_rect whole = { 0, 0, SGX_TRI_W, SGX_TRI_H };
	u32 bg[SGX_XFER_BGOBJ_DWORDS], ref[SGX_XFER_BGOBJ_DWORDS];
	struct drm_sgxfive_render_cmd c;
	unsigned int i;

	memset(&c, 0xa5, sizeof(c));
	sgx_tri_render_cmd(&c, &addrs, 0x3e001080);
	KUNIT_EXPECT_EQ(test, c.vdm_ctrl_stream_base, 0x3e001080u);
	KUNIT_EXPECT_EQ(test, c.te_aa, 0x20000000u);
	KUNIT_EXPECT_EQ(test, c.te_mtile1, 0x01004004u);
	KUNIT_EXPECT_EQ(test, c.te_mtile2, 0x01004004u);
	KUNIT_EXPECT_EQ(test, c.te_screen, 0x3001u);
	KUNIT_EXPECT_EQ(test, c.te_mtile_stride, 0x10u);
	KUNIT_EXPECT_EQ(test, c.mte_ctrl, 0xa2u);
	KUNIT_EXPECT_EQ(test, c.mte_wcompare, 0x358637bdu);
	KUNIT_EXPECT_EQ(test, c.mte_wclamp, 0x358637bdu);
	KUNIT_EXPECT_EQ(test, c.mte_screen, 0x003f003fu);
	KUNIT_EXPECT_EQ(test, c.mte_multisamplectl, 0x88u);
	KUNIT_EXPECT_EQ(test, c.pixelbe, 0x80u);
	KUNIT_EXPECT_EQ(test, c.isp_ipfmisc, 0x000f0000u);
	KUNIT_EXPECT_EQ(test, c.aa_mode_3d, 0x80000000u);
	KUNIT_EXPECT_EQ(test, c.isp_bgobjdepth, 0x3f800000u);
	KUNIT_EXPECT_EQ(test, c.isp_bgobj, 0x300u);
	KUNIT_EXPECT_EQ(test, c.isp_multisamplectl, 0x88u);
	KUNIT_EXPECT_EQ(test, c.tsp_config, 0x30u);
	KUNIT_EXPECT_EQ(test, c.event_pixel_pds_exec, 0x2e001080u);
	KUNIT_EXPECT_EQ(test, c.event_pixel_pds_data, 5u);
	KUNIT_EXPECT_EQ(test, c.event_pixel_pds_info, 1u);
	KUNIT_EXPECT_EQ(test, c.isp_fpuctrl, 1u);
	KUNIT_EXPECT_EQ(test, c.isp_depthsort, 0x100u);
	KUNIT_EXPECT_EQ(test, c.bgobj_words[0], 0x04000104u);	/* secondary */
	KUNIT_EXPECT_EQ(test, c.bgobj_words[1], 0x00030000u);
	KUNIT_EXPECT_EQ(test, c.bgobj_words[2], 0x04000102u);	/* bg primary */
	KUNIT_EXPECT_EQ(test, sgx_render_check_size(&c), 0);

	sgx_render_bgobj(bg, &c);
	sgx_xfer_bgobj(ref, 0x2e000000, 0x2e001020, 16,
		       0x2e001040, 16, &whole);	/* 4 data dwords each */
	for (i = 0; i < SGX_XFER_BGOBJ_DWORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, bg[i], ref[i], "dword %u", i);
}

/* The CPU reference: (8, 8), (56, 20), (20, 56) in pixels, edges excluded. */
static void sgx_tri_test_expect(struct kunit *test)
{
	unsigned int x, y, in = 0, edge = 0;

	KUNIT_EXPECT_EQ(test, sgx_tri_expect(28, 28), 1);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(2, 2), 0);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(60, 60), 0);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(50, 50), 0);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(37, 38), -1);	/* on x + y = 76 */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(30, 13), -1);	/* on v0-v1 */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(30, 10), 0);	/* 3 px off it */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(50, 22), 1);	/* not mirrored: */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(50, 41), 0);	/* y = 63 - y */
	for (y = 0; y < SGX_TRI_H; y++) {
		for (x = 0; x < SGX_TRI_W; x++) {
			int e = sgx_tri_expect(x, y);

			in += e == 1;
			edge += e == -1;
		}
	}
	/* The area is 1080 pixels; the edges hold about 2 px per unit length. */
	KUNIT_EXPECT_LT(test, in, 1080u);
	KUNIT_EXPECT_GT(test, in + edge, 1080u);
	KUNIT_EXPECT_GT(test, in, 800u);
}

static struct kunit_case sgx_tri_test_cases[] = {
	KUNIT_CASE(sgx_tri_test_f32),
	KUNIT_CASE(sgx_tri_test_words),
	KUNIT_CASE(sgx_tri_test_words_area),
	KUNIT_CASE(sgx_tri_test_blocks),
	KUNIT_CASE(sgx_tri_test_vdm),
	KUNIT_CASE(sgx_tri_test_scene),
	KUNIT_CASE(sgx_tri_test_render_cmd),
	KUNIT_CASE(sgx_tri_test_expect),
	{}
};

static struct kunit_suite sgx_tri_test_suite = {
	.name = "gma500_sgx_tri",
	.test_cases = sgx_tri_test_cases,
};

kunit_test_suite(sgx_tri_test_suite);
