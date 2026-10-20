/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * SGX545 transfer-queue structures: SGXMKIF_TRANSFERCMD (0x4c8 bytes) and
 * SGXMKIF_HWTRANSFERCONTEXT (0xb4), laid out as design/sgx545_transfer.md §2.
 */
#ifndef _GMA500_SGX_XFER_H_
#define _GMA500_SGX_XFER_H_

#include "sgx_fw_types.h"

#define SGX_XFER_CMD_SIZE	0x4c8
#define SGX_XFER_CTX_SIZE	0xb4
#define SGX_XFER_CCB_SIZE	0x10000		/* the EDM masks the read offset */

/* SGXMKIF_HWTRANSFERCONTEXT */
#define SGX_XFER_CTX_FLAGS	0x00
#define SGX_XFER_CTX_PRIORITY	0x04
#define SGX_XFER_CTX_PD		0x08
#define SGX_XFER_CTX_CCB	0x0c
#define SGX_XFER_CTX_CCB_CTL	0x10
#define SGX_XFER_CTX_COUNT	0x14

/* SGXMKIF_TRANSFERCMD */
#define SGX_XFER_SIZE		0x0a4
#define SGX_XFER_FLAGS		0x0a8
#define SGX_XFER_SHARED		0x0ac
#define SGX_XFER_NUM_STATUS	(SGX_XFER_SHARED + 0xb8)
#define SGX_XFER_STATUS		(SGX_XFER_SHARED + 0xbc)	/* {addr, val}[4] */
#define SGX_XFER_NUM_UPDATES	0x19c
#define SGX_XFER_UPDATES	0x1a0				/* {addr, val}[101] */
#define SGX_XFER_FLAG_DUMMY	0x20		/* SGXMKIF_TQFLAGS_DUMMYTRANSFER */

u32 sgx_xfer_dummy(u8 *cmd, u32 status_addr, u32 status_val, u32 update_addr,
		   u32 update_val);
void sgx_xfer_ctx_init(u8 *ctx, u32 pd_phys, u32 ccb, u32 ccb_ctl);

#endif
