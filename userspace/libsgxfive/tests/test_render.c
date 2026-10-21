// SPDX-License-Identifier: MIT
/*
 * The render builders (src/render.c) against the first triangle the kernel's
 * debugfs test rendered on the laptop (design/sgx545_3d.md §14): at the same
 * addresses they must produce the same words as kernel/gma500/sgxfive/
 * sgx_tri.c (whose KUnit tests pin those values), and other sizes must move
 * exactly the size-dependent words.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_render.h"
#include "tap.h"

static const struct sgxfive_tri_layout L = {
	.pdsv = 0x30401000, .usev = 0x3c001000, .pdsp = 0x2e001000, .usep = 0x37001000,
	.gen = 0x3e001000, .fb = 0x3e010000, .pds_exec_base = 0x2e000000,
	.usep_base = 0x37000000, .usev_base = 0x3c000000,
};

static uint64_t qw(const uint32_t *p, unsigned int insn)
{
	return p[2 * insn] | (uint64_t)p[2 * insn + 1] << 32;
}

static void test_words(void)
{
	CHECK_EQ(sgxfive_tri_doutu(0xfc008d80, 0xfc000000, 10, 0), 0x11b0a);
	CHECK_EQ(sgxfive_tri_doutu(0xf4000020, 0xf4000000, 0, 4), 0x01000040);
	CHECK_EQ(sgxfive_tri_pix_ptr(&L, 0x2e001040, 4), 0x04000104);
	CHECK_EQ(sgxfive_tri_doutd(12), 0x02c0000b);
	CHECK_EQ(sgxfive_tri_doutd(18), 0x02000018);
	CHECK_EQ(sgxfive_tri_doutd(2), 0x00400001);
	CHECK_EQ(sgxfive_f32(-0.75f), 0xbf400000);
	CHECK_EQ(sgxfive_f32(32.0f), 0x42000000);
}

static void test_default(void)
{
	struct sgxfive_tri_params p;

	sgxfive_tri_default(&p, 64, 64);
	CHECK_EQ(p.width, 64);
	CHECK_EQ(p.v[0][0], 8 * 16);
	CHECK_EQ(p.v[1][0], 56 * 16);
	CHECK_EQ(p.v[1][1], 20 * 16);
	CHECK_EQ(p.v[2][1], 56 * 16);
	sgxfive_tri_default(&p, 256, 128);
	CHECK_EQ(p.v[1][0], 56 * 4 * 16);
	CHECK_EQ(p.v[1][1], 20 * 2 * 16);
}

/* S.2, S.9: the kernel test's state block; and the presence word's count. */
static void test_block(void)
{
	static const uint32_t want[SGXFIVE_TRI_BLOCK_WORDS] = {
		0xdf43, 0x01d00200, 0x10f0, 0x04000104, 0x00030001, 0x04000100,
		0x42000000, 0x42000000, 0x42000000, 0x42000000, 0x3f000000,
		0x3f000000, 0, 0x43000, 0x3727c5ac, 0x01555620, 0, 0,
	};
	uint32_t blk[SGXFIVE_TRI_BLOCK_WORDS], eos[SGXFIVE_TRI_EOS_WORDS];
	struct sgxfive_tri_params p;

	sgxfive_tri_default(&p, 64, 64);
	sgxfive_tri_block(blk, &L, &p);
	for (unsigned int i = 0; i < SGXFIVE_TRI_BLOCK_WORDS; i++)
		CHECK_EQ(blk[i], want[i]);
	sgxfive_tri_eos(eos, &p);
	CHECK_EQ(eos[0], 0x2000);
	CHECK_EQ(eos[1], 0x00010001);
	/* 256 x 128: the viewport and the last 32 x 32 block move. */
	sgxfive_tri_default(&p, 256, 128);
	sgxfive_tri_block(blk, &L, &p);
	CHECK_EQ(blk[6], 0x43000000);		/* 128.0 */
	CHECK_EQ(blk[8], 0x42800000);		/* 64.0 */
	sgxfive_tri_eos(eos, &p);
	CHECK_EQ(eos[1], 0x00070003);
}

static void test_vdm(void)
{
	static const uint32_t want[SGXFIVE_TRI_VDM_WORDS] = {
		0x43040104, 0x0a022405, 0x80c00003, 0x3e001200, 0xf0000000, 0x003fffff,
		0xf3040100, 0x01c00082, 0x63040108, 0x0a022401, 0xc0000000,
	};
	uint32_t vdm[SGXFIVE_TRI_VDM_WORDS];

	sgxfive_tri_vdm(vdm, &L);
	for (unsigned int i = 0; i < SGXFIVE_TRI_VDM_WORDS; i++)
		CHECK_EQ(vdm[i], want[i]);
}

