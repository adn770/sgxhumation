// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * DEV_QUERY: what the device reports, and the size rules (a NULL pointer
 * asks for the size, a short buffer gets a truncated copy).
 */
#include <errno.h>
#include <string.h>

#include "igt.h"
#include "igt_sgxfive.h"

int igt_main()
{
	int fd = -1;

	igt_fixture()
		fd = igt_sgxfive_open();

	igt_subtest("gpu-info") {
		struct drm_sgxfive_dev_query_gpu_info g = { };

		igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, &g, sizeof(g));
		igt_assert_eq_u32(g.core_id, 0x01150000);	/* SGX545 */
		igt_assert(g.core_revision);
		igt_assert_eq_u64(g.quirks & ~DRM_SGXFIVE_QUIRKS_MASK, 0);
		/* Host bridges below revision 4 need the sample-cache layout. */
		igt_assert_eq_u64(g.quirks & DRM_SGXFIVE_QUIRK_SAMPLE_CACHE,
				  g.host_bridge_rev < 4 ? DRM_SGXFIVE_QUIRK_SAMPLE_CACHE : 0);
	}

	igt_subtest("fw-info") {
		struct drm_sgxfive_dev_query_fw_info f = { };
		uint32_t vm = igt_sgxfive_create_vm(fd);
		uint32_t ctx = igt_sgxfive_create_ctx(fd, vm);	/* boots the firmware */

		igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_FW_INFO, &f, sizeof(f));
		igt_assert_eq_u32(f.state, DRM_SGXFIVE_FW_STATE_ON);
		igt_assert(f.ddk_build);
		igt_assert(f.fw_format >= 1);
		igt_assert_eq_u32(f._padding_14, 0);
		igt_sgxfive_destroy_ctx(fd, ctx);
		igt_sgxfive_destroy_vm(fd, vm);
	}

	igt_subtest("heap-info") {
		struct drm_sgxfive_heap h[DRM_SGXFIVE_HEAP_COUNT] = { };

		igt_assert_eq_u32(igt_sgxfive_heaps(fd, h, DRM_SGXFIVE_HEAP_COUNT),
				  DRM_SGXFIVE_HEAP_COUNT);
		for (int i = 0; i < DRM_SGXFIVE_HEAP_COUNT; i++) {
			bool kernel = i == DRM_SGXFIVE_HEAP_KERNEL_CODE ||
				      i == DRM_SGXFIVE_HEAP_KERNEL_DATA;

			igt_assert_eq_u32(h[i].flags & ~DRM_SGXFIVE_HEAP_FLAGS_MASK, 0);
			igt_assert_eq(!!(h[i].flags & DRM_SGXFIVE_HEAP_RESERVED), kernel);
			igt_assert_eq_u32(h[i].page_size_log2, 12);
			igt_assert(h[i].base + h[i].size <= 1ULL << 32);
		}
		igt_assert(h[DRM_SGXFIVE_HEAP_GENERAL].size);
		igt_assert(h[DRM_SGXFIVE_HEAP_PDS_PIXEL].size);
		igt_assert(h[DRM_SGXFIVE_HEAP_USSE_PIXEL].size);
	}

	igt_subtest("job-info") {
		struct drm_sgxfive_dev_query_job_info j = { };

		igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_JOB_INFO, &j, sizeof(j));
		igt_assert_eq_u64(j.job_types, DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT);
		igt_assert(j.max_jobs >= 1);
		igt_assert(j.max_sync_ops >= 2);
		igt_assert_eq_u32(j.transfer_cmd_size, sizeof(struct drm_sgxfive_transfer_cmd));
		igt_assert_eq_u32(j.max_updates, 0);
	}

	igt_subtest("runtime-info") {
		struct drm_sgxfive_dev_query_runtime_info r = { };

		igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO, &r, sizeof(r));
		igt_assert(r.num_pixel_partitions);
		igt_assert(r.tile_width && r.tile_height);
		igt_assert_eq_u32(r._padding_c, 0);
	}

	igt_subtest("size-only") {
		uint32_t size = 0;

		igt_assert_eq(__igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, NULL,
						      &size), 0);
		igt_assert_eq_u32(size, sizeof(struct drm_sgxfive_dev_query_gpu_info));
	}

	igt_subtest("short-buffer") {
		struct drm_sgxfive_dev_query_gpu_info g;
		uint32_t size = 8;

		memset(&g, 0xa5, sizeof(g));
		igt_assert_eq(__igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, &g,
						      &size), 0);
		igt_assert_eq_u32(size, 8);
		igt_assert_eq_u32(g.core_id, 0x01150000);
		igt_assert_eq_u32(g.core_id_sw, 0xa5a5a5a5);	/* past the 8 bytes */
	}

	igt_subtest("bad-type") {
		struct drm_sgxfive_dev_query_gpu_info g = { };
		uint32_t size = sizeof(g);

		igt_assert_eq(__igt_sgxfive_dev_query(fd, 99, &g, &size), -EINVAL);
	}

	igt_fixture()
		drm_close_driver(fd);
}
