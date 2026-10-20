/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Types and error codes for sgx_fw.c, so the same core builds in the kernel
 * and in userspace (fwdump, the equivalence tests).
 */
#ifndef SGX_FW_TYPES_H
#define SGX_FW_TYPES_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#endif /* SGX_FW_TYPES_H */
