// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Builders for the first SGX545 triangle (design/sgx545_3d.md, "First
 * triangle"): every structure of one TA + 3D job, the VDM control stream, the
 * MTE state, the geometry and our own programs (sgx_tri_progs.h, assembled
 * from tools/usse-asm/progs/) with their per-placement fields. Pure code: it
 * writes into buffers it is given and the debugfs test (sgxfive.c) kicks it.
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

static void put32(u8 *p, unsigned int off, u32 v)
{
	p[off] = v;
	p[off + 1] = v >> 8;
	p[off + 2] = v >> 16;
	p[off + 3] = v >> 24;
}

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

#define KD(a, off)	((a)->kd + (off))
#define WORD(a, i)	KD(a, SGX_TRI_KD_WORDS + 4 * (i))

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

/* §12.6 "once per render context"; the EDM owns the count and the lists. */
void sgx_tri_ctx(u32 *ctx, const struct sgx_tri_addrs *a)
{
	memset(ctx, 0, SGX_TRI_CTX_SIZE);
	ctx[SGX_TRI_CTX_FLAGS / 4] = SGX_TRI_CTX_NEW;
	ctx[SGX_TRI_CTX_PD / 4] = a->pd_phys;
	ctx[SGX_TRI_CTX_CCB / 4] = a->ccb;
	ctx[SGX_TRI_CTX_CCB_CTL / 4] = KD(a, SGX_TRI_KD_CCB_CTL);
	ctx[SGX_TRI_CTX_PBDESC / 4] = KD(a, SGX_TRI_KD_PBDESC);
	ctx[SGX_TRI_CTX_PRIM / 4] = a->prim;
}

/*
 * §12.4: the pages are contiguous from a->pb; the host builds the page table,
 * one entry per page of the heap, linking them {previous << 16 | next} with
 * 0xffff at both ends, and the descriptor's free list {tail << 16 | head}.
 */
void sgx_tri_pb(u32 *desc, u32 *block, u32 *pt, const struct sgx_tri_addrs *a)
{
	const u32 head = (a->pb - a->param_base) >> 12;
	const u32 tail = head + SGX_TRI_PB_PAGES - 1;
	const u32 ht = tail << 16 | head, prev = (tail - 1) << 16;
	u32 i;

	memset(pt, 0, SGX_TRI_PT_BYTES);
	for (i = head; i <= tail; i++)
		pt[i] = (i == head ? 0xffff : i - 1) << 16 |
			(i == tail ? 0xffff : i + 1);

	memset(desc, 0, SGX_TRI_PBDESC_SIZE);
	desc[SGX_TRI_PB_NUMPAGES / 4] = SGX_TRI_PB_PAGES;
	desc[SGX_TRI_PB_INIT_HT / 4] = ht;
	desc[SGX_TRI_PB_INIT_PREV / 4] = prev;
	desc[SGX_TRI_PB_PT / 4] = a->pt;
	desc[SGX_TRI_PB_HT / 4] = ht;
	desc[SGX_TRI_PB_PREV / 4] = prev;
	/* The capture's thresholds: TA, ZLS, global, PDS. */
	desc[SGX_TRI_PB_TA_THRESH / 4] = 0x432;
	desc[SGX_TRI_PB_TA_THRESH / 4 + 1] = 0x400;
	desc[SGX_TRI_PB_TA_THRESH / 4 + 2] = 0x400;
	desc[SGX_TRI_PB_TA_THRESH / 4 + 3] = 0x300;
	desc[SGX_TRI_PB_BLOCKS / 4] = KD(a, SGX_TRI_KD_PBBLOCK);

	block[0] = SGX_TRI_PB_PAGES;
	block[1] = ht;
	block[2] = KD(a, SGX_TRI_KD_PBDESC);
	block[3] = 0;
}

/* §8 and §12.6: one RT data, free, with zeroed TADATA buffers. */
void sgx_tri_rtds(u32 *rtds, const struct sgx_tri_addrs *a)
{
	u32 *rt = rtds + SGX_TRI_RTDS_RT / 4;

	memset(rtds, 0, SGX_TRI_RTDS_SIZE);
	rtds[SGX_TRI_RTDS_PENDING / 4] = WORD(a, SGX_TRI_W_PENDING);
	rtds[SGX_TRI_RTDS_MTILEW / 4] = 8;
	rtds[SGX_TRI_RTDS_NUMRT / 4] = 1;
	rt[SGX_TRI_RT_CTX / 4] = KD(a, SGX_TRI_KD_CTX);
	rt[SGX_TRI_RT_BLOCKS / 4] = 1;
	rt[SGX_TRI_RT_RGNARRAY / 4] = 0xc0;
	rt[SGX_TRI_RT_BG / 4] = a->bg;
	rt[SGX_TRI_RT_STATE / 4] = a->tadata + SGX_TRI_TD_STATE;
	rt[SGX_TRI_RT_CONTROL / 4] = a->tadata + SGX_TRI_TD_CONTROL;
	rt[SGX_TRI_RT_OTPM / 4] = a->tadata + SGX_TRI_TD_OTPM;
	rt[SGX_TRI_RT_TAIL / 4] = a->tadata + SGX_TRI_TD_TAIL;
	rt[SGX_TRI_RT_TAILSIZE / 4] = SGX_TRI_TAIL_SIZE;
	rt[SGX_TRI_RT_NUMRTS / 4] = 1;
	rt[SGX_TRI_RT_STATUS / 4] = WORD(a, SGX_TRI_W_RTSTATUS);
	rt[SGX_TRI_RT_COMMON / 4] = SGX_TRI_RT_FREE;
}

