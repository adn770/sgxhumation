// SPDX-License-Identifier: MIT
/*
 * The in-process mock of the sgxfive render node (see sgxfive_mock.h). It
 * mirrors what the kernel side must do (design/sgx545_render_uapi.md §3-§5)
 * closely enough that the library's tests double as an executable statement
 * of the UAPI rules: zeroed padding, flag masks, the DEV_QUERY size rules,
 * heap-checked VM_MAP, the reserved update list, the §4.5 address checks and
 * lost contexts after a fault. Jobs run synchronously at submit time.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_mock.h"
#include "sgxfive_xfer.h"

#define MAX_OBJS	512
#define MAX_MAPS	64
#define MAX_JOBS	64
#define MAX_SYNC_OPS	64
#define MAX_BO_SIZE	(64u << 20)
#define FD_BASE		100
#define MMAP_SHIFT	20
#define RENDER_SLOTS	32	/* render contexts per VM */
#define RENDER_MAX_W	256	/* the kernel's render-target limit */
#define RENDER_MAX_H	128

struct mbo {
	int fd;
	uint32_t handle;
	uint64_t size, flags;
	uint8_t *mem;
	bool open;		/* the GEM handle is alive */
	uint32_t maps;		/* VM mappings referencing it */
};

struct mmap_ent {
	uint64_t addr, size, offset;
	struct mbo *bo;
	uint32_t flags;
};

struct mvm {
	int fd;
	uint32_t handle;
	bool open;
	uint32_t ctxs;
	uint32_t renders;	/* render contexts: at most 32 */
	struct mmap_ent maps[MAX_MAPS];
	uint32_t nmaps;
};

struct mctx {
	int fd;
	uint32_t handle;
	uint32_t type;		/* DRM_SGXFIVE_CTX_TYPE_* */
	struct mvm *vm;
	bool lost;
};

struct msync {
	int fd;
	uint32_t handle;
	bool fence;		/* a fence is attached (binary) */
	uint64_t point;		/* highest signalled timeline point */
};

struct mnode {
	const char *path, *driver;
	bool sgx;			/* answers the sgxfive ioctls */
};

struct sgxfive_mock {
	struct sgxfive_backend be;
	struct mnode nodes[2];
	uint32_t nnodes;
	const char *fd_driver[64];	/* by fd - FD_BASE; NULL: closed */
	bool fd_sgx[64];
	uint32_t fd_faults[64];		/* jobs of the file that faulted */
	struct drm_sgxfive_heap heaps[DRM_SGXFIVE_HEAP_COUNT];
	uint64_t quirks;
	uint32_t bridge_rev;
	struct mbo *bos[MAX_OBJS];
	struct mvm *vms[MAX_OBJS];
	struct mctx *ctxs[MAX_OBJS];
	struct msync *syncs[MAX_OBJS];
	uint32_t next_handle;
	struct sgxfive_mock_stats stats;
};

/* ---- Layouts (design §3.5, boot_sequence (d)) --------------------------------------- */

#define H(b, s, f) { .base = (b), .size = (s), .flags = (f), .page_size_log2 = 12 }
#define RSV DRM_SGXFIVE_HEAP_RESERVED
#define WIN DRM_SGXFIVE_HEAP_FIXED_WINDOWS

static const struct drm_sgxfive_heap rev1[DRM_SGXFIVE_HEAP_COUNT] = {
	[DRM_SGXFIVE_HEAP_GENERAL]	= H(0x3e000000, 0xb1fff000, 0),
	[DRM_SGXFIVE_HEAP_TEXTURE]	= H(0xf0000000, 0x0ffff000, 0),
	[DRM_SGXFIVE_HEAP_PDS_PIXEL]	= H(0x2e000000, 0x02000000, 0),
	[DRM_SGXFIVE_HEAP_PDS_VERTEX]	= H(0x30400000, 0x01c00000, 0),
	[DRM_SGXFIVE_HEAP_USSE_PIXEL]	= H(0x37000000, 0x00500000, WIN),
	[DRM_SGXFIVE_HEAP_USSE_VERTEX]	= H(0x3c000000, 0x00200000, WIN),
	[DRM_SGXFIVE_HEAP_KERNEL_CODE]	= H(0x30000000, 0x00400000, RSV),
	[DRM_SGXFIVE_HEAP_KERNEL_DATA]	= H(0x32000000, 0x05000000, RSV),
	[DRM_SGXFIVE_HEAP_PARAMETERS]	= H(0x10000000, 0x10000000, RSV),
	[DRM_SGXFIVE_HEAP_TA_DATA]	= H(0x20000000, 0x0d000000, RSV),
};

/* The harness layout has no room for render heaps: no render contexts there. */
static const struct drm_sgxfive_heap harness[DRM_SGXFIVE_HEAP_COUNT] = {
	[DRM_SGXFIVE_HEAP_GENERAL]	= H(0x00100000, 0xd0000000 - 0x100000, 0),
	[DRM_SGXFIVE_HEAP_TEXTURE]	= H(0, 0, 0),
	[DRM_SGXFIVE_HEAP_PDS_PIXEL]	= H(0xe4000000, 0x02000000, 0),
	[DRM_SGXFIVE_HEAP_PDS_VERTEX]	= H(0xec000000, 0x02000000, 0),
	[DRM_SGXFIVE_HEAP_USSE_PIXEL]	= H(0xf4000000, 0x00500000, WIN),
	[DRM_SGXFIVE_HEAP_USSE_VERTEX]	= H(0xfc000000, 0x00200000, WIN),
	[DRM_SGXFIVE_HEAP_KERNEL_CODE]	= H(0xe8000000, 0x04000000, RSV),
	[DRM_SGXFIVE_HEAP_KERNEL_DATA]	= H(0xf0000000, 0x04000000, RSV),
};

/* ---- Object tables ------------------------------------------------------------------ */

/* Every object starts with its owner and handle. */
struct objhdr {
	int fd;
	uint32_t handle;
};

static void *find(void *const *table, int fd, uint32_t handle)
{
	for (int i = 0; i < MAX_OBJS; i++) {
		const struct objhdr *o = table[i];

		if (o && o->fd == fd && o->handle == handle)
			return table[i];
	}
	return NULL;
}

#define FIND(table, f, h) find((void *const *)(table), (f), (h))

