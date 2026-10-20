/* SPDX-License-Identifier: MIT */
/*
 * libsgxfive: a thin C11 wrapper over the sgxfive render-node ioctls
 * (kernel/uapi/sgxfive_drm.h), for tests and bring-up; not the open
 * userspace a driver upstreaming needs (that is Mesa).
 *
 * Every function returning int returns 0 or a negative errno. All system
 * calls go through a struct sgxfive_backend, so the same code runs against
 * the kernel (sgxfive_os_backend()) or the in-process mock (sgxfive_mock.h).
 */
#ifndef SGXFIVE_H
#define SGXFIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sgxfive_drm.h"

#define SGXFIVE_DRIVER_NAME	"sgxfive"
/* While sgxfive is hosted by gma500 (Cedarview), its render node is gma500's. */
#define SGXFIVE_HOST_DRIVER_NAME	"gma500"
#define SGXFIVE_PAGE_SIZE	4096u

/* ---- System-call backend ------------------------------------------------------------ */

struct sgxfive_backend {
	int (*open)(void *priv, const char *path);		/* fd or -errno */
	int (*close)(void *priv, int fd);
	int (*ioctl)(void *priv, int fd, unsigned long req, void *arg);
	int (*mmap)(void *priv, int fd, uint64_t offset, size_t len, void **out);
	int (*munmap)(void *priv, void *addr, size_t len);
	void *priv;
};

/* open(2), ioctl(2) with EINTR/EAGAIN retry, mmap(2) on the render node. */
const struct sgxfive_backend *sgxfive_os_backend(void);

/* ---- Device ------------------------------------------------------------------------- */

struct sgxfive_dev;

/* Everything DEV_QUERY reports; heaps beyond SGXFIVE's known count are not copied. */
struct sgxfive_info {
	struct drm_sgxfive_dev_query_gpu_info gpu;
	struct drm_sgxfive_dev_query_fw_info fw;
	struct drm_sgxfive_dev_query_job_info job;
	struct drm_sgxfive_dev_query_runtime_info runtime;
	struct drm_sgxfive_heap heaps[DRM_SGXFIVE_HEAP_COUNT];
	uint32_t num_heaps;		/* as reported by the kernel */
};

/*
 * Open path, or with path NULL the first /dev/dri/renderD128..191 whose
 * DRM_IOCTL_VERSION name is "sgxfive" (-ENODEV if none). Runs every query.
 */
int sgxfive_dev_open(const struct sgxfive_backend *be, const char *path,
		     struct sgxfive_dev **out);
int sgxfive_dev_adopt(const struct sgxfive_backend *be, int fd, struct sgxfive_dev **out);
void sgxfive_dev_close(struct sgxfive_dev *dev);
const struct sgxfive_info *sgxfive_dev_info(const struct sgxfive_dev *dev);
const char *sgxfive_dev_path(const struct sgxfive_dev *dev);
int sgxfive_dev_ioctl(struct sgxfive_dev *dev, unsigned long req, void *arg);

/* One raw DEV_QUERY: *size in = buffer size, out = bytes written or needed. */
int sgxfive_dev_query(struct sgxfive_dev *dev, uint32_t type, void *buf, uint32_t *size);

/* The heap with this id, or NULL if absent (size 0) or unknown. */
const struct drm_sgxfive_heap *sgxfive_heap(const struct sgxfive_dev *dev, uint32_t id);

/* ---- Buffer objects ----------------------------------------------------------------- */

struct sgxfive_bo {
	struct sgxfive_dev *dev;
	uint32_t handle;
	uint64_t size;
	uint64_t flags;
	void *map;			/* CPU mapping once sgxfive_bo_mmap() succeeded */
};

int sgxfive_bo_create(struct sgxfive_dev *dev, uint64_t size, uint64_t flags,
		      struct sgxfive_bo **out);
int sgxfive_bo_mmap(struct sgxfive_bo *bo, void **ptr);
void sgxfive_bo_destroy(struct sgxfive_bo *bo);

/* ---- VM contexts -------------------------------------------------------------------- */

struct sgxfive_vm;

int sgxfive_vm_create(struct sgxfive_dev *dev, struct sgxfive_vm **out);
void sgxfive_vm_destroy(struct sgxfive_vm *vm);
uint32_t sgxfive_vm_handle(const struct sgxfive_vm *vm);
struct sgxfive_dev *sgxfive_vm_dev(const struct sgxfive_vm *vm);

/*
 * Reserve size bytes (page multiple) of GPU address space in a user heap, with
 * align a power of two >= 4 KiB; a bump allocator, never reused.
 */
int sgxfive_vm_alloc(struct sgxfive_vm *vm, uint32_t heap, uint64_t size, uint64_t align,
		     uint64_t *addr);
int sgxfive_vm_map(struct sgxfive_vm *vm, uint64_t addr, const struct sgxfive_bo *bo,
		   uint64_t offset, uint64_t size, uint32_t flags);
int sgxfive_vm_unmap(struct sgxfive_vm *vm, uint64_t addr, uint64_t size);

/* ---- Contexts and syncobjs ---------------------------------------------------------- */

int sgxfive_ctx_create(struct sgxfive_vm *vm, uint32_t *handle);	/* TRANSFER */
int sgxfive_ctx_destroy(struct sgxfive_dev *dev, uint32_t handle);

int sgxfive_syncobj_create(struct sgxfive_dev *dev, bool signaled, uint32_t *handle);
int sgxfive_syncobj_destroy(struct sgxfive_dev *dev, uint32_t handle);
/*
 * Wait for a binary syncobj (point 0) or a timeline point, timeout_ns from now
 * (-ETIME on timeout). With WAIT_FOR_SUBMIT, so an unsubmitted point waits too.
 */
int sgxfive_syncobj_wait(struct sgxfive_dev *dev, uint32_t handle, uint64_t point,
			 int64_t timeout_ns);

/* ---- Submission --------------------------------------------------------------------- */

/* On error *failed (if not NULL) is the index of the failing job. */
int sgxfive_submit(struct sgxfive_dev *dev, const struct drm_sgxfive_job *jobs,
		   uint32_t count, uint32_t *failed);

/*
 * One transfer job: cmd NULL with DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY for a
 * dummy. signal (if non-zero) is a binary syncobj to signal on completion,
 * wait (if non-zero) one to wait for first.
 */
int sgxfive_submit_transfer(struct sgxfive_dev *dev, uint32_t ctx, uint32_t flags,
			    const struct drm_sgxfive_transfer_cmd *cmd, uint32_t wait,
			    uint32_t signal);

#endif
