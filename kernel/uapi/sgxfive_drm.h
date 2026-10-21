/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/*
 * include/uapi/drm/sgxfive_drm.h - the sgxfive render-node interface for the
 * PowerVR SGX545 in Intel Cedarview (design/sgx545_render_uapi.md).
 *
 * Draft: not a stable interface until it is merged upstream together with its
 * open-source userspace.
 */
#ifndef SGXFIVE_DRM_UAPI_H
#define SGXFIVE_DRM_UAPI_H

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: sgxfive UAPI
 *
 * The sgxfive render node gives an unprivileged process GEM buffer objects,
 * its own GPU virtual address spaces (VM contexts), transfer contexts and job
 * submission with explicit synchronisation through DRM syncobjs. The kernel
 * owns the SGX firmware: no ioctl takes or returns a physical address, a
 * firmware address or an address inside the kernel's alias region.
 *
 * Rules for every argument struct, as in the PowerVR Rogue UAPI:
 *
 *  - All members are naturally aligned and the struct is padded to a multiple
 *    of 8 bytes, so i686 and x86-64 share one layout. Padding is explicit,
 *    named ``_padding_<hex offset>``, and must be zero; this is always checked.
 *  - Every ``flags`` field has a ``*_FLAGS_MASK``. Bits outside the mask are
 *    reserved and rejected with -EINVAL.
 *  - Structs reached through &struct drm_sgxfive_obj_array are versioned by
 *    their stride: a shorter (older) element is zero-extended, a longer (newer)
 *    one is accepted only if its unknown tail is zero.
 *  - GPU virtual addresses are carried as __u64 and must be below 4 GiB.
 */

/**
 * struct drm_sgxfive_obj_array - An array of objects passed by pointer.
 */
struct drm_sgxfive_obj_array {
	/** @stride: Size of one element as userspace knows it. Used for versioning. */
	__u32 stride;

	/** @count: Number of elements in the array. */
	__u32 count;

	/** @array: User pointer to the first element. */
	__u64 array;
};

/**
 * DRM_SGXFIVE_OBJ_ARRAY() - Literal of type &struct drm_sgxfive_obj_array.
 * @cnt: Number of elements pointed to by @ptr.
 * @ptr: Pointer to the start of a C array.
 */
#define DRM_SGXFIVE_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

/**
 * DOC: sgxfive IOCTL numbers
 *
 * 0x0a to 0x0f are reserved for the render objects (parameter buffers, render
 * target data sets) that TA/3D jobs will need.
 */
#define DRM_SGXFIVE_DEV_QUERY			0x00
#define DRM_SGXFIVE_CREATE_BO			0x01
#define DRM_SGXFIVE_GET_BO_MMAP_OFFSET		0x02
#define DRM_SGXFIVE_CREATE_VM_CONTEXT		0x03
#define DRM_SGXFIVE_DESTROY_VM_CONTEXT		0x04
#define DRM_SGXFIVE_VM_MAP			0x05
#define DRM_SGXFIVE_VM_UNMAP			0x06
#define DRM_SGXFIVE_CREATE_CONTEXT		0x07
#define DRM_SGXFIVE_DESTROY_CONTEXT		0x08
#define DRM_SGXFIVE_SUBMIT_JOBS			0x09

/*
 * SGXFIVE_IOCTL() - Build an sgxfive IOCTL number from its index, one of
 * DRM_IOR/DRM_IOW/DRM_IOWR and the args struct name without its
 * ``drm_sgxfive_`` prefix and ``_args`` suffix. Not for direct use.
 */
