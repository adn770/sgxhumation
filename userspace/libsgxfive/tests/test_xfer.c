// SPDX-License-Identifier: MIT
/*
 * The pure transfer builders against the worked examples of
 * design/sgx545_transfer.md (§4.2.3-§4.2.7 fill, §4.4.2-§4.4.5 copy).
 */
#include <errno.h>
#include <stddef.h>

#include "sgxfive_xfer.h"
#include "tap.h"

static int words_eq(const uint32_t *got, const uint32_t *want, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			printf("#   word %u: got %08x, expected %08x\n", i, got[i], want[i]);
			return 0;
		}
	}
	return 1;
}

static void doutu(void)
{
	const uint64_t base = 0xF4000000;
	uint32_t u0 = 0xdead;

	CHECK_EQ(sgxfive_doutu(base, base, 0, &u0), 0);
	CHECK_EQ(u0, 0);
	CHECK_EQ(sgxfive_doutu(base + 0x10, base, 0, &u0), 0);
	CHECK_EQ(u0, 0x20);
	CHECK_EQ(sgxfive_doutu(base + 0x20, base, 4, &u0), 0);
	CHECK_EQ(u0, 0x01000040);		/* EOT, 4 temps */
	CHECK_EQ(sgxfive_doutu(base + 0x60, base, 1, &u0), 0);
	CHECK_EQ(u0, 0x010000c0);		/* EOR, 1 temp */
	CHECK_EQ(sgxfive_doutu(base + 0x80000, base, 0, &u0), -EINVAL);
	CHECK_EQ(sgxfive_doutu(base + 4, base, 0, &u0), -EINVAL);
	CHECK_EQ(sgxfive_doutu(base - 8, base, 0, &u0), -EINVAL);
}

static void pbe(void)
{
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	const uint32_t want[] = { 0x00000000, 0x001f003f, 0x001f8000, 0xf0200000, 0x00000200 };
	uint32_t got[SGXFIVE_PBE_WORDS];

	sgxfive_pbe(got, &r, 256, 0xf0200000);
	CHECK(words_eq(got, want, 5));
}

static void bgobj(void)
{
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	const uint32_t fill[] = {
		0x04000002, 0x00030000, 0x04000000, 0, 0, 0, 0, 0,
		0, 0x00100000, 0x00001000, 0, 0x10400010, 0, 0x00102000, 0x00001000, 0,
	};
	const uint32_t copy[] = {
		0x04000004, 0x00030001, 0x0c000000, 0, 0, 0x3f800000, 0, 0,
		0x3f800000, 0, 0x00000202, 0, 0, 0, 0, 0x00100000,
		0x00001000, 0, 0x10400010, 0, 0x00102000, 0x00001000, 0,
	};
	const uint32_t P = 0xE4000000;
	uint32_t bg[SGXFIVE_BGOBJ1_DWORDS];

	sgxfive_bgobj_fill(bg, P, P, 0x10, P + 0x20, 0x10, &r);
	CHECK(words_eq(bg, fill, SGXFIVE_BGOBJ_DWORDS));
	sgxfive_bgobj_copy(bg, P, P, 0x30, P + 0x40, 0x10, &r, &r, 64, 32);
	CHECK(words_eq(bg, copy, SGXFIVE_BGOBJ1_DWORDS));
}

static void texcoords(void)
{
	const struct sgxfive_rect dst = { 0, 0, 32, 16 }, src = { 8, 4, 40, 20 };
	const uint32_t want[] = {
		0x3e2aaa80, 0x3e000000, 0x3f555550, 0x3e000000, 0x3e2aaa80, 0x3f200000,
	};
	uint32_t bg[SGXFIVE_BGOBJ1_DWORDS];

	CHECK_EQ(sgxfive_float_int_div(8, 48), 0x3e2aaa80);
	CHECK_EQ(sgxfive_float_int_div(0, 48), 0);
	CHECK_EQ(sgxfive_float_int_div(48, 48), 0x3f800000);
	CHECK_EQ(sgxfive_float_int_div(1, 2), 0x3f000000);
	sgxfive_bgobj_copy(bg, 0, 0, 0x30, 0x40, 0x10, &dst, &src, 48, 32);
	CHECK(words_eq(&bg[3], want, 6));
}

