// SPDX-License-Identifier: MIT
/*
 * The library's ioctl wrappers against the in-process mock: discovery, the
 * DEV_QUERY size rules, BOs, VM mappings, contexts, syncobjs and the
 * SUBMIT_JOBS validation rules (reserved bits, the reserved update list).
 */
#include <errno.h>
#include <string.h>

#include "sgxfive.h"
#include "sgxfive_mock.h"
#include "tap.h"

static struct sgxfive_mock *mock;

static struct sgxfive_dev *open_dev(const struct sgxfive_mock_config *cfg)
{
	struct sgxfive_dev *dev = NULL;

	if (mock)
		sgxfive_mock_destroy(mock);
	mock = sgxfive_mock_create(cfg);
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(mock), NULL, &dev), 0);
	return dev;
}

static void discovery(void)
{
	const struct sgxfive_mock_config other = { .other_driver = "i915" };
	const struct sgxfive_mock_config none = { .no_sgxfive = true };
	struct sgxfive_dev *dev = open_dev(&other);

	CHECK(dev && !strcmp(sgxfive_dev_path(dev), "/dev/dri/renderD129"));
	sgxfive_dev_close(dev);

	sgxfive_mock_destroy(mock);
	mock = sgxfive_mock_create(&none);
	dev = (struct sgxfive_dev *)1;
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(mock), NULL, &dev), -ENODEV);
	CHECK(dev == NULL);
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(mock), "/dev/dri/renderD128", &dev),
		 -ENOENT);

	/*
	 * Hosted by gma500 (design §9.1a decision 1), the node reports driver name
	 * "gma500". A gma500 node that answers DEV_QUERY is ours; a plain one
	 * (Poulsbo, or Cedarview without sgxfive) is skipped.
	 */
	sgxfive_mock_destroy(mock);
	mock = sgxfive_mock_create(&(struct sgxfive_mock_config){
		.other_driver = "gma500", .sgxfive_name = "gma500" });
	dev = NULL;
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(mock), NULL, &dev), 0);
	CHECK(dev && !strcmp(sgxfive_dev_path(dev), "/dev/dri/renderD129"));
	sgxfive_dev_close(dev);
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(mock), "/dev/dri/renderD128", &dev),
		 -ENODEV);
}

static void query(void)
{
	struct sgxfive_dev *dev = open_dev(&(struct sgxfive_mock_config){ 0 });
	const struct sgxfive_info *info = sgxfive_dev_info(dev);
	struct drm_sgxfive_dev_query_gpu_info gpu;
	uint32_t size;

	CHECK_EQ(info->gpu.core_id, 0x01150000);
	CHECK_EQ(info->gpu.quirks, DRM_SGXFIVE_QUIRK_SAMPLE_CACHE);
	CHECK_EQ(info->gpu.host_bridge_rev, 3);
	CHECK_EQ(info->fw.ddk_build, 862890);
	CHECK_EQ(info->job.job_types, DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT);
	CHECK_EQ(info->job.transfer_cmd_size, 0xa8);
	CHECK_EQ(info->job.max_updates, 0);
	CHECK_EQ(info->runtime.num_pixel_partitions, 31);
	CHECK_EQ(info->num_heaps, DRM_SGXFIVE_HEAP_COUNT);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_GENERAL].base, 0x3e000000);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_TEXTURE].base, 0xf0000000);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_PDS_PIXEL].base, 0x2e000000);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_USSE_PIXEL].base, 0x37000000);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_KERNEL_CODE].flags, DRM_SGXFIVE_HEAP_RESERVED);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_KERNEL_DATA].base, 0x32000000);
	CHECK(sgxfive_heap(dev, DRM_SGXFIVE_HEAP_GENERAL) != NULL);
	CHECK(sgxfive_heap(dev, DRM_SGXFIVE_HEAP_COUNT) == NULL);

	/* NULL pointer: size only. Short buffer: truncated copy. Unknown type: -EINVAL. */
	size = 0;
	CHECK_EQ(sgxfive_dev_query(dev, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, NULL, &size), 0);
	CHECK_EQ(size, sizeof(gpu));
	memset(&gpu, 0xa5, sizeof(gpu));
	size = 8;
	CHECK_EQ(sgxfive_dev_query(dev, DRM_SGXFIVE_DEV_QUERY_GPU_INFO, &gpu, &size), 0);
	CHECK_EQ(size, 8);
	CHECK_EQ(gpu.core_revision, 0x0001000d);
	CHECK_EQ(gpu.core_id_sw, 0xa5a5a5a5);
	size = sizeof(gpu);
	CHECK_EQ(sgxfive_dev_query(dev, 99, &gpu, &size), -EINVAL);
	sgxfive_dev_close(dev);

	dev = open_dev(&(struct sgxfive_mock_config){ .layout = SGXFIVE_MOCK_LAYOUT_HARNESS });
	info = sgxfive_dev_info(dev);
	CHECK_EQ(info->gpu.quirks, 0);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_TEXTURE].size, 0);
	CHECK(sgxfive_heap(dev, DRM_SGXFIVE_HEAP_TEXTURE) == NULL);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_PDS_PIXEL].base, 0xe4000000);
	CHECK_EQ(info->heaps[DRM_SGXFIVE_HEAP_USSE_PIXEL].base, 0xf4000000);
	sgxfive_dev_close(dev);
}

