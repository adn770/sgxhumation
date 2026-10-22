/* SPDX-License-Identifier: MIT */
/*
 * Non-Linux build shim: the one sync_file query libsgxfive and its mock use,
 * the layout of the kernel's struct sync_file_info (no per-fence array read).
 */
#ifndef SGXFIVE_COMPAT_LINUX_SYNC_FILE_H
#define SGXFIVE_COMPAT_LINUX_SYNC_FILE_H
#include <stdint.h>
#include <sys/ioctl.h>

struct sync_file_info {
	char name[32];
	int32_t status;		/* 1 signalled, 0 active, < 0 the fence's error */
	uint32_t flags;
	uint32_t num_fences;
	uint32_t pad;
	uint64_t sync_fence_info;
};

#define SYNC_IOC_MAGIC		'>'
#define SYNC_IOC_FILE_INFO	_IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
#endif
