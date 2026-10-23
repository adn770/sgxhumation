// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The first SGX545 triangle's scene (design/sgx545_3d.md, "First triangle"):
 * the VDM control stream, the MTE state, the geometry and our own programs
 * (sgx_tri_progs.h, assembled from asm/) with their
 * per-placement fields, and the register words of the job, which the shared
 * builders of sgx_render.c turn into the firmware's structures. Pure code: it
 * writes into buffers it is given and the debugfs test (sgxfive.c) kicks it.
 * It is what a render-node client does for itself (libsgxfive's render.c).
 *
 * The job is §12.6's minimal one. The state is the captured triangle's
 * (§13 S.9): one opaque, unclipped triangle in clip space through the 64 x 64
 * viewport, depth test off, a constant colour from the pixel shader, and the
 * background object filling the rest of the surface with a second colour.
 */
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "sgx_fw.h"
#include "sgx_tri.h"
#include "sgx_tri_progs.h"
#include "sgx_xfer.h"

/* num / 2^shift as an IEEE single, truncated (small exact values here). */
u32 sgx_tri_f32(s32 num, unsigned int shift)
{
	u32 sign = num < 0 ? BIT(31) : 0, mag = num < 0 ? -num : num, mant;
	int top;

	if (!mag)
		return 0;
	top = fls(mag) - 1;
	mant = top <= 23 ? mag << (23 - top) : mag >> (top - 23);
	return sign | (u32)(top - (int)shift + 127) << 23 | (mant & 0x7fffff);
}

/*
 * DOUTU U0 of a program at addr (design/sgx545_transfer.md §4.2.4): EXE, the
 * instruction index from the code base, in [19:4], the code base selector in
 * [3:0], temporaries in fours in [31:24]. Programs stay in the first 512 KiB.
 */
u32 sgx_tri_doutu(u32 addr, u32 code_base, u32 cbase, u32 temps)
{
	return (((addr - code_base) << 1) & 0xffff0) | cbase |
	       ((temps + 3) >> 2) << 24;
}

/* A pixel PDS pointer (S.5): data rows in [31:26], the offset >> 4 below. */
u32 sgx_tri_pix_ptr(const struct sgx_tri_addrs *a, u32 addr, u32 data_dwords)
{
	return ((addr - a->pds_exec_base) >> 4) | (data_dwords / 4) << 26;
}

/*
 * The doutd control word of an n-dword copy into attribute 0 (S.2): bursts of
 * at most 16 dwords, burst - 1 in [3:0], lines - 1 in [7:4], stride - 1 in
 * [29:22] (a line per burst).
 */
u32 sgx_tri_doutd(u32 dwords)
{
	u32 lines = 1, burst;

	while (dwords / lines > 16 || dwords % lines)
		lines++;
	burst = dwords / lines;
	return (burst - 1) | (lines - 1) << 4 | (burst - 1) << 22;
}

/* The pixel programs' pointers: the triangle's and the background's. */
#define PDS_PRIM_DATA	SGX_TRI_PDS_PRIM_DATA
#define PDS_SEC_DATA	SGX_TRI_PDS_SEC_DATA

static u32 sec_ptr(const struct sgx_tri_addrs *a)
{
	return sgx_tri_pix_ptr(a, a->pdsp + SGX_TRI_PP_SEC, PDS_SEC_DATA);
}

static u32 pri_ptr(const struct sgx_tri_addrs *a, u32 off)
{
	return sgx_tri_pix_ptr(a, a->pdsp + off, PDS_PRIM_DATA);
}

/* The size word between the pointers (S.5, the transfer's bg object). */
#define PIX_SIZE(pri_rows)	((pri_rows) | 3 << 16)

/* The scratch words: completion targets zeroed, the bg update list set. */
void sgx_tri_words(u32 *w, const struct sgx_tri_addrs *a)
{
	memset(w, 0, 4 * SGX_TRI_W_COUNT);
	w[SGX_TRI_W_BGPRI] = pri_ptr(a, SGX_TRI_PP_BGPRIM);
}

/*
 * The job's register words (§6, §7, §12.6, S.6): the captured TA and 3D
 * registers for a 64 x 64 target, the VDM stream vdm, our pixel-event program
 * and, for the background object, our secondary and background primary.
 */
void sgx_tri_render_cmd(struct drm_sgxfive_render_cmd *c,
			const struct sgx_tri_addrs *a, u32 vdm)
{
	memset(c, 0, sizeof(*c));
	c->vdm_ctrl_stream_base = vdm;
	c->te_aa = 0x20000000;
	c->te_mtile1 = 0x01004004;
	c->te_mtile2 = 0x01004004;
	/* 32 x 16 tiles across - 1 (11:0), down - 1 (23:12). */
	c->te_screen = (SGX_TRI_W / 32 - 1) | (SGX_TRI_H / 16 - 1) << 12;
	c->te_mtile_stride = 0x10;
	c->mte_ctrl = 0xa2;
	c->mte_wcompare = 0x358637bd;		/* 1e-6 */
	c->mte_wclamp = 0x358637bd;
	c->mte_screen = (SGX_TRI_W - 1) | (SGX_TRI_H - 1) << 16;
	c->mte_multisamplectl = 0x88;
	c->pixelbe = 0x80;
	c->isp_ipfmisc = 0x000f0000;
	c->aa_mode_3d = 0x80000000;
	c->isp_bgobjdepth = 0x3f800000;		/* 1.0 */
	c->isp_bgobj = 0x300;			/* MASK | ENABLEBGTAG */
	c->isp_multisamplectl = 0x88;
	c->tsp_config = 0x30;
	c->event_pixel_pds_exec = (a->pdsp + SGX_TRI_PP_PIXEV) & ~0xfu;
	c->event_pixel_pds_data = SGX_TRI_PDS_PIXEVENT_DATA / 4;
	c->event_pixel_pds_info = 1;		/* one attribute */
	c->isp_fpuctrl = 1;
	c->isp_depthsort = 0x100;
	c->bgobj_words[0] = sec_ptr(a);
	c->bgobj_words[1] = PIX_SIZE(0);
	c->bgobj_words[2] = pri_ptr(a, SGX_TRI_PP_BGPRIM);
}

/*
 * S.2-S.4, S.9: the captured triangle's state block with our pixel programs:
 * ISP words 0 and 1 (opaque, depth test off, RGBA written), the pixel
 * pointers, the viewport (x/y offset and scale, z offset and scale), the
 * output selects (4 dwords per vertex), the w clamp and the MTE control (no
 * culling). No iterated coordinates.
 */
void sgx_tri_block(u32 blk[SGX_TRI_BLOCK_WORDS], const struct sgx_tri_addrs *a)
{
	const u32 half_w = sgx_tri_f32(SGX_TRI_W / 2, 0);
	const u32 half_h = sgx_tri_f32(SGX_TRI_H / 2, 0);
	unsigned int i = 0;

	blk[i++] = 0xdf43;		/* presence: bits 0 1 6 8-12 14 15 */
	blk[i++] = 0x01d00200;		/* isp0 */
	blk[i++] = 0x10f0;		/* isp1 */
	blk[i++] = sec_ptr(a);
	blk[i++] = PIX_SIZE(1);
	blk[i++] = pri_ptr(a, SGX_TRI_PP_PRIM);
	blk[i++] = half_w;		/* vp0-vp5 */
	blk[i++] = half_w;
	blk[i++] = half_h;
	blk[i++] = half_h;
	blk[i++] = sgx_tri_f32(1, 1);
	blk[i++] = sgx_tri_f32(1, 1);
	blk[i++] = 0;			/* wrap */
	blk[i++] = 0x43000;		/* outsel */
	blk[i++] = 0x3727c5ac;		/* wclamp, 1e-5 */
	blk[i++] = 0x01555620;		/* mtectl */
	blk[i++] = 0;			/* coordsize */
	blk[i++] = 0;			/* coordfmt */
}

/* S.4: the end of scene, the last 32 x 32 block column and row. */
void sgx_tri_eos(u32 blk[SGX_TRI_EOS_WORDS])
{
	blk[0] = 0x2000;
	blk[1] = ((SGX_TRI_W - 1) / 32) << 16 | (SGX_TRI_H - 1) / 32;
}

/*
 * A VDM state block (§9.1): the PDS program >> 4, and its second word: data
 * rows (31:26), SD (25), MTE emit (17), USE pipe 1 (15:13), partitions 2
 * (11:9) and the USE attribute rows (7:0), as the captured state copies.
 */
static void vdm_state(u32 *w, u32 pds, u32 data_dwords, u32 attr_dwords,
		      bool last)
{
	w[0] = 1u << 30 | (last ? BIT(29) : 0) | ((pds >> 4) & 0x0fffffff);
	w[1] = (data_dwords / 4) << 26 | BIT(25) | BIT(17) | 1 << 13 | 2 << 9 |
	       DIV_ROUND_UP(attr_dwords, 4);
}

static unsigned int vdm_eos(u32 *w, const struct sgx_tri_addrs *a)
{
	vdm_state(w, a->pdsv + SGX_TRI_PV_EOS, SGX_TRI_PDS_MTE_DATA,
		  SGX_TRI_EOS_WORDS, true);
	w[2] = 3u << 30;		/* terminate */
	return 3;
}

/*
 * S.1: the triangle's state, one index list of three 16-bit indices with
 * the captured third and fourth words and its vertex task (partition size
 * 15, the PDS program; a 4-dword vertex, 3 in 23:22, one attribute row, the
 * PDS data rows), then the end of scene as the last task, then terminate.
 */
void sgx_tri_vdm(u32 *vdm, const struct sgx_tri_addrs *a)
{
	vdm_state(vdm, a->pdsv + SGX_TRI_PV_MTE, SGX_TRI_PDS_MTE_DATA,
		  SGX_TRI_BLOCK_WORDS, false);
	vdm[2] = 2u << 30 | BIT(23) | BIT(22) | 3;	/* triangles, 3 indices */
	vdm[3] = a->gen + SGX_TRI_G_IDX;
	vdm[4] = 0xf0000000;
	vdm[5] = 0x003fffff;
	vdm[6] = 15u << 28 | (((a->pdsv + SGX_TRI_PV_VTX) >> 4) & 0x0fffffff);
	vdm[7] = 1 << 24 | 3 << 22 | 1 << 7 | SGX_TRI_PDS_VTX_DATA / 4;
	vdm_eos(vdm + 8, a);
}

/* The second kick of the two-kick form: only the end of the scene. */
void sgx_tri_vdm1(u32 *vdm, const struct sgx_tri_addrs *a)
{
	vdm_eos(vdm, a);
}

/* Copy a program; USSE words are 64-bit, little-endian dword pairs. */
static u32 *put_pds(u32 *area, u32 off, const u32 *prog, size_t n)
{
	memcpy(area + off / 4, prog, 4 * n);
	return area + off / 4;
}

static u32 *put_usse(u32 *area, u32 off, const u64 *prog, size_t n)
{
	u32 *p = area + off / 4;
	size_t i;

	for (i = 0; i < n; i++) {
		p[2 * i] = lower_32_bits(prog[i]);
		p[2 * i + 1] = upper_32_bits(prog[i]);
	}
	return p;
}

static u64 get_insn(const u32 *p, unsigned int i)
{
	return p[2 * i] | (u64)p[2 * i + 1] << 32;
}

static void set_insn(u32 *p, unsigned int i, u64 v)
{
	p[2 * i] = lower_32_bits(v);
	p[2 * i + 1] = upper_32_bits(v);
}

#define PUT_PDS(area, off, prog)  put_pds(area, off, prog, ARRAY_SIZE(prog))
#define PUT_USSE(area, off, prog) put_usse(area, off, prog, ARRAY_SIZE(prog))

/* Vertices in 1/16 pixel: (8, 8), (56, 20), (20, 56), anticlockwise in y-up. */
static const s32 tri_px16[3][2] = {
	{ 8 * 16, 8 * 16 }, { 56 * 16, 20 * 16 }, { 20 * 16, 56 * 16 },
};

/* Clip space through the viewport (S.4): ndc = px / (W / 2) - 1. */
static u32 ndc(s32 px16, unsigned int size)
{
	return sgx_tri_f32(px16 - size * 8, ilog2(size * 8));
}

static void put_vertex_programs(const struct sgx_tri_cpu *c,
				const struct sgx_tri_addrs *a)
{
	const u32 vbase = a->usev_base, cb = SGX_TRI_USEV_CBASE;
	u32 *p;

	/* pds_vtx: the vertex buffer, 4 dwords per vertex, the shader. */
	p = PUT_PDS(c->pdsv, SGX_TRI_PV_VTX, sgx_tri_pds_vtx);
	p[0] = a->gen + SGX_TRI_G_VTX;
	p[1] = 3;			/* doutd: one burst of 4 dwords */
	p[2] = 16;			/* stride */
	p[4] = sgx_tri_doutu(a->usev + SGX_TRI_UV_VTX, vbase, cb, 0);

	/* pds_mte: the triangle's block, then the end-of-scene block. */
	p = PUT_PDS(c->pdsv, SGX_TRI_PV_MTE, sgx_tri_pds_mte);
	p[0] = a->pdsv + SGX_TRI_PV_BLOCK;
	p[1] = sgx_tri_doutd(SGX_TRI_BLOCK_WORDS);
	p[4] = sgx_tri_doutu(a->usev + SGX_TRI_UV_MTE18, vbase, cb, 0);
	p = PUT_PDS(c->pdsv, SGX_TRI_PV_EOS, sgx_tri_pds_mte);
	p[0] = a->pdsv + SGX_TRI_PV_EOSBLOCK;
	p[1] = sgx_tri_doutd(SGX_TRI_EOS_WORDS);
	p[4] = sgx_tri_doutu(a->usev + SGX_TRI_UV_MTE2, vbase, cb, 0);
	sgx_tri_block(c->pdsv + SGX_TRI_PV_BLOCK / 4, a);
	sgx_tri_eos(c->pdsv + SGX_TRI_PV_EOSBLOCK / 4);

	/* use_vtx: its next-phase address counts from the code base. */
	p = PUT_USSE(c->usev, SGX_TRI_UV_VTX, sgx_tri_use_vtx);
	set_insn(p, 0, get_insn(p, 0) + (a->usev + SGX_TRI_UV_VTX - vbase) / 8);
	PUT_USSE(c->usev, SGX_TRI_UV_MTE18, sgx_tri_use_mtest18);
	PUT_USSE(c->usev, SGX_TRI_UV_MTE2, sgx_tri_use_mtest2);
}

static void put_fill(const struct sgx_tri_cpu *c, const struct sgx_tri_addrs *a,
		     u32 pds_off, u32 use_off, u32 colour)
{
	u32 *p = PUT_USSE(c->usep, use_off, sgx_tri_use_fill);

	set_insn(p, 1, sgx_fw_limm_set(get_insn(p, 1), colour));
	p = PUT_PDS(c->pdsp, pds_off, sgx_tri_pds_prim);
	p[0] = sgx_tri_doutu(a->usep + use_off, a->usep_base, 0, 0);
	p[1] = 4;			/* SDSOFT */
}

/* The end-of-tile task: PBE words 0-3 into r0-r3, the sideband (S.7). */
static void put_eot(const struct sgx_tri_cpu *c, const struct sgx_tri_addrs *a)
{
	const struct sgx_xfer_rect r = { 0, 0, SGX_TRI_W, SGX_TRI_H };
	u32 pbe[SGX_XFER_PBE_WORDS], *p;
	unsigned int i;

	sgx_xfer_pbe(pbe, &r, SGX_TRI_W, a->fb, 0);	/* ARGB8888 */
	p = PUT_USSE(c->usep, SGX_TRI_UP_EOT, sgx_tri_use_eot);
	for (i = 0; i < 4; i++)
		set_insn(p, 1 + i, sgx_fw_limm_set(get_insn(p, 1 + i) + ((u64)i << 21),
						   pbe[i]));
	set_insn(p, 6, sgx_fw_emitpix_sb_set(get_insn(p, 6), pbe[4]));
}

static void put_pixel_programs(const struct sgx_tri_cpu *c,
			       const struct sgx_tri_addrs *a, u32 colour,
			       u32 bg_colour)
{
	const u32 ubase = a->usep_base;
	u32 *p;

	put_fill(c, a, SGX_TRI_PP_PRIM, SGX_TRI_UP_FILL, colour);
	put_fill(c, a, SGX_TRI_PP_BGPRIM, SGX_TRI_UP_BGFILL, bg_colour);
	PUT_USSE(c->usep, SGX_TRI_UP_SEC, sgx_tri_use_sec);
	p = PUT_PDS(c->pdsp, SGX_TRI_PP_SEC, sgx_tri_pds_sec);
	p[0] = sgx_tri_doutu(a->usep + SGX_TRI_UP_SEC, ubase, 0, 0);
	p[1] = 4;

	PUT_USSE(c->usep, SGX_TRI_UP_PTOFF, sgx_tri_use_ptoff);
	put_eot(c, a);
	PUT_USSE(c->usep, SGX_TRI_UP_EOR, sgx_tri_use_eor);
	/* pds_pixevent: dwords 0-5 and 12 (design/sgx545_transfer.md §4.2.4). */
	p = PUT_PDS(c->pdsp, SGX_TRI_PP_PIXEV, sgx_tri_pds_pixevent);
	p[0] = sgx_tri_doutu(a->usep + SGX_TRI_UP_PTOFF, ubase, 0, 0);
	p[1] = 0;
	p[2] = 0;
	p[3] = sgx_tri_doutu(a->usep + SGX_TRI_UP_EOT, ubase, 0, 4);
	p[4] = sgx_tri_doutu(a->usep + SGX_TRI_UP_EOR, ubase, 0, 1);
	p[5] = 0x00c00000;		/* PBENABLE | ENDOFRENDER */
	p[12] = 0x00800000;		/* PBENABLE */
}

/*
 * Everything the client side places: the programs, the state blocks, the VDM
 * streams, the vertices and indices.
 */
void sgx_tri_scene(const struct sgx_tri_cpu *c, const struct sgx_tri_addrs *a,
		   u32 colour, u32 bg_colour)
{
	u32 *v = c->gen + SGX_TRI_G_VTX / 4;
	u16 idx[4] = { 0, 1, 2, 0 };
	unsigned int i;

	put_vertex_programs(c, a);
	put_pixel_programs(c, a, colour, bg_colour);
	sgx_tri_vdm(c->gen + SGX_TRI_G_VDM / 4, a);
	sgx_tri_vdm1(c->gen + SGX_TRI_G_VDM1 / 4, a);
	for (i = 0; i < 3; i++) {
		v[4 * i] = ndc(tri_px16[i][0], SGX_TRI_W);
		v[4 * i + 1] = ndc(tri_px16[i][1], SGX_TRI_H);
		v[4 * i + 2] = 0;
		v[4 * i + 3] = sgx_tri_f32(1, 0);
	}
	for (i = 0; i < 2; i++)
		c->gen[SGX_TRI_G_IDX / 4 + i] = idx[2 * i] | idx[2 * i + 1] << 16;
}

/*
 * The CPU reference for pixel (x, y), row y = window y (S.4: the viewport's
 * y scale is positive): 1 when its centre is inside the triangle by at least
 * a pixel, 0 when outside by at least a pixel, -1 near an edge, where either
 * colour is accepted.
 */
int sgx_tri_expect(unsigned int x, unsigned int y)
{
	const s64 px = x * 16 + 8, py = y * 16 + 8;
	bool near = false;
	s64 area;
	int i;

	area = (s64)(tri_px16[1][0] - tri_px16[0][0]) * (tri_px16[2][1] - tri_px16[0][1]) -
	       (s64)(tri_px16[1][1] - tri_px16[0][1]) * (tri_px16[2][0] - tri_px16[0][0]);
	for (i = 0; i < 3; i++) {
		const s32 *p0 = tri_px16[i], *p1 = tri_px16[(i + 1) % 3];
		s64 dx = p1[0] - p0[0], dy = p1[1] - p0[1];
		s64 e = dx * (py - p0[1]) - dy * (px - p0[0]);

		if (area < 0)
			e = -e;
		/* |distance| >= 1 px (16 units) <=> e^2 >= 256 * |edge|^2 */
		if (e * e < 256 * (dx * dx + dy * dy))
			near = true;
		else if (e < 0)
			return 0;
	}
	return near ? -1 : 1;
}
