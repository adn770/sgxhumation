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
#define SGX_XFER_NUM_DST	(SGX_XFER_SHARED + 0x84)
#define SGX_XFER_DST		(SGX_XFER_SHARED + 0x88)	/* one sync */
#define SGX_XFER_NUM_STATUS	(SGX_XFER_SHARED + 0xb8)
#define SGX_XFER_STATUS		(SGX_XFER_SHARED + 0xbc)	/* {addr, val}[4] */
#define SGX_XFER_NUM_UPDATES	0x19c
#define SGX_XFER_UPDATES	0x1a0				/* {addr, val}[101] */
#define SGX_XFER_FLAG_DUMMY	0x20		/* SGXMKIF_TQFLAGS_DUMMYTRANSFER */

/*
 * PVRSRV_DEVICE_SYNC_OBJECT: the pending values a command waits for and the
 * addresses of the matching complete counters (PVRSRV_SYNC_DATA words).
 */
struct sgx_xfer_sync {
	u32 read_pending, read_complete;
	u32 write_pending, write_complete;
};

/* A transfer rectangle in pixels; x1 and y1 are exclusive. */
struct sgx_xfer_rect {
	u32 x0, y0, x1, y1;
};

#define SGX_XFER_BGOBJ_DWORDS	17	/* the background object, no layers */
#define SGX_XFER_BGOBJ1_DWORDS	23	/* ... with one texture layer (a copy) */
#define SGX_XFER_PBE_WORDS	5	/* emit1 src0/1, emit2 src0/1, sideband */

/* What a solid fill's command needs besides its programs (§4.2.6). */
struct sgx_xfer_fill {
	struct sgx_xfer_rect rect;
	u32 bg;			/* background object address */
	u32 req_base;		/* BIF_3D_REQ_BASE: 1 MiB window holding bg */
	u32 pixev_exec;		/* the pixel-event PDS program */
	u32 pds_exec_base;	/* PDS_EXEC_BASE */
	u32 status_addr;	/* the fence word */
	u32 fence_id;		/* written there as fence_id + 1 */
	u32 layers;		/* TSP layers in the background object */
};

void sgx_xfer_bgobj(u32 bg[SGX_XFER_BGOBJ_DWORDS], u32 pds_base, u32 pri,
		    u32 pri_data, u32 sec, u32 sec_data,
		    const struct sgx_xfer_rect *r);
void sgx_xfer_bgobj_words(u32 bg[SGX_XFER_BGOBJ_DWORDS], const u32 words[3],
			  const struct sgx_xfer_rect *r);
void sgx_xfer_bgobj_copy(u32 bg[SGX_XFER_BGOBJ1_DWORDS], u32 pds_base,
			 u32 pri, u32 pri_data, u32 sec, u32 sec_data,
			 const struct sgx_xfer_rect *dst,
			 const struct sgx_xfer_rect *src, u32 src_w, u32 src_h);
void sgx_xfer_pbe(u32 pbe[SGX_XFER_PBE_WORDS], const struct sgx_xfer_rect *r,
		  u32 stride_px, u32 addr, u32 packmode);
u32 sgx_xfer_fill_cmd(u8 *cmd, const struct sgx_xfer_fill *f);

u32 sgx_xfer_dummy(u8 *cmd, u32 status_addr, u32 status_val, u32 update_addr,
		   u32 update_val, const struct sgx_xfer_sync *dst);
void sgx_xfer_ctx_init(u8 *ctx, u32 pd_phys, u32 ccb, u32 ccb_ctl);

#endif
