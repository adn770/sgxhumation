// SPDX-License-Identifier: GPL-2.0-only
/*
 * The firmware form of an sgxfive transfer job: the UAPI register block
 * (struct drm_sgxfive_transfer_cmd) becomes an SGXMKIF_TRANSFERCMD
 * (design/sgx545_transfer.md §2.3), with the fields the kernel owns filled in.
 */
#include <linux/stddef.h>
#include <linux/string.h>

#include "sgx_xfer.h"
#include "sgxfive_priv.h"

/* SGXMKIF_TRANSFERCMD offsets beyond sgx_xfer.h's. */
#define CMD_HWREGS_SIZE		0xa4	/* sHWRegs: 41 register words */
#define CMD_LOAD_YUV		0x188
#define CMD_LOAD_LUMAKEY	0x18c
#define CMD_LOAD_FIR		0x190
#define CMD_PIXEL_PARTITIONS	0x194
#define CMD_FENCE_ID		0x198
#define PIXEL_PARTITIONS	0x07c00000	/* DMS_CTRL field, 31 partitions */
#define CMD_SIZE_IN_CCB		0x1c0		/* used part, 64-byte multiple */

static void put32(u8 *p, unsigned int off, u32 v)
{
	memcpy(p + off, &v, 4);		/* little-endian host */
}

/*
 * Fill cmd (SGX_XFER_CMD_SIZE bytes) for one job; regs is NULL for a dummy.
 * The firmware writes value to status_addr on completion. Returns the size
 * the command takes in the client CCB.
 */
u32 sgxfive_job_build(u8 *cmd, const struct drm_sgxfive_transfer_cmd *regs,
		      bool dummy, u32 status_addr, u32 value)
{
	memset(cmd, 0, SGX_XFER_CMD_SIZE);
	if (regs) {
		/*
		 * The UAPI block is sHWRegs with load_flags inserted after the
		 * 17 fixed registers: copy around it.
		 */
		const u8 *src = (const u8 *)regs;
		const size_t tail = offsetof(struct drm_sgxfive_transfer_cmd,
					     csc0_coeff);

		memcpy(cmd, src, 17 * 4);
		memcpy(cmd + 17 * 4, src + tail, CMD_HWREGS_SIZE - 17 * 4);
		put32(cmd, CMD_LOAD_YUV,
		      !!(regs->load_flags & DRM_SGXFIVE_TRANSFER_LOAD_YUV));
		put32(cmd, CMD_LOAD_LUMAKEY,
		      !!(regs->load_flags & DRM_SGXFIVE_TRANSFER_LOAD_LUMAKEY));
		put32(cmd, CMD_LOAD_FIR,
		      !!(regs->load_flags & DRM_SGXFIVE_TRANSFER_LOAD_FIR));
		put32(cmd, CMD_PIXEL_PARTITIONS, PIXEL_PARTITIONS);
		put32(cmd, CMD_FENCE_ID, value);
	}
	put32(cmd, SGX_XFER_SIZE, CMD_SIZE_IN_CCB);
	put32(cmd, SGX_XFER_FLAGS, dummy ? SGX_XFER_FLAG_DUMMY : 0);
	put32(cmd, SGX_XFER_NUM_STATUS, 1);
	put32(cmd, SGX_XFER_STATUS, status_addr);
	put32(cmd, SGX_XFER_STATUS + 4, value);
	return CMD_SIZE_IN_CCB;
}

/* addr lies in a heap clients may map (one page from it must fit). */
static bool in_user_heap(u32 addr)
{
	return !sgxfive_vm_check_range(sgxfive_rev1_heaps, DRM_SGXFIVE_HEAP_COUNT,
				       addr & PAGE_MASK, PAGE_SIZE);
}

/*
 * The checks on a job's register block: the addresses the pixel pipe
 * fetches from or runs code at must be in the client's heaps, and fields
 * this kernel does not support must be clear.
 */
int sgxfive_job_check(const struct drm_sgxfive_transfer_cmd *regs)
{
	if (regs->load_flags & ~DRM_SGXFIVE_TRANSFER_LOAD_FLAGS_MASK ||
	    regs->double_pixel_partitions)
		return -EINVAL;
	if (regs->bif_3d_req_base & (SZ_1M - 1) ||
	    !in_user_heap(regs->bif_3d_req_base) ||
	    !in_user_heap(regs->pds_exec_base) ||
	    !in_user_heap(regs->event_pixel_pds_exec))
		return -EINVAL;
	if (regs->isp_rgn_base && !in_user_heap(regs->isp_rgn_base))
		return -EINVAL;
	return 0;
}
