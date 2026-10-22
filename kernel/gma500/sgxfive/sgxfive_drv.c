// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The sgxfive render-node interface (include/uapi/drm/sgxfive_drm.h): device
 * queries and buffer objects. Contexts, address spaces and job submission
 * follow (design/sgx545_render_uapi.md §8, phases P4 and P5).
 */
#include <linux/minmax.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_gem_shmem_helper.h>
#include <drm/sgxfive_drm.h>

#include "sgx_render.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

/*
 * The heaps of a client VM: the DDK's SGX545 REV1 table (sgxconfig.h). The
 * shader heaps are reported as the windows their code bases reach: ten and
 * four 512 KiB USE_CODE_BASE windows. The kernel heaps are reserved: a VM
 * holds the firmware's alias there only once it has a render context. The
 * 3D-parameter and TA-data heaps hold the render contexts' kernel-owned
 * slots (design/sgx545_render_uapi.md §11.3), reserved as well.
 */
const struct drm_sgxfive_heap sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_COUNT] = {
	[DRM_SGXFIVE_HEAP_GENERAL] = { 0x3e000000, 0xb2000000 - 0x1000, 0, 12 },
	[DRM_SGXFIVE_HEAP_TEXTURE] = { 0xf0000000, 0x10000000 - 0x1000, 0, 12 },
	[DRM_SGXFIVE_HEAP_PDS_PIXEL] = { 0x2e000000, 0x02000000 - 0x1000, 0, 12 },
	[DRM_SGXFIVE_HEAP_PDS_VERTEX] = { 0x30400000, 0x01c00000 - 0x1000, 0, 12 },
	[DRM_SGXFIVE_HEAP_USSE_PIXEL] = { 0x37000000, 10 * 0x80000,
					  DRM_SGXFIVE_HEAP_FIXED_WINDOWS, 12 },
	[DRM_SGXFIVE_HEAP_USSE_VERTEX] = { 0x3c000000, 4 * 0x80000,
					   DRM_SGXFIVE_HEAP_FIXED_WINDOWS, 12 },
	[DRM_SGXFIVE_HEAP_KERNEL_CODE] = { 0x30000000, 0x00080000 - 0x1000,
					   DRM_SGXFIVE_HEAP_RESERVED, 12 },
	[DRM_SGXFIVE_HEAP_KERNEL_DATA] = { 0x32000000, 0x05000000 - 0x1000,
					   DRM_SGXFIVE_HEAP_RESERVED, 12 },
	[DRM_SGXFIVE_HEAP_PARAMETERS] = { 0x10000000, 0x10000000,
					  DRM_SGXFIVE_HEAP_RESERVED, 12 },
	[DRM_SGXFIVE_HEAP_TA_DATA] = { 0x20000000, 0x0d000000,
				       DRM_SGXFIVE_HEAP_RESERVED, 12 },
};

/*
 * Copy a query result out: with a NULL pointer report the size; otherwise
 * copy what both sides know and report that size.
 */
static int sgxfive_query_out(struct drm_sgxfive_dev_query_args *a,
			     const void *obj, u32 size)
{
	u32 n = min(a->size, size);

	if (!a->pointer) {
		a->size = size;
		return 0;
	}
	if (copy_to_user(u64_to_user_ptr(a->pointer), obj, n))
		return -EFAULT;
	a->size = n;
	return 0;
}

static int sgxfive_query_heaps(struct drm_sgxfive_dev_query_args *a)
{
	struct drm_sgxfive_dev_query_heap_info info = { 0 };
	u32 i, n, stride;
	u8 __user *dst;

	if (a->pointer && copy_from_user(&info, u64_to_user_ptr(a->pointer),
					 min_t(u32, a->size, sizeof(info))))
		return -EFAULT;
	if (!a->pointer || !info.heaps.array) {
		info.heaps.count = DRM_SGXFIVE_HEAP_COUNT;
		info.heaps.stride = sizeof(struct drm_sgxfive_heap);
		return sgxfive_query_out(a, &info, sizeof(info));
	}
	n = min_t(u32, info.heaps.count, DRM_SGXFIVE_HEAP_COUNT);
	stride = info.heaps.stride;
	dst = u64_to_user_ptr(info.heaps.array);
	for (i = 0; i < n; i++, dst += stride) {
		u32 known = min_t(u32, stride, sizeof(sgxfive_rev1_heaps[i]));

		if (copy_to_user(dst, &sgxfive_rev1_heaps[i], known) ||
		    clear_user(dst + known, stride - known))
			return -EFAULT;
	}
	info.heaps.count = n;
	return sgxfive_query_out(a, &info, sizeof(info));
}

