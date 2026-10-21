// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Builders for the kernel-owned side of an SGX545 TA + 3D job: the firmware
 * structures of a render context (HW render context, PB descriptor and page
 * table, RT data set) and, for each scene, the render details, the
 * SGXMKIF_CMDTA and the background object, from the register words userspace
 * gives (struct drm_sgxfive_render_cmd). Pure code: it writes into buffers it
 * is given. The values are the first triangle's (design/sgx545_3d.md §12.6,
 * §13, §14), which the debugfs test and the render node both build here.
 */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include "sgx_render.h"
#include "sgx_xfer.h"

static void put32(u8 *p, unsigned int off, u32 v)
{
	p[off] = v;
	p[off + 1] = v >> 8;
	p[off + 2] = v >> 16;
	p[off + 3] = v >> 24;
}

/* §12.6 "once per render context"; the EDM owns the count and the lists. */
void sgx_render_ctx(u32 *ctx, const struct sgx_render_addrs *a)
{
	memset(ctx, 0, SGX_RENDER_CTX_SIZE);
	ctx[SGX_RENDER_CTX_FLAGS / 4] = SGX_RENDER_CTX_NEW;
	ctx[SGX_RENDER_CTX_PD / 4] = a->pd_phys;
	ctx[SGX_RENDER_CTX_CCB / 4] = a->ccb;
	ctx[SGX_RENDER_CTX_CCB_CTL / 4] = a->ccb_ctl;
	ctx[SGX_RENDER_CTX_PBDESC / 4] = a->pbdesc;
	ctx[SGX_RENDER_CTX_PRIM / 4] = a->param_base + SGX_RENDER_PARAM_PRIM;
}

/*
 * §12.4: the pages are contiguous from the slot's PB; the host builds the
 * page table, one entry per page of the slot, linking them {previous << 16 |
 * next} with 0xffff at both ends, and the descriptor's free list {tail << 16 |
 * head}.
 */
void sgx_render_pb(u32 *desc, u32 *block, u32 *pt, const struct sgx_render_addrs *a)
{
	const u32 head = SGX_RENDER_PARAM_PB >> 12;
	const u32 tail = head + SGX_RENDER_PB_PAGES - 1;
	const u32 ht = tail << 16 | head, prev = (tail - 1) << 16;
	u32 i;

	memset(pt, 0, SGX_RENDER_PT_BYTES);
	for (i = head; i <= tail; i++)
		pt[i] = (i == head ? 0xffff : i - 1) << 16 |
			(i == tail ? 0xffff : i + 1);

	memset(desc, 0, SGX_RENDER_PBDESC_SIZE);
	desc[SGX_RENDER_PB_NUMPAGES / 4] = SGX_RENDER_PB_PAGES;
	desc[SGX_RENDER_PB_INIT_HT / 4] = ht;
	desc[SGX_RENDER_PB_INIT_PREV / 4] = prev;
	desc[SGX_RENDER_PB_PT / 4] = a->param_base + SGX_RENDER_PARAM_PT;
	desc[SGX_RENDER_PB_HT / 4] = ht;
	desc[SGX_RENDER_PB_PREV / 4] = prev;
	/* The capture's thresholds: TA, ZLS, global, PDS. */
	desc[SGX_RENDER_PB_TA_THRESH / 4] = 0x432;
	desc[SGX_RENDER_PB_TA_THRESH / 4 + 1] = 0x400;
	desc[SGX_RENDER_PB_TA_THRESH / 4 + 2] = 0x400;
	desc[SGX_RENDER_PB_TA_THRESH / 4 + 3] = 0x300;
	desc[SGX_RENDER_PB_BLOCKS / 4] = a->pbblock;

	block[0] = SGX_RENDER_PB_PAGES;
	block[1] = ht;
	block[2] = a->pbdesc;
	block[3] = 0;
}

/*
 * §14.6: the layout the closed driver chose for every size it was asked
 * (harness size=WxH captures, 32 x 16 to 4096 x 4096): n = 4 macrotiles
 * across and down from 640 x 480 pixels on, else 2; a macrotile spans the
 * tiles over n, rounded up to whole 4-tile blocks. TE_MTILE1 (across) and
 * TE_MTILE2 (down) hold the macrotile size in blocks (31:24) and the second
 * and third macrotile boundaries in tiles (19:12, 11:0), both the macrotile
 * size when n = 2; TE_MTILE1 bit 31 says n = 4. One region header is 12
 * bytes per tile; the tail pointers are a square of 4-byte words, the power
 * of two that covers the larger span of macrotiles.
 */
static u32 sgx_render_mtile(u32 m, u32 n)
{
	return (m / 4) << 24 | (n == 4 ? 2 * m : m) << 12 | (n == 4 ? 3 * m : m);
}

