// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * VM contexts and VM_MAP/VM_UNMAP: mappings stay inside one user heap, never
 * overlap, are page aligned and within their BO, and are unmapped whole.
 */
#include <errno.h>

#include "igt.h"
#include "igt_sgxfive.h"

#define PAGE SGXFIVE_PAGE_SIZE

int igt_main()
{
	struct drm_sgxfive_heap gen, usse, kcode, kdata;
	uint32_t vm = 0, bo = 0;
	int fd = -1;

	igt_fixture() {
		fd = igt_sgxfive_open();
		gen = igt_sgxfive_heap(fd, DRM_SGXFIVE_HEAP_GENERAL);
		usse = igt_sgxfive_heap(fd, DRM_SGXFIVE_HEAP_USSE_PIXEL);
		kcode = igt_sgxfive_heap(fd, DRM_SGXFIVE_HEAP_KERNEL_CODE);
		kdata = igt_sgxfive_heap(fd, DRM_SGXFIVE_HEAP_KERNEL_DATA);
		vm = igt_sgxfive_create_vm(fd);
		bo = igt_sgxfive_create_bo(fd, 3 * PAGE, 0);
	}

	igt_subtest("create-destroy") {
		uint32_t v = igt_sgxfive_create_vm(fd);

		igt_assert(v && v != vm);
		igt_sgxfive_destroy_vm(fd, v);
		igt_assert_eq(__igt_sgxfive_destroy_vm(fd, v), -EINVAL);
	}

	igt_subtest("destroy-bad-handle")
		igt_assert_eq(__igt_sgxfive_destroy_vm(fd, 0xdead), -EINVAL);

	igt_subtest("map-unmap") {
		igt_sgxfive_vm_map(fd, vm, gen.base, bo, 0, 2 * PAGE, 0);
		igt_sgxfive_vm_unmap(fd, vm, gen.base, 2 * PAGE);
		igt_assert_eq(__igt_sgxfive_vm_unmap(fd, vm, gen.base, 2 * PAGE), -ENOENT);
	}

	igt_subtest("map-read-only") {
		igt_sgxfive_vm_map(fd, vm, gen.base, bo, PAGE, PAGE,
				   DRM_SGXFIVE_VM_MAP_READ_ONLY);
		igt_sgxfive_vm_unmap(fd, vm, gen.base, PAGE);
	}

	igt_subtest("overlap") {
		igt_sgxfive_vm_map(fd, vm, gen.base, bo, 0, 2 * PAGE, 0);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base + PAGE, bo, 0, PAGE, 0),
			      -EEXIST);
		igt_sgxfive_vm_unmap(fd, vm, gen.base, 2 * PAGE);
	}

	igt_subtest("unmap-whole-only") {
		igt_sgxfive_vm_map(fd, vm, gen.base, bo, 0, 2 * PAGE, 0);
		igt_assert_eq(__igt_sgxfive_vm_unmap(fd, vm, gen.base, PAGE), -ENOENT);
		igt_assert_eq(__igt_sgxfive_vm_unmap(fd, vm, gen.base + PAGE, PAGE), -ENOENT);
		igt_sgxfive_vm_unmap(fd, vm, gen.base, 2 * PAGE);
	}

	igt_subtest("kernel-heaps-unmappable") {
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, kcode.base, bo, 0, PAGE, 0), -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, kdata.base, bo, 0, PAGE,
						   DRM_SGXFIVE_VM_MAP_READ_ONLY), -EINVAL);
	}

	igt_subtest("bad-ranges") {
		/* Unaligned address, past the BO, across a heap's end, above 4 GiB. */
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base + 8, bo, 0, PAGE, 0), -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base, bo, 0, 4 * PAGE, 0), -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base, bo, 3 * PAGE, PAGE, 0),
			      -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, usse.base + usse.size - PAGE, bo, 0,
						   2 * PAGE, 0), -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, 1ULL << 32, bo, 0, PAGE, 0), -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base, bo, 0, 0, 0), -EINVAL);
	}

	igt_subtest("bad-flags")
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base, bo, 0, PAGE, 1u << 5),
			      -EINVAL);

	igt_subtest("bad-handles") {
		igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, gen.base, 0xdead, 0, PAGE, 0), -ENOENT);
		igt_assert_eq(__igt_sgxfive_vm_map(fd, 0xdead, gen.base, bo, 0, PAGE, 0), -EINVAL);
		igt_assert_eq(__igt_sgxfive_vm_unmap(fd, 0xdead, gen.base, PAGE), -EINVAL);
	}

	igt_subtest("bo-outlives-its-handle") {
		uint32_t b = igt_sgxfive_create_bo(fd, PAGE, 0);

		igt_sgxfive_vm_map(fd, vm, gen.base, b, 0, PAGE, 0);
		gem_close(fd, b);			/* the mapping keeps the pages */
		igt_sgxfive_vm_unmap(fd, vm, gen.base, PAGE);
	}

	igt_subtest("destroy-with-mappings") {
		uint32_t v = igt_sgxfive_create_vm(fd);

		igt_sgxfive_vm_map(fd, v, gen.base, bo, 0, PAGE, 0);
		igt_sgxfive_destroy_vm(fd, v);		/* releases the mapping */
	}

	igt_fixture() {
		gem_close(fd, bo);
		igt_sgxfive_destroy_vm(fd, vm);
		drm_close_driver(fd);
	}
}
