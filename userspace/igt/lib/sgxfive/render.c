// SPDX-License-Identifier: MIT
/*
 * Render jobs from userspace (design/sgx545_render_uapi.md §11): the first
 * triangle's scene (design/sgx545_3d.md §14), what the kernel's debugfs test
 * builds in kernel/gma500/sgxfive/sgx_tri.c, here at the GPU addresses of a
 * client VM and for any render-target size the kernel accepts. The programs
 * are our own (tri_progs_gen.h); this file patches the fields their sources
 * leave at 0: DOUTU words, immediates, the PBE state and phase addresses.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_render.h"
#include "tri_progs_gen.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define BIT(n)		(1u << (n))

uint32_t sgxfive_f32(float f)
{
	uint32_t v;

	memcpy(&v, &f, 4);
	return v;
}

/*
 * DOUTU U0 of a program at addr (design/sgx545_transfer.md §4.2.4): the
 * instruction index from the code base in [19:4], the code base selector in
 * [3:0], temporaries in fours in [31:24].
 */
uint32_t sgxfive_tri_doutu(uint32_t addr, uint32_t code_base, uint32_t cbase, uint32_t temps)
{
	return (((addr - code_base) << 1) & 0xffff0) | cbase | ((temps + 3) >> 2) << 24;
}

/* A pixel PDS pointer (S.5): data rows in [31:26], the offset >> 4 below. */
uint32_t sgxfive_tri_pix_ptr(const struct sgxfive_tri_layout *l, uint32_t addr,
			     uint32_t data_dwords)
{
	return ((addr - l->pds_exec_base) >> 4) | (data_dwords / 4) << 26;
}

/*
 * The doutd control word of an n-dword copy into attribute 0 (S.2): bursts of
 * at most 16 dwords, burst - 1 in [3:0], lines - 1 in [7:4], stride - 1 in
 * [29:22] (a line per burst).
 */
uint32_t sgxfive_tri_doutd(uint32_t dwords)
{
	uint32_t lines = 1, burst;

	while (dwords / lines > 16 || dwords % lines)
		lines++;
	burst = dwords / lines;
	return (burst - 1) | (lines - 1) << 4 | (burst - 1) << 22;
}

void sgxfive_tri_default(struct sgxfive_tri_params *p, uint32_t width, uint32_t height)
{
	static const int32_t v64[3][2] = { { 8, 8 }, { 56, 20 }, { 20, 56 } };

	memset(p, 0, sizeof(*p));
	p->width = width;
	p->height = height;
	p->colour = 0xff00ff00;
	p->bg_colour = 0xff203040;
	for (int i = 0; i < 3; i++) {
		p->v[i][0] = v64[i][0] * 16 * (int32_t)width / 64;
		p->v[i][1] = v64[i][1] * 16 * (int32_t)height / 64;
	}
}

static uint32_t sec_ptr(const struct sgxfive_tri_layout *l)
{
	return sgxfive_tri_pix_ptr(l, l->pdsp + SGXFIVE_TRI_PP_SEC, TRI_PDS_SEC_DATA);
}

static uint32_t pri_ptr(const struct sgxfive_tri_layout *l, uint32_t off)
{
	return sgxfive_tri_pix_ptr(l, l->pdsp + off, TRI_PDS_PRIM_DATA);
}

/* The size word between the pointers (S.5, the transfer's bg object). */
#define PIX_SIZE(pri_rows)	((pri_rows) | 3u << 16)

/*
 * S.2-S.4, S.9: the captured triangle's state block with our pixel programs:
 * ISP words 0 and 1 (opaque, depth test off, RGBA written), the pixel
 * pointers, the viewport (x/y offset and scale, z offset and scale), the
 * output selects (4 dwords per vertex), the w clamp and the MTE control (no
 * culling). No iterated coordinates.
 */