/* The programs in place with their patched fields, as sgx_tri_test_scene. */
static void test_scene(void)
{
	struct sgxfive_tri_cpu c = {
		calloc(1, 4096), calloc(1, 4096), calloc(1, 4096), calloc(1, 4096),
		calloc(1, 4096),
	};
	struct drm_sgxfive_render_cmd cmd;
	struct sgxfive_tri_params p;
	const uint32_t *pv, *pp, *g;

	sgxfive_tri_default(&p, 64, 64);
	p.colour = 0xff00ff00;
	p.bg_colour = 0xff203040;
	memset(&cmd, 0xa5, sizeof(cmd));
	CHECK_EQ(sgxfive_tri_scene(&c, &L, &p, &cmd), 0);

	pv = c.pdsv + SGXFIVE_TRI_PV_VTX / 4;
	CHECK_EQ(pv[0], 0x3e001100);
	CHECK_EQ(pv[1], 3);
	CHECK_EQ(pv[2], 16);
	CHECK_EQ(pv[4], 0x200a);
	CHECK_EQ(pv[8], 0x67800072);
	pv = c.pdsv + SGXFIVE_TRI_PV_MTE / 4;
	CHECK_EQ(pv[0], 0x30401100);
	CHECK_EQ(pv[1], 0x02000018);
	CHECK_EQ(pv[4], 0x210a);
	pv = c.pdsv + SGXFIVE_TRI_PV_EOS / 4;
	CHECK_EQ(pv[0], 0x30401180);
	CHECK_EQ(pv[1], 0x00400001);
	CHECK_EQ(pv[4], 0x220a);
	CHECK_EQ(c.pdsv[SGXFIVE_TRI_PV_BLOCK / 4], 0xdf43);
	CHECK_EQ(c.pdsv[SGXFIVE_TRI_PV_EOSBLOCK / 4], 0x2000);
	CHECK_EQ(qw(c.usev, 0), 0xfa44020000000207ull);
	CHECK_EQ(qw(c.usev + SGXFIVE_TRI_UV_MTE18 / 4, 9), 0xfb2f4000a0200000ull);

	pp = c.pdsp;
	CHECK_EQ(pp[SGXFIVE_TRI_PP_PRIM / 4], 0x2000);
	CHECK_EQ(pp[SGXFIVE_TRI_PP_PRIM / 4 + 1], 4);
	CHECK_EQ(pp[SGXFIVE_TRI_PP_BGPRIM / 4], 0x2020);
	CHECK_EQ(pp[SGXFIVE_TRI_PP_SEC / 4], 0x2040);
	pp = c.pdsp + SGXFIVE_TRI_PP_PIXEV / 4;
	CHECK_EQ(pp[0], 0x2060);
	CHECK_EQ(pp[3], 0x01002080);
	CHECK_EQ(pp[4], 0x01002100);
	CHECK_EQ(pp[5], 0x00c00000);
	CHECK_EQ(pp[12], 0x00800000);
	CHECK_EQ(pp[6], 8);

	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_FILL / 4, 1)), 0xff00ff00);
	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_BGFILL / 4, 1)), 0xff203040);
	CHECK_EQ(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 1), 0xfca0000000000000ull);
	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 2)), 0x003f003f);
	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 3)), 0x001f8000);
	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 4)), 0x3e010000);
	CHECK_EQ((qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 4) >> 21) & 0x7f, 3);
	CHECK_EQ(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 6),
		 sgxfive_emitpix_sb_set(0xfb24000000208180ull, 0x200));

	g = c.gen;
	CHECK_EQ(g[SGXFIVE_TRI_G_VTX / 4 + 0], 0xbf400000);
	CHECK_EQ(g[SGXFIVE_TRI_G_VTX / 4 + 3], 0x3f800000);
	CHECK_EQ(g[SGXFIVE_TRI_G_VTX / 4 + 4], 0x3f400000);
	CHECK_EQ(g[SGXFIVE_TRI_G_VTX / 4 + 5], 0xbec00000);
	CHECK_EQ(g[SGXFIVE_TRI_G_VTX / 4 + 9], 0x3f400000);
	CHECK_EQ(g[SGXFIVE_TRI_G_IDX / 4], 0x00010000);
	CHECK_EQ(g[SGXFIVE_TRI_G_IDX / 4 + 1] & 0xffff, 2);
	CHECK_EQ(g[SGXFIVE_TRI_G_VDM / 4 + 10], 0xc0000000);

	/* The register words: sgx_tri_render_cmd's for 64 x 64. */
	CHECK_EQ(cmd.vdm_ctrl_stream_base, 0x3e001000);
	CHECK_EQ(cmd.te_aa, 0x20000000);
	CHECK_EQ(cmd.te_mtile1, 0x01004004);
	CHECK_EQ(cmd.te_mtile2, 0x01004004);
	CHECK_EQ(cmd.te_screen, 0x3001);
	CHECK_EQ(cmd.te_mtile_stride, 0x10);
	CHECK_EQ(cmd.mte_ctrl, 0xa2);
	CHECK_EQ(cmd.mte_wcompare, 0x358637bd);
	CHECK_EQ(cmd.mte_wclamp, 0x358637bd);
	CHECK_EQ(cmd.mte_screen, 0x003f003f);
	CHECK_EQ(cmd.mte_multisamplectl, 0x88);
	CHECK_EQ(cmd.pixelbe, 0x80);
	/* The capture's 0x000f0000 plus process-empty: larger targets need it [HW]. */
	CHECK_EQ(cmd.isp_ipfmisc, 0x000f0100);
	CHECK_EQ(cmd.aa_mode_3d, 0x80000000);
	CHECK_EQ(cmd.isp_bgobjdepth, 0x3f800000);
	CHECK_EQ(cmd.isp_bgobj, 0x300);
	CHECK_EQ(cmd.isp_multisamplectl, 0x88);
	CHECK_EQ(cmd.tsp_config, 0x30);
	CHECK_EQ(cmd.event_pixel_pds_exec, 0x2e001080);
	CHECK_EQ(cmd.event_pixel_pds_data, 5);
	CHECK_EQ(cmd.event_pixel_pds_info, 1);
	CHECK_EQ(cmd.isp_fpuctrl, 1);
	CHECK_EQ(cmd.isp_depthsort, 0x100);
	CHECK_EQ(cmd.bgobj_words[0], 0x04000104);
	CHECK_EQ(cmd.bgobj_words[1], 0x00030000);
	CHECK_EQ(cmd.bgobj_words[2], 0x04000102);

	/* 256 x 128: the screen words and the PBE state follow the size. */
	sgxfive_tri_default(&p, 256, 128);
	CHECK_EQ(sgxfive_tri_scene(&c, &L, &p, &cmd), 0);
	CHECK_EQ(cmd.te_screen, 7 | 7 << 12);
	CHECK_EQ(cmd.mte_screen, 255 | 127 << 16);
	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 2)), 0x007f00ff);
	CHECK_EQ(sgxfive_limm_get(qw(c.usep + SGXFIVE_TRI_UP_EOT / 4, 3)), 0x007f8000);
	/* Sizes the kernel refuses, odd sizes. */
	sgxfive_tri_default(&p, 64, 256);
	CHECK_EQ(sgxfive_tri_scene(&c, &L, &p, &cmd), -EINVAL);
	sgxfive_tri_default(&p, 512, 64);
	CHECK_EQ(sgxfive_tri_scene(&c, &L, &p, &cmd), -EINVAL);
	sgxfive_tri_default(&p, 63, 64);
	CHECK_EQ(sgxfive_tri_scene(&c, &L, &p, &cmd), -EINVAL);
	free(c.pdsv);
	free(c.usev);
	free(c.pdsp);
	free(c.usep);
	free(c.gen);
}