/*
 * §7 and §12.6 "render details": the captured 3D registers with our
 * addresses. The EDM copies the command's per-kick fields in at the TA kick.
 */
void sgx_tri_rd(u32 *rd, const struct sgx_tri_addrs *a)
{
	memset(rd, 0, SGX_TRI_RD_SIZE);
	rd[SGX_TRI_RD_REQBASE / 4] = a->param_base;
	rd[SGX_TRI_RD_PIXELBE / 4] = 0x80;
	rd[SGX_TRI_RD_IPFMISC / 4] = 0x000f0000;
	rd[SGX_TRI_RD_AAMODE / 4] = 0x80000000;
	rd[SGX_TRI_RD_RGNBASE / 4] = a->tadata + SGX_TRI_TD_RGN;
	rd[SGX_TRI_RD_ZLSEXTZ / 4] = a->tadata + SGX_TRI_TD_ZLS;
	rd[SGX_TRI_RD_SLOAD / 4] = 0x80000000;		/* no stencil buffer */
	rd[SGX_TRI_RD_SSTORE / 4] = 0x80000000;
	rd[SGX_TRI_RD_BGDEPTH / 4] = 0x3f800000;	/* 1.0 */
	rd[SGX_TRI_RD_BGOBJ / 4] = 0x300;		/* MASK | ENABLEBGTAG */
	/* VERTEXPTR: the object, in 16-byte units from BIF_3D_REQ_BASE. */
	rd[SGX_TRI_RD_BGTAG / 4] = (a->bg - a->param_base) >> 4;
	rd[SGX_TRI_RD_MSCTL / 4] = 0x88;
	rd[SGX_TRI_RD_TSPCONFIG / 4] = 0x30;
	rd[SGX_TRI_RD_PIXEV_EXEC / 4] = (a->pdsp + SGX_TRI_PP_PIXEV) & ~0xfu;
	rd[SGX_TRI_RD_PIXEV_DATA / 4] = SGX_TRI_PDS_PIXEVENT_DATA / 4;
	rd[SGX_TRI_RD_PIXEV_INFO / 4] = 1;		/* one attribute */
	rd[SGX_TRI_RD_FPUCTRL / 4] = 1;
	rd[SGX_TRI_RD_DEPTHSORT / 4] = 0x100;
	rd[SGX_TRI_RD_ACCESS / 4] = WORD(a, SGX_TRI_W_ACCESS);
	rd[SGX_TRI_RD_RTDS / 4] = KD(a, SGX_TRI_KD_RTDS);
	rd[SGX_TRI_RD_RTDATA / 4] = KD(a, SGX_TRI_KD_RTDS + SGX_TRI_RTDS_RT);
	rd[SGX_TRI_RD_NUMRTS / 4] = 1;
	rd[SGX_TRI_RD_BGPRI_LIST / 4] = WORD(a, SGX_TRI_W_BGPRI);
	rd[SGX_TRI_RD_NUMPIXPART / 4] = 0x07c00000;	/* 31, as for transfers */
}

/* The scratch words: completion targets zeroed, the bg update list set. */
void sgx_tri_words(u32 *w, const struct sgx_tri_addrs *a)
{
	memset(w, 0, 4 * SGX_TRI_W_COUNT);
	w[SGX_TRI_W_BGPRI] = pri_ptr(a, SGX_TRI_PP_BGPRIM);
}

/*
 * §6 and §12.6 "SGXMKIF_CMDTA": one kick of the scene, with a TA and a 3D
 * status word as its completion fences. READY goes last: the caller writes
 * the command before it advances the CCB write offset.
 */