void sgx_render_layout(struct sgx_render_layout *l, u32 width, u32 height)
{
	const u32 tx = DIV_ROUND_UP(width, 32), ty = DIV_ROUND_UP(height, 16);
	const u32 n = (u64)width * height >= 640 * 480 ? 4 : 2;
	const u32 mx = round_up(DIV_ROUND_UP(tx, n), 4);
	const u32 my = round_up(DIV_ROUND_UP(ty, n), 4);
	u32 span = max(n * mx, n * my), side = 1;

	while (side < span)
		side <<= 1;
	l->te_screen = (ty - 1) << 12 | (tx - 1);
	l->te_mtile1 = sgx_render_mtile(mx, n) | (n == 4 ? BIT(31) : 0);
	l->te_mtile2 = sgx_render_mtile(my, n);
	l->blocks_per_mt = (mx / 4) * (my / 4);
	l->te_mtile_stride = 0x10 * l->blocks_per_mt;
	l->mtile_width = n * mx;
	l->rgn_array_size = 0xc0 * l->blocks_per_mt;
	l->rgn_size = l->rgn_array_size * n * n;
	l->tail_size = 4 * side * side;
}

/* A render context's largest target: 1 to the device's, each way. */
int sgx_render_check_max(u32 max_w, u32 max_h)
{
	if (!max_w || !max_h || max_w > SGX_RENDER_MAX_W || max_h > SGX_RENDER_MAX_H)
		return -EINVAL;
	return 0;
}

/*
 * The TA data of a context whose targets are at most max_w x max_h: that
 * target's layout is the largest of them all, since n x the macrotile span is
 * the tile count rounded up to a multiple of 4n, so the region headers (12
 * bytes x those counts across and down) and the tail pointers (the square of
 * the power of two above them) grow with either size and with n, which grows
 * with the area (design/sgx545_render_uapi.md §11.4).
 */
int sgx_render_tadata_layout(struct sgx_render_tadata *t, u32 max_w, u32 max_h)
{
	struct sgx_render_layout l;

	if (sgx_render_check_max(max_w, max_h))
		return -EINVAL;
	sgx_render_layout(&l, max_w, max_h);
	t->tail = 0;
	t->control = t->tail + round_up(l.tail_size, SZ_4K);
	t->otpm = t->control + SZ_4K;
	t->state = t->otpm + SZ_4K;
	t->rgn = t->state + SZ_4K;
	t->zls = t->rgn + round_up(l.rgn_size, SZ_4K);
	t->size = t->zls + round_up(l.rgn_size, SZ_4K);
	return 0;
}

static void sgx_render_screen(struct sgx_render_layout *l, u32 mte_screen)
{
	sgx_render_layout(l, (mte_screen & 0xffff) + 1, (mte_screen >> 16) + 1);
}

/* The size words of the RT data set and its RT data, for a target size. */
void sgx_render_rtds_resize(u32 *rtds, u32 mte_screen)
{
	u32 *rt = rtds + SGX_RENDER_RTDS_RT / 4;
	struct sgx_render_layout l;

	sgx_render_screen(&l, mte_screen);
	rtds[SGX_RENDER_RTDS_MTILEW / 4] = l.mtile_width;
	rt[SGX_RENDER_RT_BLOCKS / 4] = l.blocks_per_mt;
	rt[SGX_RENDER_RT_RGNARRAY / 4] = l.rgn_array_size;
	rt[SGX_RENDER_RT_TAILSIZE / 4] = l.tail_size;
}

/* §8 and §12.6: one RT data, free, with zeroed TA-data buffers. */
void sgx_render_rtds(u32 *rtds, const struct sgx_render_addrs *a, u32 mte_screen)
{
	u32 *rt = rtds + SGX_RENDER_RTDS_RT / 4;

	memset(rtds, 0, SGX_RENDER_RTDS_SIZE);
	rtds[SGX_RENDER_RTDS_PENDING / 4] = a->pending;
	rtds[SGX_RENDER_RTDS_NUMRT / 4] = 1;
	rt[SGX_RENDER_RT_CTX / 4] = a->ctx;
	rt[SGX_RENDER_RT_BG / 4] = a->param_base + SGX_RENDER_PARAM_BG;
	rt[SGX_RENDER_RT_STATE / 4] = a->tadata + a->td.state;
	rt[SGX_RENDER_RT_CONTROL / 4] = a->tadata + a->td.control;
	rt[SGX_RENDER_RT_OTPM / 4] = a->tadata + a->td.otpm;
	rt[SGX_RENDER_RT_TAIL / 4] = a->tadata + a->td.tail;
	rt[SGX_RENDER_RT_NUMRTS / 4] = 1;
	rt[SGX_RENDER_RT_STATUS / 4] = a->rt_status;
	rt[SGX_RENDER_RT_COMMON / 4] = SGX_RENDER_RT_FREE;
	sgx_render_rtds_resize(rtds, mte_screen);
}