static int insert(void **table, void *obj)
{
	for (int i = 0; i < MAX_OBJS; i++) {
		if (!table[i]) {
			table[i] = obj;
			return 0;
		}
	}
	free(obj);
	return -ENOMEM;
}

static void drop(void **table, void *obj)
{
	for (int i = 0; i < MAX_OBJS; i++)
		if (table[i] == obj)
			table[i] = NULL;
}

static uint32_t count(void *const *table)
{
	uint32_t n = 0;

	for (int i = 0; i < MAX_OBJS; i++)
		n += table[i] != NULL;
	return n;
}

static void bo_put(struct sgxfive_mock *m, struct mbo *bo)
{
	if (bo->open || bo->maps)
		return;
	drop((void **)m->bos, bo);
	free(bo->mem);
	free(bo);
}

static void vm_put(struct sgxfive_mock *m, struct mvm *vm)
{
	if (vm->open || vm->ctxs)
		return;
	for (uint32_t i = 0; i < vm->nmaps; i++) {
		vm->maps[i].bo->maps--;
		bo_put(m, vm->maps[i].bo);
	}
	drop((void **)m->vms, vm);
	free(vm);
}

/* ---- GPU memory access through a VM ------------------------------------------------- */

static struct mmap_ent *lookup(struct mvm *vm, uint64_t addr)
{
	for (uint32_t i = 0; i < vm->nmaps; i++)
		if (addr >= vm->maps[i].addr && addr < vm->maps[i].addr + vm->maps[i].size)
			return &vm->maps[i];
	return NULL;
}

static uint8_t *gpu_ptr(struct mvm *vm, uint64_t addr, bool write)
{
	struct mmap_ent *e = lookup(vm, addr);

	if (!e || (write && e->flags & DRM_SGXFIVE_VM_MAP_READ_ONLY))
		return NULL;
	return e->bo->mem + e->offset + (addr - e->addr);
}

/* ---- Discovery and DEV_QUERY -------------------------------------------------------- */

static int m_version(struct sgxfive_mock *m, int fd, struct drm_version *v)
{
	const char *name = m->fd_driver[fd - FD_BASE];
	size_t len = strlen(name);

	if (v->name && v->name_len)
		memcpy(v->name, name, len < v->name_len ? len : v->name_len);
	v->name_len = len;
	return 0;
}

static int copy_out(struct drm_sgxfive_dev_query_args *q, const void *src, uint32_t size)
{
	if (q->pointer) {
		uint32_t n = q->size < size ? q->size : size;

		memcpy((void *)(uintptr_t)q->pointer, src, n);
		size = n;
	}
	q->size = size;
	return 0;
}

static int m_query(struct sgxfive_mock *m, struct drm_sgxfive_dev_query_args *q)
{
	switch (q->type) {
	case DRM_SGXFIVE_DEV_QUERY_GPU_INFO: {
		const struct drm_sgxfive_dev_query_gpu_info g = {
			.core_id = 0x01150000, .core_revision = 0x0001000d,
			.quirks = m->quirks, .host_bridge_rev = m->bridge_rev,
		};
		return copy_out(q, &g, sizeof(g));
	}
	case DRM_SGXFIVE_DEV_QUERY_FW_INFO: {
		const struct drm_sgxfive_dev_query_fw_info f = {
			.ddk_version = 0x00010711, .ddk_build = 862890, .build_options = 0x60218,
			.fw_format = 3, .state = DRM_SGXFIVE_FW_STATE_OFF,
		};
		return copy_out(q, &f, sizeof(f));
	}
	case DRM_SGXFIVE_DEV_QUERY_JOB_INFO: {
		const struct drm_sgxfive_dev_query_job_info j = {
			.job_types = DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT |
				     DRM_SGXFIVE_JOB_TYPE_RENDER_BIT,
			.max_jobs = MAX_JOBS, .max_sync_ops = MAX_SYNC_OPS,
			.transfer_cmd_size = sizeof(struct drm_sgxfive_transfer_cmd),
			.render_cmd_size = sizeof(struct drm_sgxfive_render_cmd),
			.render_pb_pages = 0x422, .render_max_width = RENDER_MAX_W,
			.render_max_height = RENDER_MAX_H,
		};
		return copy_out(q, &j, sizeof(j));
	}
	case DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO: {
		const struct drm_sgxfive_dev_query_runtime_info r = {
			.num_pixel_partitions = 31, .tile_width = 32, .tile_height = 16,
		};
		return copy_out(q, &r, sizeof(r));
	}
	case DRM_SGXFIVE_DEV_QUERY_HEAP_INFO: {
		struct drm_sgxfive_dev_query_heap_info h = { 0 };
		uint32_t n, sz = sizeof(struct drm_sgxfive_heap);

		if (!q->pointer)
			return copy_out(q, &h, sizeof(h));
		memcpy(&h, (void *)(uintptr_t)q->pointer,
		       q->size < sizeof(h) ? q->size : sizeof(h));
		if (h.heaps.array) {
			n = h.heaps.count < DRM_SGXFIVE_HEAP_COUNT ? h.heaps.count
								     : DRM_SGXFIVE_HEAP_COUNT;
			for (uint32_t i = 0; i < n; i++) {
				uint8_t *dst = (uint8_t *)(uintptr_t)h.heaps.array +
					       (size_t)i * h.heaps.stride;

				memset(dst, 0, h.heaps.stride);
				memcpy(dst, &m->heaps[i],
				       h.heaps.stride < sz ? h.heaps.stride : sz);
			}
			h.heaps.count = n;
		} else {
			h.heaps.count = DRM_SGXFIVE_HEAP_COUNT;
			h.heaps.stride = sz;
		}
		return copy_out(q, &h, sizeof(h));
	}
	}
	return -EINVAL;
}

/* ---- BOs ---------------------------------------------------------------------------- */

static int m_create_bo(struct sgxfive_mock *m, int fd, struct drm_sgxfive_create_bo_args *a)
{
	struct mbo *bo;

	if (a->_padding_c || !a->size || a->flags & ~DRM_SGXFIVE_BO_FLAGS_MASK)
		return -EINVAL;
	if (a->size > MAX_BO_SIZE)
		return -ENOMEM;
	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return -ENOMEM;
	bo->size = (a->size + SGXFIVE_PAGE_SIZE - 1) & ~(uint64_t)(SGXFIVE_PAGE_SIZE - 1);
	bo->mem = calloc(1, bo->size);
	if (!bo->mem) {
		free(bo);
		return -ENOMEM;
	}
	bo->fd = fd;
	bo->handle = ++m->next_handle;
	bo->flags = a->flags;
	bo->open = true;
	if (insert((void **)m->bos, bo))
		return -ENOMEM;
	a->handle = bo->handle;
	return 0;
}