static void bos(void)
{
	struct sgxfive_dev *dev = open_dev(&(struct sgxfive_mock_config){ 0 });
	struct drm_sgxfive_create_bo_args args = { .size = 4096, ._padding_c = 1 };
	struct sgxfive_bo *bo = NULL, *priv = NULL;
	struct sgxfive_mock_stats st;
	uint32_t *p = NULL;

	CHECK_EQ(sgxfive_bo_create(dev, 5000, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, &bo), 0);
	CHECK_EQ(bo->size, 8192);
	CHECK_EQ(sgxfive_bo_mmap(bo, (void **)&p), 0);
	p[2047] = 0x12345678;
	CHECK_EQ(((uint32_t *)bo->map)[2047], 0x12345678);
	CHECK_EQ(sgxfive_bo_create(dev, 0, 0, &priv), -EINVAL);
	CHECK_EQ(sgxfive_bo_create(dev, 4096, DRM_SGXFIVE_BO_CPU_CACHED, &priv), -EINVAL);
	CHECK_EQ(sgxfive_bo_create(dev, 4096, 1ULL << 40, &priv), -EINVAL);
	CHECK_EQ(sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_BO, &args), -EINVAL);
	CHECK_EQ(sgxfive_bo_create(dev, 4096, 0, &priv), 0);
	CHECK_EQ(sgxfive_bo_mmap(priv, (void **)&p), -EINVAL);	/* no CPU access */
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.bos, 2);
	sgxfive_bo_destroy(priv);
	sgxfive_bo_destroy(bo);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.bos, 0);
	sgxfive_dev_close(dev);
}

static void vms(void)
{
	struct sgxfive_dev *dev = open_dev(&(struct sgxfive_mock_config){ 0 });
	const struct sgxfive_info *info = sgxfive_dev_info(dev);
	const uint64_t kcode = info->heaps[DRM_SGXFIVE_HEAP_KERNEL_CODE].base;
	const struct drm_sgxfive_heap *usse = sgxfive_heap(dev, DRM_SGXFIVE_HEAP_USSE_PIXEL);
	struct sgxfive_vm *vm = NULL;
	struct sgxfive_bo *bo = NULL;
	struct sgxfive_mock_stats st;
	uint64_t a = 0, b = 0;
	uint32_t ctx = 0;

	CHECK_EQ(sgxfive_vm_create(dev, &vm), 0);
	CHECK_EQ(sgxfive_bo_create(dev, 3 * 4096, 0, &bo), 0);
	CHECK_EQ(sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, 8192, 4096, &a), 0);
	CHECK_EQ(a, 0x3e000000);
	CHECK_EQ(sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, 4096, 0x10000, &b), 0);
	CHECK_EQ(b, 0x3e010000);
	CHECK_EQ(sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_KERNEL_CODE, 4096, 4096, &b), -EINVAL);
	CHECK_EQ(sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, 100, 4096, &b), -EINVAL);

	CHECK_EQ(sgxfive_vm_map(vm, a, bo, 0, 8192, 0), 0);
	CHECK_EQ(sgxfive_vm_map(vm, a + 4096, bo, 0, 4096, 0), -EEXIST);
	CHECK_EQ(sgxfive_vm_map(vm, a + 8192, bo, 4096, 4096, DRM_SGXFIVE_VM_MAP_READ_ONLY), 0);
	CHECK_EQ(sgxfive_vm_map(vm, kcode, bo, 0, 4096, 0), -EINVAL);
	CHECK_EQ(sgxfive_vm_map(vm, a + 0x100000 + 8, bo, 0, 4096, 0), -EINVAL);
	CHECK_EQ(sgxfive_vm_map(vm, a + 0x100000, bo, 0, 4 * 4096, 0), -EINVAL); /* > BO */
	CHECK_EQ(sgxfive_vm_map(vm, a + 0x100000, bo, 0, 4096, 1u << 5), -EINVAL);
	CHECK_EQ(sgxfive_vm_map(vm, usse->base + usse->size - 4096, bo, 0, 8192, 0), -EINVAL);
	CHECK_EQ(sgxfive_vm_map(vm, 0x100000000ULL, bo, 0, 4096, 0), -EINVAL);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.mappings, 2);
	CHECK_EQ(sgxfive_vm_unmap(vm, a, 4096), -ENOENT);
	CHECK_EQ(sgxfive_vm_unmap(vm, a, 8192), 0);
	CHECK_EQ(sgxfive_vm_unmap(vm, a, 8192), -ENOENT);

	CHECK_EQ(sgxfive_ctx_create(vm, &ctx), 0);
	CHECK(ctx != 0);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.ctxs, 1);
	CHECK_EQ(sgxfive_ctx_destroy(dev, ctx), 0);
	CHECK_EQ(sgxfive_ctx_destroy(dev, ctx), -EINVAL);

	/* A BO stays alive while a VM maps it; destroying the VM releases everything. */
	sgxfive_bo_destroy(bo);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.bos, 1);
	sgxfive_vm_destroy(vm);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.bos + st.vms + st.mappings, 0);
	sgxfive_dev_close(dev);
}