void sgxfive_tri_block(uint32_t blk[SGXFIVE_TRI_BLOCK_WORDS], const struct sgxfive_tri_layout *l,
		       const struct sgxfive_tri_params *p)
{
	const uint32_t half_w = sgxfive_f32(p->width / 2.0f);
	const uint32_t half_h = sgxfive_f32(p->height / 2.0f);
	unsigned int i = 0;

	blk[i++] = 0xdf43;		/* presence: bits 0 1 6 8-12 14 15 */
	blk[i++] = 0x01d00200;		/* isp0 */
	blk[i++] = 0x10f0;		/* isp1 */
	blk[i++] = sec_ptr(l);
	blk[i++] = PIX_SIZE(1);
	blk[i++] = pri_ptr(l, SGXFIVE_TRI_PP_PRIM);
	blk[i++] = half_w;		/* vp0-vp5 */
	blk[i++] = half_w;
	blk[i++] = half_h;
	blk[i++] = half_h;
	blk[i++] = sgxfive_f32(0.5f);
	blk[i++] = sgxfive_f32(0.5f);
	blk[i++] = 0;			/* wrap */
	blk[i++] = 0x43000;		/* outsel */
	blk[i++] = 0x3727c5ac;		/* wclamp, 1e-5 */
	blk[i++] = 0x01555620;		/* mtectl */
	blk[i++] = 0;			/* coordsize */
	blk[i++] = 0;			/* coordfmt */
}

/* S.4: the end of scene, the last 32 x 32 block column and row. */
void sgxfive_tri_eos(uint32_t blk[SGXFIVE_TRI_EOS_WORDS], const struct sgxfive_tri_params *p)
{
	blk[0] = 0x2000;
	blk[1] = ((p->width - 1) / 32) << 16 | (p->height - 1) / 32;
}

/*
 * A VDM state block (§9.1): the PDS program >> 4, and its second word: data
 * rows (31:26), SD (25), MTE emit (17), USE pipe 1 (15:13), partitions 2
 * (11:9) and the USE attribute rows (7:0), as the captured state copies.
 */
static void vdm_state(uint32_t *w, uint32_t pds, uint32_t data_dwords, uint32_t attr_dwords,
		      bool last)
{
	w[0] = 1u << 30 | (last ? BIT(29) : 0) | ((pds >> 4) & 0x0fffffff);
	w[1] = (data_dwords / 4) << 26 | BIT(25) | BIT(17) | 1u << 13 | 2u << 9 |
	       (attr_dwords + 3) / 4;
}

/*
 * S.1: the triangle's state, one index list of three 16-bit indices with the
 * captured third and fourth words and its vertex task (partition size 15,
 * the PDS program; a 4-dword vertex, 3 in 23:22, one attribute row, the PDS
 * data rows), then the end of scene as the last task, then terminate.
 */
void sgxfive_tri_vdm(uint32_t vdm[SGXFIVE_TRI_VDM_WORDS], const struct sgxfive_tri_layout *l)
{
	vdm_state(vdm, l->pdsv + SGXFIVE_TRI_PV_MTE, TRI_PDS_MTE_DATA, SGXFIVE_TRI_BLOCK_WORDS,
		  false);
	vdm[2] = 2u << 30 | BIT(23) | BIT(22) | 3;	/* triangles, 3 indices */
	vdm[3] = l->gen + SGXFIVE_TRI_G_IDX;
	vdm[4] = 0xf0000000;
	vdm[5] = 0x003fffff;
	vdm[6] = 15u << 28 | (((l->pdsv + SGXFIVE_TRI_PV_VTX) >> 4) & 0x0fffffff);
	vdm[7] = 1u << 24 | 3u << 22 | 1u << 7 | TRI_PDS_VTX_DATA / 4;
	vdm_state(vdm + 8, l->pdsv + SGXFIVE_TRI_PV_EOS, TRI_PDS_MTE_DATA,
		  SGXFIVE_TRI_EOS_WORDS, true);
	vdm[10] = 3u << 30;				/* terminate */
}

/* Programs: PDS words are dwords, USSE instructions little-endian dword pairs. */
static uint32_t *put_pds(uint32_t *area, uint32_t off, const uint32_t *prog, size_t n)
{
	memcpy(area + off / 4, prog, 4 * n);
	return area + off / 4;
}

static uint32_t *put_usse(uint32_t *area, uint32_t off, const uint64_t *prog, size_t n)
{
	uint32_t *p = area + off / 4;

	for (size_t i = 0; i < n; i++) {
		p[2 * i] = (uint32_t)prog[i];
		p[2 * i + 1] = (uint32_t)(prog[i] >> 32);
	}
	return p;
}

