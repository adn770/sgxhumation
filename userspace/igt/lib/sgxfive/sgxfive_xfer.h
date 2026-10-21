/* SPDX-License-Identifier: MIT */
/*
 * Transfer jobs from userspace: the pure builders of a FASTSCALE transfer
 * (design/sgx545_transfer.md §4.2 fill, §4.4 copy), the program-source
 * interface, and a per-context engine that places programs in the client's
 * VM and submits fills and copies.
 */
#ifndef SGXFIVE_XFER_H
#define SGXFIVE_XFER_H

#include "sgxfive.h"

/* ---- Pure builders ------------------------------------------------------------------ */

struct sgxfive_rect {
	uint32_t x0, y0, x1, y1;	/* pixels; x1 and y1 exclusive */
};

#define SGXFIVE_BGOBJ_DWORDS	17	/* background object, no layers (fill) */
#define SGXFIVE_BGOBJ1_DWORDS	23	/* ... one texture layer (copy) */
#define SGXFIVE_PBE_WORDS	5	/* emit1 src0/1, emit2 src0/1, sideband */
#define SGXFIVE_DOUTU_RANGE	0x80000	/* CBASE 0: the first 512 KiB of the code heap */
#define SGXFIVE_TEX_MAX		4096	/* 12-bit WIDTH/HEIGHT in T1 */

/*
 * DOUTU word U0 of a USE program at addr, in the code heap at heap_base, with
 * temps temporaries (§4.2.4). -EINVAL unless 8-byte aligned and in the window.
 */
int sgxfive_doutu(uint64_t addr, uint64_t heap_base, uint32_t temps, uint32_t *u0);

/* PBE state of an ARGB8888 linear destination (§4.2.5); stride in bytes. */
void sgxfive_pbe(uint32_t pbe[SGXFIVE_PBE_WORDS], const struct sgxfive_rect *r,
		 uint32_t stride, uint32_t addr);

/* Background objects (§4.2.3, §4.4.2); pri/sec are PDS addresses, *_data byte sizes. */
void sgxfive_bgobj_fill(uint32_t bg[SGXFIVE_BGOBJ_DWORDS], uint32_t pds_base, uint32_t pri,
			uint32_t pri_data, uint32_t sec, uint32_t sec_data,
			const struct sgxfive_rect *r);
void sgxfive_bgobj_copy(uint32_t bg[SGXFIVE_BGOBJ1_DWORDS], uint32_t pds_base, uint32_t pri,
			uint32_t pri_data, uint32_t sec, uint32_t sec_data,
			const struct sgxfive_rect *dst, const struct sgxfive_rect *src,
			uint32_t src_w, uint32_t src_h);

/* FloatIntDiv of the closed code: x / n truncated to 20 fraction bits, as IEEE single. */
uint32_t sgxfive_float_int_div(uint32_t x, uint32_t n);

/* DOUTT words T0..T3 of a linear ARGB8888 source (§4.4.3); stride in bytes. */
void sgxfive_tex_words(uint32_t t[4], uint32_t addr, uint32_t width, uint32_t height,
		       uint32_t stride);

/* What the register block of a FASTSCALE transfer depends on (§4.2.6). */
struct sgxfive_xfer_regs {
	struct sgxfive_rect rect;	/* destination rectangle: the render box */
	uint32_t bg;			/* background object */
	uint32_t req_base;		/* BIF_3D_REQ_BASE: 1 MiB window holding bg */
	uint32_t pixev_exec;		/* pixel-event PDS program */
	uint32_t pds_exec_base;		/* PDS_EXEC_BASE */
	uint32_t layers;		/* TSP layers in the background object */
};

void sgxfive_transfer_cmd(struct drm_sgxfive_transfer_cmd *cmd,
			  const struct sgxfive_xfer_regs *r);

/* ---- Program sources ---------------------------------------------------------------- */

/*
 * The GPU programs of a transfer. The ids, metadata and parameters are those
 * of the firmware file's transfer-queue templates (design/sgx545_fw_format.md
 * §9-§11): sgxfive_prog_asm_create() builds them from our own assembled
 * code, and a source backed by a .fw file would be a thin adapter.
 */
enum sgxfive_prog_id {
	SGXFIVE_PROG_USE_FILL = 0,	/* fill shader: mov o0, #colour */
	SGXFIVE_PROG_USE_SEC,		/* USE secondary (no work) */
	SGXFIVE_PROG_USE_EOT,		/* end of tile: PBE state, emitpix */
	SGXFIVE_PROG_USE_EOR,		/* end of render */
	SGXFIVE_PROG_PDS_PRIM,		/* PDS primary KICKONLY */
	SGXFIVE_PROG_PDS_SEC,		/* PDS secondary BASIC */
	SGXFIVE_PROG_PDS_PIXEVENT,	/* pixel-event PDS program */
	SGXFIVE_PROG_USE_COPY,		/* copy shader: mov o0, pa0 */
	SGXFIVE_PROG_PDS_PRIM_1SRC,	/* PDS primary single_source: texture fetch */
	SGXFIVE_PROG_COUNT,
};

enum sgxfive_prog_param {
	SGXFIVE_PARAM_COLOUR = 0,	/* fill colour, ARGB8888 */
	SGXFIVE_PARAM_PBE0,		/* PBE words 0..3 */
	SGXFIVE_PARAM_PBE1,
	SGXFIVE_PARAM_PBE2,
	SGXFIVE_PARAM_PBE3,
	SGXFIVE_PARAM_SIDEBAND,		/* PBE word 4 */
	SGXFIVE_PARAM_T0,		/* copy: DOUTT T0, T1, T2 (source address) */
	SGXFIVE_PARAM_T1,
	SGXFIVE_PARAM_SRC,
	SGXFIVE_PARAM_COUNT,
};