static void raw_ctx(struct sgxfive_dev *dev, uint32_t vm)
{
	struct drm_sgxfive_create_context_args c = { .vm_context_handle = vm };

	c.type = 1;
	CHECK_EQ(sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_CONTEXT, &c), -EINVAL);
	c.type = 0;
	c.priority = 1;
	CHECK_EQ(sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_CONTEXT, &c), -EINVAL);
	c.priority = 0;
	c.flags = 1;
	CHECK_EQ(sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_CONTEXT, &c), -EINVAL);
	c.flags = 0;
	c.vm_context_handle = 77;
	CHECK_EQ(sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_CONTEXT, &c), -EINVAL);
}

static void sync_and_submit(void)
{
	struct sgxfive_dev *dev = open_dev(&(struct sgxfive_mock_config){ 0 });
	const uint32_t dummy = DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY;
	struct drm_sgxfive_transfer_cmd cmd = { 0 };
	struct drm_sgxfive_mem_update upd = { .device_addr = 0x3e000000, .value = 1 };
	struct drm_sgxfive_sync_op op, ops2[3];
	struct drm_sgxfive_job jobs[2];
	struct sgxfive_mock_stats st;
	struct sgxfive_vm *vm = NULL;
	uint32_t ctx = 0, a = 0, b = 0, t = 0, failed = 99;

	CHECK_EQ(sgxfive_vm_create(dev, &vm), 0);
	raw_ctx(dev, sgxfive_vm_handle(vm));
	CHECK_EQ(sgxfive_ctx_create(vm, &ctx), 0);
	CHECK_EQ(sgxfive_syncobj_create(dev, false, &a), 0);
	CHECK_EQ(sgxfive_syncobj_create(dev, false, &b), 0);
	CHECK_EQ(sgxfive_syncobj_wait(dev, a, 0, 0), -ETIME);

	/* A dummy signals its syncobj. */
	CHECK_EQ(sgxfive_submit_transfer(dev, ctx, dummy, NULL, 0, a), 0);
	CHECK_EQ(sgxfive_syncobj_wait(dev, a, 0, 1000000), 0);

	/* Waiting on a syncobj without a fence is -EINVAL (drm_syncobj_find_fence)... */
	CHECK_EQ(sgxfive_syncobj_destroy(dev, a), 0);
	CHECK_EQ(sgxfive_syncobj_create(dev, false, &a), 0);
	CHECK_EQ(sgxfive_submit_transfer(dev, ctx, dummy, NULL, a, b), -EINVAL);
	CHECK_EQ(sgxfive_syncobj_wait(dev, b, 0, 0), -ETIME);
	/* ...unless an earlier job of the same call signals it. */
	ops2[0] = (struct drm_sgxfive_sync_op){ a, DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL, 0 };
	ops2[1] = (struct drm_sgxfive_sync_op){ a, DRM_SGXFIVE_SYNC_OP_FLAG_WAIT, 0 };
	ops2[2] = (struct drm_sgxfive_sync_op){ b, DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL, 0 };
	jobs[0] = (struct drm_sgxfive_job){
		.type = DRM_SGXFIVE_JOB_TYPE_TRANSFER, .context_handle = ctx, .flags = dummy,
		.sync_ops = DRM_SGXFIVE_OBJ_ARRAY(1, &ops2[0]),
	};
	jobs[1] = jobs[0];
	jobs[1].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(2, &ops2[1]);
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), 0);
	CHECK_EQ(sgxfive_syncobj_wait(dev, b, 0, 0), 0);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.dummies_run, 3);

	/* Timeline points. */
	CHECK_EQ(sgxfive_syncobj_create(dev, false, &t), 0);
	op = (struct drm_sgxfive_sync_op){
		.handle = t, .value = 2,
		.flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL |
			 DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ,
	};
	jobs[0] = (struct drm_sgxfive_job){
		.type = DRM_SGXFIVE_JOB_TYPE_TRANSFER, .context_handle = ctx, .flags = dummy,
		.sync_ops = DRM_SGXFIVE_OBJ_ARRAY(1, &op),
	};
	CHECK_EQ(sgxfive_syncobj_wait(dev, t, 2, 0), -ETIME);
	CHECK_EQ(sgxfive_submit(dev, jobs, 1, &failed), 0);
	CHECK_EQ(sgxfive_syncobj_wait(dev, t, 1, 0), 0);
	CHECK_EQ(sgxfive_syncobj_wait(dev, t, 2, 0), 0);
	CHECK_EQ(sgxfive_syncobj_wait(dev, t, 3, 0), -ETIME);

	/* Validation: one bad job fails the whole call, reporting its index. */
	jobs[0] = (struct drm_sgxfive_job){
		.type = DRM_SGXFIVE_JOB_TYPE_TRANSFER, .context_handle = ctx, .flags = dummy,
	};
	jobs[1] = jobs[0];
	jobs[1].updates = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(1, &upd);
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);
	CHECK_EQ(failed, 1);
	jobs[1].updates = (struct drm_sgxfive_obj_array){ .stride = sizeof(upd) };
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);	/* stride alone */
	jobs[1].updates = (struct drm_sgxfive_obj_array){ 0 };
	jobs[1].flags = 1u << 7;
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);
	CHECK_EQ(failed, 1);
	jobs[1].flags = dummy;
	jobs[1].cmd_stream = (uintptr_t)&cmd;
	jobs[1].cmd_stream_len = sizeof(cmd);
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);	/* dummy with a stream */
	jobs[1].flags = 0;
	jobs[1].cmd_stream_len = 0x40;
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);	/* short stream */
	jobs[1].cmd_stream_len = sizeof(cmd);
	jobs[1].type = 1;
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);
	jobs[1].type = 0;
	jobs[1].context_handle = 1234;
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);
	jobs[1].context_handle = ctx;
	jobs[1].flags = dummy;
	jobs[1].cmd_stream = 0;
	jobs[1].cmd_stream_len = 0;
	op = (struct drm_sgxfive_sync_op){ .handle = b, .flags = 1u << 8 };
	jobs[1].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(1, &op);
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);
	op.flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL;
	op.value = 5;					/* binary syncobj with a point */
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -EINVAL);
	op.value = 0;
	op.handle = 999;				/* no such syncobj */
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), -ENOENT);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.dummies_run, 4);			/* none of the failed calls ran */
	op.handle = b;
	CHECK_EQ(sgxfive_submit(dev, jobs, 2, &failed), 0);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.dummies_run, 6);

	/* A transfer whose PDS addresses lie outside the user heaps is rejected. */
	cmd.event_pixel_pds_exec = 0x30000000;		/* KERNEL_CODE */
	CHECK_EQ(sgxfive_submit_transfer(dev, ctx, 0, &cmd, 0, 0), -EINVAL);

	CHECK_EQ(sgxfive_ctx_destroy(dev, ctx), 0);
	sgxfive_vm_destroy(vm);
	sgxfive_dev_close(dev);
	sgxfive_mock_get_stats(mock, &st);
	CHECK_EQ(st.syncobjs + st.ctxs + st.vms, 0);		/* closing the file frees all */
}

int main(void)
{
	discovery();
	query();
	bos();
	vms();
	sync_and_submit();
	sgxfive_mock_destroy(mock);
	return tap_done();
}