static uint64_t get_insn(const uint32_t *p, unsigned int i)
{
	return p[2 * i] | (uint64_t)p[2 * i + 1] << 32;
}

static void set_insn(uint32_t *p, unsigned int i, uint64_t v)
{
	p[2 * i] = (uint32_t)v;
	p[2 * i + 1] = (uint32_t)(v >> 32);
}

#define PUT_PDS(area, off, prog)  put_pds(area, off, prog, ARRAY_SIZE(prog))
#define PUT_USSE(area, off, prog) put_usse(area, off, prog, ARRAY_SIZE(prog))

static void put_vertex_programs(const struct sgxfive_tri_cpu *c,
				const struct sgxfive_tri_layout *l,
				const struct sgxfive_tri_params *p)
{
	const uint32_t vbase = l->usev_base, cb = SGXFIVE_TRI_USEV_CBASE;
	uint32_t *w;

	/* pds_vtx: the vertex buffer, 4 dwords per vertex, the shader. */
	w = PUT_PDS(c->pdsv, SGXFIVE_TRI_PV_VTX, tri_pds_vtx);
	w[0] = l->gen + SGXFIVE_TRI_G_VTX;
	w[1] = 3;			/* doutd: one burst of 4 dwords */
	w[2] = 16;			/* stride */
	w[4] = sgxfive_tri_doutu(l->usev + SGXFIVE_TRI_UV_VTX, vbase, cb, 0);

	/* pds_mte: the triangle's block, then the end-of-scene block. */
	w = PUT_PDS(c->pdsv, SGXFIVE_TRI_PV_MTE, tri_pds_mte);
	w[0] = l->pdsv + SGXFIVE_TRI_PV_BLOCK;
	w[1] = sgxfive_tri_doutd(SGXFIVE_TRI_BLOCK_WORDS);
	w[4] = sgxfive_tri_doutu(l->usev + SGXFIVE_TRI_UV_MTE18, vbase, cb, 0);
	w = PUT_PDS(c->pdsv, SGXFIVE_TRI_PV_EOS, tri_pds_mte);
	w[0] = l->pdsv + SGXFIVE_TRI_PV_EOSBLOCK;
	w[1] = sgxfive_tri_doutd(SGXFIVE_TRI_EOS_WORDS);
	w[4] = sgxfive_tri_doutu(l->usev + SGXFIVE_TRI_UV_MTE2, vbase, cb, 0);
	sgxfive_tri_block(c->pdsv + SGXFIVE_TRI_PV_BLOCK / 4, l, p);
	sgxfive_tri_eos(c->pdsv + SGXFIVE_TRI_PV_EOSBLOCK / 4, p);

	/* use_vtx: its next-phase address counts from the code base. */
	w = PUT_USSE(c->usev, SGXFIVE_TRI_UV_VTX, tri_use_vtx);
	set_insn(w, 0, get_insn(w, 0) + (l->usev + SGXFIVE_TRI_UV_VTX - vbase) / 8);
	PUT_USSE(c->usev, SGXFIVE_TRI_UV_MTE18, tri_use_mtest18);
	PUT_USSE(c->usev, SGXFIVE_TRI_UV_MTE2, tri_use_mtest2);
}

static void put_fill(const struct sgxfive_tri_cpu *c, const struct sgxfive_tri_layout *l,
		     uint32_t pds_off, uint32_t use_off, uint32_t colour)
{
	uint32_t *w = PUT_USSE(c->usep, use_off, tri_use_fill);

	set_insn(w, 1, sgxfive_limm_set(get_insn(w, 1), colour));
	w = PUT_PDS(c->pdsp, pds_off, tri_pds_prim);
	w[0] = sgxfive_tri_doutu(l->usep + use_off, l->usep_base, 0, 0);
	w[1] = 4;			/* SDSOFT */
}