#define SGXFIVE_IOCTL(_ioctl, _mode, _data) \
	_mode(DRM_COMMAND_BASE + DRM_SGXFIVE_##_ioctl, struct drm_sgxfive_##_data##_args)

#define DRM_IOCTL_SGXFIVE_DEV_QUERY \
	SGXFIVE_IOCTL(DEV_QUERY, DRM_IOWR, dev_query)
#define DRM_IOCTL_SGXFIVE_CREATE_BO \
	SGXFIVE_IOCTL(CREATE_BO, DRM_IOWR, create_bo)
#define DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET \
	SGXFIVE_IOCTL(GET_BO_MMAP_OFFSET, DRM_IOWR, get_bo_mmap_offset)
#define DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT \
	SGXFIVE_IOCTL(CREATE_VM_CONTEXT, DRM_IOWR, create_vm_context)
#define DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT \
	SGXFIVE_IOCTL(DESTROY_VM_CONTEXT, DRM_IOW, destroy_vm_context)
#define DRM_IOCTL_SGXFIVE_VM_MAP \
	SGXFIVE_IOCTL(VM_MAP, DRM_IOW, vm_map)
#define DRM_IOCTL_SGXFIVE_VM_UNMAP \
	SGXFIVE_IOCTL(VM_UNMAP, DRM_IOW, vm_unmap)
#define DRM_IOCTL_SGXFIVE_CREATE_CONTEXT \
	SGXFIVE_IOCTL(CREATE_CONTEXT, DRM_IOWR, create_context)
#define DRM_IOCTL_SGXFIVE_DESTROY_CONTEXT \
	SGXFIVE_IOCTL(DESTROY_CONTEXT, DRM_IOW, destroy_context)
#define DRM_IOCTL_SGXFIVE_SUBMIT_JOBS \
	SGXFIVE_IOCTL(SUBMIT_JOBS, DRM_IOWR, submit_jobs)

/**
 * DOC: sgxfive DEV_QUERY
 *
 * One extensible query ioctl. &struct drm_sgxfive_dev_query_args.type names
 * the output struct. With a NULL pointer only the size the kernel knows is
 * returned; a smaller size gets a truncated copy; a larger one gets the known
 * struct and the size is set to what was written. Arrays inside query structs
 * follow the same rule through their &struct drm_sgxfive_obj_array.
 */

/**
 * enum drm_sgxfive_dev_query - Values for &drm_sgxfive_dev_query_args.type.
 *
 * Append only.
 */
enum drm_sgxfive_dev_query {
	/** @DRM_SGXFIVE_DEV_QUERY_GPU_INFO: &struct drm_sgxfive_dev_query_gpu_info. */
	DRM_SGXFIVE_DEV_QUERY_GPU_INFO = 0,
	/** @DRM_SGXFIVE_DEV_QUERY_FW_INFO: &struct drm_sgxfive_dev_query_fw_info. */
	DRM_SGXFIVE_DEV_QUERY_FW_INFO,
	/** @DRM_SGXFIVE_DEV_QUERY_HEAP_INFO: &struct drm_sgxfive_dev_query_heap_info. */
	DRM_SGXFIVE_DEV_QUERY_HEAP_INFO,
	/** @DRM_SGXFIVE_DEV_QUERY_JOB_INFO: &struct drm_sgxfive_dev_query_job_info. */
	DRM_SGXFIVE_DEV_QUERY_JOB_INFO,
	/** @DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO: &struct drm_sgxfive_dev_query_runtime_info. */
	DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO,
};

/*
 * DRM_SGXFIVE_QUIRK_SAMPLE_CACHE: the host bridge is below revision 4
 * (FIX_HW_BRN_SAMPLE_CACHE). Textures must be in the TEXTURE heap, and the
 * other heaps use the REV1 layout.
 */
#define DRM_SGXFIVE_QUIRK_SAMPLE_CACHE		(1ULL << 0)
#define DRM_SGXFIVE_QUIRKS_MASK			DRM_SGXFIVE_QUIRK_SAMPLE_CACHE

/* The firmware has booted since probe: core_id_sw and core_revision_sw are valid. */
#define DRM_SGXFIVE_GPU_INFO_SW_VALID		(1U << 0)

/**
 * struct drm_sgxfive_dev_query_gpu_info - The GPU core.
 *
 * Answered from registers read at probe and the firmware's last GETMISCINFO
 * reply; the query never boots the firmware.
 */
struct drm_sgxfive_dev_query_gpu_info {
	/** @core_id: EUR_CR_CORE_ID, 0x01150000 on SGX545. */
	__u32 core_id;

	/** @core_revision: EUR_CR_CORE_REVISION, e.g. 0x0001000d. */
	__u32 core_revision;

	/** @core_id_sw: The firmware's core id (545), if %DRM_SGXFIVE_GPU_INFO_SW_VALID. */
	__u32 core_id_sw;

	/** @core_revision_sw: The firmware's core revision, as @core_id_sw. */
	__u32 core_revision_sw;

	/** @quirks: ``DRM_SGXFIVE_QUIRK_*`` bits. */
	__u64 quirks;

	/** @host_bridge_rev: PCI revision of 00:00.0, which selects the quirks. */
	__u32 host_bridge_rev;

	/** @flags: ``DRM_SGXFIVE_GPU_INFO_*`` bits. */
	__u32 flags;
};

/**
 * enum drm_sgxfive_fw_state - Values for &drm_sgxfive_dev_query_fw_info.state.
 */
enum drm_sgxfive_fw_state {
	/** @DRM_SGXFIVE_FW_STATE_OFF: Not booted (lazy boot) or powered off. */
	DRM_SGXFIVE_FW_STATE_OFF = 0,
	/** @DRM_SGXFIVE_FW_STATE_ON: Booted and accepting commands. */
	DRM_SGXFIVE_FW_STATE_ON,
	/** @DRM_SGXFIVE_FW_STATE_IDLE: Idle with clocks gated; the next job wakes it. */
	DRM_SGXFIVE_FW_STATE_IDLE,
};

/**
 * struct drm_sgxfive_dev_query_fw_info - The firmware the kernel loaded.
 *
 * Userspace may refuse a firmware build it does not know.
 */
struct drm_sgxfive_dev_query_fw_info {
	/** @ddk_version: major << 16 | minor << 8 | branch, 0x00010711 for 1.7.17. */
	__u32 ddk_version;

	/** @ddk_build: DDK build number, 862890. */
	__u32 ddk_build;

	/** @build_options: The firmware's build options word, 0x60218. */
	__u32 build_options;

	/** @fw_format: format_version of sgx545_ukernel.fw. */
	__u32 fw_format;

	/** @state: &enum drm_sgxfive_fw_state, informational only. */
	__u32 state;

	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;
};

/**
 * enum drm_sgxfive_heap_id - Index into the HEAP_INFO array.
 *
 * Every id is always reported; a heap absent on this device has size 0.
 * Userspace never hardcodes heap addresses: the layout depends on the
 * silicon (the REV1 layout under %DRM_SGXFIVE_QUIRK_SAMPLE_CACHE).
 */
enum drm_sgxfive_heap_id {
	/** @DRM_SGXFIVE_HEAP_GENERAL: Surfaces, background objects, streams. */
	DRM_SGXFIVE_HEAP_GENERAL = 0,
	/** @DRM_SGXFIVE_HEAP_TEXTURE: Textures; size 0 unless QUIRK_SAMPLE_CACHE. */
	DRM_SGXFIVE_HEAP_TEXTURE,
	/** @DRM_SGXFIVE_HEAP_PDS_PIXEL: Pixel PDS code and data (PDS_EXEC_BASE). */
	DRM_SGXFIVE_HEAP_PDS_PIXEL,
	/** @DRM_SGXFIVE_HEAP_PDS_VERTEX: Vertex PDS code and data (TA work, later). */
	DRM_SGXFIVE_HEAP_PDS_VERTEX,
	/** @DRM_SGXFIVE_HEAP_USSE_PIXEL: Pixel USSE code, USE_CODE_BASE_0..9 windows. */
	DRM_SGXFIVE_HEAP_USSE_PIXEL,
	/** @DRM_SGXFIVE_HEAP_USSE_VERTEX: Vertex USSE code, USE_CODE_BASE_10..13. */
	DRM_SGXFIVE_HEAP_USSE_VERTEX,
	/** @DRM_SGXFIVE_HEAP_KERNEL_CODE: Kernel alias of firmware code; reserved. */
	DRM_SGXFIVE_HEAP_KERNEL_CODE,
	/** @DRM_SGXFIVE_HEAP_KERNEL_DATA: Kernel alias of firmware data; reserved. */
	DRM_SGXFIVE_HEAP_KERNEL_DATA,
	/**
	 * @DRM_SGXFIVE_HEAP_PARAMETERS: Render contexts' parameter buffers and
	 * background objects, one slot per render context; reserved.
	 */
	DRM_SGXFIVE_HEAP_PARAMETERS,
	/** @DRM_SGXFIVE_HEAP_TA_DATA: Render contexts' tiling data; reserved. */
	DRM_SGXFIVE_HEAP_TA_DATA,
	/** @DRM_SGXFIVE_HEAP_COUNT: Number of ids known to this header. */
	DRM_SGXFIVE_HEAP_COUNT,
	/* Add new heaps above DRM_SGXFIVE_HEAP_COUNT only. */
};

/* The heap is the kernel's: VM_MAP into it fails with -EINVAL. */
#define DRM_SGXFIVE_HEAP_RESERVED		(1U << 0)
/*
 * The heap base is a USE code base: a DOUTU reaches code within the 512 KiB
 * window selected by its CBASE field, counted from the heap base.
 */
#define DRM_SGXFIVE_HEAP_FIXED_WINDOWS		(1U << 1)
#define DRM_SGXFIVE_HEAP_FLAGS_MASK \
	(DRM_SGXFIVE_HEAP_RESERVED | DRM_SGXFIVE_HEAP_FIXED_WINDOWS)

/**
 * struct drm_sgxfive_heap - One GPU virtual address range.
 */
struct drm_sgxfive_heap {
	/** @base: First GPU virtual address of the heap. */
	__u64 base;

	/** @size: Size in bytes; 0 if the heap is absent on this device. */
	__u64 size;

	/** @flags: ``DRM_SGXFIVE_HEAP_*`` bits. */
	__u32 flags;

	/** @page_size_log2: Log2 of the GPU page size, 12. */
	__u32 page_size_log2;
};

/**
 * struct drm_sgxfive_dev_query_heap_info - The heaps of every VM context.
 */
struct drm_sgxfive_dev_query_heap_info {
	/**
	 * @heaps: Array of &struct drm_sgxfive_heap indexed by
	 * &enum drm_sgxfive_heap_id. With a NULL array the kernel sets count and
	 * stride to what it knows.
	 */
	struct drm_sgxfive_obj_array heaps;
};

/* Supported job types, as bits of &drm_sgxfive_dev_query_job_info.job_types. */
#define DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT	(1ULL << 0)
#define DRM_SGXFIVE_JOB_TYPE_RENDER_BIT		(1ULL << 1)

/**
 * struct drm_sgxfive_dev_query_job_info - Limits of SUBMIT_JOBS.
 */
struct drm_sgxfive_dev_query_job_info {
	/** @job_types: ``DRM_SGXFIVE_JOB_TYPE_*_BIT`` bits. */
	__u64 job_types;

	/** @max_jobs: Maximum jobs per SUBMIT_JOBS call. */
	__u32 max_jobs;

	/** @max_sync_ops: Maximum sync operations per job. */
	__u32 max_sync_ops;

	/** @transfer_cmd_size: Largest &struct drm_sgxfive_transfer_cmd known. */
	__u32 transfer_cmd_size;

	/**
	 * @max_updates: Maximum entries in &drm_sgxfive_job.updates. 0: memory
	 * updates are not supported and any update list is rejected.
	 */
	__u32 max_updates;

	/** @render_cmd_size: Largest &struct drm_sgxfive_render_cmd known. */
	__u32 render_cmd_size;

	/** @render_pb_pages: 4 KiB pages of each render context's parameter buffer. */
	__u32 render_pb_pages;

	/** @render_max_width: Widest render target (MTE_SCREEN) in pixels. */
	__u32 render_max_width;

	/** @render_max_height: Tallest render target (MTE_SCREEN) in pixels. */
	__u32 render_max_height;
};

/**
 * struct drm_sgxfive_dev_query_runtime_info - Fixed rendering parameters.
 */
struct drm_sgxfive_dev_query_runtime_info {
	/** @num_pixel_partitions: Pixel partitions the kernel programs, 31. */
	__u32 num_pixel_partitions;

	/** @tile_width: Tile width in pixels, 32. */
	__u32 tile_width;

	/** @tile_height: Tile height in pixels, 16. */
	__u32 tile_height;

	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
};

/**
 * struct drm_sgxfive_dev_query_args - Arguments for %DRM_IOCTL_SGXFIVE_DEV_QUERY.
 */
struct drm_sgxfive_dev_query_args {
	/** @type: &enum drm_sgxfive_dev_query. */
	__u32 type;

	/** @size: [IN] Size of the buffer at @pointer. [OUT] Bytes written or needed. */
	__u32 size;

	/** @pointer: User pointer to the output struct; NULL to query its size. */
	__u64 pointer;
};

/**
 * DOC: sgxfive CREATE_BO and GET_BO_MMAP_OFFSET
 *
 * Buffer objects are shmem GEM objects with pages below 4 GiB. GPU access
 * attributes are per mapping (VM_MAP), not per object. A BO can be mmap()ed
 * on the render node, write-combined, only if it was created with
 * %DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS. Handles are released with the core
 * %DRM_IOCTL_GEM_CLOSE.
 */
#define DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS		(1ULL << 0)
/* Reserved until PTE cache coherency is understood (design §7 Q5): -EINVAL. */
#define DRM_SGXFIVE_BO_CPU_CACHED		(1ULL << 1)
#define DRM_SGXFIVE_BO_FLAGS_MASK		DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS

/**
 * struct drm_sgxfive_create_bo_args - Arguments for %DRM_IOCTL_SGXFIVE_CREATE_BO.
 */
struct drm_sgxfive_create_bo_args {
	/** @size: [IN/OUT] Size in bytes, non-zero; on return, rounded up to 4 KiB. */
	__u64 size;

	/** @handle: [OUT] GEM handle. */
	__u32 handle;

	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;

	/** @flags: [IN] ``DRM_SGXFIVE_BO_*`` bits within %DRM_SGXFIVE_BO_FLAGS_MASK. */
	__u64 flags;
};

/**
 * struct drm_sgxfive_get_bo_mmap_offset_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET.
 */
struct drm_sgxfive_get_bo_mmap_offset_args {
	/** @handle: [IN] GEM handle of a CPU-accessible BO; -ENOENT if there is none. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;

	/** @offset: [OUT] Fake offset for mmap() on the render node. */
	__u64 offset;
};

/**
 * DOC: sgxfive VM contexts, VM_MAP and VM_UNMAP
 *
 * A VM context is one GPU address space (one SGX page directory). The client
 * manages it: VM_MAP places a range of a BO at an address of its choosing,
 * 4 KiB aligned and inside one heap that is not %DRM_SGXFIVE_HEAP_RESERVED.
 * Overlapping an existing mapping fails with -EEXIST. The mapping is in the
 * page tables when the ioctl returns. VM_UNMAP removes exactly one whole
 * mapping (-ENOENT otherwise); its pages stay pinned until the GPU can no
 * longer reach them.
 */

/**
 * struct drm_sgxfive_create_vm_context_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT.
 */
struct drm_sgxfive_create_vm_context_args {
	/** @handle: [OUT] VM context handle. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/**
 * struct drm_sgxfive_destroy_vm_context_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT.
 */
struct drm_sgxfive_destroy_vm_context_args {
	/** @handle: [IN] VM context handle. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/* The GPU may only read the mapping (SGX PTE READONLY); writes fault. */
#define DRM_SGXFIVE_VM_MAP_READ_ONLY		(1U << 0)
#define DRM_SGXFIVE_VM_MAP_FLAGS_MASK		DRM_SGXFIVE_VM_MAP_READ_ONLY

/**
 * struct drm_sgxfive_vm_map_args - Arguments for %DRM_IOCTL_SGXFIVE_VM_MAP.
 */
struct drm_sgxfive_vm_map_args {
	/** @vm_context_handle: [IN] VM context. */
	__u32 vm_context_handle;

	/** @flags: [IN] ``DRM_SGXFIVE_VM_MAP_*`` bits. */
	__u32 flags;

	/** @device_addr: [IN] GPU address: 4 KiB aligned, non-zero, in one user heap. */
	__u64 device_addr;

	/** @handle: [IN] GEM handle of the BO; -ENOENT if there is none. */
	__u32 handle;

	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;

	/** @offset: [IN] Offset into the BO, 4 KiB aligned. */
	__u64 offset;

	/** @size: [IN] Non-zero multiple of 4 KiB, within the BO and the heap. */
	__u64 size;
};

/**
 * struct drm_sgxfive_vm_unmap_args - Arguments for %DRM_IOCTL_SGXFIVE_VM_UNMAP.
 */
struct drm_sgxfive_vm_unmap_args {
	/** @vm_context_handle: [IN] VM context. */
	__u32 vm_context_handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;

	/** @device_addr: [IN] Start of the mapping. */
	__u64 device_addr;

	/** @size: [IN] Size of the mapping; must match it exactly. */
	__u64 size;
};

/**
 * DOC: sgxfive CREATE_CONTEXT and DESTROY_CONTEXT
 *
 * A context is one firmware queue (an SGX HW context and its client CCB, all
 * kernel-owned) bound to one VM context for its lifetime. Contexts run their
 * jobs in submission order; the firmware arbitrates between contexts. A
 * context whose job faulted or hung is lost: its pending jobs complete with
 * an error and further submissions fail with -ECANCELED. After three such
 * jobs within a minute the whole file is banned: CREATE_CONTEXT and
 * SUBMIT_JOBS on it fail with -ECANCELED, while jobs it already queued on
 * contexts that are not lost still run.
 */

/**
 * enum drm_sgxfive_ctx_type - Values for &drm_sgxfive_create_context_args.type.
 */
enum drm_sgxfive_ctx_type {
	/** @DRM_SGXFIVE_CTX_TYPE_TRANSFER: Transfer (blit, fill, copy) jobs. */
	DRM_SGXFIVE_CTX_TYPE_TRANSFER = 0,
	/**
	 * @DRM_SGXFIVE_CTX_TYPE_RENDER: Render (TA + 3D) jobs. The kernel gives
	 * the context a parameter buffer and the tiling data of one render
	 * target, in reserved heaps of the VM, and runs one of its jobs at a
	 * time. At most 32 render contexts per VM context (-ENOSPC).
	 */
	DRM_SGXFIVE_CTX_TYPE_RENDER = 1,
};

/**
 * enum drm_sgxfive_ctx_priority - Values for &drm_sgxfive_create_context_args.priority.
 */
enum drm_sgxfive_ctx_priority {
	/** @DRM_SGXFIVE_CTX_PRIORITY_NORMAL: The only priority the firmware schedules. */
	DRM_SGXFIVE_CTX_PRIORITY_NORMAL = 0,
};

/**
 * struct drm_sgxfive_create_context_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_CREATE_CONTEXT.
 */
struct drm_sgxfive_create_context_args {
	/** @type: [IN] &enum drm_sgxfive_ctx_type. */
	__u32 type;

	/** @flags: [IN] None defined; zero. */
	__u32 flags;

	/** @priority: [IN] &enum drm_sgxfive_ctx_priority. */
	__s32 priority;

	/** @vm_context_handle: [IN] The VM context the jobs run in. */
	__u32 vm_context_handle;

	/** @handle: [OUT] Context handle. */
	__u32 handle;

	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;
};

/**
 * struct drm_sgxfive_destroy_context_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_DESTROY_CONTEXT.
 */
struct drm_sgxfive_destroy_context_args {
	/** @handle: [IN] Context handle. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/**
 * DOC: sgxfive SUBMIT_JOBS
 *
 * An array of jobs, validated together: on error no job of the call is
 * queued and &drm_sgxfive_submit_jobs_args.jobs.count is set to the index of
 * the failing job. Dependencies are explicit (sync ops on DRM syncobjs); there
 * is no BO list, since every mapped BO of the VM is resident.
 *
 * A job's out-fence signals once the firmware reports completion, after the
 * job's memory writes are visible to write-combined CPU mappings.
 */

/* Sync op encoding, as in pvr_drm.h. */
#define DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK		0xfU
#define DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ		0x0U
#define DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ	0x1U
#define DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL				(1U << 31)
#define DRM_SGXFIVE_SYNC_OP_FLAG_WAIT				0x0U
#define DRM_SGXFIVE_SYNC_OP_FLAGS_MASK \
	(DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK | DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL)

/**
 * struct drm_sgxfive_sync_op - One wait or signal operation.
 */
struct drm_sgxfive_sync_op {
	/** @handle: DRM syncobj handle. */
	__u32 handle;

	/** @flags: ``DRM_SGXFIVE_SYNC_OP_FLAG_*``; handle types above 1 are -EINVAL. */
	__u32 flags;

	/** @value: Timeline point; zero for a binary syncobj. */
	__u64 value;
};

/**
 * struct drm_sgxfive_mem_update - Reserved: one firmware memory update.
 *
 * The extension point for the firmware's per-command update list (updates):
 * a 32-bit value the firmware writes to a GPU address of the job's VM before
 * the job starts. No kernel accepts updates yet
 * (&drm_sgxfive_dev_query_job_info.max_updates is 0); the struct is defined
 * so that the layout of &drm_sgxfive_job does not change when one does. A
 * future kernel will require each target to lie in a writable mapping of the
 * job's VM.
 */
struct drm_sgxfive_mem_update {
	/** @device_addr: GPU address, 4-byte aligned. */
	__u64 device_addr;

	/** @value: Value to write. */
	__u32 value;

	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
};

/**
 * enum drm_sgxfive_job_type - Values for &drm_sgxfive_job.type.
 */
enum drm_sgxfive_job_type {
	/** @DRM_SGXFIVE_JOB_TYPE_TRANSFER: A transfer; @cmd_stream is a transfer cmd. */
	DRM_SGXFIVE_JOB_TYPE_TRANSFER = 0,
	/**
	 * @DRM_SGXFIVE_JOB_TYPE_RENDER: One whole scene, tiled and rendered;
	 * @cmd_stream is a &struct drm_sgxfive_render_cmd.
	 */
	DRM_SGXFIVE_JOB_TYPE_RENDER = 1,
};

/*
 * DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY: the firmware queues and completes the
 * job without rendering; signal-only. cmd_stream and cmd_stream_len must be 0.
 */
#define DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY		(1U << 0)
#define DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_FLAGS_MASK	DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY

/* Which optional register groups of the transfer command the firmware loads. */
#define DRM_SGXFIVE_TRANSFER_LOAD_YUV			(1U << 0)
#define DRM_SGXFIVE_TRANSFER_LOAD_LUMAKEY		(1U << 1)
#define DRM_SGXFIVE_TRANSFER_LOAD_FIR			(1U << 2)
#define DRM_SGXFIVE_TRANSFER_LOAD_FLAGS_MASK \
	(DRM_SGXFIVE_TRANSFER_LOAD_YUV | DRM_SGXFIVE_TRANSFER_LOAD_LUMAKEY | \
	 DRM_SGXFIVE_TRANSFER_LOAD_FIR)

/**
 * struct drm_sgxfive_transfer_cmd - The register words of one transfer.
 *
 * Each field is the value of one SGX register, with GPU addresses of the
 * job's VM encoded as that register wants them. The kernel builds the
 * firmware command from it and fills in what it owns (size, flags, fence
 * status, pixel partitions, fence id, updates). It checks that the addresses
 * that make the pixel pipe fetch or execute (@event_pixel_pds_exec,
 * @bif_3d_req_base, a non-zero @isp_rgn_base) lie in a user heap, that
 * @pds_exec_base is the PDS_PIXEL heap base, and masks the fields with global
 * side effects.
 *
 * Version 1 is 0xa8 bytes; a later, longer version keeps this prefix, and a
 * kernel accepts a longer struct than it knows only if the tail is zero.
 */
struct drm_sgxfive_transfer_cmd {
	/** @isp_bgobjtag: ISP_BGOBJTAG: VERTEXPTR (16-byte units from req base), TSPDATASIZE. */
	__u32 isp_bgobjtag;
	/** @isp_bgobj: ISP_BGOBJ. */
	__u32 isp_bgobj;
	/** @isp_bgobjdepth: ISP_BGOBJDEPTH (IEEE single). */
	__u32 isp_bgobjdepth;
	/** @isp_render: ISP_RENDER: render type and scan direction. */
	__u32 isp_render;
	/** @isp_rgn_base: ISP_RGN_BASE; 0 for FASTSCALE renders. */
	__u32 isp_rgn_base;
	/** @isp_ipfmisc: ISP_IPFMISC. */
	__u32 isp_ipfmisc;
	/** @bif_3d_req_base: BIF_3D_REQ_BASE, 1 MiB aligned. */
	__u32 bif_3d_req_base;
	/** @aa_mode_3d: 3D_AA_MODE. */
	__u32 aa_mode_3d;
	/** @isp_multisamplectl: ISP_MULTISAMPLECTL. */
	__u32 isp_multisamplectl;
	/** @event_pixel_pds_exec: EVENT_PIXEL_PDS_EXEC, 16-byte aligned address. */
	__u32 event_pixel_pds_exec;
	/** @event_pixel_pds_data: EVENT_PIXEL_PDS_DATA, data size in 16-byte rows. */
	__u32 event_pixel_pds_data;
	/** @event_pixel_pds_info: EVENT_PIXEL_PDS_INFO. */
	__u32 event_pixel_pds_info;
	/** @isp_render_box1: ISP_RENDBOX1, first tile. */
	__u32 isp_render_box1;
	/** @isp_render_box2: ISP_RENDBOX2, last tile. */
	__u32 isp_render_box2;
	/** @pixelbe: PIXELBE. */
	__u32 pixelbe;
	/**
	 * @pds_exec_base: PDS_EXEC_BASE: must be the base of the PDS_PIXEL
	 * heap, the value render jobs rely on (the 3D pipe never sets it).
	 */
	__u32 pds_exec_base;
	/** @double_pixel_partitions: DOUBLE_PIXEL_PARTITIONS; must be 0 for now. */
	__u32 double_pixel_partitions;
	/** @load_flags: ``DRM_SGXFIVE_TRANSFER_LOAD_*`` bits. */
	__u32 load_flags;
	/** @csc0_coeff: TPU_YUV00..04, with %DRM_SGXFIVE_TRANSFER_LOAD_YUV. */
	__u32 csc0_coeff[5];
	/** @csc1_coeff: TPU_YUV10..14, with %DRM_SGXFIVE_TRANSFER_LOAD_YUV. */
	__u32 csc1_coeff[5];
	/** @csc_scale: TPU_YUVSCALE, with %DRM_SGXFIVE_TRANSFER_LOAD_YUV. */
	__u32 csc_scale;
	/** @lumakey_coeff: TPU_LUMA0..2, with %DRM_SGXFIVE_TRANSFER_LOAD_LUMAKEY. */
	__u32 lumakey_coeff[3];
	/** @fir_htable: USE_FILTER_TABLE, with %DRM_SGXFIVE_TRANSFER_LOAD_FIR. */
	__u32 fir_htable;
	/** @fir_hfilter: USE_FILTER0/1/2 left/right/extra, with LOAD_FIR. */
	__u32 fir_hfilter[9];
};

#define DRM_SGXFIVE_TRANSFER_CMD_SIZE_V1	0xa8

/* No render job flags are defined yet. */
#define DRM_SGXFIVE_SUBMIT_JOB_RENDER_FLAGS_MASK	0U

/**
 * struct drm_sgxfive_render_cmd - The register words of one scene.
 *
 * One render job tiles the scene described by the VDM control stream and
 * renders it: the firmware runs the TA, then the 3D pipe over every tile,
 * and the job's fence signals once the pixel back end has finished. The
 * fields are SGX register values, with GPU addresses of the job's VM. The
 * kernel fills everything that points into memory it owns or has device-wide
 * effects: the parameter buffer and tiling data of the context, the
 * background object (built from @bgobj_words and the size in @mte_screen),
 * pixel and vertex partitions, the depth/stencil controls (no depth or
 * stencil buffer in version 1) and every firmware flag, sync and status word.
 *
 * Pixel PDS pointers (in the MTE state and @bgobj_words) are relative to
 * PDS_EXEC_BASE, which the kernel keeps at the base of the PDS_PIXEL heap for
 * every job, transfers included. The end-of-tile program, written by
 * userspace, holds the PBE state, so no colour buffer address appears here.
 *
 * The kernel checks that @vdm_ctrl_stream_base (4-byte aligned) and
 * @event_pixel_pds_exec (16-byte aligned) lie in a user heap, that the screen
 * sizes stay within &drm_sgxfive_dev_query_job_info.render_max_width and
 * render_max_height, and that @isp_bgobj sets no bit but 8 and 9.
 *
 * Version 1 is 0x68 bytes; a longer version keeps this prefix, and a kernel
 * accepts a longer struct than it knows only if the tail is zero.
 */
struct drm_sgxfive_render_cmd {
	/** @vdm_ctrl_stream_base: VDM_CTRL_STREAM_BASE: the scene's control stream. */
	__u32 vdm_ctrl_stream_base;
	/** @te_aa: TE_AA. */
	__u32 te_aa;
	/** @te_mtile1: TE_MTILE1. */
	__u32 te_mtile1;
	/** @te_mtile2: TE_MTILE2. */
	__u32 te_mtile2;
	/** @te_screen: TE_SCREEN: 32 x 16 tiles across - 1 (11:0), down - 1 (23:12). */
	__u32 te_screen;
	/** @te_mtile_stride: TE_MTILE. */
	__u32 te_mtile_stride;
	/** @mte_ctrl: MTE_CTRL. */
	__u32 mte_ctrl;
	/** @mte_wcompare: MTE_WCOMPARE (IEEE single). */
	__u32 mte_wcompare;
	/** @mte_wclamp: MTE_WCLAMP (IEEE single). */
	__u32 mte_wclamp;
	/** @mte_screen: MTE_SCREEN: width - 1 (15:0), height - 1 (31:16). */
	__u32 mte_screen;
	/** @mte_multisamplectl: MTE_MULTISAMPLECTL. */
	__u32 mte_multisamplectl;
	/** @pixelbe: PIXELBE. */
	__u32 pixelbe;
	/** @isp_ipfmisc: ISP_IPFMISC. */
	__u32 isp_ipfmisc;
	/** @aa_mode_3d: 3D_AA_MODE. */
	__u32 aa_mode_3d;
	/** @isp_bgobjdepth: ISP_BGOBJDEPTH: the clear depth (IEEE single). */
	__u32 isp_bgobjdepth;
	/** @isp_bgobj: ISP_BGOBJ: bits 8 and 9 only. */
	__u32 isp_bgobj;
	/** @isp_multisamplectl: ISP_MULTISAMPLECTL. */
	__u32 isp_multisamplectl;
	/** @tsp_config: TSP_CONFIG. */
	__u32 tsp_config;
	/** @event_pixel_pds_exec: EVENT_PIXEL_PDS_EXEC: the pixel-event program. */
	__u32 event_pixel_pds_exec;
	/** @event_pixel_pds_data: EVENT_PIXEL_PDS_DATA, data size in 16-byte rows. */
	__u32 event_pixel_pds_data;
	/** @event_pixel_pds_info: EVENT_PIXEL_PDS_INFO. */
	__u32 event_pixel_pds_info;
	/** @isp_fpuctrl: ISP_FPUCTRL. */
	__u32 isp_fpuctrl;
	/** @isp_depthsort: ISP_DEPTHSORT. */
	__u32 isp_depthsort;
	/**
	 * @bgobj_words: The background object's pixel PDS words: secondary
	 * pointer, size word, primary pointer (spec_object).
	 */
	__u32 bgobj_words[3];
};

#define DRM_SGXFIVE_RENDER_CMD_SIZE_V1		0x68

/**
 * struct drm_sgxfive_job - One job of %DRM_IOCTL_SGXFIVE_SUBMIT_JOBS.
 */
struct drm_sgxfive_job {
	/** @type: [IN] &enum drm_sgxfive_job_type. */
	__u32 type;

	/** @context_handle: [IN] A context of the matching type. */
	__u32 context_handle;

	/** @flags: [IN] Job-type specific, e.g. ``DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_*``. */
	__u32 flags;

	/** @cmd_stream_len: [IN] Bytes at @cmd_stream, at most one page. */
	__u32 cmd_stream_len;

	/** @cmd_stream: [IN] User pointer, 8-byte aligned, to the job-type command. */
	__u64 cmd_stream;

	/** @sync_ops: [IN] Array of &struct drm_sgxfive_sync_op. */
	struct drm_sgxfive_obj_array sync_ops;

	/**
	 * @updates: [IN] Reserved: array of &struct drm_sgxfive_mem_update.
	 * Must be all zero while &drm_sgxfive_dev_query_job_info.max_updates
	 * is 0; a non-zero count, stride or pointer is -EINVAL.
	 */
	struct drm_sgxfive_obj_array updates;
};

/**
 * struct drm_sgxfive_submit_jobs_args - Arguments for %DRM_IOCTL_SGXFIVE_SUBMIT_JOBS.
 */
struct drm_sgxfive_submit_jobs_args {
	/** @jobs: [IN] Array of &struct drm_sgxfive_job. [OUT] On error, count = failing index. */
	struct drm_sgxfive_obj_array jobs;
};

#if defined(__cplusplus)
}
#endif

#endif /* SGXFIVE_DRM_UAPI_H */
