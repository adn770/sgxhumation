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
