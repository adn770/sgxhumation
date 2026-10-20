// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * CREATE_BO and GET_BO_MMAP_OFFSET: page-rounded sizes, CPU mappings only
 * with ALLOW_CPU_ACCESS, and every reserved bit rejected.
 */
#include <errno.h>
#include <sys/mman.h>

#include "igt.h"
#include "igt_sgxfive.h"

int igt_main()
{
	int fd = -1;

	igt_fixture()
		fd = igt_sgxfive_open();

	igt_subtest("create-rounds-up") {
		uint64_t size = 5000;
		uint32_t bo = 0;

		igt_assert_eq(__igt_sgxfive_create_bo(fd, &size, 0, &bo), 0);
		igt_assert_eq_u64(size, 2 * SGXFIVE_PAGE_SIZE);
		gem_close(fd, bo);
	}

	igt_subtest("mmap") {
		const uint64_t size = 4 * SGXFIVE_PAGE_SIZE;
		uint32_t bo = igt_sgxfive_create_bo(fd, size, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS);
		uint32_t *p = igt_sgxfive_mmap_bo(fd, bo, size);
		uint32_t *q = igt_sgxfive_mmap_bo(fd, bo, size);

		for (uint32_t i = 0; i < size / 4; i++)
			igt_assert_eq_u32(p[i], 0);		/* zeroed */
		p[size / 4 - 1] = 0x12345678;
		igt_assert_eq_u32(q[size / 4 - 1], 0x12345678);	/* the same pages */
		munmap(p, size);
		munmap(q, size);
		gem_close(fd, bo);
	}

	igt_subtest("mmap-needs-cpu-access") {
		uint32_t bo = igt_sgxfive_create_bo(fd, SGXFIVE_PAGE_SIZE, 0);
		uint64_t offset;

		igt_assert_eq(__igt_sgxfive_mmap_offset(fd, bo, &offset), -EINVAL);
		gem_close(fd, bo);
	}

	igt_subtest("mmap-bad-handle") {
		uint64_t offset;

		igt_assert_eq(__igt_sgxfive_mmap_offset(fd, 0xdead, &offset), -ENOENT);
	}

	igt_subtest("zero-size") {
		uint64_t size = 0;
		uint32_t bo;

		igt_assert_eq(__igt_sgxfive_create_bo(fd, &size, 0, &bo), -EINVAL);
	}

	igt_subtest("bad-flags") {
		uint64_t size = SGXFIVE_PAGE_SIZE;
		uint32_t bo;

		/* Defined in the UAPI but not offered yet (design §3.1, probe Q5). */
		igt_assert_eq(__igt_sgxfive_create_bo(fd, &size, DRM_SGXFIVE_BO_CPU_CACHED, &bo),
			      -EINVAL);
		igt_assert_eq(__igt_sgxfive_create_bo(fd, &size, 1ULL << 40, &bo), -EINVAL);
	}

	igt_subtest("bad-padding") {
		struct drm_sgxfive_create_bo_args args = {
			.size = SGXFIVE_PAGE_SIZE, ._padding_c = 1,
		};

		do_ioctl_err(fd, DRM_IOCTL_SGXFIVE_CREATE_BO, &args, EINVAL);
	}

	igt_fixture()
		drm_close_driver(fd);
}
