// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 */
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_sgxfive.h"
#include "igt_syncobj.h"
#include "ioctl_wrappers.h"
#include "sw_sync.h"

/**
 * SECTION:igt_sgxfive
 * @short_description: sgxfive support library
 * @title: sgxfive
 * @include: igt_sgxfive.h
 *
 * Helpers for the sgxfive render node (PowerVR SGX545, hosted by gma500).
 * The double-underscore variants return 0 or a negative errno; the others
 * assert success.
 */

static int sgxfive_ioctl(int fd, unsigned long request, void *arg)
{
	int err = 0;

	if (igt_ioctl(fd, request, arg))
		err = -errno;
	errno = 0;
	return err;
}

/**
 * igt_sgxfive_open:
 *
 * Opens the render node of a gma500 device that has sgxfive: the device must
 * answer DEV_QUERY, otherwise the test skips (Poulsbo, or Cedarview without
 * sgxfive). It also skips when there is no render node: gma500 creates it
 * only when loaded with sgx_render=1 (trusted users only), and without it the
 * primary node refuses everything but DEV_QUERY (see sgxfive_gate).
 *
 * Returns: the file descriptor.
 */
int igt_sgxfive_open(void)
{
	int fd = __drm_open_driver_render(DRIVER_SGXFIVE);
	uint32_t size = 0;

	igt_require_f(fd >= 0, "no sgxfive render node: boot with gma500_gfx.sgx_render=1\n");

	igt_require_f(__igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, NULL,
					      &size) == 0, "gma500 without sgxfive\n");
	return fd;
}

int __igt_sgxfive_dev_query(int fd, uint32_t type, void *data, uint32_t *size)
{
	struct drm_sgxfive_dev_query_args args = {
		.type = type, .size = *size, .pointer = (uintptr_t)data,
	};
	int err = sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_DEV_QUERY, &args);

	if (!err)
		*size = args.size;
	return err;
}

/* A query that must fill all of size. */
void igt_sgxfive_dev_query(int fd, uint32_t type, void *data, uint32_t size)
{
	uint32_t got = size;

	igt_assert_eq(__igt_sgxfive_dev_query(fd, type, data, &got), 0);
	igt_assert_eq(got, size);
}

/* Up to max heaps into heaps; returns how many the device has. */
uint32_t igt_sgxfive_heaps(int fd, struct drm_sgxfive_heap *heaps, uint32_t max)
{
	struct drm_sgxfive_dev_query_heap_info info = { };
	uint32_t count;

	igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_HEAP_INFO, &info, sizeof(info));
	count = info.heaps.count;
	info.heaps = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(
		count < max ? count : max, heaps);
	igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_HEAP_INFO, &info, sizeof(info));
	return count;
}

struct drm_sgxfive_heap igt_sgxfive_heap(int fd, uint32_t id)
{
	struct drm_sgxfive_heap heaps[DRM_SGXFIVE_HEAP_COUNT] = { };

	igt_assert(id < igt_sgxfive_heaps(fd, heaps, DRM_SGXFIVE_HEAP_COUNT));
	return heaps[id];
}

int __igt_sgxfive_create_bo(int fd, uint64_t *size, uint64_t flags, uint32_t *handle)
{
	struct drm_sgxfive_create_bo_args args = { .size = *size, .flags = flags };
	int err = sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_CREATE_BO, &args);

	if (!err) {
		*size = args.size;
		*handle = args.handle;
	}
	return err;
}

uint32_t igt_sgxfive_create_bo(int fd, uint64_t size, uint64_t flags)
{
	uint32_t handle = 0;

	igt_assert_eq(__igt_sgxfive_create_bo(fd, &size, flags, &handle), 0);
	return handle;
}

int __igt_sgxfive_mmap_offset(int fd, uint32_t handle, uint64_t *offset)
{
	struct drm_sgxfive_get_bo_mmap_offset_args args = { .handle = handle };
	int err = sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET, &args);

	if (!err)
		*offset = args.offset;
	return err;
}

void *igt_sgxfive_mmap_bo(int fd, uint32_t handle, uint64_t size)
{
	uint64_t offset = 0;
	void *p;

	igt_assert_eq(__igt_sgxfive_mmap_offset(fd, handle, &offset), 0);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	igt_assert(p != MAP_FAILED);
	return p;
}

int __igt_sgxfive_create_vm(int fd, uint32_t *vm)
{
	struct drm_sgxfive_create_vm_context_args args = { };
	int err = sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT, &args);

	if (!err)
		*vm = args.handle;
	return err;
}

uint32_t igt_sgxfive_create_vm(int fd)
{
	uint32_t vm = 0;

	igt_assert_eq(__igt_sgxfive_create_vm(fd, &vm), 0);
	return vm;
}

int __igt_sgxfive_destroy_vm(int fd, uint32_t vm)
{
	struct drm_sgxfive_destroy_vm_context_args args = { .handle = vm };

	return sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT, &args);
}

void igt_sgxfive_destroy_vm(int fd, uint32_t vm)
{
	igt_assert_eq(__igt_sgxfive_destroy_vm(fd, vm), 0);
}

int __igt_sgxfive_vm_map(int fd, uint32_t vm, uint64_t addr, uint32_t bo, uint64_t offset,
			 uint64_t size, uint32_t flags)
{
	struct drm_sgxfive_vm_map_args args = {
		.vm_context_handle = vm, .flags = flags, .device_addr = addr,
		.handle = bo, .offset = offset, .size = size,
	};

	return sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_VM_MAP, &args);
}

