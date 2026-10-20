// SPDX-License-Identifier: MIT
/*
 * libsgxfive: device discovery, DEV_QUERY, BOs, VM contexts, contexts,
 * syncobjs and SUBMIT_JOBS, all through a struct sgxfive_backend.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sgxfive.h"

struct sgxfive_dev {
	const struct sgxfive_backend *be;
	int fd;
	bool owns_fd;			/* false: adopted, the caller closes it */
	char path[64];
	struct sgxfive_info info;
};

struct sgxfive_vm {
	struct sgxfive_dev *dev;
	uint32_t handle;
	uint64_t next[DRM_SGXFIVE_HEAP_COUNT];	/* bump pointer per heap */
};

/* ---- The OS backend ----------------------------------------------------------------- */

static int os_open(void *priv, const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);

	return fd < 0 ? -errno : fd;
}

static int os_close(void *priv, int fd)
{
	return close(fd) ? -errno : 0;
}

static int os_ioctl(void *priv, int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret ? -errno : 0;
}

static int os_mmap(void *priv, int fd, uint64_t offset, size_t len, void **out)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);

	if (p == MAP_FAILED)
		return -errno;
	*out = p;
	return 0;
}

static int os_munmap(void *priv, void *addr, size_t len)
{
	return munmap(addr, len) ? -errno : 0;
}

static const struct sgxfive_backend os_backend = {
	.open = os_open, .close = os_close, .ioctl = os_ioctl,
	.mmap = os_mmap, .munmap = os_munmap,
};

const struct sgxfive_backend *sgxfive_os_backend(void)
{
	return &os_backend;
}

/* ---- Device ------------------------------------------------------------------------- */

int sgxfive_dev_ioctl(struct sgxfive_dev *dev, unsigned long req, void *arg)
{
	return dev->be->ioctl(dev->be->priv, dev->fd, req, arg);
}

int sgxfive_dev_query(struct sgxfive_dev *dev, uint32_t type, void *buf, uint32_t *size)
{
	struct drm_sgxfive_dev_query_args q = {
		.type = type, .size = buf ? *size : 0, .pointer = (uintptr_t)buf,
	};
	int ret = sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_DEV_QUERY, &q);

	if (!ret)
		*size = q.size;
	return ret;
}

static int query_all(struct sgxfive_dev *dev)
{
	struct sgxfive_info *i = &dev->info;
	struct drm_sgxfive_dev_query_heap_info h = { 0 };
	const struct { uint32_t type; void *buf; uint32_t size; } q[] = {
		{ DRM_SGXFIVE_DEV_QUERY_GPU_INFO, &i->gpu, sizeof(i->gpu) },
		{ DRM_SGXFIVE_DEV_QUERY_FW_INFO, &i->fw, sizeof(i->fw) },
		{ DRM_SGXFIVE_DEV_QUERY_JOB_INFO, &i->job, sizeof(i->job) },
		{ DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO, &i->runtime, sizeof(i->runtime) },
	};
	uint32_t size;
	int ret;

	for (unsigned int k = 0; k < sizeof(q) / sizeof(q[0]); k++) {
		size = q[k].size;
		ret = sgxfive_dev_query(dev, q[k].type, q[k].buf, &size);
		if (ret)
			return ret;
	}
	/* First the count, then at most the heaps this library knows. */
	size = sizeof(h);
	ret = sgxfive_dev_query(dev, DRM_SGXFIVE_DEV_QUERY_HEAP_INFO, &h, &size);
	if (ret)
		return ret;
	i->num_heaps = h.heaps.count;
	h.heaps = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(
		i->num_heaps < DRM_SGXFIVE_HEAP_COUNT ? i->num_heaps : DRM_SGXFIVE_HEAP_COUNT,
		i->heaps);
	size = sizeof(h);
	return sgxfive_dev_query(dev, DRM_SGXFIVE_DEV_QUERY_HEAP_INFO, &h, &size);
}