/*
 * §7 and §12.6 "render details": the user's 3D registers, and the kernel's
 * (the parameter slot, the region headers, the background object, no depth
 * or stencil buffer, the pixel partitions). The EDM copies the command's
 * per-kick fields in at the TA kick.
 */
void sgx_render_rd(u32 *rd, const struct sgx_render_addrs *a,
		   const struct drm_sgxfive_render_cmd *c)
{
	memset(rd, 0, SGX_RENDER_RD_SIZE);
	rd[SGX_RENDER_RD_REQBASE / 4] = a->param_base;
	rd[SGX_RENDER_RD_PIXELBE / 4] = c->pixelbe;
	rd[SGX_RENDER_RD_IPFMISC / 4] = c->isp_ipfmisc;
	rd[SGX_RENDER_RD_AAMODE / 4] = c->aa_mode_3d;
	rd[SGX_RENDER_RD_RGNBASE / 4] = a->tadata + a->td.rgn;
	rd[SGX_RENDER_RD_ZLSEXTZ / 4] = a->tadata + a->td.zls;
	rd[SGX_RENDER_RD_SLOAD / 4] = 0x80000000;	/* no stencil buffer */
	rd[SGX_RENDER_RD_SSTORE / 4] = 0x80000000;
	rd[SGX_RENDER_RD_BGDEPTH / 4] = c->isp_bgobjdepth;
	rd[SGX_RENDER_RD_BGOBJ / 4] = c->isp_bgobj;
	/* VERTEXPTR: the object, in 16-byte units from BIF_3D_REQ_BASE. */
	rd[SGX_RENDER_RD_BGTAG / 4] = SGX_RENDER_PARAM_BG >> 4;
	rd[SGX_RENDER_RD_MSCTL / 4] = c->isp_multisamplectl;
	rd[SGX_RENDER_RD_TSPCONFIG / 4] = c->tsp_config;
	rd[SGX_RENDER_RD_PIXEV_EXEC / 4] = c->event_pixel_pds_exec;
	rd[SGX_RENDER_RD_PIXEV_DATA / 4] = c->event_pixel_pds_data;
	rd[SGX_RENDER_RD_PIXEV_INFO / 4] = c->event_pixel_pds_info;
	rd[SGX_RENDER_RD_FPUCTRL / 4] = c->isp_fpuctrl;
	rd[SGX_RENDER_RD_DEPTHSORT / 4] = c->isp_depthsort;
	rd[SGX_RENDER_RD_ACCESS / 4] = a->access;
	rd[SGX_RENDER_RD_RTDS / 4] = a->rtds;
	rd[SGX_RENDER_RD_RTDATA / 4] = a->rtds + SGX_RENDER_RTDS_RT;
	rd[SGX_RENDER_RD_NUMRTS / 4] = 1;
	rd[SGX_RENDER_RD_BGPRI_LIST / 4] = a->bgpri;
	rd[SGX_RENDER_RD_NUMPIXPART / 4] = 0x07c00000;	/* 31, as for transfers */
}

/*
 * §6 and §12.6 "SGXMKIF_CMDTA": one kick of the scene, with a TA and a 3D
 * status word as its completion fences, the user's TA registers and the
 * kernel's. READY goes last: the caller writes the command before it advances
 * the CCB write offset. Returns the command's size in the CCB.
 */