static int sgxfive_ioctl_dev_query(struct sgxfive *sgx, void *data)
{
	struct drm_sgxfive_dev_query_args *a = data;

	switch (a->type) {
	case DRM_SGXFIVE_DEV_QUERY_GPU_INFO: {
		struct drm_sgxfive_dev_query_gpu_info g = {
			.core_id = sgx->core_id,
			.core_revision = sgx->core_revision,
			.host_bridge_rev = sgx->host_bridge_rev,
		};

		if (sgx->host_bridge_rev < 4)
			g.quirks |= DRM_SGXFIVE_QUIRK_SAMPLE_CACHE;
		if (sgx->sw_valid) {
			g.core_id_sw = sgx->core_id_sw;
			g.core_revision_sw = sgx->core_revision_sw;
			g.flags |= DRM_SGXFIVE_GPU_INFO_SW_VALID;
		}
		return sgxfive_query_out(a, &g, sizeof(g));
	}
	case DRM_SGXFIVE_DEV_QUERY_FW_INFO: {
		struct drm_sgxfive_dev_query_fw_info f = { 0 };

		mutex_lock(&sgx->lock);
		if (sgx->loaded) {
			f.ddk_version = sgx->ddk_version;
			f.ddk_build = sgx->fw.ddk_build;
			f.build_options = sgx->fw.build_options;
			f.fw_format = sgx->fw.version;
			f.state = sgx->live ? DRM_SGXFIVE_FW_STATE_ON :
					      DRM_SGXFIVE_FW_STATE_OFF;
		}
		mutex_unlock(&sgx->lock);
		return sgxfive_query_out(a, &f, sizeof(f));
	}
	case DRM_SGXFIVE_DEV_QUERY_HEAP_INFO:
		return sgxfive_query_heaps(a);
	case DRM_SGXFIVE_DEV_QUERY_JOB_INFO: {
		struct drm_sgxfive_dev_query_job_info j = {
			.job_types = DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT |
				     DRM_SGXFIVE_JOB_TYPE_RENDER_BIT,
			.max_jobs = 16,
			.max_sync_ops = 16,
			.transfer_cmd_size = DRM_SGXFIVE_TRANSFER_CMD_SIZE_V1,
			.max_updates = 0,
			.render_cmd_size = DRM_SGXFIVE_RENDER_CMD_SIZE_V1,
			.render_pb_pages =
				sgx_render_pb_pages(READ_ONCE(sgx->render_pb_pages)),
			.render_max_width = SGX_RENDER_MAX_W,
			.render_max_height = SGX_RENDER_MAX_H,
		};

		return sgxfive_query_out(a, &j, sizeof(j));
	}
	case DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO: {
		struct drm_sgxfive_dev_query_runtime_info r = {
			.num_pixel_partitions = 31, .tile_width = 32,
			.tile_height = 16,
		};

		return sgxfive_query_out(a, &r, sizeof(r));
	}
	default:
		return -EINVAL;
	}
}

/* Buffer objects: shmem GEM objects, mapped write-combined on the CPU. */
struct sgxfive_bo {
	struct drm_gem_shmem_object base;
	u64 flags;			/* DRM_SGXFIVE_BO_* */
};