static int driver_name(const struct sgxfive_backend *be, int fd, char *name, size_t len)
{
	struct drm_version v = { .name_len = len - 1, .name = name };
	int ret;

	memset(name, 0, len);
	ret = be->ioctl(be->priv, fd, DRM_IOCTL_VERSION, &v);
	if (ret)
		return ret;
	name[v.name_len < len - 1 ? v.name_len : len - 1] = '\0';
	return 0;
}

/* Make fd a device if it is an sgxfive node; on failure fd is left open. */
static int dev_init(const struct sgxfive_backend *be, int fd, const char *path, bool check,
		    struct sgxfive_dev **out)
{
	struct sgxfive_dev *dev;
	char name[32];
	int ret;

	ret = driver_name(be, fd, name, sizeof(name));
	if (!ret && check && strcmp(name, SGXFIVE_DRIVER_NAME) &&
	    strcmp(name, SGXFIVE_HOST_DRIVER_NAME))
		ret = -ENODEV;
	dev = ret ? NULL : calloc(1, sizeof(*dev));
	if (!ret && !dev)
		ret = -ENOMEM;
	if (ret)
		return ret;
	dev->be = be;
	dev->fd = fd;
	snprintf(dev->path, sizeof(dev->path), "%s", path);
	ret = query_all(dev);
	if (ret) {
		free(dev);
		/* A host node without sgxfive (no DEV_QUERY) is not ours. */
		return ret == -ENOTTY ? -ENODEV : ret;
	}
	*out = dev;
	return 0;
}

static int try_open(const struct sgxfive_backend *be, const char *path, bool check,
		    struct sgxfive_dev **out)
{
	int fd = be->open(be->priv, path);
	int ret;

	if (fd < 0)
		return fd;
	ret = dev_init(be, fd, path, check, out);
	if (ret)
		be->close(be->priv, fd);
	else
		(*out)->owns_fd = true;
	return ret;
}

/* A node the caller opened (IGT's drm_open_driver); closing the device leaves fd open. */
int sgxfive_dev_adopt(const struct sgxfive_backend *be, int fd, struct sgxfive_dev **out)
{
	*out = NULL;
	return dev_init(be ? be : sgxfive_os_backend(), fd, "(adopted)", true, out);
}

int sgxfive_dev_open(const struct sgxfive_backend *be, const char *path,
		     struct sgxfive_dev **out)
{
	char p[32];

	*out = NULL;
	if (!be)
		be = sgxfive_os_backend();
	if (path)
		return try_open(be, path, true, out);
	for (int minor = 128; minor < 192; minor++) {
		snprintf(p, sizeof(p), "/dev/dri/renderD%d", minor);
		if (!try_open(be, p, true, out))
			return 0;
	}
	return -ENODEV;
}

void sgxfive_dev_close(struct sgxfive_dev *dev)
{
	if (!dev)
		return;
	if (dev->owns_fd)
		dev->be->close(dev->be->priv, dev->fd);
	free(dev);
}

const struct sgxfive_info *sgxfive_dev_info(const struct sgxfive_dev *dev)
{
	return &dev->info;
}

const char *sgxfive_dev_path(const struct sgxfive_dev *dev)
{
	return dev->path;
}

const struct drm_sgxfive_heap *sgxfive_heap(const struct sgxfive_dev *dev, uint32_t id)
{
	if (id >= DRM_SGXFIVE_HEAP_COUNT || id >= dev->info.num_heaps ||
	    !dev->info.heaps[id].size)
		return NULL;
	return &dev->info.heaps[id];
}

/* ---- Buffer objects ----------------------------------------------------------------- */

int sgxfive_bo_create(struct sgxfive_dev *dev, uint64_t size, uint64_t flags,
		      struct sgxfive_bo **out)
{
	struct drm_sgxfive_create_bo_args a = { .size = size, .flags = flags };
	struct sgxfive_bo *bo;
	int ret = sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_BO, &a);

	if (ret)
		return ret;
	bo = calloc(1, sizeof(*bo));
	if (!bo) {
		struct drm_gem_close c = { .handle = a.handle };

		sgxfive_dev_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &c);
		return -ENOMEM;
	}
	bo->dev = dev;
	bo->handle = a.handle;
	bo->size = (size + SGXFIVE_PAGE_SIZE - 1) & ~(uint64_t)(SGXFIVE_PAGE_SIZE - 1);
	bo->flags = flags;
	*out = bo;
	return 0;
}