static int m_mmap_offset(struct sgxfive_mock *m, int fd,
			 struct drm_sgxfive_get_bo_mmap_offset_args *a)
{
	struct mbo *bo = FIND(m->bos, fd, a->handle);

	if (a->_padding_4)
		return -EINVAL;
	if (!bo || !bo->open)
		return -ENOENT;			/* GEM lookups, as the kernel */
	if (!(bo->flags & DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS))
		return -EINVAL;
	a->offset = (uint64_t)bo->handle << MMAP_SHIFT;
	return 0;
}

static int m_gem_close(struct sgxfive_mock *m, int fd, struct drm_gem_close *a)
{
	struct mbo *bo = FIND(m->bos, fd, a->handle);

	if (a->pad || !bo || !bo->open)
		return -EINVAL;
	bo->open = false;
	bo_put(m, bo);
	return 0;
}

/* ---- VMs ---------------------------------------------------------------------------- */

static int m_create_vm(struct sgxfive_mock *m, int fd,
		       struct drm_sgxfive_create_vm_context_args *a)
{
	struct mvm *vm;

	if (a->_padding_4)
		return -EINVAL;
	vm = calloc(1, sizeof(*vm));
	if (!vm)
		return -ENOMEM;
	vm->fd = fd;
	vm->handle = ++m->next_handle;
	vm->open = true;
	if (insert((void **)m->vms, vm))
		return -ENOMEM;
	a->handle = vm->handle;
	return 0;
}

static int m_destroy_vm(struct sgxfive_mock *m, int fd,
			struct drm_sgxfive_destroy_vm_context_args *a)
{
	struct mvm *vm = FIND(m->vms, fd, a->handle);

	if (a->_padding_4 || !vm || !vm->open)
		return -EINVAL;
	vm->open = false;
	vm_put(m, vm);
	return 0;
}

/* The user heap holding [addr, addr + size), or NULL. */
static const struct drm_sgxfive_heap *user_heap(struct sgxfive_mock *m, uint64_t addr,
						uint64_t size)
{
	for (int i = 0; i < DRM_SGXFIVE_HEAP_COUNT; i++) {
		const struct drm_sgxfive_heap *h = &m->heaps[i];

		if (h->size && !(h->flags & DRM_SGXFIVE_HEAP_RESERVED) && addr >= h->base &&
		    size <= h->size && addr - h->base <= h->size - size)
			return h;
	}
	return NULL;
}

static int m_vm_map(struct sgxfive_mock *m, int fd, struct drm_sgxfive_vm_map_args *a)
{
	struct mvm *vm = FIND(m->vms, fd, a->vm_context_handle);
	struct mbo *bo = FIND(m->bos, fd, a->handle);
	const uint64_t pg = SGXFIVE_PAGE_SIZE;

	if (a->_padding_14 || a->flags & ~DRM_SGXFIVE_VM_MAP_FLAGS_MASK || a->offset % pg ||
	    !a->device_addr || a->device_addr % pg || !a->size || a->size % pg ||
	    !user_heap(m, a->device_addr, a->size))
		return -EINVAL;
	if (!bo || !bo->open)
		return -ENOENT;
	if (a->offset > bo->size || a->size > bo->size - a->offset || !vm || !vm->open)
		return -EINVAL;
	for (uint32_t i = 0; i < vm->nmaps; i++)
		if (a->device_addr < vm->maps[i].addr + vm->maps[i].size &&
		    vm->maps[i].addr < a->device_addr + a->size)
			return -EEXIST;
	if (vm->nmaps == MAX_MAPS)
		return -ENOMEM;
	vm->maps[vm->nmaps++] = (struct mmap_ent){
		.addr = a->device_addr, .size = a->size, .offset = a->offset, .bo = bo,
		.flags = a->flags,
	};
	bo->maps++;
	return 0;
}

static int m_vm_unmap(struct sgxfive_mock *m, int fd, struct drm_sgxfive_vm_unmap_args *a)
{
	struct mvm *vm = FIND(m->vms, fd, a->vm_context_handle);

	if (a->_padding_4 || !vm || !vm->open)
		return -EINVAL;
	for (uint32_t i = 0; i < vm->nmaps; i++) {
		struct mmap_ent e = vm->maps[i];

		if (e.addr == a->device_addr && e.size == a->size) {
			vm->maps[i] = vm->maps[--vm->nmaps];
			e.bo->maps--;
			bo_put(m, e.bo);
			return 0;
		}
	}
	return -ENOENT;
}

/* ---- Contexts and syncobjs ---------------------------------------------------------- */

/* The kernel's fault budget: three faults within a minute (all of a test's run here). */
static bool banned(const struct sgxfive_mock *m, int fd)
{
	return m->fd_faults[fd - FD_BASE] >= 3;
}

static int m_create_ctx(struct sgxfive_mock *m, int fd,
			struct drm_sgxfive_create_context_args *a)
{
	struct mvm *vm = FIND(m->vms, fd, a->vm_context_handle);
	struct mctx *c;

	if (a->_padding_14 || (a->type != DRM_SGXFIVE_CTX_TYPE_TRANSFER &&
				a->type != DRM_SGXFIVE_CTX_TYPE_RENDER) || a->flags ||
	    a->priority != DRM_SGXFIVE_CTX_PRIORITY_NORMAL || !vm || !vm->open)
		return -EINVAL;
	if (banned(m, fd))
		return -ECANCELED;
	if (a->type == DRM_SGXFIVE_CTX_TYPE_RENDER) {
		if (!m->heaps[DRM_SGXFIVE_HEAP_PARAMETERS].size)
			return -ENODEV;
		if (vm->renders == RENDER_SLOTS)
			return -ENOSPC;
	}
	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->fd = fd;
	c->handle = ++m->next_handle;
	c->type = a->type;
	c->vm = vm;
	if (insert((void **)m->ctxs, c))
		return -ENOMEM;
	vm->ctxs++;
	vm->renders += c->type == DRM_SGXFIVE_CTX_TYPE_RENDER;
	a->handle = c->handle;
	return 0;
}

