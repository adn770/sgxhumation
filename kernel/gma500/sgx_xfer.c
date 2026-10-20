// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Builders for the SGX545 transfer queue (design/sgx545_transfer.md). A dummy
 * transfer runs the EDM's whole queue path except the render: the sync test,
 * the BIF directory-list switch, the memory updates, the status writes, the
 * SW_EVENT interrupt and the client CCB advance.
 */
#include "sgx_xfer.h"

static void put32(u8 *p, unsigned int off, u32 v)
{
	p[off] = v;
	p[off + 1] = v >> 8;
	p[off + 2] = v >> 16;
	p[off + 3] = v >> 24;
}

/*
 * Returns the command's size in the CCB: its used part, in 64-byte units. With
 * a destination sync the EDM starts the command only once both complete
 * counters equal their pending values, and moves the write counter past its
 * pending value when done.
 */
u32 sgx_xfer_dummy(u8 *cmd, u32 status_addr, u32 status_val, u32 update_addr,
		   u32 update_val, const struct sgx_xfer_sync *dst)
{
	const u32 size = (SGX_XFER_UPDATES + 8 + 0x3f) & ~0x3fu;
	unsigned int i;

	for (i = 0; i < SGX_XFER_CMD_SIZE; i++)
		cmd[i] = 0;
	put32(cmd, SGX_XFER_SIZE, size);
	put32(cmd, SGX_XFER_FLAGS, SGX_XFER_FLAG_DUMMY);
	put32(cmd, SGX_XFER_NUM_STATUS, 1);
	put32(cmd, SGX_XFER_STATUS, status_addr);
	put32(cmd, SGX_XFER_STATUS + 4, status_val);
	if (dst) {
		put32(cmd, SGX_XFER_NUM_DST, 1);
		put32(cmd, SGX_XFER_DST, dst->read_pending);
		put32(cmd, SGX_XFER_DST + 4, dst->read_complete);
		put32(cmd, SGX_XFER_DST + 8, dst->write_pending);
		put32(cmd, SGX_XFER_DST + 12, dst->write_complete);
	}
	put32(cmd, SGX_XFER_NUM_UPDATES, 1);
	put32(cmd, SGX_XFER_UPDATES, update_addr);
	put32(cmd, SGX_XFER_UPDATES + 4, update_val);
	return size;
}

/* A priority-0 context: the EDM scans only that queue. */
void sgx_xfer_ctx_init(u8 *ctx, u32 pd_phys, u32 ccb, u32 ccb_ctl)
{
	unsigned int i;

	for (i = 0; i < SGX_XFER_CTX_SIZE; i++)
		ctx[i] = 0;
	put32(ctx, SGX_XFER_CTX_PD, pd_phys);
	put32(ctx, SGX_XFER_CTX_CCB, ccb);
	put32(ctx, SGX_XFER_CTX_CCB_CTL, ccb_ctl);
}

/* Vertex coordinates: 24-bit fixed point, 8 fraction bits, offset 4096. */
static u32 vtx(u32 c)
{
	return ((c + 0x1000) << 8) & 0xffffff;
}

/*
 * The background object of a FASTSCALE render (design/sgx545_transfer.md
 * §4.2.3): the PDS state for the secondary and primary programs, then three
 * vertices (top-left, top-right, bottom-left) packed two to five dwords.
 * pri_data and sec_data are the programs' data-segment sizes in bytes.
 */