/* The end-of-tile task: PBE words 0-3 into r0-r3, the sideband (S.7). */
static void put_eot(const struct sgxfive_tri_cpu *c, const struct sgxfive_tri_layout *l,
		    const struct sgxfive_tri_params *p)
{
	const struct sgxfive_rect r = { 0, 0, p->width, p->height };
	uint32_t pbe[SGXFIVE_PBE_WORDS], *w;

	sgxfive_pbe(pbe, &r, 4 * p->width, l->fb);	/* ARGB8888 */
	w = PUT_USSE(c->usep, SGXFIVE_TRI_UP_EOT, tri_use_eot);
	for (unsigned int i = 0; i < 4; i++)
		set_insn(w, 1 + i, sgxfive_limm_set(get_insn(w, 1 + i) + ((uint64_t)i << 21),
						    pbe[i]));
	set_insn(w, 6, sgxfive_emitpix_sb_set(get_insn(w, 6), pbe[4]));
}

static void put_pixel_programs(const struct sgxfive_tri_cpu *c,
			       const struct sgxfive_tri_layout *l,
			       const struct sgxfive_tri_params *p)
{
	const uint32_t ubase = l->usep_base;
	uint32_t *w;

	put_fill(c, l, SGXFIVE_TRI_PP_PRIM, SGXFIVE_TRI_UP_FILL, p->colour);
	put_fill(c, l, SGXFIVE_TRI_PP_BGPRIM, SGXFIVE_TRI_UP_BGFILL, p->bg_colour);
	PUT_USSE(c->usep, SGXFIVE_TRI_UP_SEC, tri_use_sec);
	w = PUT_PDS(c->pdsp, SGXFIVE_TRI_PP_SEC, tri_pds_sec);
	w[0] = sgxfive_tri_doutu(l->usep + SGXFIVE_TRI_UP_SEC, ubase, 0, 0);
	w[1] = 4;

	PUT_USSE(c->usep, SGXFIVE_TRI_UP_PTOFF, tri_use_ptoff);
	put_eot(c, l, p);
	PUT_USSE(c->usep, SGXFIVE_TRI_UP_EOR, tri_use_eor);
	/* pds_pixevent: dwords 0-5 and 12 (design/sgx545_transfer.md §4.2.4). */
	w = PUT_PDS(c->pdsp, SGXFIVE_TRI_PP_PIXEV, tri_pds_pixevent);
	w[0] = sgxfive_tri_doutu(l->usep + SGXFIVE_TRI_UP_PTOFF, ubase, 0, 0);
	w[1] = 0;
	w[2] = 0;
	w[3] = sgxfive_tri_doutu(l->usep + SGXFIVE_TRI_UP_EOT, ubase, 0, 4);
	w[4] = sgxfive_tri_doutu(l->usep + SGXFIVE_TRI_UP_EOR, ubase, 0, 1);
	w[5] = 0x00c00000;		/* PBENABLE | ENDOFRENDER */
	w[12] = 0x00800000;		/* PBENABLE */
}

/* Clip space through the viewport (S.4): ndc = px / (W / 2) - 1, px in 1/16. */
static uint32_t ndc(int32_t px16, uint32_t size)
{
	return sgxfive_f32((float)px16 / (8.0f * (float)size) - 1.0f);
}

/*
 * The job's register words (§6, §7, §12.6, S.6): the captured TA and 3D
 * registers with the size's screen words, our pixel-event program and, for
 * the background object, our secondary and background primary.
 */