static void ctx_free(struct sgxfive_mock *m, struct mctx *c)
{
	struct mvm *vm = c->vm;

	drop((void **)m->ctxs, c);
	vm->renders -= c->type == DRM_SGXFIVE_CTX_TYPE_RENDER;
	free(c);
	vm->ctxs--;
	vm_put(m, vm);
}

static int m_destroy_ctx(struct sgxfive_mock *m, int fd,
			 struct drm_sgxfive_destroy_context_args *a)
{
	struct mctx *c = FIND(m->ctxs, fd, a->handle);

	if (a->_padding_4 || !c)
		return -EINVAL;
	ctx_free(m, c);
	return 0;
}

static int m_syncobj_create(struct sgxfive_mock *m, int fd, struct drm_syncobj_create *a)
{
	struct msync *s;

	if (a->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;
	s->fd = fd;
	s->handle = ++m->next_handle;
	s->fence = a->flags & DRM_SYNCOBJ_CREATE_SIGNALED;
	if (insert((void **)m->syncs, s))
		return -ENOMEM;
	a->handle = s->handle;
	return 0;
}

static int m_syncobj_destroy(struct sgxfive_mock *m, int fd, struct drm_syncobj_destroy *a)
{
	struct msync *s = FIND(m->syncs, fd, a->handle);

	if (a->pad || !s)
		return -EINVAL;
	drop((void **)m->syncs, s);
	free(s);
	return 0;
}

/* Jobs complete at submit, so an attached fence is a signalled one. */
static int m_syncobj_wait(struct sgxfive_mock *m, int fd, const uint32_t *handles,
			  const uint64_t *points, uint32_t n)
{
	int ret = 0;

	for (uint32_t i = 0; i < n; i++) {
		struct msync *s = FIND(m->syncs, fd, handles[i]);

		if (!s)
			return -EINVAL;
		if (points ? s->point < points[i] : !s->fence)
			ret = -ETIME;
	}
	return ret;
}

/* ---- SUBMIT_JOBS -------------------------------------------------------------------- */

struct vjob {
	struct drm_sgxfive_job job;
	struct mctx *ctx;
	struct drm_sgxfive_transfer_cmd cmd;
	struct drm_sgxfive_render_cmd rc;
	struct drm_sgxfive_sync_op ops[MAX_SYNC_OPS];
};

static bool zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	for (size_t i = 0; i < n; i++)
		if (b[i])
			return false;
	return true;
}

/* Read a versioned element: shorter is zero-extended, a longer one's tail must be zero. */
static int read_elem(void *dst, size_t size, const void *src, uint32_t stride)
{
	memset(dst, 0, size);
	if (stride > size && !zero((const uint8_t *)src + size, stride - size))
		return -EINVAL;
	memcpy(dst, src, stride < size ? stride : size);
	return 0;
}

/* §4.5: addresses that make the pixel pipe fetch or execute stay in user heaps. */
static int check_cmd(struct sgxfive_mock *m, const struct drm_sgxfive_transfer_cmd *c)
{
	if (c->load_flags & ~DRM_SGXFIVE_TRANSFER_LOAD_FLAGS_MASK || c->double_pixel_partitions)
		return -EINVAL;
	if (c->event_pixel_pds_exec & 0xf || !user_heap(m, c->event_pixel_pds_exec, 16))
		return -EINVAL;
	/* PDS_EXEC_BASE stays the PDS_PIXEL heap base: renders rely on it (§11.5). */
	if (c->pds_exec_base != m->heaps[DRM_SGXFIVE_HEAP_PDS_PIXEL].base)
		return -EINVAL;
	if (c->bif_3d_req_base & 0xfffff || !user_heap(m, c->bif_3d_req_base, 16))
		return -EINVAL;
	if (c->isp_rgn_base && !user_heap(m, c->isp_rgn_base, 4))
		return -EINVAL;
	/* The render box in 32 x 16 tiles within 4K x 4K. */
	if ((c->isp_render_box2 & 0xffff) > 4096 / 16 - 1 ||
	    (c->isp_render_box2 >> 16) > 4096 / 32 - 1)
		return -EINVAL;
	return 0;
}

/* §11.4: the stream and the pixel-event program in user heaps, the target fits. */
static int check_render(struct sgxfive_mock *m, const struct drm_sgxfive_render_cmd *c)
{
	uint32_t w = (c->mte_screen & 0xffff) + 1, h = (c->mte_screen >> 16) + 1;
	uint32_t tx = (c->te_screen & 0xfff) + 1, ty = ((c->te_screen >> 12) & 0xfff) + 1;

	if (c->vdm_ctrl_stream_base & 3 || !user_heap(m, c->vdm_ctrl_stream_base, 4) ||
	    c->event_pixel_pds_exec & 0xf || !user_heap(m, c->event_pixel_pds_exec, 16))
		return -EINVAL;
	if (w > RENDER_MAX_W || h > RENDER_MAX_H || c->te_screen >> 24 ||
	    tx > RENDER_MAX_W / 32 || ty > RENDER_MAX_H / 16 || c->isp_bgobj & ~0x300u)
		return -EINVAL;
	return 0;
}

struct pending_sig {
	uint32_t handle;
	uint64_t point;
};