#define SGXFIVE_PROG_NO_STRADDLE	(1u << 0)	/* must not cross a 4 KiB page */

struct sgxfive_prog_meta {
	uint32_t heap;			/* DRM_SGXFIVE_HEAP_USSE_PIXEL or _PDS_PIXEL */
	uint32_t temps;			/* USE temporaries (DOUTU TRC) */
	uint32_t attrs;			/* PDS primary attributes */
	uint32_t size;			/* bytes */
	uint32_t align;			/* bytes */
	uint32_t data_size;		/* PDS data segment bytes */
	uint32_t flags;			/* SGXFIVE_PROG_* */
};

/* Where everything an image may refer to has been placed. */
struct sgxfive_prog_env {
	uint32_t params[SGXFIVE_PARAM_COUNT];
	uint64_t addr[SGXFIVE_PROG_COUNT];	/* 0: not placed */
	uint32_t doutu[SGXFIVE_PROG_COUNT];	/* U0 of each placed USE program */
	uint64_t heap_base[DRM_SGXFIVE_HEAP_COUNT];
};

struct sgxfive_prog_source;

struct sgxfive_prog_source_ops {
	/* -ENOENT if the source has no such program. */
	int (*meta)(struct sgxfive_prog_source *src, uint32_t id, struct sgxfive_prog_meta *m);
	/* Write the finished image of program id (meta.size bytes) to out. */
	int (*build)(struct sgxfive_prog_source *src, uint32_t id,
		     const struct sgxfive_prog_env *env, void *out);
	void (*destroy)(struct sgxfive_prog_source *src);
};

/* Embed as the first member of a provider's own struct. */
struct sgxfive_prog_source {
	const struct sgxfive_prog_source_ops *ops;
	const char *name;
};

/*
 * Our own programs: tools/usse-asm/progs/ assembled at generation time
 * (src/prog_asm_gen.h), with the TQ_META records and TQ_PATCHES of
 * design/sgx545_fw_format.md §9-§11. They equal the templates of the closed
 * libsrv_um.so bit for bit, so their images run on hardware; build() equals
 * fwload.build_tq() for the same placement and parameters. Needs no file.
 */
struct sgxfive_prog_source *sgxfive_prog_asm_create(void);

/*
 * Test double: builds every program from the words the design documents give
 * (§4.2.4, §4.2.7, §4.4.5). The closed template bodies it does not know
 * (USE secondary, pixel-event PDS) are left zero, so its images are for the
 * mock only and must never run on hardware.
 */
struct sgxfive_prog_source *sgxfive_prog_fake_create(void);
void sgxfive_prog_source_destroy(struct sgxfive_prog_source *src);

/* USSE_LIMM immediate split (fw_format §4) of a 64-bit instruction. */
uint64_t sgxfive_limm_set(uint64_t insn, uint32_t imm);
uint32_t sgxfive_limm_get(uint64_t insn);

/* The emitpix sideband split (fw_format §9.4) of a 64-bit instruction. */
uint64_t sgxfive_emitpix_sb_set(uint64_t insn, uint32_t sb);

/* ---- Transfer engine ---------------------------------------------------------------- */

/* A linear ARGB8888 surface at a GPU address of the engine's VM. */
struct sgxfive_surface {
	uint64_t addr;
	uint32_t width, height;
	uint32_t stride;		/* bytes */
};

struct sgxfive_xfer;

/*
 * Programs, background object and command of one context's transfers, placed
 * in a private 3-page BO mapped into the VM (PDS page, USE page, stream page).
 * One transfer in flight per engine: each job waits for the previous one
 * before its programs are rewritten.
 */
int sgxfive_xfer_create(struct sgxfive_vm *vm, uint32_t ctx, struct sgxfive_prog_source *src,
			struct sgxfive_xfer **out);
void sgxfive_xfer_destroy(struct sgxfive_xfer *x);

/* Addresses the engine chose, for tests and debugging. */
struct sgxfive_xfer_layout {
	uint64_t pds_page, use_page, stream_page;
	uint64_t prog[SGXFIVE_PROG_COUNT];
	uint64_t bg;
};
void sgxfive_xfer_get_layout(const struct sgxfive_xfer *x, struct sgxfive_xfer_layout *l);
/* The last command and program images written (for tests). */
const struct drm_sgxfive_transfer_cmd *sgxfive_xfer_last_cmd(const struct sgxfive_xfer *x);
const void *sgxfive_xfer_page(const struct sgxfive_xfer *x, uint32_t heap);

/*
 * Fill rect of dst with colour; copy the w x h rectangle at (sx, sy) of src to
 * rect of dst. signal (if non-zero) is a binary syncobj signalled on completion.
 */
int sgxfive_xfer_fill(struct sgxfive_xfer *x, const struct sgxfive_surface *dst,
		      const struct sgxfive_rect *rect, uint32_t colour, uint32_t signal);
int sgxfive_xfer_copy(struct sgxfive_xfer *x, const struct sgxfive_surface *src,
		      uint32_t sx, uint32_t sy, const struct sgxfive_surface *dst,
		      const struct sgxfive_rect *rect, uint32_t signal);
/* Wait for the engine's last job (timeout from now). */
int sgxfive_xfer_wait(struct sgxfive_xfer *x, int64_t timeout_ns);

#endif
