// SPDX-License-Identifier: MIT
/*
 * Pure builders of a FASTSCALE transfer: DOUTU words, PBE state, background
 * objects, texture words and the register block. They follow
 * design/sgx545_transfer.md §4.2 and §4.4 (and the kernel's sgx_xfer.c,
 * which runs the same recipes from debugfs).
 */
#include <errno.h>
#include <string.h>

#include "sgxfive_xfer.h"

int sgxfive_doutu(uint64_t addr, uint64_t heap_base, uint32_t temps, uint32_t *u0)
{
	uint64_t off = addr - heap_base;

	if (addr < heap_base || off >= SGXFIVE_DOUTU_RANGE || off % 8)
		return -EINVAL;
	/* EXE [19:4] (instruction index), CBASE [3:0] = 0, TRC [31:24] in fours. */
	*u0 = (uint32_t)((off << 1) & 0xffff0) | (uint32_t)((off >> 19) & 0xf) |
	      ((temps + 3) >> 2) << 24;
	return 0;
}

void sgxfive_pbe(uint32_t pbe[SGXFIVE_PBE_WORDS], const struct sgxfive_rect *r,
		 uint32_t stride, uint32_t addr)
{
	pbe[0] = r->x0 | r->y0 << 16;				/* XMIN, YMIN */
	pbe[1] = (r->x1 - 1) | (r->y1 - 1) << 16;		/* XMAX, YMAX inclusive */
	pbe[2] = 0x80 << 8 | (stride / 8 - 1) << 16;		/* COUNT, LINESTRIDE */
	pbe[3] = addr & ~3u;					/* FBADDR */
	pbe[4] = 0x200;						/* TILERELATIVE, ARGB8888 */
}

/* Vertex coordinates: 24-bit fixed point, 8 fraction bits, offset 4096. */
static uint32_t vtx(uint32_t c)
{
	return ((c + 0x1000) << 8) & 0xffffff;
}

/* Top-left, top-right, bottom-left, packed even/odd/even into 8 dwords; Z = 0. */
static void put_vertices(uint32_t v[8], const struct sgxfive_rect *r)
{
	uint32_t x0 = vtx(r->x0), y0 = vtx(r->y0), x1 = vtx(r->x1), y1 = vtx(r->y1);

	v[0] = y0 | (x0 & 0xff) << 24;
	v[1] = x0 >> 8;
	v[2] = (y0 & 0xffff) << 16;
	v[3] = (y0 >> 16) | x1 << 8;
	v[4] = 0;
	v[5] = y1 | (x0 & 0xff) << 24;
	v[6] = x0 >> 8;
	v[7] = 0;
}

/* PDS state: secondary pointer, sizes (PIXELSIZE, USETASKSIZE 3), primary pointer. */
static void put_pds_state(uint32_t bg[3], uint32_t pds_base, uint32_t pri, uint32_t pri_data,
			  uint32_t sec, uint32_t sec_data, uint32_t pri_attrs)
{
	bg[0] = ((sec - pds_base) >> 4) | (sec_data >> 4) << 26;
	bg[1] = ((pri_attrs + 3) >> 2) | 3 << 16;
	bg[2] = ((pri - pds_base) >> 4) | (pri_data >> 4) << 26;
}

void sgxfive_bgobj_fill(uint32_t bg[SGXFIVE_BGOBJ_DWORDS], uint32_t pds_base, uint32_t pri,
			uint32_t pri_data, uint32_t sec, uint32_t sec_data,
			const struct sgxfive_rect *r)
{
	memset(bg, 0, SGXFIVE_BGOBJ_DWORDS * sizeof(*bg));
	put_pds_state(bg, pds_base, pri, pri_data, sec, sec_data, 0);
	put_vertices(&bg[9], r);
}

uint32_t sgxfive_float_int_div(uint32_t x, uint32_t n)
{
	uint32_t q, m;

	if (!x || !n)
		return 0;
	if (x >= n)
		return 0x3f800000;
	q = (uint32_t)(((uint64_t)x << 20) / n);	/* in [1, 1 << 20) */
	for (m = 19; !(q >> m); m--)
		;
	return (127 + m - 20) << 23 | ((q << (23 - m)) & 0x7fffff);
}