void igt_sgxfive_vm_map(int fd, uint32_t vm, uint64_t addr, uint32_t bo, uint64_t offset,
			uint64_t size, uint32_t flags)
{
	igt_assert_eq(__igt_sgxfive_vm_map(fd, vm, addr, bo, offset, size, flags), 0);
}

int __igt_sgxfive_vm_unmap(int fd, uint32_t vm, uint64_t addr, uint64_t size)
{
	struct drm_sgxfive_vm_unmap_args args = {
		.vm_context_handle = vm, .device_addr = addr, .size = size,
	};

	return sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_VM_UNMAP, &args);
}

void igt_sgxfive_vm_unmap(int fd, uint32_t vm, uint64_t addr, uint64_t size)
{
	igt_assert_eq(__igt_sgxfive_vm_unmap(fd, vm, addr, size), 0);
}

int __igt_sgxfive_create_ctx(int fd, struct drm_sgxfive_create_context_args *args)
{
	return sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_CREATE_CONTEXT, args);
}

/* A transfer context on vm. */
uint32_t igt_sgxfive_create_ctx(int fd, uint32_t vm)
{
	struct drm_sgxfive_create_context_args args = {
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER,
		.priority = DRM_SGXFIVE_CTX_PRIORITY_NORMAL,
		.vm_context_handle = vm,
	};

	igt_assert_eq(__igt_sgxfive_create_ctx(fd, &args), 0);
	igt_assert(args.handle);
	return args.handle;
}

int __igt_sgxfive_destroy_ctx(int fd, uint32_t ctx)
{
	struct drm_sgxfive_destroy_context_args args = { .handle = ctx };

	return sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_DESTROY_CONTEXT, &args);
}

void igt_sgxfive_destroy_ctx(int fd, uint32_t ctx)
{
	igt_assert_eq(__igt_sgxfive_destroy_ctx(fd, ctx), 0);
}

/* On error, *failed (if not NULL) is the index of the job that failed. */
int __igt_sgxfive_submit(int fd, struct drm_sgxfive_job *jobs, uint32_t count,
			 uint32_t *failed)
{
	struct drm_sgxfive_submit_jobs_args args = {
		.jobs = DRM_SGXFIVE_OBJ_ARRAY(count, jobs),
	};
	int err = sgxfive_ioctl(fd, DRM_IOCTL_SGXFIVE_SUBMIT_JOBS, &args);

	if (err && failed)
		*failed = args.jobs.count;
	return err;
}

/* A dummy transfer on ctx: firmware work that touches no memory. */
struct drm_sgxfive_job igt_sgxfive_dummy_job(uint32_t ctx)
{
	return (struct drm_sgxfive_job){
		.type = DRM_SGXFIVE_JOB_TYPE_TRANSFER,
		.context_handle = ctx,
		.flags = DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY,
	};
}

/* A dummy waiting on binary syncobj wait and signalling signal (0: none). */
int __igt_sgxfive_submit_dummy(int fd, uint32_t ctx, uint32_t wait, uint32_t signal)
{
	struct drm_sgxfive_sync_op ops[2];
	struct drm_sgxfive_job job = igt_sgxfive_dummy_job(ctx);
	uint32_t n = 0;

	if (wait)
		ops[n++] = (struct drm_sgxfive_sync_op){
			.handle = wait, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_WAIT,
		};
	if (signal)
		ops[n++] = (struct drm_sgxfive_sync_op){
			.handle = signal, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL,
		};
	job.sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(n, ops);
	return __igt_sgxfive_submit(fd, &job, 1, NULL);
}

void igt_sgxfive_submit_dummy(int fd, uint32_t ctx, uint32_t wait, uint32_t signal)
{
	igt_assert_eq(__igt_sgxfive_submit_dummy(fd, ctx, wait, signal), 0);
}

/*
 * Wait up to timeout_ms for syncobj (point 0: binary; otherwise that timeline
 * point), also for its fence to be attached. Returns 0 or a negative errno,
 * -ETIME on timeout.
 */
int igt_sgxfive_wait(int fd, uint32_t syncobj, uint64_t point, uint32_t timeout_ms)
{
	const uint32_t flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	struct timespec ts;
	int64_t deadline;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	deadline = ts.tv_sec * 1000000000LL + ts.tv_nsec + timeout_ms * 1000000LL;
	if (!point)
		return syncobj_wait_err(fd, &syncobj, 1, deadline, flags);
	return syncobj_timeline_wait_err(fd, &syncobj, &point, 1, deadline, flags);
}

/*
 * The status of a binary syncobj's fence, from a sync_file exported from it:
 * 1 signalled, 0 pending, or the error it signalled with (-EIO for a job that
 * faulted, -ETIMEDOUT for one that hung, -ECANCELED for a job of a lost
 * context). A syncobj wait succeeds whatever the error; this reads it.
 */
int igt_sgxfive_fence_status(int fd, uint32_t syncobj)
{
	int sf = syncobj_handle_to_fd(fd, syncobj,
				      DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE);
	int status;

	igt_assert_lte(0, sf);
	status = sync_fence_status(sf);
	close(sf);
	return status;
}