u32 sgx_tri_cmd(u8 *cmd, const struct sgx_tri_addrs *a,
		const struct sgx_tri_kick *k)
{
	const unsigned int ta = SGX_TRI_CMD_TAREGS;

	memset(cmd, 0, SGX_TRI_CMD_STRIDE);
	put32(cmd, SGX_TRI_CMD_SIZE_W, SGX_TRI_CMD_STRIDE);
	put32(cmd, SGX_TRI_CMD_FLAGS, k->flags);
	/* The background object's dwords 0-2, which the EDM writes per render. */
	put32(cmd, SGX_TRI_CMD_SPEC, sec_ptr(a));
	put32(cmd, SGX_TRI_CMD_SPEC + 4, PIX_SIZE(0));
	put32(cmd, SGX_TRI_CMD_SPEC + 8, pri_ptr(a, SGX_TRI_PP_BGPRIM));
	put32(cmd, SGX_TRI_CMD_VPART, 0x50000000);	/* 10, pre-shifted */
	put32(cmd, SGX_TRI_CMD_SPM_VPART, 0x50000000);
	put32(cmd, SGX_TRI_CMD_RFLAGS, k->render_flags);
	put32(cmd, SGX_TRI_CMD_FRAME, k->frame);
	put32(cmd, SGX_TRI_CMD_PBDESC, KD(a, SGX_TRI_KD_PBDESC));
	put32(cmd, SGX_TRI_CMD_RD, KD(a, SGX_TRI_KD_RD));
	put32(cmd, SGX_TRI_CMD_RTDS, KD(a, SGX_TRI_KD_RTDS));
	put32(cmd, SGX_TRI_CMD_RTDATA, KD(a, SGX_TRI_KD_RTDS + SGX_TRI_RTDS_RT));

	put32(cmd, SGX_TRI_SH_NUM_TA, 1);
	put32(cmd, SGX_TRI_SH_NUM_3D, 1);
	put32(cmd, SGX_TRI_SH_TA_STATUS, WORD(a, SGX_TRI_W_TA));
	put32(cmd, SGX_TRI_SH_TA_STATUS + 4, k->ta_val);
	put32(cmd, SGX_TRI_SH_3D_STATUS, WORD(a, SGX_TRI_W_3D));
	put32(cmd, SGX_TRI_SH_3D_STATUS + 4, k->td_val);

	/* The captured TA registers for 64 x 64 (§6, S.6). */
	put32(cmd, ta + SGX_TRI_TA_TEAA, 0x20000000);
	put32(cmd, ta + SGX_TRI_TA_TEMTILE1, 0x01004004);
	put32(cmd, ta + SGX_TRI_TA_TEMTILE2, 0x01004004);
	/* 32 x 16 tiles across - 1 (11:0), down - 1 (23:12). */
	put32(cmd, ta + SGX_TRI_TA_TESCREEN,
	      (SGX_TRI_W / 32 - 1) | (SGX_TRI_H / 16 - 1) << 12);
	put32(cmd, ta + SGX_TRI_TA_TEMTILESTRIDE, 0x10);
	put32(cmd, ta + SGX_TRI_TA_TEPSG, k->te_psg);
	put32(cmd, ta + SGX_TRI_TA_VDM, k->vdm);
	put32(cmd, ta + SGX_TRI_TA_MTECTRL, 0xa2);
	put32(cmd, ta + SGX_TRI_TA_PSGRGN, a->tadata + SGX_TRI_TD_RGN);
	put32(cmd, ta + SGX_TRI_TA_TPC, a->tadata + SGX_TRI_TD_TAIL);
	put32(cmd, ta + SGX_TRI_TA_WCOMPARE, 0x358637bd);	/* 1e-6 */
	put32(cmd, ta + SGX_TRI_TA_WCLAMP, 0x358637bd);
	put32(cmd, ta + SGX_TRI_TA_MTESCREEN, (SGX_TRI_W - 1) | (SGX_TRI_H - 1) << 16);
	put32(cmd, ta + SGX_TRI_TA_REQBASE, a->param_base);
	put32(cmd, ta + SGX_TRI_TA_MTEMSCTRL, 0x88);

	put32(cmd, SGX_TRI_SH_CTRL, 1);			/* READY */
	return SGX_TRI_CMD_STRIDE;
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
 * Everything but the structures: the programs, the state blocks, the VDM
 * streams, the vertices and indices, and the background object (the
 * transfer queue's, zero layers, over the whole surface).
 */
void sgx_tri_scene(const struct sgx_tri_cpu *c, const struct sgx_tri_addrs *a,
		   u32 colour, u32 bg_colour)
{
	const struct sgx_xfer_rect whole = { 0, 0, SGX_TRI_W, SGX_TRI_H };
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
	sgx_xfer_bgobj(c->bg, a->pds_exec_base, a->pdsp + SGX_TRI_PP_BGPRIM,
		       4 * SGX_TRI_PDS_PRIM_DATA, a->pdsp + SGX_TRI_PP_SEC,
		       4 * SGX_TRI_PDS_SEC_DATA, &whole);
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