static int validate(struct sgxfive_mock *m, int fd, const struct drm_sgxfive_job *uj,
		    uint32_t stride, struct vjob *v, struct pending_sig *sigs, uint32_t *nsigs)
{
	struct drm_sgxfive_job *j = &v->job;
	bool dummy;
	int ret;

	ret = read_elem(j, sizeof(*j), uj, stride);
	if (ret)
		return ret;
	if ((j->type != DRM_SGXFIVE_JOB_TYPE_TRANSFER && j->type != DRM_SGXFIVE_JOB_TYPE_RENDER) ||
	    j->flags & ~(j->type == DRM_SGXFIVE_JOB_TYPE_RENDER ?
			 DRM_SGXFIVE_SUBMIT_JOB_RENDER_FLAGS_MASK :
			 DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_FLAGS_MASK))
		return -EINVAL;
	/* The reserved update list: max_updates is 0. */
	if (j->updates.count || j->updates.stride || j->updates.array)
		return -EINVAL;
	v->ctx = FIND(m->ctxs, fd, j->context_handle);
	if (!v->ctx || v->ctx->type != j->type)
		return -EINVAL;
	if (v->ctx->lost || banned(m, fd))
		return -ECANCELED;
	dummy = j->type == DRM_SGXFIVE_JOB_TYPE_TRANSFER &&
		(j->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY);
	if (dummy && (j->cmd_stream || j->cmd_stream_len))
		return -EINVAL;
	if (j->type == DRM_SGXFIVE_JOB_TYPE_RENDER) {
		if (j->cmd_stream_len < DRM_SGXFIVE_RENDER_CMD_SIZE_V1 ||
		    j->cmd_stream_len > SGXFIVE_PAGE_SIZE || !j->cmd_stream || j->cmd_stream % 8)
			return -EINVAL;
		ret = read_elem(&v->rc, sizeof(v->rc), (const void *)(uintptr_t)j->cmd_stream,
				j->cmd_stream_len);
		if (!ret)
			ret = check_render(m, &v->rc);
		if (ret)
			return ret;
	} else if (!dummy) {
		if (j->cmd_stream_len < DRM_SGXFIVE_TRANSFER_CMD_SIZE_V1 ||
		    j->cmd_stream_len > SGXFIVE_PAGE_SIZE || !j->cmd_stream || j->cmd_stream % 8)
			return -EINVAL;
		ret = read_elem(&v->cmd, sizeof(v->cmd), (const void *)(uintptr_t)j->cmd_stream,
				j->cmd_stream_len);
		if (!ret)
			ret = check_cmd(m, &v->cmd);
		if (ret)
			return ret;
	}
	if (j->sync_ops.count > MAX_SYNC_OPS || (j->sync_ops.count && !j->sync_ops.array) ||
	    (j->sync_ops.count && j->sync_ops.stride < 4))
		return -EINVAL;
	for (uint32_t i = 0; i < j->sync_ops.count; i++) {
		struct drm_sgxfive_sync_op *op = &v->ops[i];
		const uint8_t *src = (const uint8_t *)(uintptr_t)j->sync_ops.array +
				     (size_t)i * j->sync_ops.stride;
		uint32_t type;
		struct msync *s;
		bool satisfied;

		ret = read_elem(op, sizeof(*op), src, j->sync_ops.stride);
		if (ret)
			return ret;
		type = op->flags & DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK;
		s = FIND(m->syncs, fd, op->handle);
		if (op->flags & ~DRM_SGXFIVE_SYNC_OP_FLAGS_MASK ||
		    type > DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ)
			return -EINVAL;
		if (!s)
			return -ENOENT;		/* drm_syncobj_find, as the kernel */
		if (type == DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ ? op->value != 0
									 : op->value == 0)
			return -EINVAL;
		if (op->flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL) {
			if (*nsigs < MAX_JOBS * MAX_SYNC_OPS)
				sigs[(*nsigs)++] = (struct pending_sig){ op->handle, op->value };
			continue;
		}
		/* A wait needs a fence: attached now, or signalled by an earlier job here. */
		satisfied = op->value ? s->point >= op->value : s->fence;
		for (uint32_t k = 0; k < *nsigs && !satisfied; k++)
			satisfied = sigs[k].handle == op->handle && sigs[k].point >= op->value;
		if (!satisfied)
			return -EINVAL;
	}
	return 0;
}

/* ---- The transfer interpreter ------------------------------------------------------- */

struct xfer_state {
	struct mvm *vm;
	uint64_t use_base;
	bool fault;
	uint64_t fault_addr;
};

static uint32_t rd(struct xfer_state *x, uint64_t addr)
{
	const uint8_t *p = gpu_ptr(x->vm, addr, false);
	uint32_t v;

	if (!p) {
		if (!x->fault)
			x->fault_addr = addr;
		x->fault = true;
		return 0;
	}
	memcpy(&v, p, 4);
	return v;
}

static uint64_t rd64(struct xfer_state *x, uint64_t addr)
{
	return rd(x, addr) | (uint64_t)rd(x, addr + 4) << 32;
}

/* A USE program address from its DOUTU U0: EXE [19:4], CBASE [3:0]. */
static uint64_t doutu_addr(struct xfer_state *x, uint32_t u0)
{
	return x->use_base + ((u0 & 0xffff0) >> 1) + ((uint64_t)(u0 & 0xf) << 19);
}

static float f32(uint32_t v)
{
	float f;

	memcpy(&f, &v, 4);
	return f;
}