int sgxfive_bo_mmap(struct sgxfive_bo *bo, void **ptr)
{
	struct drm_sgxfive_get_bo_mmap_offset_args a = { .handle = bo->handle };
	const struct sgxfive_backend *be = bo->dev->be;
	int ret;

	if (!bo->map) {
		ret = sgxfive_dev_ioctl(bo->dev, DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET, &a);
		if (!ret)
			ret = be->mmap(be->priv, bo->dev->fd, a.offset, bo->size, &bo->map);
		if (ret)
			return ret;
	}
	*ptr = bo->map;
	return 0;
}

void sgxfive_bo_destroy(struct sgxfive_bo *bo)
{
	struct drm_gem_close c;

	if (!bo)
		return;
	if (bo->map)
		bo->dev->be->munmap(bo->dev->be->priv, bo->map, bo->size);
	c = (struct drm_gem_close){ .handle = bo->handle };
	sgxfive_dev_ioctl(bo->dev, DRM_IOCTL_GEM_CLOSE, &c);
	free(bo);
}

/* ---- VM contexts -------------------------------------------------------------------- */

int sgxfive_vm_create(struct sgxfive_dev *dev, struct sgxfive_vm **out)
{
	struct drm_sgxfive_create_vm_context_args a = { 0 };
	struct sgxfive_vm *vm = calloc(1, sizeof(*vm));
	int ret;

	if (!vm)
		return -ENOMEM;
	ret = sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT, &a);
	if (ret) {
		free(vm);
		return ret;
	}
	vm->dev = dev;
	vm->handle = a.handle;
	for (uint32_t i = 0; i < DRM_SGXFIVE_HEAP_COUNT; i++)
		vm->next[i] = dev->info.heaps[i].base;
	*out = vm;
	return 0;
}

void sgxfive_vm_destroy(struct sgxfive_vm *vm)
{
	struct drm_sgxfive_destroy_vm_context_args a;

	if (!vm)
		return;
	a = (struct drm_sgxfive_destroy_vm_context_args){ .handle = vm->handle };
	sgxfive_dev_ioctl(vm->dev, DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT, &a);
	free(vm);
}

uint32_t sgxfive_vm_handle(const struct sgxfive_vm *vm)
{
	return vm->handle;
}

struct sgxfive_dev *sgxfive_vm_dev(const struct sgxfive_vm *vm)
{
	return vm->dev;
}

int sgxfive_vm_alloc(struct sgxfive_vm *vm, uint32_t heap, uint64_t size, uint64_t align,
		     uint64_t *addr)
{
	const struct drm_sgxfive_heap *h = sgxfive_heap(vm->dev, heap);
	uint64_t a;

	if (!h || h->flags & DRM_SGXFIVE_HEAP_RESERVED || !size || size % SGXFIVE_PAGE_SIZE ||
	    align < SGXFIVE_PAGE_SIZE || align & (align - 1))
		return -EINVAL;
	a = (vm->next[heap] + align - 1) & ~(align - 1);
	if (a + size > h->base + h->size)
		return -ENOSPC;
	vm->next[heap] = a + size;
	*addr = a;
	return 0;
}

int sgxfive_vm_map(struct sgxfive_vm *vm, uint64_t addr, const struct sgxfive_bo *bo,
		   uint64_t offset, uint64_t size, uint32_t flags)
{
	struct drm_sgxfive_vm_map_args a = {
		.vm_context_handle = vm->handle, .flags = flags, .device_addr = addr,
		.handle = bo->handle, .offset = offset, .size = size,
	};

	return sgxfive_dev_ioctl(vm->dev, DRM_IOCTL_SGXFIVE_VM_MAP, &a);
}

