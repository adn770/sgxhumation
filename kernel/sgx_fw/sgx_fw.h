/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * SGX545 firmware loader: parses sgx545_ukernel.fw (format v1) and produces what
 * a driver uploads and programs for a given device address layout.
 * See design/sgx545_fw_format.md; tools/fwcutter/fwload.py is the reference.
 *
 * Nothing here allocates. sgx_fw_parse() validates the whole file and keeps
 * pointers into it, so the file data must outlive the struct sgx_fw.
 */
#ifndef SGX_FW_H
#define SGX_FW_H

#include "sgx_fw_types.h"

#define SGX_FW_MAX_BUFFERS	16	/* buffer ids are 0..15 */
#define SGX_FW_MAX_HEAPS	16	/* heap ids are 0..15 */
#define SGX_FW_META_WORDS	31
#define SGX_FW_DEV_DATA_WORDS	24

/* Buffer and heap ids the layout checks need (format §3). */
#define SGX_FW_UKERNEL		0
#define SGX_FW_HEAP_KERNEL_CODE	2

/* VALUES lists (format §5). */
enum sgx_fw_list {
	SGX_FW_PART1,
	SGX_FW_PART2,
	SGX_FW_DEINIT,
	SGX_FW_DEV_DATA,
	SGX_FW_NUM_LISTS,
};

/* EDM_META word indices (format §5). */
#define SGX_FW_META_KICK		0	/* 11 kick addresses */
#define SGX_FW_META_STRUCT_SIZES	11	/* 13 MKIF struct sizes */
#define SGX_FW_META_EDM_TASK_REGS	24	/* 2 registers */
#define SGX_FW_META_CLKGATE		26	/* ctl, ctl2, status reg, mask */
#define SGX_FW_META_CACHE_CONTROL	30

struct sgx_fw_buffer {
	u8 id;
	u8 heap;
	u8 pds_rows;		/* PDS data segment, rows of 4 dwords */
	u32 size;
	u32 align;
	const u8 *image;	/* template, zero-padded to size; may be NULL */
	u32 image_size;
};

/* A run of 16-byte expression records inside the file. */
struct sgx_fw_exprs {
	const u8 *rec;
	u32 count;
};

struct sgx_fw {
	u8 ddk_major;
	u8 ddk_minor;
	u32 ddk_build;
	u32 build_options;
	unsigned int nbuffers;
	struct sgx_fw_buffer buffers[SGX_FW_MAX_BUFFERS];	/* file order */
	struct sgx_fw_exprs patches;
	struct sgx_fw_exprs values[SGX_FW_NUM_LISTS];
	u32 meta[SGX_FW_META_WORDS];	/* zero without an EDM_META section */
};

/* Where the driver placed things; a bit in each mask marks an entry as set. */
struct sgx_fw_layout {
	u32 addr[SGX_FW_MAX_BUFFERS];		/* device virtual, by buffer id */
	u32 addr_set;
	u32 heap_base[SGX_FW_MAX_HEAPS];	/* by heap id */
	u32 heap_set;
	u32 pd_phys;		/* the EDM page directory, physical */
};

/* One register write of an init script. */
struct sgx_fw_write {
	u32 reg;
	u32 val;
};

u32 sgx_fw_crc32(const u8 *p, size_t len);
u64 sgx_fw_limm_set(u64 insn, u32 imm);
u32 sgx_fw_limm_get(u64 insn);

int sgx_fw_parse(struct sgx_fw *fw, const u8 *data, size_t len);
const struct sgx_fw_buffer *sgx_fw_buffer(const struct sgx_fw *fw,
					  unsigned int id);

int sgx_fw_check(const struct sgx_fw *fw, const struct sgx_fw_layout *l);
int sgx_fw_build(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		 u8 *const bufs[SGX_FW_MAX_BUFFERS]);
int sgx_fw_script(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		  enum sgx_fw_list list, struct sgx_fw_write *out,
		  unsigned int max);
int sgx_fw_dev_data(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		    u32 out[SGX_FW_DEV_DATA_WORDS]);
int sgx_fw_place(const struct sgx_fw_buffer *bufs, unsigned int n,
		 const u8 *order, unsigned int norder, u32 start, u32 page,
		 struct sgx_fw_layout *l);

#endif /* SGX_FW_H */