static uint32_t imax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t imin(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void run_transfer(struct sgxfive_mock *m, struct mctx *ctx,
			 const struct drm_sgxfive_transfer_cmd *c)
{
	struct xfer_state x = {
		.vm = ctx->vm, .use_base = m->heaps[DRM_SGXFIVE_HEAP_USSE_PIXEL].base,
	};
	uint32_t pbe[4], tag = c->isp_bgobjtag, layers = ((tag >> 24) & 0x7f) / 2;
	uint64_t eot, bg, pri, shader, insn;
	uint32_t vx, vy, colour = 0, t0 = 0, t1 = 0, t2 = 0, tw = 1, th = 1;
	uint32_t x0, y0, x1, y1, stride, sstride = 0, su = 0, sv = 0;
	bool copy = false;

	/* Pixel-event PDS -> EOT -> PBE state. */
	eot = doutu_addr(&x, rd(&x, c->event_pixel_pds_exec + 12));
	for (unsigned int i = 0; i < 4; i++)
		pbe[i] = sgxfive_limm_get(rd64(&x, eot + 8 + 8 * i));
	/* Background object -> PDS primary -> shader. */
	bg = c->bif_3d_req_base + ((uint64_t)(tag & 0xffffff) << 4);
	pri = c->pds_exec_base + ((uint64_t)(rd(&x, bg + 8) & 0xffffff) << 4);
	shader = doutu_addr(&x, rd(&x, pri));
	insn = rd64(&x, shader + 8);
	if (layers == 0) {
		colour = sgxfive_limm_get(insn);
	} else if (insn == ((uint64_t)0x28851001 << 32 | 0xa0000000)) {
		copy = true;
		t2 = rd(&x, pri + 24);
		t0 = rd(&x, pri + 32);
		t1 = rd(&x, pri + 36);
		tw = ((t1 >> 12) & 0xfff) + 1;
		th = (t1 & 0xfff) + 1;
		sstride = ((((t0 >> 9) & 7) | ((t0 >> 17) & 0x3ff) << 3) + 1) * 4;
		su = (uint32_t)(f32(rd(&x, bg + 12)) * tw + 0.5f);
		sv = (uint32_t)(f32(rd(&x, bg + 16)) * th + 0.5f);
	} else {
		x.fault = true;			/* an unknown program: treat as a hang */
	}
	/* Vertices (top-left, top-right, bottom-left) after the TSP words. */
	{
		uint64_t v = bg + (layers ? 15 : 9) * 4;
		uint32_t w0 = rd(&x, v), w1 = rd(&x, v + 4), w3 = rd(&x, v + 12);
		uint32_t w5 = rd(&x, v + 20);

		vx = ((w0 >> 24) | w1 << 8) & 0xffffff;
		vy = w0 & 0xffffff;
		x0 = (vx >> 8) - 0x1000;
		y0 = (vy >> 8) - 0x1000;
		x1 = (w3 >> 8 >> 8) - 0x1000;
		y1 = ((w5 & 0xffffff) >> 8) - 0x1000;
	}
	/* Clip: vertex rectangle, render box (32 x 16 tiles), PBE window (inclusive). */
	x0 = imax(imax(x0, (c->isp_render_box1 >> 16) * 32), pbe[0] & 0x1fff);
	y0 = imax(imax(y0, (c->isp_render_box1 & 0xffff) * 16), (pbe[0] >> 16) & 0x1fff);
	x1 = imin(imin(x1, ((c->isp_render_box2 >> 16) + 1) * 32), (pbe[1] & 0x1fff) + 1);
	y1 = imin(imin(y1, ((c->isp_render_box2 & 0xffff) + 1) * 16),
		  ((pbe[1] >> 16) & 0x1fff) + 1);
	stride = (((pbe[2] >> 16) & 0xfff) + 1) * 8;

	/* Two passes: check every access (a fault writes nothing), then write. */
	for (int pass = 0; pass < 2 && !x.fault; pass++) {
		for (uint32_t y = y0; y < y1 && !x.fault; y++) {
			for (uint32_t px = x0; px < x1 && !x.fault; px++) {
				uint64_t d = (pbe[3] & ~3u) + (uint64_t)y * stride + 4ull * px;
				uint8_t *dp = gpu_ptr(x.vm, d, true);
				uint32_t v = colour;

				if (copy) {
					uint32_t tx = su + (px - x0), ty = sv + (y - y0);

					if (tx >= tw || ty >= th)
						continue;	/* outside the texture */
					v = rd(&x, (t2 & ~3u) + (uint64_t)ty * sstride + 4ull * tx);
				}
				if (!dp) {
					x.fault = true;
					x.fault_addr = d;
				} else if (pass) {
					memcpy(dp, &v, 4);
				}
			}
		}
	}
	if (x.fault) {
		ctx->lost = true;
		m->fd_faults[ctx->fd - FD_BASE]++;
		m->stats.faults++;
		m->stats.last_fault_addr = x.fault_addr;
	}
}

/* ---- The render interpreter (design/sgx545_3d.md S.1-S.8, §14) ---------------------- */

#define MAX_TRIS	16

/* The fill colour a pixel PDS primary at pri starts (use_fill's immediate). */
static uint32_t fill_colour(struct xfer_state *x, uint64_t pri)
{
	return sgxfive_limm_get(rd64(x, doutu_addr(x, rd(x, pri)) + 8));
}

/* Words each presence bit of an MTE state block adds (S.2). */
static uint32_t block_word(uint32_t presence, unsigned int bit)
{
	static const uint8_t words[16] = { 1, 1, 1, 1, 1, 1, 3, 2, 6, 1, 1, 1, 1, 1, 1, 1 };
	uint32_t off = 1;

	for (unsigned int b = 0; b < bit; b++)
		if (presence & (1u << b))
			off += words[b];
	return off;
}

static float edge(const float *a, const float *b, float px, float py)
{
	return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
}

/*
 * One scene, as our programs would draw it: the VDM stream's state blocks
 * (their pixel primary gives the colour, the viewport the window mapping),
 * index lists of triangles through their vertex PDS (buffer and stride),
 * then every pixel of the PBE window from the end-of-tile program: the
 * background object's colour, or the last triangle covering its centre. A
 * read outside the VM's mappings is a fault and nothing is written.
 */
static void run_render(struct sgxfive_mock *m, struct mctx *ctx,
		       const struct drm_sgxfive_render_cmd *c)
{
	struct xfer_state x = {
		.vm = ctx->vm, .use_base = m->heaps[DRM_SGXFIVE_HEAP_USSE_PIXEL].base,
	};
	const uint64_t pds = m->heaps[DRM_SGXFIVE_HEAP_PDS_PIXEL].base;
	float tri[MAX_TRIS][3][2], vp[6] = { 0 };
	uint32_t colour[MAX_TRIS], cur = 0, ntris = 0, pbe[4], bg, stride, x0, y0, x1, y1;
	uint64_t a = c->vdm_ctrl_stream_base, eot;

	for (unsigned int n = 0; n < 64 && !x.fault; n++) {
		uint32_t w0 = rd(&x, a);

		if (w0 >> 30 == 1) {			/* state: its MTE block */
			uint64_t blk = rd(&x, (uint64_t)(w0 & 0x0fffffff) << 4);
			uint32_t pres = rd(&x, blk), o;

			if (pres & (1u << 6)) {
				o = block_word(pres, 6);
				cur = fill_colour(&x, pds + ((uint64_t)(rd(&x, blk + 4 * (o + 2)) &
								    0xffffff) << 4));
			}
			if (pres & (1u << 8)) {
				o = block_word(pres, 8);
				for (unsigned int i = 0; i < 6; i++)
					vp[i] = f32(rd(&x, blk + 4 * (o + i)));
			}
			a += 8;
		} else if (w0 >> 30 == 2) {		/* index list and its vertex task */
			uint32_t extra = !!(w0 & (1u << 22)) + !!(w0 & (1u << 23));
			uint32_t count = w0 & 0x3fffff, prim = (w0 >> 26) & 0xf;
			uint64_t ib = rd(&x, a + 4), vt = rd(&x, a + 8 + 4 * extra);
			uint64_t vpds = (uint64_t)(vt & 0x0fffffff) << 4;
			uint64_t vb = rd(&x, vpds), vs = rd(&x, vpds + 8);

			if (prim) {
				x.fault = true;		/* only triangle lists */
				break;
			}
			for (uint32_t i = 0; i + 3 <= count && ntris < MAX_TRIS; i += 3, ntris++) {
				for (unsigned int k = 0; k < 3; k++) {
					uint32_t iw = rd(&x, ib + ((2ull * (i + k)) & ~3ull));
					uint32_t idx = (i + k) % 2 ? iw >> 16 : iw & 0xffff;
					uint64_t v = vb + idx * vs;
					float w = f32(rd(&x, v + 12));

					tri[ntris][k][0] = f32(rd(&x, v)) / w * vp[1] + vp[0];
					tri[ntris][k][1] = f32(rd(&x, v + 4)) / w * vp[3] + vp[2];
				}
				colour[ntris] = cur;
			}
			a += 8 + 4 * extra + 8;
		} else if (w0 >> 30 == 3) {		/* terminate */
			break;
		} else {
			x.fault = true;			/* links: not used by our streams */
		}
	}
	/* The target: pixel-event PDS -> EOT -> PBE state; the background colour. */
	eot = doutu_addr(&x, rd(&x, c->event_pixel_pds_exec + 12));
	for (unsigned int i = 0; i < 4; i++)
		pbe[i] = sgxfive_limm_get(rd64(&x, eot + 8 + 8 * i));
	bg = fill_colour(&x, pds + ((uint64_t)(c->bgobj_words[2] & 0xffffff) << 4));
	x0 = pbe[0] & 0x1fff;
	y0 = (pbe[0] >> 16) & 0x1fff;
	x1 = imin((pbe[1] & 0x1fff) + 1, (c->mte_screen & 0xffff) + 1);
	y1 = imin(((pbe[1] >> 16) & 0x1fff) + 1, (c->mte_screen >> 16) + 1);
	stride = (((pbe[2] >> 16) & 0xfff) + 1) * 8;

	for (int pass = 0; pass < 2 && !x.fault; pass++) {
		for (uint32_t y = y0; y < y1 && !x.fault; y++) {
			for (uint32_t px = x0; px < x1 && !x.fault; px++) {
				uint64_t d = (pbe[3] & ~3u) + (uint64_t)y * stride + 4ull * px;
				uint8_t *dp = gpu_ptr(x.vm, d, true);
				uint32_t v = bg;

				for (uint32_t t = 0; t < ntris; t++) {
					float cx = px + 0.5f, cy = y + 0.5f;
					float e0 = edge(tri[t][0], tri[t][1], cx, cy);
					float e1 = edge(tri[t][1], tri[t][2], cx, cy);
					float e2 = edge(tri[t][2], tri[t][0], cx, cy);

					if ((e0 >= 0 && e1 >= 0 && e2 >= 0) ||
					    (e0 <= 0 && e1 <= 0 && e2 <= 0))
						v = colour[t];
				}
				if (!dp) {
					x.fault = true;
					x.fault_addr = d;
				} else if (pass) {
					memcpy(dp, &v, 4);
				}
			}
		}
	}
	if (x.fault) {
		ctx->lost = true;
		m->fd_faults[ctx->fd - FD_BASE]++;
		m->stats.faults++;
		m->stats.last_fault_addr = x.fault_addr;
	}
}

static void signal_ops(struct sgxfive_mock *m, int fd, const struct vjob *v)
{
	for (uint32_t i = 0; i < v->job.sync_ops.count; i++) {
		const struct drm_sgxfive_sync_op *op = &v->ops[i];
		struct msync *s = FIND(m->syncs, fd, op->handle);

		if (!(op->flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL) || !s)
			continue;
		if (op->value)
			s->point = op->value > s->point ? op->value : s->point;
		else
			s->fence = true;
	}
}

static int m_submit(struct sgxfive_mock *m, int fd, struct drm_sgxfive_submit_jobs_args *a)
{
	static struct pending_sig sigs[MAX_JOBS * MAX_SYNC_OPS];
	struct vjob *v;
	uint32_t n = a->jobs.count, nsigs = 0;
	int ret = 0;

	if (n > MAX_JOBS || (n && (!a->jobs.array || a->jobs.stride < 8)))
		return -EINVAL;
	v = calloc(n ? n : 1, sizeof(*v));
	if (!v)
		return -ENOMEM;
	for (uint32_t i = 0; i < n; i++) {
		ret = validate(m, fd, (const struct drm_sgxfive_job *)((uintptr_t)a->jobs.array +
								       (size_t)i * a->jobs.stride),
			       a->jobs.stride, &v[i], sigs, &nsigs);
		if (ret) {
			a->jobs.count = i;
			free(v);
			return ret;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		if (v[i].ctx->lost) {
			ret = -ECANCELED;	/* lost by an earlier job of this call */
		} else if (v[i].job.type == DRM_SGXFIVE_JOB_TYPE_RENDER) {
			run_render(m, v[i].ctx, &v[i].rc);
			m->stats.renders_run++;
		} else if (v[i].job.flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY) {
			m->stats.dummies_run++;
		} else {
			run_transfer(m, v[i].ctx, &v[i].cmd);
			m->stats.jobs_run++;
		}
		signal_ops(m, fd, &v[i]);	/* a failed job's fence signals with an error */
	}
	free(v);
	return ret == -ECANCELED ? 0 : ret;
}

/* ---- Backend ------------------------------------------------------------------------ */

static int mock_open(void *priv, const char *path)
{
	struct sgxfive_mock *m = priv;

	for (uint32_t i = 0; i < m->nnodes; i++) {
		if (strcmp(path, m->nodes[i].path))
			continue;
		for (int fd = 0; fd < 64; fd++) {
			if (!m->fd_driver[fd]) {
				m->fd_driver[fd] = m->nodes[i].driver;
				m->fd_sgx[fd] = m->nodes[i].sgx;
				return fd + FD_BASE;
			}
		}
		return -EMFILE;
	}
	return -ENOENT;
}

static bool valid_fd(struct sgxfive_mock *m, int fd)
{
	return fd >= FD_BASE && fd < FD_BASE + 64 && m->fd_driver[fd - FD_BASE];
}

static int mock_close(void *priv, int fd)
{
	struct sgxfive_mock *m = priv;

	if (!valid_fd(m, fd))
		return -EBADF;
	/* Closing the file releases everything it owns. */
	for (int i = 0; i < MAX_OBJS; i++) {
		if (m->ctxs[i] && m->ctxs[i]->fd == fd)
			ctx_free(m, m->ctxs[i]);
		if (m->syncs[i] && m->syncs[i]->fd == fd) {
			free(m->syncs[i]);
			m->syncs[i] = NULL;
		}
	}
	for (int i = 0; i < MAX_OBJS; i++) {
		if (m->vms[i] && m->vms[i]->fd == fd) {
			m->vms[i]->open = false;
			vm_put(m, m->vms[i]);
		}
	}
	for (int i = 0; i < MAX_OBJS; i++) {
		if (m->bos[i] && m->bos[i]->fd == fd) {
			m->bos[i]->open = false;
			bo_put(m, m->bos[i]);
		}
	}
	m->fd_driver[fd - FD_BASE] = NULL;
	m->fd_faults[fd - FD_BASE] = 0;
	return 0;
}

static int mock_ioctl(void *priv, int fd, unsigned long req, void *arg)
{
	struct sgxfive_mock *m = priv;

	if (!valid_fd(m, fd))
		return -EBADF;
	if (req == DRM_IOCTL_VERSION)
		return m_version(m, fd, arg);
	if (!m->fd_sgx[fd - FD_BASE])
		return -ENOTTY;
	if (req == DRM_IOCTL_SGXFIVE_DEV_QUERY)
		return m_query(m, arg);
	if (req == DRM_IOCTL_SGXFIVE_CREATE_BO)
		return m_create_bo(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET)
		return m_mmap_offset(m, fd, arg);
	if (req == DRM_IOCTL_GEM_CLOSE)
		return m_gem_close(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT)
		return m_create_vm(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT)
		return m_destroy_vm(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_VM_MAP)
		return m_vm_map(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_VM_UNMAP)
		return m_vm_unmap(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_CREATE_CONTEXT)
		return m_create_ctx(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_DESTROY_CONTEXT)
		return m_destroy_ctx(m, fd, arg);
	if (req == DRM_IOCTL_SGXFIVE_SUBMIT_JOBS)
		return m_submit(m, fd, arg);
	if (req == DRM_IOCTL_SYNCOBJ_CREATE)
		return m_syncobj_create(m, fd, arg);
	if (req == DRM_IOCTL_SYNCOBJ_DESTROY)
		return m_syncobj_destroy(m, fd, arg);
	if (req == DRM_IOCTL_SYNCOBJ_WAIT) {
		const struct drm_syncobj_wait *w = arg;

		return m_syncobj_wait(m, fd, (const uint32_t *)(uintptr_t)w->handles, NULL,
				      w->count_handles);
	}
	if (req == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) {
		const struct drm_syncobj_timeline_wait *w = arg;

		return m_syncobj_wait(m, fd, (const uint32_t *)(uintptr_t)w->handles,
				      (const uint64_t *)(uintptr_t)w->points, w->count_handles);
	}
	return -ENOTTY;
}

static int mock_mmap(void *priv, int fd, uint64_t offset, size_t len, void **out)
{
	struct sgxfive_mock *m = priv;
	struct mbo *bo;

	if (!valid_fd(m, fd))
		return -EBADF;
	bo = FIND(m->bos, fd, (uint32_t)(offset >> MMAP_SHIFT));
	if (!bo || offset & ((1u << MMAP_SHIFT) - 1) || len > bo->size)
		return -EINVAL;
	*out = bo->mem;
	return 0;
}

static int mock_munmap(void *priv, void *addr, size_t len)
{
	return 0;
}

struct sgxfive_mock *sgxfive_mock_create(const struct sgxfive_mock_config *cfg)
{
	static const struct sgxfive_mock_config defaults;
	struct sgxfive_mock *m = calloc(1, sizeof(*m));
	static const char *const paths[] = { "/dev/dri/renderD128", "/dev/dri/renderD129" };

	if (!m)
		return NULL;
	if (!cfg)
		cfg = &defaults;
	m->be = (struct sgxfive_backend){
		.open = mock_open, .close = mock_close, .ioctl = mock_ioctl,
		.mmap = mock_mmap, .munmap = mock_munmap, .priv = m,
	};
	if (cfg->other_driver) {
		m->nodes[m->nnodes] = (struct mnode){ paths[m->nnodes], cfg->other_driver,
						      false };
		m->nnodes++;
	}
	if (!cfg->no_sgxfive) {
		m->nodes[m->nnodes] = (struct mnode){ paths[m->nnodes],
						      cfg->sgxfive_name ? cfg->sgxfive_name :
						      SGXFIVE_DRIVER_NAME, true };
		m->nnodes++;
	}
	if (cfg->layout == SGXFIVE_MOCK_LAYOUT_HARNESS) {
		memcpy(m->heaps, harness, sizeof(harness));
		m->bridge_rev = 4;
	} else {
		memcpy(m->heaps, rev1, sizeof(rev1));
		m->quirks = DRM_SGXFIVE_QUIRK_SAMPLE_CACHE;
		m->bridge_rev = 3;
	}
	return m;
}

void sgxfive_mock_destroy(struct sgxfive_mock *m)
{
	if (!m)
		return;
	for (int fd = 0; fd < 64; fd++)
		if (m->fd_driver[fd])
			mock_close(m, fd + FD_BASE);
	free(m);
}

const struct sgxfive_backend *sgxfive_mock_backend(struct sgxfive_mock *m)
{
	return &m->be;
}

void sgxfive_mock_get_stats(const struct sgxfive_mock *m, struct sgxfive_mock_stats *s)
{
	*s = m->stats;
	s->bos = count((void *const *)m->bos);
	s->vms = count((void *const *)m->vms);
	s->ctxs = count((void *const *)m->ctxs);
	s->syncobjs = count((void *const *)m->syncs);
	s->mappings = 0;
	for (int i = 0; i < MAX_OBJS; i++)
		if (m->vms[i])
			s->mappings += m->vms[i]->nmaps;
}
