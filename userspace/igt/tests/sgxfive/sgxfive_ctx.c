// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * CREATE_CONTEXT and DESTROY_CONTEXT: transfer contexts at normal priority on
 * an existing VM; every other value is rejected.
 */
#include <errno.h>

#include "igt.h"
#include "igt_sgxfive.h"

/* Arguments CREATE_CONTEXT accepts; each subtest breaks one field. */
static struct drm_sgxfive_create_context_args valid(uint32_t vm)
{
	return (struct drm_sgxfive_create_context_args){
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER,
		.priority = DRM_SGXFIVE_CTX_PRIORITY_NORMAL,
		.vm_context_handle = vm,
	};
}

int igt_main()
{
	struct drm_sgxfive_create_context_args args;
	uint32_t vm = 0;
	int fd = -1;

	igt_fixture() {
		fd = igt_sgxfive_open();
		vm = igt_sgxfive_create_vm(fd);
	}

	igt_subtest("create-destroy") {
		uint32_t a = igt_sgxfive_create_ctx(fd, vm);
		uint32_t b = igt_sgxfive_create_ctx(fd, vm);	/* several per VM */

		igt_assert(a != b);
		igt_sgxfive_destroy_ctx(fd, a);
		igt_sgxfive_destroy_ctx(fd, b);
		igt_assert_eq(__igt_sgxfive_destroy_ctx(fd, a), -EINVAL);
	}

	igt_subtest("outlives-its-vm-handle") {
		uint32_t v = igt_sgxfive_create_vm(fd);
		uint32_t c = igt_sgxfive_create_ctx(fd, v);

		igt_sgxfive_destroy_vm(fd, v);		/* the context holds the VM */
		igt_sgxfive_submit_dummy(fd, c, 0, 0);
		igt_sgxfive_destroy_ctx(fd, c);
	}

	igt_subtest("bad-type") {
		args = valid(vm);
		args.type = 1;
		igt_assert_eq(__igt_sgxfive_create_ctx(fd, &args), -EINVAL);
	}

	igt_subtest("bad-priority") {
		args = valid(vm);
		args.priority = 1;
		igt_assert_eq(__igt_sgxfive_create_ctx(fd, &args), -EINVAL);
	}

	igt_subtest("bad-flags") {
		args = valid(vm);
		args.flags = 1;
		igt_assert_eq(__igt_sgxfive_create_ctx(fd, &args), -EINVAL);
	}

	igt_subtest("bad-padding") {
		args = valid(vm);
		args._padding_14 = 1;
		igt_assert_eq(__igt_sgxfive_create_ctx(fd, &args), -EINVAL);
	}

	igt_subtest("bad-vm") {
		args = valid(vm);
		args.vm_context_handle = 0xdead;
		igt_assert_eq(__igt_sgxfive_create_ctx(fd, &args), -EINVAL);
	}

	igt_subtest("destroy-bad-handle")
		igt_assert_eq(__igt_sgxfive_destroy_ctx(fd, 0xdead), -EINVAL);

	igt_fixture() {
		igt_sgxfive_destroy_vm(fd, vm);
		drm_close_driver(fd);
	}
}