u32 sgx_render_cmd(u8 *cmd, const struct sgx_render_addrs *a,
		   const struct drm_sgxfive_render_cmd *c,
		   const struct sgx_render_kick *k)
{
	const unsigned int ta = SGX_RENDER_CMD_TAREGS;
	unsigned int i;

	memset(cmd, 0, SGX_RENDER_CMD_STRIDE);
	put32(cmd, SGX_RENDER_CMD_SIZE_W, SGX_RENDER_CMD_STRIDE);
	put32(cmd, SGX_RENDER_CMD_FLAGS, k->flags);
	/* The background object's dwords 0-2, which the EDM writes per render. */
	for (i = 0; i < 3; i++)
		put32(cmd, SGX_RENDER_CMD_SPEC + 4 * i, c->bgobj_words[i]);
	put32(cmd, SGX_RENDER_CMD_VPART, 0x50000000);	/* 10, pre-shifted */
	put32(cmd, SGX_RENDER_CMD_SPM_VPART, 0x50000000);
	put32(cmd, SGX_RENDER_CMD_RFLAGS, k->render_flags);
	put32(cmd, SGX_RENDER_CMD_FRAME, k->frame);
	put32(cmd, SGX_RENDER_CMD_PBDESC, a->pbdesc);
	put32(cmd, SGX_RENDER_CMD_RD, a->rd);
	put32(cmd, SGX_RENDER_CMD_RTDS, a->rtds);
	put32(cmd, SGX_RENDER_CMD_RTDATA, a->rtds + SGX_RENDER_RTDS_RT);

	put32(cmd, SGX_RENDER_SH_NUM_TA, 1);
	put32(cmd, SGX_RENDER_SH_NUM_3D, 1);
	put32(cmd, SGX_RENDER_SH_TA_STATUS, a->ta_status);
	put32(cmd, SGX_RENDER_SH_TA_STATUS + 4, k->ta_val);
	put32(cmd, SGX_RENDER_SH_3D_STATUS, a->td_status);
	put32(cmd, SGX_RENDER_SH_3D_STATUS + 4, k->td_val);

	put32(cmd, ta + SGX_RENDER_TA_TEAA, c->te_aa);
	put32(cmd, ta + SGX_RENDER_TA_TEMTILE1, c->te_mtile1);
	put32(cmd, ta + SGX_RENDER_TA_TEMTILE2, c->te_mtile2);
	put32(cmd, ta + SGX_RENDER_TA_TESCREEN, c->te_screen);
	put32(cmd, ta + SGX_RENDER_TA_TEMTILESTRIDE, c->te_mtile_stride);
	put32(cmd, ta + SGX_RENDER_TA_TEPSG, k->te_psg);
	put32(cmd, ta + SGX_RENDER_TA_VDM, c->vdm_ctrl_stream_base);
	put32(cmd, ta + SGX_RENDER_TA_MTECTRL, c->mte_ctrl);
	put32(cmd, ta + SGX_RENDER_TA_PSGRGN, a->tadata + a->td.rgn);
	put32(cmd, ta + SGX_RENDER_TA_TPC, a->tadata + a->td.tail);
	put32(cmd, ta + SGX_RENDER_TA_WCOMPARE, c->mte_wcompare);
	put32(cmd, ta + SGX_RENDER_TA_WCLAMP, c->mte_wclamp);
	put32(cmd, ta + SGX_RENDER_TA_MTESCREEN, c->mte_screen);
	put32(cmd, ta + SGX_RENDER_TA_REQBASE, a->param_base);
	put32(cmd, ta + SGX_RENDER_TA_MTEMSCTRL, c->mte_multisamplectl);

	put32(cmd, SGX_RENDER_SH_CTRL, 1);			/* READY */
	return SGX_RENDER_CMD_STRIDE;
}

/*
 * The kick of one render job: the whole scene at once (first and last kick,
 * cache invalidations, render it, te_psg's end-of-scene bit), the job's
 * number as the frame and value as both status values.
 */
void sgx_render_scene_kick(struct sgx_render_kick *k, u32 seqno, u32 status_val)
{
	k->flags = SGX_RENDER_TA_FIRST | SGX_RENDER_TA_LAST | SGX_RENDER_TA_INVAL;
	k->render_flags = SGX_RENDER_SCENE;
	k->te_psg = SGX_RENDER_TEPSG_END;
	k->frame = seqno;
	k->ta_val = status_val;
	k->td_val = status_val;
}

/* S.8: the user's PDS words over the whole render target, zero layers. */
void sgx_render_bgobj(u32 *bg, const struct drm_sgxfive_render_cmd *c)
{
	const struct sgx_xfer_rect r = {
		0, 0, (c->mte_screen & 0xffff) + 1, (c->mte_screen >> 16) + 1,
	};

	sgx_xfer_bgobj_words(bg, c->bgobj_words, &r);
}

/*
 * The render target must fit the context's tiling buffers: at most max_w x
 * max_h, the maximum the context was created with (checked against the
 * device's then), and TE_SCREEN, TE_MTILE1/2 and TE_MTILE the layout of its
 * size, the one the buffers' sizes in the RT data describe. ISP_BGOBJ may
 * only set bits 8 and 9.
 */
int sgx_render_check_size(const struct drm_sgxfive_render_cmd *c, u32 max_w,
			  u32 max_h)
{
	const u32 w = (c->mte_screen & 0xffff) + 1, h = (c->mte_screen >> 16) + 1;
	struct sgx_render_layout l;

	if (w > max_w || h > max_h || sgx_render_check_max(max_w, max_h))
		return -EINVAL;
	sgx_render_layout(&l, w, h);
	if (c->te_screen != l.te_screen || c->te_mtile1 != l.te_mtile1 ||
	    c->te_mtile2 != l.te_mtile2 || c->te_mtile_stride != l.te_mtile_stride)
		return -EINVAL;
	if (c->isp_bgobj & ~0x300u)
		return -EINVAL;
	return 0;
}
