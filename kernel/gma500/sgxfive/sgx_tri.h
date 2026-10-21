/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The first SGX545 triangle: the TA + 3D job a debugfs self-test builds in
 * kernel memory, from our own programs (design/sgx545_3d.md, "First
 * triangle"). The structure layouts are those of design/sgx545_3d.md §4-§8,
 * the job that of §12.6, the state words those of §13.
 */
#ifndef _GMA500_SGX_TRI_H_
#define _GMA500_SGX_TRI_H_

#include <linux/types.h>

/* The render target: 64 x 64 ARGB8888, linear. */
#define SGX_TRI_W		64
#define SGX_TRI_H		64
#define SGX_TRI_STRIDE		(SGX_TRI_W * 4)

/* Structure sizes in bytes (§4.1, §6-§8). */
#define SGX_TRI_CMD_SIZE	0x2b0	/* SGXMKIF_CMDTA, one reg_updates_3d pair */
#define SGX_TRI_CMD_STRIDE	0x2c0	/* its ui32Size: 64-byte units */
#define SGX_TRI_CTX_SIZE	0x44	/* SGXMKIF_HWRENDERCONTEXT */
#define SGX_TRI_RD_SIZE		0x1ec	/* hw_render_details */
#define SGX_TRI_RTDS_SIZE	0x50	/* hw_rt_data_set, one RT data */
#define SGX_TRI_PBDESC_SIZE	0x3c	/* hw_pb_desc */
#define SGX_TRI_PBBLOCK_SIZE	0x10	/* hw_pb_block */
#define SGX_TRI_CCB_SIZE	0x10000	/* the EDM masks the read offset */

/* SGXMKIF_CMDTA (§6). */
#define SGX_TRI_CMD_SIZE_W	0x000
#define SGX_TRI_CMD_FLAGS	0x004
#define SGX_TRI_CMD_SPEC	0x008	/* spec_object[3] */
#define SGX_TRI_CMD_VPART	0x014
#define SGX_TRI_CMD_SPM_VPART	0x018
#define SGX_TRI_CMD_RFLAGS	0x01c
#define SGX_TRI_CMD_FRAME	0x020
#define SGX_TRI_CMD_PBDESC	0x024
#define SGX_TRI_CMD_RD		0x038
#define SGX_TRI_CMD_DSTSYNC	0x03c
#define SGX_TRI_CMD_RTDS	0x040
#define SGX_TRI_CMD_RTDATA	0x044
#define SGX_TRI_CMD_NUM3DREGS	0x054
#define SGX_TRI_CMD_SHARED	0x058	/* SGXMKIF_CMDTA_SHARED */
#define SGX_TRI_SH_CTRL		(SGX_TRI_CMD_SHARED + 0x000)
#define SGX_TRI_SH_NUM_TA	(SGX_TRI_CMD_SHARED + 0x004)
#define SGX_TRI_SH_NUM_3D	(SGX_TRI_CMD_SHARED + 0x008)
#define SGX_TRI_SH_TATQ		(SGX_TRI_CMD_SHARED + 0x00c)
#define SGX_TRI_SH_TA_STATUS	(SGX_TRI_CMD_SHARED + 0x0c0)	/* {addr, val}[32] */
#define SGX_TRI_SH_3D_STATUS	(SGX_TRI_CMD_SHARED + 0x1c0)	/* {addr, val}[4] */
#define SGX_TRI_CMD_TAREGS	0x238	/* hw_ta_registers */
#define SGX_TRI_CMD_3DREGUPD	0x2a8

/* hw_ta_registers, from SGX_TRI_CMD_TAREGS (§12.2 step 5). */
#define SGX_TRI_TA_TEAA		0x00
#define SGX_TRI_TA_TEMTILE1	0x04
#define SGX_TRI_TA_TEMTILE2	0x08
#define SGX_TRI_TA_TESCREEN	0x0c
#define SGX_TRI_TA_TEMTILESTRIDE 0x10
#define SGX_TRI_TA_TEPSG	0x14
#define SGX_TRI_TA_VDM		0x18
#define SGX_TRI_TA_MTECTRL	0x1c
#define SGX_TRI_TA_PSGRGN	0x20
#define SGX_TRI_TA_TPC		0x24
#define SGX_TRI_TA_WCOMPARE	0x28
#define SGX_TRI_TA_WCLAMP	0x2c
#define SGX_TRI_TA_MTESCREEN	0x30
#define SGX_TRI_TA_REQBASE	0x3c
#define SGX_TRI_TA_MTEMSCTRL	0x6c

/* TA flags (ui32Flags) and render flags. */
#define SGX_TRI_TA_FIRST	0x01
#define SGX_TRI_TA_LAST		0x02
#define SGX_TRI_TA_INVAL	0x70	/* USE, PDS and data caches */
#define SGX_TRI_RENDER_SCENE	0x400
#define SGX_TRI_TEPSG_END	0x400000	/* the scene's last kick [CAP] */

/* SGXMKIF_HWRENDERCONTEXT (§4.1). */
#define SGX_TRI_CTX_FLAGS	0x00
#define SGX_TRI_CTX_PRIORITY	0x04
#define SGX_TRI_CTX_PD		0x08
#define SGX_TRI_CTX_CCB		0x0c
#define SGX_TRI_CTX_CCB_CTL	0x10
#define SGX_TRI_CTX_TACOUNT	0x14
#define SGX_TRI_CTX_PBDESC	0x18
#define SGX_TRI_CTX_PRIM	0x1c
#define SGX_TRI_CTX_PARTIAL	0x20	/* list heads: partial, complete */
#define SGX_TRI_CTX_COMPLETE	0x28
#define SGX_TRI_CTX_PID		0x40
#define SGX_TRI_CTX_NEW		0x1

/* hw_render_details (§7): the 52 3D registers, then the rest. */
#define SGX_TRI_RD_REQBASE	0x000
#define SGX_TRI_RD_PIXELBE	0x008
#define SGX_TRI_RD_IPFMISC	0x00c
#define SGX_TRI_RD_AAMODE	0x010
#define SGX_TRI_RD_RGNBASE	0x020
#define SGX_TRI_RD_ZLSEXTZ	0x028
#define SGX_TRI_RD_ZLSCTL	0x02c
#define SGX_TRI_RD_SLOAD	0x038
#define SGX_TRI_RD_SSTORE	0x03c
#define SGX_TRI_RD_BGDEPTH	0x040
#define SGX_TRI_RD_BGOBJ	0x044
#define SGX_TRI_RD_BGTAG	0x048
#define SGX_TRI_RD_MSCTL	0x050
#define SGX_TRI_RD_TSPCONFIG	0x058
#define SGX_TRI_RD_PIXEV_EXEC	0x05c
#define SGX_TRI_RD_PIXEV_DATA	0x060
#define SGX_TRI_RD_PIXEV_INFO	0x064
#define SGX_TRI_RD_FPUCTRL	0x068
#define SGX_TRI_RD_DEPTHSORT	0x0cc
#define SGX_TRI_RD_ACCESS	0x0d0
#define SGX_TRI_RD_RFLAGS	0x0d4
#define SGX_TRI_RD_RTDS		0x0d8
#define SGX_TRI_RD_RTDATA	0x0dc
#define SGX_TRI_RD_TQSYNC	0x0e4
#define SGX_TRI_RD_NUMRTS	0x104
#define SGX_TRI_RD_NEXTRT	0x108
#define SGX_TRI_RD_PIXEV_UPD	0x10c
#define SGX_TRI_RD_BGPRI_LIST	0x114
#define SGX_TRI_RD_TESTATE	0x124
#define SGX_TRI_RD_SPEC		0x130
#define SGX_TRI_RD_NUMPIXPART	0x13c
#define SGX_TRI_RD_NUM3DSTATUS	0x144

/* hw_rt_data_set (§8) and its hw_rt_data, at SGX_TRI_RTDS_RT. */
#define SGX_TRI_RTDS_PENDING	0x00
#define SGX_TRI_RTDS_COMPLETE	0x04
#define SGX_TRI_RTDS_MTILEW	0x08
#define SGX_TRI_RTDS_NUMRT	0x14
#define SGX_TRI_RTDS_RT		0x18
#define SGX_TRI_RT_CTX		0x00
#define SGX_TRI_RT_RD		0x04
#define SGX_TRI_RT_BLOCKS	0x08
#define SGX_TRI_RT_RGNARRAY	0x0c
#define SGX_TRI_RT_BG		0x10
#define SGX_TRI_RT_STATE	0x14
#define SGX_TRI_RT_CONTROL	0x18
#define SGX_TRI_RT_OTPM		0x1c
#define SGX_TRI_RT_TAIL		0x20
#define SGX_TRI_RT_TAILSIZE	0x24
#define SGX_TRI_RT_NUMRTS	0x28
#define SGX_TRI_RT_RGNSTRIDE	0x2c
#define SGX_TRI_RT_STATUS	0x30
#define SGX_TRI_RT_COMMON	0x34
#define SGX_TRI_RT_FREE		0x10	/* CommonStatus: no render outstanding */

/* hw_pb_desc and hw_pb_block (§5, §12.4). */
#define SGX_TRI_PB_FLAGS	0x00
#define SGX_TRI_PB_NUMPAGES	0x04
#define SGX_TRI_PB_INIT_HT	0x08
#define SGX_TRI_PB_INIT_PREV	0x0c
#define SGX_TRI_PB_PT		0x10
#define SGX_TRI_PB_ALLOC	0x14
#define SGX_TRI_PB_HT		0x18
#define SGX_TRI_PB_PREV		0x1c
#define SGX_TRI_PB_TA_THRESH	0x28
#define SGX_TRI_PB_BLOCKS	0x38

/* The parameter buffer: the capture's size and thresholds (§12.4). */
#define SGX_TRI_PB_PAGES	0x422
#define SGX_TRI_PT_BYTES	0x40000	/* one entry per page of the heap */

/*
 * Where everything is, by device address: each area is one buffer, and the
 * items in it sit at the offsets below.
 */
struct sgx_tri_addrs {
	u32 pd_phys;		/* the render context's page directory */
	u32 param_base;		/* BIF_TA/3D_REQ_BASE: the parameter heap */
	u32 pb, pt, prim, bg;	/* PB pages, page table, scratch, bg object */
	u32 tadata;		/* SGX_TRI_TD_*: the RT data's buffers */
	u32 ccb;		/* the TA client CCB */
	u32 kd;			/* SGX_TRI_KD_*: the kernel-data structures */
	u32 pdsv, usev;		/* vertex PDS and USSE programs: SGX_TRI_PV_*, _UV_ */
	u32 pdsp, usep;		/* pixel PDS and USSE programs: SGX_TRI_PP_*, _UP_ */
	u32 gen;		/* VDM streams and geometry: SGX_TRI_G_* */
	u32 fb;			/* the colour buffer */
	u32 pds_exec_base;	/* PDS_EXEC_BASE */
	u32 usep_base;		/* USE_CODE_BASE_0: pixel code */
	u32 usev_base;		/* USE_CODE_BASE_10: vertex code */
};

#define SGX_TRI_USEV_CBASE	10	/* the code base of vertex programs */

/* tadata: zeroed buffers of the RT data (§12.4), a page each. */
#define SGX_TRI_TD_TAIL		0x0000	/* tail pointers, 0x100 */
#define SGX_TRI_TD_CONTROL	0x1000	/* DPM context control */
#define SGX_TRI_TD_OTPM		0x2000
#define SGX_TRI_TD_STATE	0x3000	/* DPM context state */
#define SGX_TRI_TD_RGN		0x4000	/* region headers, 0x300 */
#define SGX_TRI_TD_ZLS		0x5000	/* ZLS external-Z region headers */
#define SGX_TRI_TD_SIZE		0x6000
#define SGX_TRI_TAIL_SIZE	0x100

/* kd: one page. */
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

/* What one kick sets in the command. */
struct sgx_tri_kick {
	u32 flags;		/* SGX_TRI_TA_* */
	u32 render_flags;
	u32 vdm;		/* the VDM stream */
	u32 te_psg;		/* te_psg */
	u32 frame;
	u32 ta_val, td_val;	/* written to SGX_TRI_W_TA and _3D when done */
};

/* CPU views of the areas the scene builder writes. */
struct sgx_tri_cpu {
	u32 *pdsv, *usev, *pdsp, *usep, *gen, *bg;
};

u32 sgx_tri_f32(s32 num, unsigned int shift);
u32 sgx_tri_doutu(u32 addr, u32 code_base, u32 cbase, u32 temps);
u32 sgx_tri_pix_ptr(const struct sgx_tri_addrs *a, u32 addr, u32 data_dwords);
u32 sgx_tri_doutd(u32 dwords);

void sgx_tri_ctx(u32 *ctx, const struct sgx_tri_addrs *a);
void sgx_tri_pb(u32 *desc, u32 *block, u32 *pt, const struct sgx_tri_addrs *a);
void sgx_tri_rtds(u32 *rtds, const struct sgx_tri_addrs *a);
void sgx_tri_rd(u32 *rd, const struct sgx_tri_addrs *a);
void sgx_tri_words(u32 *w, const struct sgx_tri_addrs *a);
u32 sgx_tri_cmd(u8 *cmd, const struct sgx_tri_addrs *a,
		const struct sgx_tri_kick *k);

void sgx_tri_block(u32 blk[SGX_TRI_BLOCK_WORDS], const struct sgx_tri_addrs *a);
void sgx_tri_eos(u32 blk[SGX_TRI_EOS_WORDS]);
void sgx_tri_vdm(u32 *vdm, const struct sgx_tri_addrs *a);
void sgx_tri_vdm1(u32 *vdm, const struct sgx_tri_addrs *a);
void sgx_tri_scene(const struct sgx_tri_cpu *c, const struct sgx_tri_addrs *a,
		   u32 colour, u32 bg_colour);

int sgx_tri_expect(unsigned int x, unsigned int y);

#endif
