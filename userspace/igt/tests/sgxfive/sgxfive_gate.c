// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * The render-node gate, gma500_gfx.sgx_render: off by default, there is no
 * render node and the primary node (card0, handed to the seat user and the
 * compositor) answers only DEV_QUERY, every other sgxfive ioctl failing with
 * EPERM; on, the render node exists and card0 works as it does. Each subtest
 * skips unless the gate is in the state it checks.
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "igt.h"
#include "igt_sgxfive.h"

#define SGX_RENDER_PARAM	"/sys/module/gma500_gfx/parameters/sgx_render"

/* 1 or 0 as the parameter reads Y or N, -1 if gma500 has no such parameter. */
static int sgx_render_param(void)
{
	char c = 0;
	int fd = open(SGX_RENDER_PARAM, O_RDONLY);

	if (fd < 0)
		return -1;
	if (read(fd, &c, 1) != 1)
		c = 0;
	close(fd);
	return c == 'Y' ? 1 : c == 'N' ? 0 : -1;
}

static bool sgxfive_render_node_exists(void)
{
	int fd = __drm_open_driver_render(DRIVER_SGXFIVE);

	if (fd < 0)
		return false;
	close(fd);
	return true;
}

int igt_main()
{
	int fd = -1, on = -1;

	igt_fixture() {
		uint32_t size = 0;

		fd = drm_open_driver(DRIVER_SGXFIVE);	/* the primary node */
		igt_require_f(__igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, NULL,
						      &size) == 0, "gma500 without sgxfive\n");
		on = sgx_render_param();
		igt_require_f(on >= 0, "gma500 has no sgx_render parameter\n");
	}

	igt_subtest("primary-dev-query") {
		struct drm_sgxfive_dev_query_gpu_info g = { };

		igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, &g, sizeof(g));
		igt_assert_eq_u32(g.core_id, 0x01150000);	/* SGX545 */
	}

	igt_subtest("off-no-render-node") {
		igt_require_f(!on, "gma500_gfx.sgx_render=1: the render node is on\n");
		igt_assert_f(!sgxfive_render_node_exists(), "render node with sgx_render=0\n");
	}

	igt_subtest("off-primary-eperm") {
		struct drm_sgxfive_create_context_args ctx = {
			.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER,
			.priority = DRM_SGXFIVE_CTX_PRIORITY_NORMAL,
		};
		struct drm_sgxfive_job job = igt_sgxfive_dummy_job(1);
		uint64_t size = SGXFIVE_PAGE_SIZE, offset;
		uint32_t handle, vm, failed;

		igt_require_f(!on, "gma500_gfx.sgx_render=1: the render node is on\n");
		igt_assert_eq(__igt_sgxfive_create_bo(fd, &size, 0, &handle), -EPERM);
		igt_assert_eq(__igt_sgxfive_mmap_offset(fd, 1, &offset), -EPERM);
		igt_assert_eq(__igt_sgxfive_create_vm(fd, &vm), -EPERM);
		igt_assert_eq(__igt_sgxfive_destroy_vm(fd, 1), -EPERM);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, 1, 0x3e000000, 1, 0, SGXFIVE_PAGE_SIZE, 0),
			      -EPERM);
		igt_assert_eq(__igt_sgxfive_vm_unmap(fd, 1, 0x3e000000, SGXFIVE_PAGE_SIZE),
			      -EPERM);
		igt_assert_eq(__igt_sgxfive_create_ctx(fd, &ctx), -EPERM);
		igt_assert_eq(__igt_sgxfive_destroy_ctx(fd, 1), -EPERM);
		igt_assert_eq(__igt_sgxfive_submit(fd, &job, 1, &failed), -EPERM);
	}

	igt_subtest("on-render-node") {
		igt_require_f(on, "gma500_gfx.sgx_render=0: the render node is off\n");
		igt_assert(sgxfive_render_node_exists());
	}

	igt_subtest("on-primary-allowed") {
		uint64_t addr;
		uint32_t vm, bo;

		igt_require_f(on, "gma500_gfx.sgx_render=0: the render node is off\n");
		addr = igt_sgxfive_heap(fd, DRM_SGXFIVE_HEAP_GENERAL).base;
		bo = igt_sgxfive_create_bo(fd, SGXFIVE_PAGE_SIZE, 0);
		vm = igt_sgxfive_create_vm(fd);
		igt_sgxfive_vm_map(fd, vm, addr, bo, 0, SGXFIVE_PAGE_SIZE, 0);
		igt_sgxfive_vm_unmap(fd, vm, addr, SGXFIVE_PAGE_SIZE);
		igt_sgxfive_destroy_vm(fd, vm);
		gem_close(fd, bo);
	}

	igt_fixture()
		drm_close_driver(fd);
}