int sgxfive_vm_unmap(struct sgxfive_vm *vm, uint64_t addr, uint64_t size)
{
	struct drm_sgxfive_vm_unmap_args a = {
		.vm_context_handle = vm->handle, .device_addr = addr, .size = size,
	};

	return sgxfive_dev_ioctl(vm->dev, DRM_IOCTL_SGXFIVE_VM_UNMAP, &a);
}

/* ---- Contexts and syncobjs ---------------------------------------------------------- */

int sgxfive_ctx_create(struct sgxfive_vm *vm, uint32_t *handle)
{
	struct drm_sgxfive_create_context_args a = {
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER,
		.priority = DRM_SGXFIVE_CTX_PRIORITY_NORMAL,
		.vm_context_handle = vm->handle,
	};
	int ret = sgxfive_dev_ioctl(vm->dev, DRM_IOCTL_SGXFIVE_CREATE_CONTEXT, &a);

	if (!ret)
		*handle = a.handle;
	return ret;
}

int sgxfive_ctx_destroy(struct sgxfive_dev *dev, uint32_t handle)
{
	struct drm_sgxfive_destroy_context_args a = { .handle = handle };

	return sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_DESTROY_CONTEXT, &a);
}

int sgxfive_syncobj_create(struct sgxfive_dev *dev, bool signaled, uint32_t *handle)
{
	struct drm_syncobj_create a = { .flags = signaled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0 };
	int ret = sgxfive_dev_ioctl(dev, DRM_IOCTL_SYNCOBJ_CREATE, &a);

	if (!ret)
		*handle = a.handle;
	return ret;
}

int sgxfive_syncobj_destroy(struct sgxfive_dev *dev, uint32_t handle)
{
	struct drm_syncobj_destroy a = { .handle = handle };

	return sgxfive_dev_ioctl(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &a);
}

/* DRM syncobj timeouts are absolute CLOCK_MONOTONIC nanoseconds. */
static int64_t deadline(int64_t timeout_ns)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + timeout_ns;
}

int sgxfive_syncobj_wait(struct sgxfive_dev *dev, uint32_t handle, uint64_t point,
			 int64_t timeout_ns)
{
	struct drm_syncobj_timeline_wait tw = {
		.handles = (uintptr_t)&handle, .points = (uintptr_t)&point,
		.timeout_nsec = deadline(timeout_ns), .count_handles = 1,
		.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
	};
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&handle, .timeout_nsec = tw.timeout_nsec,
		.count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
	};

	if (point)
		return sgxfive_dev_ioctl(dev, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw);
	return sgxfive_dev_ioctl(dev, DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

/* ---- Submission --------------------------------------------------------------------- */

int sgxfive_submit(struct sgxfive_dev *dev, const struct drm_sgxfive_job *jobs,
		   uint32_t count, uint32_t *failed)
{
	struct drm_sgxfive_submit_jobs_args a = {
		.jobs = DRM_SGXFIVE_OBJ_ARRAY(count, jobs),
	};
	int ret = sgxfive_dev_ioctl(dev, DRM_IOCTL_SGXFIVE_SUBMIT_JOBS, &a);

	if (ret && failed)
		*failed = a.jobs.count;
	return ret;
}

int sgxfive_submit_transfer(struct sgxfive_dev *dev, uint32_t ctx, uint32_t flags,
			    const struct drm_sgxfive_transfer_cmd *cmd, uint32_t wait,
			    uint32_t signal)
{
	struct drm_sgxfive_sync_op ops[2];
	uint32_t n = 0;
	struct drm_sgxfive_job job = {
		.type = DRM_SGXFIVE_JOB_TYPE_TRANSFER, .context_handle = ctx, .flags = flags,
		.cmd_stream = (uintptr_t)cmd, .cmd_stream_len = cmd ? sizeof(*cmd) : 0,
	};

	if (wait)
		ops[n++] = (struct drm_sgxfive_sync_op){
			.handle = wait, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_WAIT };
	if (signal)
		ops[n++] = (struct drm_sgxfive_sync_op){
			.handle = signal, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL };
	if (n)
		job.sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(n, ops);
	return sgxfive_submit(dev, &job, 1, NULL);
}