static void render_cmd(struct drm_sgxfive_render_cmd *c, const struct sgxfive_tri_layout *l,
		       const struct sgxfive_tri_params *p)
{
	memset(c, 0, sizeof(*c));
	c->vdm_ctrl_stream_base = l->gen + SGXFIVE_TRI_G_VDM;
	c->te_aa = 0x20000000;
	c->te_mtile1 = 0x01004004;
	c->te_mtile2 = 0x01004004;
	/* 32 x 16 tiles across - 1 (11:0), down - 1 (23:12). */
	c->te_screen = ((p->width + 31) / 32 - 1) | ((p->height + 15) / 16 - 1) << 12;
	c->te_mtile_stride = 0x10;
	c->mte_ctrl = 0xa2;
	c->mte_wcompare = 0x358637bd;		/* 1e-6 */
	c->mte_wclamp = 0x358637bd;
	c->mte_screen = (p->width - 1) | (p->height - 1) << 16;
	c->mte_multisamplectl = 0x88;
	c->pixelbe = 0x80;
	/*
	 * The capture's value plus bit 8, process empty regions: without it
	 * tiles no geometry touches are not rendered once the target is larger
	 * than 64 x 64 (seen on hardware).
	 */
	c->isp_ipfmisc = 0x000f0100;
	c->aa_mode_3d = 0x80000000;
	c->isp_bgobjdepth = 0x3f800000;		/* 1.0 */
	c->isp_bgobj = 0x300;			/* MASK | ENABLEBGTAG */
	c->isp_multisamplectl = 0x88;
	c->tsp_config = 0x30;
	c->event_pixel_pds_exec = (l->pdsp + SGXFIVE_TRI_PP_PIXEV) & ~0xfu;
	c->event_pixel_pds_data = TRI_PDS_PIXEVENT_DATA / 4;
	c->event_pixel_pds_info = 1;		/* one attribute */
	c->isp_fpuctrl = 1;
	c->isp_depthsort = 0x100;
	c->bgobj_words[0] = sec_ptr(l);
	c->bgobj_words[1] = PIX_SIZE(0);
	c->bgobj_words[2] = pri_ptr(l, SGXFIVE_TRI_PP_BGPRIM);
}

int sgxfive_tri_scene(const struct sgxfive_tri_cpu *c, const struct sgxfive_tri_layout *l,
		      const struct sgxfive_tri_params *p, struct drm_sgxfive_render_cmd *cmd)
{
	uint32_t *v = c->gen + SGXFIVE_TRI_G_VTX / 4;
	const uint16_t idx[4] = { 0, 1, 2, 0 };

	if (!p->width || !p->height || p->width % 2 || p->height % 2 ||
	    p->width > SGXFIVE_TRI_MAX_W || p->height > SGXFIVE_TRI_MAX_H)
		return -EINVAL;
	memset(c->pdsv, 0, SGXFIVE_PAGE_SIZE);
	memset(c->usev, 0, SGXFIVE_PAGE_SIZE);
	memset(c->pdsp, 0, SGXFIVE_PAGE_SIZE);
	memset(c->usep, 0, SGXFIVE_PAGE_SIZE);
	memset(c->gen, 0, SGXFIVE_PAGE_SIZE);
	put_vertex_programs(c, l, p);
	put_pixel_programs(c, l, p);
	sgxfive_tri_vdm(c->gen + SGXFIVE_TRI_G_VDM / 4, l);
	for (unsigned int i = 0; i < 3; i++) {
		v[4 * i] = ndc(p->v[i][0], p->width);
		v[4 * i + 1] = ndc(p->v[i][1], p->height);
		v[4 * i + 2] = 0;
		v[4 * i + 3] = sgxfive_f32(1.0f);
	}
	for (unsigned int i = 0; i < 2; i++)
		c->gen[SGXFIVE_TRI_G_IDX / 4 + i] = idx[2 * i] | (uint32_t)idx[2 * i + 1] << 16;
	render_cmd(cmd, l, p);
	return 0;
}