static const struct drm_gem_object_funcs sgxfive_bo_funcs = {
	.free = drm_gem_shmem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

/* The host driver's gem_create_object hook: used by drm_gem_shmem_create(). */
struct drm_gem_object *sgxfive_gem_create_object(struct drm_device *drm,
						 size_t size)
{
	struct sgxfive_bo *bo = kzalloc_obj(*bo);

	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.map_wc = true;
	bo->base.base.funcs = &sgxfive_bo_funcs;
	return &bo->base.base;
}

static struct sgxfive_bo *to_sgxfive_bo(struct drm_gem_object *obj)
{
	return obj->funcs == &sgxfive_bo_funcs ?
	       container_of(to_drm_gem_shmem_obj(obj), struct sgxfive_bo, base) :
	       NULL;
}

bool sgxfive_is_bo(struct drm_gem_object *obj)
{
	return to_sgxfive_bo(obj);
}

static int sgxfive_ioctl_create_bo(struct sgxfive *sgx, void *data,
				   struct drm_file *file)
{
	struct drm_sgxfive_create_bo_args *a = data;
	struct drm_gem_shmem_object *shmem;
	struct sgxfive_bo *bo;
	int ret;

	if (!a->size || a->size > SZ_1G || a->_padding_c ||
	    (a->flags & ~DRM_SGXFIVE_BO_FLAGS_MASK))
		return -EINVAL;
	shmem = drm_gem_shmem_create(sgx->dev, PAGE_ALIGN(a->size));
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	/*
	 * Lowmem pages only. A write-combined shmem BO has its pages' kernel
	 * mapping set WC when they are first pinned, and set_pages_array_wc()
	 * cannot handle HIGHMEM pages, which have no permanent kernel mapping
	 * (seen on 32-bit Cedarview with 2 GiB: a NULL dereference in
	 * __cpa_process_fault on the first mmap). Lowmem is also below 4 GiB,
	 * which the SGX MMU needs.
	 */
	mapping_set_gfp_mask(shmem->base.filp->f_mapping, GFP_USER);
	bo = to_sgxfive_bo(&shmem->base);
	bo->flags = a->flags;
	a->size = shmem->base.size;
	ret = drm_gem_handle_create(file, &shmem->base, &a->handle);
	drm_gem_object_put(&shmem->base);	/* the handle holds it now */
	return ret;
}

static int sgxfive_ioctl_get_bo_mmap_offset(void *data, struct drm_file *file)
{
	struct drm_sgxfive_get_bo_mmap_offset_args *a = data;
	struct drm_gem_object *obj;
	struct sgxfive_bo *bo;
	int ret;

	if (a->_padding_4)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, a->handle);
	if (!obj)
		return -ENOENT;
	bo = to_sgxfive_bo(obj);
	ret = bo && (bo->flags & DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS) ?
	      drm_gem_create_mmap_offset(obj) : -EINVAL;
	if (!ret)
		a->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return ret;
}

/*
 * Client programs read every SGX register and stale temporaries (design §6.3),
 * so with the render node off nothing may allocate, map or run on the GPU:
 * only DEV_QUERY, which reports fixed device facts.
 */
bool sgxfive_ioctl_allowed(bool render, unsigned int nr)
{
	return render || nr == DRM_SGXFIVE_DEV_QUERY;
}

int sgxfive_ioctl(struct sgxfive *sgx, unsigned int nr, void *data,
		  struct drm_file *file)
{
	switch (nr) {
	case DRM_SGXFIVE_DEV_QUERY:
		return sgxfive_ioctl_dev_query(sgx, data);
	case DRM_SGXFIVE_CREATE_BO:
		return sgxfive_ioctl_create_bo(sgx, data, file);
	case DRM_SGXFIVE_GET_BO_MMAP_OFFSET:
		return sgxfive_ioctl_get_bo_mmap_offset(data, file);
	case DRM_SGXFIVE_CREATE_VM_CONTEXT:
		return sgxfive_ioctl_create_vm(sgx, data, file);
	case DRM_SGXFIVE_DESTROY_VM_CONTEXT:
		return sgxfive_ioctl_destroy_vm(sgx, data, file);
	case DRM_SGXFIVE_VM_MAP:
		return sgxfive_ioctl_vm_map(sgx, data, file);
	case DRM_SGXFIVE_VM_UNMAP:
		return sgxfive_ioctl_vm_unmap(sgx, data, file);
	case DRM_SGXFIVE_CREATE_CONTEXT:
		return sgxfive_ioctl_create_ctx(sgx, data, file);
	case DRM_SGXFIVE_DESTROY_CONTEXT:
		return sgxfive_ioctl_destroy_ctx(sgx, data, file);
	case DRM_SGXFIVE_SUBMIT_JOBS:
		return sgxfive_ioctl_submit(sgx, data, file);
	default:
		return -EINVAL;
	}
}
