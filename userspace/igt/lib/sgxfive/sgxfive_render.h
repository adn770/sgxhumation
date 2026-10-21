/* SPDX-License-Identifier: MIT */
/*
 * libsgxfive render jobs (design/sgx545_render_uapi.md §11): the first
 * triangle (design/sgx545_3d.md §14) built in userspace. The scene builders
 * are pure: they write the programs, the MTE state, the VDM stream and the
 * geometry into buffers at given GPU addresses and fill the job's register
 * words (struct drm_sgxfive_render_cmd); the kernel builds the firmware side.
 * The render engine places a scene in a VM, submits it on a render context
 * and checks the image against a CPU reference.
 *
 * Every program is our own (tools/usse-asm/progs/, generated into
 * src/tri_progs_gen.h); none comes from the closed driver.
 */
#ifndef SGXFIVE_RENDER_H
#define SGXFIVE_RENDER_H

#include "sgxfive.h"
#include "sgxfive_xfer.h"

#define SGXFIVE_TRI_MAX_W	256	/* the kernel's render-target limit today */
#define SGXFIVE_TRI_MAX_H	128

/* One flat-coloured triangle over a background colour, ARGB8888 W x H. */
struct sgxfive_tri_params {
	uint32_t width, height;		/* even, at most SGXFIVE_TRI_MAX_W x _H */
	uint32_t colour, bg_colour;	/* ARGB8888 */
	int32_t v[3][2];		/* vertices in 1/16 pixel, window coordinates */
};

/* The first triangle's: (8, 8), (56, 20), (20, 56) in a 64 x 64 target, scaled. */
void sgxfive_tri_default(struct sgxfive_tri_params *p, uint32_t width, uint32_t height);

/*
 * Where the scene lives, by GPU address: one page each for the vertex PDS and
 * USSE programs, the pixel PDS and USSE programs and the VDM streams with the
 * geometry, then the colour buffer (stride = 4 x width). The code and PDS
 * bases are the heap bases DEV_QUERY reports: USE_CODE_BASE_0 (pixel),
 * USE_CODE_BASE_10 (vertex) and PDS_EXEC_BASE, which the kernel keeps at the
 * PDS_PIXEL heap base.
 */
struct sgxfive_tri_layout {
	uint32_t pdsv, usev, pdsp, usep, gen, fb;
	uint32_t pds_exec_base, usep_base, usev_base;
};

/* CPU views of the five program/stream pages (4 KiB each). */
struct sgxfive_tri_cpu {
	uint32_t *pdsv, *usev, *pdsp, *usep, *gen;
};

/* Offsets inside the pages (the debugfs test's, design/sgx545_3d.md §14.1). */
#define SGXFIVE_TRI_PV_VTX	0x000	/* pds_vtx */
#define SGXFIVE_TRI_PV_MTE	0x040	/* pds_mte: the triangle's state */
#define SGXFIVE_TRI_PV_EOS	0x080	/* pds_mte: end of scene */
#define SGXFIVE_TRI_PV_BLOCK	0x100	/* the triangle's MTE state block */
#define SGXFIVE_TRI_PV_EOSBLOCK	0x180	/* the end-of-scene state block */
#define SGXFIVE_TRI_UV_VTX	0x000	/* use_vtx */
#define SGXFIVE_TRI_UV_MTE18	0x080	/* use_mtest18 */
#define SGXFIVE_TRI_UV_MTE2	0x100	/* use_mtest2 */
#define SGXFIVE_TRI_PP_PRIM	0x000	/* pds_prim: the triangle's colour */
#define SGXFIVE_TRI_PP_BGPRIM	0x020	/* pds_prim: the background colour */
#define SGXFIVE_TRI_PP_SEC	0x040	/* pds_sec */
#define SGXFIVE_TRI_PP_PIXEV	0x080	/* pds_pixevent */
#define SGXFIVE_TRI_UP_FILL	0x000	/* use_fill: the triangle's colour */
#define SGXFIVE_TRI_UP_BGFILL	0x010	/* use_fill: the background colour */
#define SGXFIVE_TRI_UP_SEC	0x020	/* use_sec */
#define SGXFIVE_TRI_UP_PTOFF	0x030	/* use_ptoff */
#define SGXFIVE_TRI_UP_EOT	0x040	/* use_eot */
#define SGXFIVE_TRI_UP_EOR	0x080	/* use_eor */
#define SGXFIVE_TRI_G_VDM	0x000	/* the VDM stream */
#define SGXFIVE_TRI_G_VTX	0x100	/* three vertices {x, y, z, w} */
#define SGXFIVE_TRI_G_IDX	0x200	/* three 16-bit indices */

