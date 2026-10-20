/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * SGX545 firmware loader: parses sgx545_ukernel.fw (formats v1 to v3) and
 * produces what a driver uploads and programs for a given device address
 * layout; with v2, also the transfer-queue programs of a solid fill, and with
 * v3 those of a copy.
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

/*
 * Transfer-queue templates (format §9, §10): ids 0..15 and nine per-transfer
 * params in v3; a v2 file may only use ids 0..7 and params 0..5.
 */
#define SGX_FW_TQ_MAX		16
#define SGX_FW_TQ_NUM_PARAMS	9
#define SGX_FW_TQ_NO_STRADDLE	1	/* template flag: within one 4 KiB page */

enum sgx_fw_tq_id {
	SGX_FW_TQ_USE_FILL,
	SGX_FW_TQ_USE_SEC,
	SGX_FW_TQ_USE_EOT,
	SGX_FW_TQ_USE_EOR,
	SGX_FW_TQ_PDS_PRIM,
	SGX_FW_TQ_PDS_SEC,
	SGX_FW_TQ_PDS_PIXEVENT,
	SGX_FW_TQ_USE_COPY,		/* v3 */
	SGX_FW_TQ_PDS_PRIM_1SRC,	/* v3 */
};

/* The templates a fill or a copy runs: sgx_fw_tq_need() checks them placed. */
#define SGX_FW_TQ_FILL_SET						\
	(1u << SGX_FW_TQ_USE_FILL | 1u << SGX_FW_TQ_USE_SEC |		\
	 1u << SGX_FW_TQ_USE_EOT | 1u << SGX_FW_TQ_USE_EOR |		\
	 1u << SGX_FW_TQ_PDS_PRIM | 1u << SGX_FW_TQ_PDS_SEC |		\
	 1u << SGX_FW_TQ_PDS_PIXEVENT)
#define SGX_FW_TQ_COPY_SET						\
	(1u << SGX_FW_TQ_USE_COPY | 1u << SGX_FW_TQ_USE_SEC |		\
	 1u << SGX_FW_TQ_USE_EOT | 1u << SGX_FW_TQ_USE_EOR |		\
	 1u << SGX_FW_TQ_PDS_PRIM_1SRC | 1u << SGX_FW_TQ_PDS_SEC |	\
	 1u << SGX_FW_TQ_PDS_PIXEVENT)

/* VALUES lists (format §5, §9.5). */
enum sgx_fw_list {
	SGX_FW_PART1,
	SGX_FW_PART2,
	SGX_FW_DEINIT,
	SGX_FW_DEV_DATA,
	SGX_FW_TQ_CMD,		/* v2: SGXMKIF_TRANSFERCMD words, sgx_fw_tq_cmd() */
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

/* A transfer-queue template: its TQ_META record and its TQ_TEMPLATE image. */
struct sgx_fw_tq {
	u8 heap;
	u8 temps;		/* USE temporaries, for the DOUTU word */
	u8 pds_attrs;		/* attributes a PDS program's task receives */
	u8 flags;		/* SGX_FW_TQ_NO_STRADDLE */
	u16 pds_data;		/* PDS data segment, bytes; 0 for USE code */
	u32 size;
	u32 align;
	const u8 *image;	/* template, zero-padded to size; may be NULL */
	u32 image_size;
};

struct sgx_fw {
	u8 version;		/* format version, 1 to 3 */
	u8 ddk_major;
	u8 ddk_minor;
	u32 ddk_build;
	u32 build_options;
	unsigned int nbuffers;
	struct sgx_fw_buffer buffers[SGX_FW_MAX_BUFFERS];	/* file order */
	struct sgx_fw_exprs patches;
	struct sgx_fw_exprs values[SGX_FW_NUM_LISTS];
	u32 meta[SGX_FW_META_WORDS];	/* zero without an EDM_META section */
	u32 tq_present;			/* a bit per template id with TQ_META */
	struct sgx_fw_tq tq[SGX_FW_TQ_MAX];
	struct sgx_fw_exprs tq_patches;
};

/* Where the driver placed things; a bit in each mask marks an entry as set. */
struct sgx_fw_layout {
	u32 addr[SGX_FW_MAX_BUFFERS];		/* device virtual, by buffer id */
	u32 addr_set;
	u32 heap_base[SGX_FW_MAX_HEAPS];	/* by heap id */
	u32 heap_set;
	u32 pd_phys;		/* the EDM page directory, physical */
	u32 tq_addr[SGX_FW_TQ_MAX];		/* by template id */
	u32 tq_set;
};

/*
 * An ARGB8888 fill of a width x height rectangle at (x, y) of a linear
 * surface (format §9.6); the PBE clips to the rectangle.
 */
struct sgx_fw_tq_fill {
	u32 dest;		/* device virtual address of pixel (0, 0) */
	u32 width;
	u32 height;
	u32 stride;		/* bytes per line */
	u32 colour;		/* 0xAARRGGBB */
	u32 x;
	u32 y;
};

/*
 * An ARGB8888 copy (format §10.4): the linear width x height texture whose
 * first texel is at src lands 1:1 on the rectangle at (x, y) of the linear
 * destination surface whose pixel (0, 0) is at dst.
 */
struct sgx_fw_tq_copy {
	u32 src;		/* device virtual address of the first texel */
	u32 dst;		/* device virtual address of pixel (0, 0) */
	u32 width;		/* 1..4096 */
	u32 height;		/* 1..4096 */
	u32 src_stride;		/* bytes per source line */
	u32 dst_stride;		/* bytes per destination line */
	u32 x;
	u32 y;
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

u64 sgx_fw_emitpix_sb_set(u64 insn, u32 sb);
u32 sgx_fw_emitpix_sb_get(u64 insn);
const struct sgx_fw_tq *sgx_fw_tq(const struct sgx_fw *fw, unsigned int id);
int sgx_fw_tq_fill_params(const struct sgx_fw_tq_fill *f,
			  u32 params[SGX_FW_TQ_NUM_PARAMS]);
int sgx_fw_tq_copy_params(const struct sgx_fw_tq_copy *c,
			  u32 params[SGX_FW_TQ_NUM_PARAMS]);
int sgx_fw_tq_check(const struct sgx_fw *fw, const struct sgx_fw_layout *l);
int sgx_fw_tq_need(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		   u32 set);
int sgx_fw_tq_build(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		    const u32 params[SGX_FW_TQ_NUM_PARAMS],
		    u8 *const progs[SGX_FW_TQ_MAX]);
int sgx_fw_tq_cmd(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		  const u32 params[SGX_FW_TQ_NUM_PARAMS],
		  struct sgx_fw_write *out, unsigned int max);

#endif /* SGX_FW_H */
