// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * Helpers for the sgxfive render node (PowerVR SGX545 on Cedarview, hosted
 * by gma500): raw ioctls, as the tests in tests/sgxfive/ exercise the UAPI.
 */
#ifndef IGT_SGXFIVE_H
#define IGT_SGXFIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "sgxfive_drm.h"

#define SGXFIVE_PAGE_SIZE 4096

int igt_sgxfive_open(void);

int __igt_sgxfive_dev_query(int fd, uint32_t type, void *data, uint32_t *size);
void igt_sgxfive_dev_query(int fd, uint32_t type, void *data, uint32_t size);
uint32_t igt_sgxfive_heaps(int fd, struct drm_sgxfive_heap *heaps, uint32_t max);
struct drm_sgxfive_heap igt_sgxfive_heap(int fd, uint32_t id);

int __igt_sgxfive_create_bo(int fd, uint64_t *size, uint64_t flags, uint32_t *handle);
uint32_t igt_sgxfive_create_bo(int fd, uint64_t size, uint64_t flags);
int __igt_sgxfive_mmap_offset(int fd, uint32_t handle, uint64_t *offset);
void *igt_sgxfive_mmap_bo(int fd, uint32_t handle, uint64_t size);

int __igt_sgxfive_create_vm(int fd, uint32_t *vm);
uint32_t igt_sgxfive_create_vm(int fd);
int __igt_sgxfive_destroy_vm(int fd, uint32_t vm);
void igt_sgxfive_destroy_vm(int fd, uint32_t vm);
int __igt_sgxfive_vm_map(int fd, uint32_t vm, uint64_t addr, uint32_t bo, uint64_t offset,
			 uint64_t size, uint32_t flags);
void igt_sgxfive_vm_map(int fd, uint32_t vm, uint64_t addr, uint32_t bo, uint64_t offset,
			uint64_t size, uint32_t flags);
int __igt_sgxfive_vm_unmap(int fd, uint32_t vm, uint64_t addr, uint64_t size);
void igt_sgxfive_vm_unmap(int fd, uint32_t vm, uint64_t addr, uint64_t size);

int __igt_sgxfive_create_ctx(int fd, struct drm_sgxfive_create_context_args *args);
uint32_t igt_sgxfive_create_ctx(int fd, uint32_t vm);
int __igt_sgxfive_destroy_ctx(int fd, uint32_t ctx);
void igt_sgxfive_destroy_ctx(int fd, uint32_t ctx);

int __igt_sgxfive_submit(int fd, struct drm_sgxfive_job *jobs, uint32_t count,
			 uint32_t *failed);
struct drm_sgxfive_job igt_sgxfive_dummy_job(uint32_t ctx);
int __igt_sgxfive_submit_dummy(int fd, uint32_t ctx, uint32_t wait, uint32_t signal);
void igt_sgxfive_submit_dummy(int fd, uint32_t ctx, uint32_t wait, uint32_t signal);
int igt_sgxfive_wait(int fd, uint32_t syncobj, uint64_t point, uint32_t timeout_ms);

#endif /* IGT_SGXFIVE_H */