static void tex_words(void)
{
	const uint32_t want[] = { 0x000e0e00, 0xcc03f01f, 0xf0300000, 0xa0800000 };
	const struct { uint32_t stride, t0; } t0[] = {
		{ 260, 0x00100000 }, { 264, 0x00100200 }, { 288, 0x00100e00 },
		{ 320, 0x00120e00 }, { 512, 0x001e0e00 },
	};
	uint32_t t[4];

	sgxfive_tex_words(t, 0xf0300000, 64, 32, 256);
	CHECK(words_eq(t, want, 4));
	for (unsigned int i = 0; i < sizeof(t0) / sizeof(t0[0]); i++) {
		sgxfive_tex_words(t, 0, 64, 32, t0[i].stride);
		CHECK_EQ(t[0], t0[i].t0);
	}
}

static void cmd(void)
{
	struct sgxfive_xfer_regs r = {
		.rect = { 0, 0, 64, 32 }, .bg = 0xF0130000, .req_base = 0xF0100000,
		.pixev_exec = 0xE4000040, .pds_exec_base = 0xE4000000, .layers = 0,
	};
	struct drm_sgxfive_transfer_cmd c;

	sgxfive_transfer_cmd(&c, &r);
	CHECK_EQ(c.isp_bgobjtag, 0x3000);
	CHECK_EQ(c.isp_bgobj, 0x200);
	CHECK_EQ(c.isp_bgobjdepth, 0x3f800000);
	CHECK_EQ(c.isp_render, 3);
	CHECK_EQ(c.isp_rgn_base, 0);
	CHECK_EQ(c.isp_ipfmisc, 0x100);
	CHECK_EQ(c.bif_3d_req_base, 0xF0100000);
	CHECK_EQ(c.aa_mode_3d, 0x80000000);
	CHECK_EQ(c.isp_multisamplectl, 0x88);
	CHECK_EQ(c.event_pixel_pds_exec, 0xE4000040);
	CHECK_EQ(c.event_pixel_pds_data, 5);
	CHECK_EQ(c.event_pixel_pds_info, 1);
	CHECK_EQ(c.isp_render_box1, 0);
	CHECK_EQ(c.isp_render_box2, 0x00010001);
	CHECK_EQ(c.pixelbe, 0x80);
	CHECK_EQ(c.pds_exec_base, 0xE4000000);
	CHECK_EQ(c.double_pixel_partitions, 0);
	CHECK_EQ(c.load_flags, 0);
	CHECK_EQ(c.fir_hfilter[8], 0);

	r.layers = 1;
	r.pixev_exec = 0xE4000060;
	sgxfive_transfer_cmd(&c, &r);
	CHECK_EQ(c.isp_bgobjtag, 0x02003000);
	CHECK_EQ(c.event_pixel_pds_exec, 0xE4000060);

	/* A 33 x 16 rectangle at (8, 4) spans tiles 0-1 horizontally, 0-1 vertically. */
	r.rect = (struct sgxfive_rect){ 8, 4, 41, 20 };
	sgxfive_transfer_cmd(&c, &r);
	CHECK_EQ(c.isp_render_box1, 0);
	CHECK_EQ(c.isp_render_box2, 0x00010001);
}

static void limm(void)
{
	const uint64_t base = (uint64_t)0xfca40001 << 32;
	const uint64_t insn = sgxfive_limm_set(base, 0xff336699);

	CHECK_EQ((uint32_t)insn, 0x00136699);
	CHECK_EQ((uint32_t)(insn >> 32), 0xfca7f191);
	CHECK_EQ(sgxfive_limm_get(insn), 0xff336699);
	CHECK_EQ(sgxfive_limm_get(sgxfive_limm_set(base, 0xf0200000)), 0xf0200000);
}

int main(void)
{
	doutu();
	pbe();
	bgobj();
	texcoords();
	tex_words();
	cmd();
	limm();
	return tap_done();
}
