// SPDX-License-Identifier: MIT
/*
 * The sgxfive UAPI layout: every struct is the sum of its members (no implicit
 * padding, so i686 and x86-64 agree), a multiple of 8 bytes, the transfer
 * command keeps the offsets of SGXMKIF_TRANSFERCMD.sHWRegs, and the render
 * command and job info grew as design/sgx545_render_uapi.md §11 says.
 */
#include <stddef.h>

#include "sgxfive_drm.h"
#include "tap.h"

#define M(s, f) sizeof(((struct s *)0)->f)

static void layout(void)
{
	CHECK_EQ(sizeof(struct drm_sgxfive_obj_array), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_dev_query_gpu_info), 32);
	CHECK_EQ(offsetof(struct drm_sgxfive_dev_query_gpu_info, quirks), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_dev_query_fw_info), 24);
	CHECK_EQ(sizeof(struct drm_sgxfive_heap), 24);
	CHECK_EQ(sizeof(struct drm_sgxfive_dev_query_heap_info), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_dev_query_job_info), 40);
	CHECK_EQ(offsetof(struct drm_sgxfive_dev_query_job_info, render_cmd_size), 24);
	CHECK_EQ(offsetof(struct drm_sgxfive_dev_query_job_info, render_max_height), 36);
	CHECK_EQ(sizeof(struct drm_sgxfive_dev_query_runtime_info), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_dev_query_args), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_create_bo_args), 24);
	CHECK_EQ(offsetof(struct drm_sgxfive_create_bo_args, flags), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_get_bo_mmap_offset_args), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_create_vm_context_args), 8);
	CHECK_EQ(sizeof(struct drm_sgxfive_destroy_vm_context_args), 8);
	CHECK_EQ(sizeof(struct drm_sgxfive_vm_map_args), 40);
	CHECK_EQ(offsetof(struct drm_sgxfive_vm_map_args, offset), 24);
	CHECK_EQ(sizeof(struct drm_sgxfive_vm_unmap_args), 24);
	CHECK_EQ(sizeof(struct drm_sgxfive_create_context_args), 32);
	CHECK_EQ(offsetof(struct drm_sgxfive_create_context_args, max_width), 20);
	CHECK_EQ(offsetof(struct drm_sgxfive_create_context_args, max_height), 24);
	CHECK_EQ(sizeof(struct drm_sgxfive_destroy_context_args), 8);
	CHECK_EQ(sizeof(struct drm_sgxfive_sync_op), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_mem_update), 16);
	CHECK_EQ(sizeof(struct drm_sgxfive_job), 56);
	CHECK_EQ(offsetof(struct drm_sgxfive_job, cmd_stream), 16);
	CHECK_EQ(offsetof(struct drm_sgxfive_job, sync_ops), 24);
	CHECK_EQ(offsetof(struct drm_sgxfive_job, updates), 40);
	CHECK_EQ(sizeof(struct drm_sgxfive_submit_jobs_args), 16);
}

/* sHWRegs words 0x000-0x0a3 of design/sgx545_transfer.md §2.3, load_flags at 0x044. */
static void transfer_cmd(void)
{
	CHECK_EQ(sizeof(struct drm_sgxfive_transfer_cmd), 0xa8);
	CHECK_EQ(DRM_SGXFIVE_TRANSFER_CMD_SIZE_V1, 0xa8);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, isp_bgobjtag), 0x000);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, isp_render), 0x00c);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, bif_3d_req_base), 0x018);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, event_pixel_pds_exec), 0x024);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, isp_render_box1), 0x030);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, pixelbe), 0x038);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, pds_exec_base), 0x03c);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, double_pixel_partitions), 0x040);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, load_flags), 0x044);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, csc0_coeff), 0x048);
	CHECK_EQ(offsetof(struct drm_sgxfive_transfer_cmd, fir_hfilter), 0x084);
	CHECK_EQ(M(drm_sgxfive_transfer_cmd, fir_hfilter), 36);
}

/* The scene's register words, all __u32, the background words last (§11.4). */
static void render_cmd(void)
{
	CHECK_EQ(sizeof(struct drm_sgxfive_render_cmd), 0x68);
	CHECK_EQ(DRM_SGXFIVE_RENDER_CMD_SIZE_V1, 0x68);
	CHECK_EQ(sizeof(struct drm_sgxfive_render_cmd) % 8, 0);
	CHECK_EQ(offsetof(struct drm_sgxfive_render_cmd, vdm_ctrl_stream_base), 0x00);
	CHECK_EQ(offsetof(struct drm_sgxfive_render_cmd, te_screen), 0x10);
	CHECK_EQ(offsetof(struct drm_sgxfive_render_cmd, mte_screen), 0x24);
	CHECK_EQ(offsetof(struct drm_sgxfive_render_cmd, pixelbe), 0x2c);
	CHECK_EQ(offsetof(struct drm_sgxfive_render_cmd, event_pixel_pds_exec), 0x48);
	CHECK_EQ(offsetof(struct drm_sgxfive_render_cmd, bgobj_words), 0x5c);
	CHECK_EQ(M(drm_sgxfive_render_cmd, bgobj_words), 12);
	CHECK_EQ(DRM_SGXFIVE_CTX_TYPE_RENDER, DRM_SGXFIVE_JOB_TYPE_RENDER);
	CHECK_EQ(DRM_SGXFIVE_JOB_TYPE_RENDER_BIT, 1ull << DRM_SGXFIVE_JOB_TYPE_RENDER);
	CHECK_EQ(DRM_SGXFIVE_SUBMIT_JOB_RENDER_FLAGS_MASK, 0);
}

static void flags(void)
{
	CHECK_EQ(DRM_SGXFIVE_BO_FLAGS_MASK, 1);
	CHECK(!(DRM_SGXFIVE_BO_FLAGS_MASK & DRM_SGXFIVE_BO_CPU_CACHED));
	CHECK_EQ(DRM_SGXFIVE_VM_MAP_FLAGS_MASK, 1);
	CHECK_EQ(DRM_SGXFIVE_SYNC_OP_FLAGS_MASK, 0x8000000fu);
	CHECK_EQ(DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_FLAGS_MASK, 1);
	CHECK_EQ(DRM_SGXFIVE_TRANSFER_LOAD_FLAGS_MASK, 7);
	CHECK_EQ(DRM_SGXFIVE_HEAP_COUNT, 10);
	CHECK_EQ(DRM_SGXFIVE_HEAP_KERNEL_DATA, 7);
	CHECK_EQ(DRM_SGXFIVE_HEAP_PARAMETERS, 8);
	CHECK_EQ(DRM_SGXFIVE_HEAP_TA_DATA, 9);
	/* WR: on an error the kernel writes the failing job's index back. */
	CHECK_EQ(DRM_IOCTL_SGXFIVE_SUBMIT_JOBS,
		 DRM_IOWR(DRM_COMMAND_BASE + 0x09, struct drm_sgxfive_submit_jobs_args));
}

int main(void)
{
	layout();
	transfer_cmd();
	render_cmd();
	flags();
	return tap_done();
}