#define SGXFIVE_TRI_BLOCK_WORDS	18	/* the triangle's MTE state block */
#define SGXFIVE_TRI_EOS_WORDS	2
#define SGXFIVE_TRI_VDM_WORDS	11	/* state, index list, end of scene, end */
#define SGXFIVE_TRI_USEV_CBASE	10	/* the code base of vertex programs */

/* Word builders (design/sgx545_3d.md S.1-S.5). */
uint32_t sgxfive_tri_doutu(uint32_t addr, uint32_t code_base, uint32_t cbase, uint32_t temps);
uint32_t sgxfive_tri_pix_ptr(const struct sgxfive_tri_layout *l, uint32_t addr,
			     uint32_t data_dwords);
uint32_t sgxfive_tri_doutd(uint32_t dwords);
uint32_t sgxfive_f32(float f);
void sgxfive_tri_block(uint32_t blk[SGXFIVE_TRI_BLOCK_WORDS], const struct sgxfive_tri_layout *l,
		       const struct sgxfive_tri_params *p);
void sgxfive_tri_eos(uint32_t blk[SGXFIVE_TRI_EOS_WORDS], const struct sgxfive_tri_params *p);
void sgxfive_tri_vdm(uint32_t vdm[SGXFIVE_TRI_VDM_WORDS], const struct sgxfive_tri_layout *l);

/*
 * The whole client side: programs, state blocks, VDM stream, vertices and
 * indices into c, and the job's register words into cmd. -EINVAL for a size
 * the kernel would refuse or that does not fit the layout.
 */
int sgxfive_tri_scene(const struct sgxfive_tri_cpu *c, const struct sgxfive_tri_layout *l,
		      const struct sgxfive_tri_params *p, struct drm_sgxfive_render_cmd *cmd);

/*
 * The CPU reference for pixel (x, y), row y = window y: 1 when its centre is
 * inside the triangle by at least a pixel, 0 when outside by at least a
 * pixel, -1 near an edge, where either colour is accepted.
 */
int sgxfive_tri_expect(const struct sgxfive_tri_params *p, uint32_t x, uint32_t y);

/* Every pixel of fb (stride 4 x width) against the reference. */
struct sgxfive_tri_counts {
	uint32_t tri, bg, other;	/* pixels of each colour, and neither */
	uint32_t wrong_in, wrong_out, wrong_edge;
	uint32_t flipped;		/* mismatches against the image flipped in y */
};

int sgxfive_tri_check(const struct sgxfive_tri_params *p, const uint32_t *fb,
		      struct sgxfive_tri_counts *n);

/* ---- Render engine ------------------------------------------------------------------ */

#define SGXFIVE_TRI_SENTINEL	0xdeadbeefu

struct sgxfive_render;

/*
 * One render context's engine in vm: a BO of five program/stream pages mapped
 * in the PDS_VERTEX, USSE_VERTEX, PDS_PIXEL, USSE_PIXEL and GENERAL heaps,
 * and a colour buffer of SGXFIVE_TRI_MAX_W x _H pixels plus a guard page in
 * GENERAL. ctx is a render context on vm.
 */
int sgxfive_render_create(struct sgxfive_vm *vm, uint32_t ctx, struct sgxfive_render **out);
void sgxfive_render_destroy(struct sgxfive_render *r);

/*
 * Build the scene, fill the colour buffer and the guard page with the
 * sentinel, submit (waiting for syncobj wait, signalling signal, if not 0).
 * The previous job must be finished (sgxfive_render_wait).
 */
int sgxfive_render_tri(struct sgxfive_render *r, const struct sgxfive_tri_params *p,
		       uint32_t wait, uint32_t signal);
int sgxfive_render_wait(struct sgxfive_render *r, int64_t timeout_ns);

/* The colour buffer, and the dwords of the guard page that are not the sentinel. */
const uint32_t *sgxfive_render_fb(const struct sgxfive_render *r);
uint32_t sgxfive_render_guard_dirty(const struct sgxfive_render *r,
				    const struct sgxfive_tri_params *p);
const struct drm_sgxfive_render_cmd *sgxfive_render_last_cmd(const struct sgxfive_render *r);
void sgxfive_render_get_layout(const struct sgxfive_render *r, struct sgxfive_tri_layout *l);

#endif