int sgxfive_tri_expect(const struct sgxfive_tri_params *p, uint32_t x, uint32_t y)
{
	const int64_t px = (int64_t)x * 16 + 8, py = (int64_t)y * 16 + 8;
	const int32_t (*v)[2] = p->v;
	int64_t area;
	bool near = false;

	area = (int64_t)(v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) -
	       (int64_t)(v[1][1] - v[0][1]) * (v[2][0] - v[0][0]);
	for (int i = 0; i < 3; i++) {
		const int32_t *p0 = v[i], *p1 = v[(i + 1) % 3];
		int64_t dx = p1[0] - p0[0], dy = p1[1] - p0[1];
		int64_t e = dx * (py - p0[1]) - dy * (px - p0[0]);

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

int sgxfive_tri_check(const struct sgxfive_tri_params *p, const uint32_t *fb,
		      struct sgxfive_tri_counts *n)
{
	memset(n, 0, sizeof(*n));
	for (uint32_t y = 0; y < p->height; y++) {
		for (uint32_t x = 0; x < p->width; x++) {
			uint32_t v = fb[y * p->width + x];
			int e = sgxfive_tri_expect(p, x, y);
			int f = sgxfive_tri_expect(p, x, p->height - 1 - y);

			n->tri += v == p->colour;
			n->bg += v == p->bg_colour;
			n->other += v != p->colour && v != p->bg_colour;
			if (e == 1 && v != p->colour)
				n->wrong_in++;
			else if (e == 0 && v != p->bg_colour)
				n->wrong_out++;
			else if (e == -1 && v != p->colour && v != p->bg_colour)
				n->wrong_edge++;
			if ((f == 1 && v != p->colour) || (f == 0 && v != p->bg_colour))
				n->flipped++;
		}
	}
	return n->wrong_in || n->wrong_out || n->wrong_edge ? -EIO : 0;
}

/* ---- Render engine ------------------------------------------------------------------ */

enum { PG_PDSV, PG_USEV, PG_PDSP, PG_USEP, PG_GEN, PG_COUNT };

static const uint32_t page_heap[PG_COUNT] = {
	DRM_SGXFIVE_HEAP_PDS_VERTEX, DRM_SGXFIVE_HEAP_USSE_VERTEX, DRM_SGXFIVE_HEAP_PDS_PIXEL,
	DRM_SGXFIVE_HEAP_USSE_PIXEL, DRM_SGXFIVE_HEAP_GENERAL,
};

#define FB_SIZE	(SGXFIVE_TRI_MAX_W * SGXFIVE_TRI_MAX_H * 4u + SGXFIVE_PAGE_SIZE)

struct sgxfive_render {
	struct sgxfive_vm *vm;
	struct sgxfive_dev *dev;
	uint32_t ctx;
	struct sgxfive_bo *bo, *fb_bo;
	uint8_t *map;
	uint32_t *fb;
	uint64_t page[PG_COUNT], fb_addr;
	uint32_t done;		/* syncobj signalled by the engine's last job */
	bool pending;
	struct sgxfive_tri_layout layout;
	struct drm_sgxfive_render_cmd cmd __attribute__((aligned(8)));	/* cmd_stream: 8-aligned */
};

int sgxfive_render_create(struct sgxfive_vm *vm, uint32_t ctx, struct sgxfive_render **out)
{
	struct sgxfive_dev *dev = sgxfive_vm_dev(vm);
	const struct sgxfive_info *info = sgxfive_dev_info(dev);
	struct sgxfive_render *r = calloc(1, sizeof(*r));
	void *map = NULL, *fb = NULL;
	int ret;

	if (!r)
		return -ENOMEM;
	r->vm = vm;
	r->dev = dev;
	r->ctx = ctx;
	ret = sgxfive_bo_create(dev, PG_COUNT * SGXFIVE_PAGE_SIZE,
				DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, &r->bo);
	if (!ret)
		ret = sgxfive_bo_mmap(r->bo, &map);
	for (int i = 0; !ret && i < PG_COUNT; i++) {
		ret = sgxfive_vm_alloc(vm, page_heap[i], SGXFIVE_PAGE_SIZE, SGXFIVE_PAGE_SIZE,
				       &r->page[i]);
		if (!ret)
			ret = sgxfive_vm_map(vm, r->page[i], r->bo,
					     (uint64_t)i * SGXFIVE_PAGE_SIZE, SGXFIVE_PAGE_SIZE, 0);
	}
	if (!ret)
		ret = sgxfive_bo_create(dev, FB_SIZE, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, &r->fb_bo);
	if (!ret)
		ret = sgxfive_bo_mmap(r->fb_bo, &fb);
	if (!ret)
		ret = sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, FB_SIZE, SGXFIVE_PAGE_SIZE,
				       &r->fb_addr);
	if (!ret)
		ret = sgxfive_vm_map(vm, r->fb_addr, r->fb_bo, 0, FB_SIZE, 0);
	if (!ret)
		ret = sgxfive_syncobj_create(dev, false, &r->done);
	if (ret) {
		r->map = map;
		sgxfive_render_destroy(r);
		return ret;
	}
	r->map = map;
	r->fb = fb;
	r->layout = (struct sgxfive_tri_layout){
		.pdsv = (uint32_t)r->page[PG_PDSV], .usev = (uint32_t)r->page[PG_USEV],
		.pdsp = (uint32_t)r->page[PG_PDSP], .usep = (uint32_t)r->page[PG_USEP],
		.gen = (uint32_t)r->page[PG_GEN], .fb = (uint32_t)r->fb_addr,
		.pds_exec_base = (uint32_t)info->heaps[DRM_SGXFIVE_HEAP_PDS_PIXEL].base,
		.usep_base = (uint32_t)info->heaps[DRM_SGXFIVE_HEAP_USSE_PIXEL].base,
		.usev_base = (uint32_t)info->heaps[DRM_SGXFIVE_HEAP_USSE_VERTEX].base,
	};
	*out = r;
	return 0;
}

void sgxfive_render_destroy(struct sgxfive_render *r)
{
	if (!r)
		return;
	if (r->pending)
		sgxfive_render_wait(r, 10LL * 1000000000LL);
	for (int i = 0; i < PG_COUNT; i++)
		if (r->page[i])
			sgxfive_vm_unmap(r->vm, r->page[i], SGXFIVE_PAGE_SIZE);
	if (r->fb_addr)
		sgxfive_vm_unmap(r->vm, r->fb_addr, FB_SIZE);
	if (r->done)
		sgxfive_syncobj_destroy(r->dev, r->done);
	sgxfive_bo_destroy(r->fb_bo);
	sgxfive_bo_destroy(r->bo);
	free(r);
}

int sgxfive_render_tri(struct sgxfive_render *r, const struct sgxfive_tri_params *p,
		       uint32_t wait, uint32_t signal)
{
	const struct sgxfive_tri_cpu c = {
		(uint32_t *)(r->map + PG_PDSV * SGXFIVE_PAGE_SIZE),
		(uint32_t *)(r->map + PG_USEV * SGXFIVE_PAGE_SIZE),
		(uint32_t *)(r->map + PG_PDSP * SGXFIVE_PAGE_SIZE),
		(uint32_t *)(r->map + PG_USEP * SGXFIVE_PAGE_SIZE),
		(uint32_t *)(r->map + PG_GEN * SGXFIVE_PAGE_SIZE),
	};
	struct drm_sgxfive_sync_op ops[3] = {
		{ .handle = r->done, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL },
	};
	struct drm_sgxfive_job job = {
		.type = DRM_SGXFIVE_JOB_TYPE_RENDER, .context_handle = r->ctx,
		.cmd_stream = (uintptr_t)&r->cmd, .cmd_stream_len = sizeof(r->cmd),
	};
	uint32_t nops = 1;
	int ret;

	if (r->pending)
		return -EBUSY;
	ret = sgxfive_tri_scene(&c, &r->layout, p, &r->cmd);
	if (ret)
		return ret;
	for (uint32_t i = 0; i < FB_SIZE / 4; i++)
		r->fb[i] = SGXFIVE_TRI_SENTINEL;
	if (signal)
		ops[nops++] = (struct drm_sgxfive_sync_op){
			.handle = signal, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL };
	if (wait)
		ops[nops++] = (struct drm_sgxfive_sync_op){ .handle = wait };
	job.sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(nops, ops);
	ret = sgxfive_submit(r->dev, &job, 1, NULL);
	if (!ret)
		r->pending = true;
	return ret;
}

int sgxfive_render_wait(struct sgxfive_render *r, int64_t timeout_ns)
{
	int ret;

	if (!r->pending)
		return 0;
	ret = sgxfive_syncobj_wait(r->dev, r->done, 0, timeout_ns);
	if (!ret)
		r->pending = false;
	return ret;
}

const uint32_t *sgxfive_render_fb(const struct sgxfive_render *r)
{
	return r->fb;
}

uint32_t sgxfive_render_guard_dirty(const struct sgxfive_render *r,
				    const struct sgxfive_tri_params *p)
{
	uint32_t n = 0;

	for (uint32_t i = p->width * p->height; i < FB_SIZE / 4; i++)
		n += r->fb[i] != SGXFIVE_TRI_SENTINEL;
	return n;
}

const struct drm_sgxfive_render_cmd *sgxfive_render_last_cmd(const struct sgxfive_render *r)
{
	return &r->cmd;
}

void sgxfive_render_get_layout(const struct sgxfive_render *r, struct sgxfive_tri_layout *l)
{
	*l = r->layout;
}