void sgxfive_bgobj_copy(uint32_t bg[SGXFIVE_BGOBJ1_DWORDS], uint32_t pds_base, uint32_t pri,
			uint32_t pri_data, uint32_t sec, uint32_t sec_data,
			const struct sgxfive_rect *dst, const struct sgxfive_rect *src,
			uint32_t src_w, uint32_t src_h)
{
	uint32_t u0 = sgxfive_float_int_div(src->x0, src_w);
	uint32_t u1 = sgxfive_float_int_div(src->x1, src_w);
	uint32_t v0 = sgxfive_float_int_div(src->y0, src_h);
	uint32_t v1 = sgxfive_float_int_div(src->y1, src_h);

	memset(bg, 0, SGXFIVE_BGOBJ1_DWORDS * sizeof(*bg));
	put_pds_state(bg, pds_base, pri, pri_data, sec, sec_data, 1);
	bg[3] = u0;
	bg[4] = v0;
	bg[5] = u1;
	bg[6] = v0;
	bg[7] = u0;
	bg[8] = v1;
	bg[10] = 2 << 8 | 2;			/* PITCHF, PITCHB: 2 dwords per layer */
	put_vertices(&bg[15], dst);
}

void sgxfive_tex_words(uint32_t t[4], uint32_t addr, uint32_t width, uint32_t height,
		       uint32_t stride)
{
	uint32_t s = stride / 4 - 1;		/* the stride in dwords, minus one */

	t[0] = (s & 7) << 9 | (s >> 3) << 17;	/* STRIDELO [11:9], STRIDEHI [26:17], POINT */
	t[1] = 0xcc000000 | (width - 1) << 12 | (height - 1);	/* NP2_STRIDE_EXT, B8G8R8A8 */
	t[2] = addr & ~3u;
	t[3] = 0xa0800000;
}

void sgxfive_transfer_cmd(struct drm_sgxfive_transfer_cmd *c, const struct sgxfive_xfer_regs *r)
{
	const struct sgxfive_rect *b = &r->rect;

	memset(c, 0, sizeof(*c));
	c->isp_bgobjtag = (r->bg - r->req_base) >> 4 | r->layers << 25;
	c->isp_bgobj = 0x200;			/* ENABLEBGTAG */
	c->isp_bgobjdepth = 0x3f800000;		/* 1.0f */
	c->isp_render = 3;			/* FASTSCALE, scan TL2BR */
	c->isp_ipfmisc = 0x100;			/* PROCESSEMPTY */
	c->bif_3d_req_base = r->req_base;
	c->aa_mode_3d = 0x80000000;		/* GLOBALREGISTER */
	c->isp_multisamplectl = 0x88;		/* one sample at the pixel centre */
	c->event_pixel_pds_exec = r->pixev_exec & ~0xfu;
	c->event_pixel_pds_data = 0x50 >> 4;
	c->event_pixel_pds_info = (1 * 4 + 15) >> 4;
	c->isp_render_box1 = (b->y0 >> 4) | (b->x0 >> 5) << 16;
	c->isp_render_box2 = (((b->y1 + 15) >> 4) - 1) | (((b->x1 + 31) >> 5) - 1) << 16;
	c->pixelbe = 0x80;			/* ALPHATHRESHOLD */
	c->pds_exec_base = r->pds_exec_base;
}

/* USSE_LIMM: imm[20:0] at bit 0, imm[25:21] at bit 36, imm[31:26] at bit 44. */
static const struct { unsigned int insn, width, imm; } limm_fields[] = {
	{ 0, 21, 0 }, { 36, 5, 21 }, { 44, 6, 26 },
};

uint64_t sgxfive_limm_set(uint64_t insn, uint32_t imm)
{
	for (unsigned int i = 0; i < 3; i++) {
		uint64_t mask = ((1ull << limm_fields[i].width) - 1) << limm_fields[i].insn;

		insn = (insn & ~mask) |
		       ((uint64_t)(imm >> limm_fields[i].imm) << limm_fields[i].insn & mask);
	}
	return insn;
}

uint32_t sgxfive_limm_get(uint64_t insn)
{
	uint32_t imm = 0;

	for (unsigned int i = 0; i < 3; i++)
		imm |= (uint32_t)((insn >> limm_fields[i].insn) &
				  ((1ull << limm_fields[i].width) - 1)) << limm_fields[i].imm;
	return imm;
}
