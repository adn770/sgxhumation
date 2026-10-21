/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The first SGX545 triangle: the scene a debugfs self-test builds in kernel
 * memory, from our own programs (design/sgx545_3d.md, "First triangle"): the
 * programs, MTE state, VDM streams and geometry, which a render-node client
 * places in its own VM, and the register words it hands the job builders of
 * sgx_render.h. The state words are those of design/sgx545_3d.md §13.
 */
#ifndef _GMA500_SGX_TRI_H_
#define _GMA500_SGX_TRI_H_

#include <linux/types.h>

#include "sgx_render.h"

/* The render target: 64 x 64 ARGB8888, linear. */
#define SGX_TRI_W		64
#define SGX_TRI_H		64
#define SGX_TRI_STRIDE		(SGX_TRI_W * 4)

/*
 * Where the scene is, by device address: each area is one buffer, and the
 * items in it sit at the offsets below.
 */
struct sgx_tri_addrs {
	u32 pdsv, usev;		/* vertex PDS and USSE programs: SGX_TRI_PV_*, _UV_ */
	u32 pdsp, usep;		/* pixel PDS and USSE programs: SGX_TRI_PP_*, _UP_ */
	u32 gen;		/* VDM streams and geometry: SGX_TRI_G_* */
	u32 fb;			/* the colour buffer */
	u32 pds_exec_base;	/* PDS_EXEC_BASE */
	u32 usep_base;		/* USE_CODE_BASE_0: pixel code */
	u32 usev_base;		/* USE_CODE_BASE_10: vertex code */
};

#define SGX_TRI_USEV_CBASE	10	/* the code base of vertex programs */

/* The debugfs test's kernel-data page (struct sgx_render_addrs from it). */
#define SGX_TRI_KD_CCB_CTL	0x000	/* {write offset, read offset} */
#define SGX_TRI_KD_CTX		0x100
#define SGX_TRI_KD_RTDS		0x200
#define SGX_TRI_KD_PBDESC	0x300
#define SGX_TRI_KD_PBBLOCK	0x340
#define SGX_TRI_KD_WORDS	0x400	/* SGX_TRI_W_* dwords */
#define SGX_TRI_KD_RD		0x800
#define SGX_TRI_KD_SIZE		0x1000
enum {
	SGX_TRI_W_TA,		/* TA status: written at the TA end */
	SGX_TRI_W_3D,		/* 3D status: written at the render end */
	SGX_TRI_W_ACCESS,	/* the render details' access word */
	SGX_TRI_W_PENDING,	/* the RT data set's pending count */
	SGX_TRI_W_RTSTATUS,	/* the render target's status word */
	SGX_TRI_W_BGPRI,	/* the background primary-PDS update list */
	SGX_TRI_W_COUNT
};

/* Programs and data, by area. */
#define SGX_TRI_PV_VTX		0x000	/* pds_vtx */
#define SGX_TRI_PV_MTE		0x040	/* pds_mte: the triangle's state */
#define SGX_TRI_PV_EOS		0x080	/* pds_mte: end of scene */
#define SGX_TRI_PV_BLOCK	0x100	/* the triangle's MTE state block */
#define SGX_TRI_PV_EOSBLOCK	0x180	/* the end-of-scene state block */
#define SGX_TRI_UV_VTX		0x000	/* use_vtx */
#define SGX_TRI_UV_MTE18	0x080	/* use_mtest18 */
#define SGX_TRI_UV_MTE2		0x100	/* use_mtest2 */
#define SGX_TRI_PP_PRIM		0x000	/* pds_prim: the triangle's colour */
#define SGX_TRI_PP_BGPRIM	0x020	/* pds_prim: the background colour */
#define SGX_TRI_PP_SEC		0x040	/* pds_sec */
#define SGX_TRI_PP_PIXEV	0x080	/* pds_pixevent */
#define SGX_TRI_UP_FILL		0x000	/* use_fill: the triangle's colour */
#define SGX_TRI_UP_BGFILL	0x010	/* use_fill: the background colour */
#define SGX_TRI_UP_SEC		0x020	/* use_sec */
#define SGX_TRI_UP_PTOFF	0x030	/* use_ptoff */
#define SGX_TRI_UP_EOT		0x040	/* use_eot */
#define SGX_TRI_UP_EOR		0x080	/* use_eor */
#define SGX_TRI_G_VDM		0x000	/* the (first) kick's VDM stream */
#define SGX_TRI_G_VDM1		0x080	/* the second kick's, in the two-kick form */
#define SGX_TRI_G_VTX		0x100	/* three vertices {x, y, z, w} */
#define SGX_TRI_G_IDX		0x200	/* three 16-bit indices */
#define SGX_TRI_FB_SIZE		(SGX_TRI_STRIDE * SGX_TRI_H)

#define SGX_TRI_BLOCK_WORDS	18	/* the triangle's MTE state block */
#define SGX_TRI_EOS_WORDS	2
#define SGX_TRI_VDM_WORDS	11	/* state, index list, end of scene, end */
#define SGX_TRI_VDM1_WORDS	3	/* end of scene, end */

/* CPU views of the areas the scene builder writes. */
struct sgx_tri_cpu {
	u32 *pdsv, *usev, *pdsp, *usep, *gen;
};

u32 sgx_tri_f32(s32 num, unsigned int shift);
u32 sgx_tri_doutu(u32 addr, u32 code_base, u32 cbase, u32 temps);
u32 sgx_tri_pix_ptr(const struct sgx_tri_addrs *a, u32 addr, u32 data_dwords);
u32 sgx_tri_doutd(u32 dwords);

void sgx_tri_words(u32 *w, const struct sgx_tri_addrs *a);
void sgx_tri_render_cmd(struct drm_sgxfive_render_cmd *c,
			const struct sgx_tri_addrs *a, u32 vdm);

void sgx_tri_block(u32 blk[SGX_TRI_BLOCK_WORDS], const struct sgx_tri_addrs *a);
void sgx_tri_eos(u32 blk[SGX_TRI_EOS_WORDS]);
void sgx_tri_vdm(u32 *vdm, const struct sgx_tri_addrs *a);
void sgx_tri_vdm1(u32 *vdm, const struct sgx_tri_addrs *a);
void sgx_tri_scene(const struct sgx_tri_cpu *c, const struct sgx_tri_addrs *a,
		   u32 colour, u32 bg_colour);

int sgx_tri_expect(unsigned int x, unsigned int y);

#endif