/* The kernel test's reference points, and a reference image checks clean. */
static void test_expect(void)
{
	struct sgxfive_tri_params p;
	struct sgxfive_tri_counts n;
	uint32_t *fb = calloc(64 * 64, 4), in = 0, edge = 0;

	sgxfive_tri_default(&p, 64, 64);
	p.colour = 0xff00ff00;
	p.bg_colour = 0xff203040;
	CHECK_EQ(sgxfive_tri_expect(&p, 28, 28), 1);
	CHECK_EQ(sgxfive_tri_expect(&p, 2, 2), 0);
	CHECK_EQ(sgxfive_tri_expect(&p, 60, 60), 0);
	CHECK_EQ(sgxfive_tri_expect(&p, 37, 38), (uint64_t)-1);
	CHECK_EQ(sgxfive_tri_expect(&p, 30, 13), (uint64_t)-1);
	CHECK_EQ(sgxfive_tri_expect(&p, 50, 22), 1);
	CHECK_EQ(sgxfive_tri_expect(&p, 50, 41), 0);
	for (uint32_t y = 0; y < 64; y++)
		for (uint32_t x = 0; x < 64; x++) {
			int e = sgxfive_tri_expect(&p, x, y);

			in += e == 1;
			edge += e == -1;
			fb[y * 64 + x] = e == 1 ? p.colour : p.bg_colour;
		}
	CHECK(in < 1080 && in + edge > 1080 && in > 800);
	CHECK_EQ(sgxfive_tri_check(&p, fb, &n), 0);
	CHECK_EQ(n.wrong_in + n.wrong_out + n.wrong_edge, 0);
	CHECK_EQ(n.tri, in);
	CHECK(n.flipped > 0);
	fb[28 * 64 + 28] = 0x12345678;			/* one wrong pixel inside */
	CHECK_EQ(sgxfive_tri_check(&p, fb, &n), -EIO);
	CHECK_EQ(n.wrong_in, 1);
	CHECK_EQ(n.other, 1);
	free(fb);
}

int main(void)
{
	test_words();
	test_default();
	test_block();
	test_vdm();
	test_scene();
	test_expect();
	return tap_done();
}