void sgx_xfer_bgobj(u32 bg[SGX_XFER_BGOBJ_DWORDS], u32 pds_base, u32 pri,
		    u32 pri_data, u32 sec, u32 sec_data,
		    const struct sgx_xfer_rect *r)
{
	u32 x0 = vtx(r->x0), y0 = vtx(r->y0), x1 = vtx(r->x1), y1 = vtx(r->y1);
	unsigned int i;

	for (i = 0; i < SGX_XFER_BGOBJ_DWORDS; i++)
		bg[i] = 0;
	bg[0] = ((sec - pds_base) >> 4) | (sec_data >> 4) << 26;
	bg[1] = 3 << 16;		/* USETASKSIZE; no attributes */
	bg[2] = ((pri - pds_base) >> 4) | (pri_data >> 4) << 26;
	/* vertex 0 (even slot at dword 9), Z = 0 */
	bg[9] = y0 | (x0 & 0xff) << 24;
	bg[10] = x0 >> 8;
	/* vertex 1 (odd slot from the high half of dword 11) */
	bg[11] = (y0 & 0xffff) << 16;
	bg[12] = (y0 >> 16) | x1 << 8;
	/* vertex 2 (even slot at dword 14) */
	bg[14] = y1 | (x0 & 0xff) << 24;
	bg[15] = x0 >> 8;
}

/* The PBE state of a linear destination (§4.2.5); stride_px must be even. */
void sgx_xfer_pbe(u32 pbe[SGX_XFER_PBE_WORDS], const struct sgx_xfer_rect *r,
		  u32 stride_px, u32 addr, u32 packmode)
{
	pbe[0] = r->x0 | r->y0 << 16;
	pbe[1] = (r->x1 - 1) | (r->y1 - 1) << 16;
	pbe[2] = 0x80 << 8 | (stride_px / 2 - 1) << 16;	/* COUNT, LINESTRIDE */
	pbe[3] = addr & ~3u;
	pbe[4] = 0x200 | packmode;			/* TILERELATIVE */
}

/* setup_transfer_regs for a fill (§4.2.6); returns the command size. */
u32 sgx_xfer_fill_cmd(u8 *cmd, const struct sgx_xfer_fill *f)
{
	const struct sgx_xfer_rect *r = &f->rect;
	const u32 size = 0x1c0;
	unsigned int i;

	for (i = 0; i < SGX_XFER_CMD_SIZE; i++)
		cmd[i] = 0;
	put32(cmd, 0x000, (f->bg - f->req_base) >> 4);	/* ISPBgObjTag */
	put32(cmd, 0x004, 0x200);			/* ISPBgObj: ENABLEBGTAG */
	put32(cmd, 0x008, 0x3f800000);			/* ISPBgObjDepth: 1.0f */
	put32(cmd, 0x00c, 3);				/* ISPRender: FASTSCALE */
	put32(cmd, 0x014, 0x100);			/* ISPIPFMisc: PROCESSEMPTY */
	put32(cmd, 0x018, f->req_base);			/* Bif3DReqBase */
	put32(cmd, 0x01c, 0x80000000);			/* 3DAAMode */
	put32(cmd, 0x020, 0x88);			/* ISPMultiSampCtl */
	put32(cmd, 0x024, f->pixev_exec & ~0xfu);	/* EDMPixelPDSExec */
	put32(cmd, 0x028, 5);				/* ...Data: 0x50 bytes */
	put32(cmd, 0x02c, 1);				/* ...Info: 1 attribute */
	put32(cmd, 0x030, (r->y0 >> 4) | (r->x0 >> 5) << 16);
	put32(cmd, 0x034, (((r->y1 + 15) >> 4) - 1) |
			  (((r->x1 + 31) >> 5) - 1) << 16);
	put32(cmd, 0x038, 0x80);			/* PixelBE */
	put32(cmd, 0x03c, f->pds_exec_base);		/* PDSExecBase */
	put32(cmd, SGX_XFER_SIZE, size);
	put32(cmd, SGX_XFER_NUM_STATUS, 1);
	put32(cmd, SGX_XFER_STATUS, f->status_addr);
	put32(cmd, SGX_XFER_STATUS + 4, f->fence_id + 1);
	put32(cmd, 0x194, 0x07c00000);			/* NumPixelPartitions */
	put32(cmd, 0x198, f->fence_id);			/* FenceID */
	return size;
}
