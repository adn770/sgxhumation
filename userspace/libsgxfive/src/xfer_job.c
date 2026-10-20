// SPDX-License-Identifier: MIT
/*
 * The transfer engine: one context's fill and copy jobs. Programs come from a
 * struct sgxfive_prog_source and are placed in a private three-page BO mapped
 * into the context's VM (one page each in PDS_PIXEL, USSE_PIXEL and GENERAL);
 * the background object goes in the GENERAL page, and the job is the register
 * block of design/sgx545_transfer.md §4.2.6.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_xfer.h"

enum { PG_PDS, PG_USE, PG_STREAM, PG_COUNT };

static const uint32_t page_heap[PG_COUNT] = {
	DRM_SGXFIVE_HEAP_PDS_PIXEL, DRM_SGXFIVE_HEAP_USSE_PIXEL, DRM_SGXFIVE_HEAP_GENERAL,
};

#define SLOT_ALIGN	16	/* placement granularity: reproduces the debugfs recipes */
#define BG_ALIGN	64
#define WAIT_NS		(10LL * 1000000000LL)

struct sgxfive_xfer {
	struct sgxfive_vm *vm;
	struct sgxfive_dev *dev;
	uint32_t ctx;
	struct sgxfive_prog_source *src;
	struct sgxfive_bo *bo;
	uint8_t *map;
	uint32_t done;		/* syncobj signalled by the engine's last job */
	bool pending;
	struct sgxfive_xfer_layout layout;
	struct drm_sgxfive_transfer_cmd cmd;
};

static const uint32_t fill_progs[] = {
	SGXFIVE_PROG_USE_FILL, SGXFIVE_PROG_USE_SEC, SGXFIVE_PROG_USE_EOT, SGXFIVE_PROG_USE_EOR,
	SGXFIVE_PROG_PDS_PRIM, SGXFIVE_PROG_PDS_SEC, SGXFIVE_PROG_PDS_PIXEVENT,
};
static const uint32_t copy_progs[] = {
	SGXFIVE_PROG_USE_COPY, SGXFIVE_PROG_USE_SEC, SGXFIVE_PROG_USE_EOT, SGXFIVE_PROG_USE_EOR,
	SGXFIVE_PROG_PDS_PRIM_1SRC, SGXFIVE_PROG_PDS_SEC, SGXFIVE_PROG_PDS_PIXEVENT,
};
#define NPROGS (sizeof(fill_progs) / sizeof(fill_progs[0]))

int sgxfive_xfer_create(struct sgxfive_vm *vm, uint32_t ctx, struct sgxfive_prog_source *src,
			struct sgxfive_xfer **out)
{
	struct sgxfive_dev *dev = sgxfive_vm_dev(vm);
	struct sgxfive_xfer *x;
	uint64_t *pages[PG_COUNT];
	void *map = NULL;
	int ret;

	if (!src)
		return -EINVAL;
	x = calloc(1, sizeof(*x));
	if (!x)
		return -ENOMEM;
	x->vm = vm;
	x->dev = dev;
	x->ctx = ctx;
	x->src = src;
	pages[PG_PDS] = &x->layout.pds_page;
	pages[PG_USE] = &x->layout.use_page;
	pages[PG_STREAM] = &x->layout.stream_page;
	ret = sgxfive_bo_create(dev, PG_COUNT * SGXFIVE_PAGE_SIZE,
				DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, &x->bo);
	if (!ret)
		ret = sgxfive_bo_mmap(x->bo, &map);
	for (int i = 0; !ret && i < PG_COUNT; i++) {
		ret = sgxfive_vm_alloc(vm, page_heap[i], SGXFIVE_PAGE_SIZE, SGXFIVE_PAGE_SIZE,
				       pages[i]);
		if (!ret)
			ret = sgxfive_vm_map(vm, *pages[i], x->bo, (uint64_t)i * SGXFIVE_PAGE_SIZE,
					     SGXFIVE_PAGE_SIZE, 0);
	}
	if (!ret)
		ret = sgxfive_syncobj_create(dev, false, &x->done);
	if (ret) {
		sgxfive_xfer_destroy(x);
		return ret;
	}
	x->map = map;
	*out = x;
	return 0;
}

void sgxfive_xfer_destroy(struct sgxfive_xfer *x)
{
	const uint64_t pages[PG_COUNT] = {
		x ? x->layout.pds_page : 0, x ? x->layout.use_page : 0,
		x ? x->layout.stream_page : 0,
	};

	if (!x)
		return;
	if (x->pending)
		sgxfive_xfer_wait(x, WAIT_NS);
	for (int i = 0; i < PG_COUNT; i++)
		if (pages[i])
			sgxfive_vm_unmap(x->vm, pages[i], SGXFIVE_PAGE_SIZE);
	if (x->done)
		sgxfive_syncobj_destroy(x->dev, x->done);
	sgxfive_bo_destroy(x->bo);
	free(x);
}

void sgxfive_xfer_get_layout(const struct sgxfive_xfer *x, struct sgxfive_xfer_layout *l)
{
	*l = x->layout;
}

const struct drm_sgxfive_transfer_cmd *sgxfive_xfer_last_cmd(const struct sgxfive_xfer *x)
{
	return &x->cmd;
}

const void *sgxfive_xfer_page(const struct sgxfive_xfer *x, uint32_t heap)
{
	for (int i = 0; i < PG_COUNT; i++)
		if (page_heap[i] == heap)
			return x->map + i * SGXFIVE_PAGE_SIZE;
	return NULL;
}

int sgxfive_xfer_wait(struct sgxfive_xfer *x, int64_t timeout_ns)
{
	int ret;

	if (!x->pending)
		return 0;
	ret = sgxfive_syncobj_wait(x->dev, x->done, 0, timeout_ns);
	if (!ret)
		x->pending = false;
	return ret;
}

static int page_of(uint32_t heap)
{
	for (int i = 0; i < PG_COUNT; i++)
		if (page_heap[i] == heap)
			return i;
	return -1;
}

/* Place, build and write the programs of one job; fill env with their addresses. */
static int build_programs(struct sgxfive_xfer *x, const uint32_t *ids,
			  struct sgxfive_prog_env *env, struct sgxfive_prog_meta *m)
{
	const struct sgxfive_info *info = sgxfive_dev_info(x->dev);
	const uint64_t page_addr[PG_COUNT] = {
		x->layout.pds_page, x->layout.use_page, x->layout.stream_page,
	};
	uint32_t cursor[PG_COUNT] = { 0 };
	int ret;

	memset(x->layout.prog, 0, sizeof(x->layout.prog));
	memset(env->addr, 0, sizeof(env->addr));
	memset(env->doutu, 0, sizeof(env->doutu));
	for (uint32_t i = 0; i < DRM_SGXFIVE_HEAP_COUNT; i++)
		env->heap_base[i] = info->heaps[i].base;

	for (unsigned int k = 0; k < NPROGS; k++) {
		const uint32_t id = ids[k];
		uint32_t align, off;
		int pg;

		ret = x->src->ops->meta(x->src, id, &m[id]);
		if (ret)
			return ret;
		pg = page_of(m[id].heap);
		if (pg < 0 || pg == PG_STREAM || !m[id].align || m[id].align & (m[id].align - 1))
			return -EINVAL;
		align = m[id].align > SLOT_ALIGN ? m[id].align : SLOT_ALIGN;
		off = (cursor[pg] + align - 1) & ~(align - 1);
		if (off + m[id].size > SGXFIVE_PAGE_SIZE)
			return -ENOSPC;		/* NO_STRADDLE holds: one page each */
		cursor[pg] = off + m[id].size;
		env->addr[id] = page_addr[pg] + off;
		x->layout.prog[id] = env->addr[id];
		if (pg == PG_USE) {
			ret = sgxfive_doutu(env->addr[id], info->heaps[m[id].heap].base,
					    m[id].temps, &env->doutu[id]);
			if (ret)
				return ret;
		}
	}
	for (unsigned int k = 0; k < NPROGS; k++) {
		const uint32_t id = ids[k];
		uint8_t *dst = x->map + page_of(m[id].heap) * SGXFIVE_PAGE_SIZE +
			       (env->addr[id] - page_addr[page_of(m[id].heap)]);

		ret = x->src->ops->build(x->src, id, env, dst);
		if (ret)
			return ret;
	}
	return 0;
}

static int submit(struct sgxfive_xfer *x, uint32_t signal)
{
	struct drm_sgxfive_sync_op ops[2] = {
		{ .handle = x->done, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL },
		{ .handle = signal, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL },
	};
	struct drm_sgxfive_job job = {
		.type = DRM_SGXFIVE_JOB_TYPE_TRANSFER, .context_handle = x->ctx,
		.cmd_stream = (uintptr_t)&x->cmd, .cmd_stream_len = sizeof(x->cmd),
		.sync_ops = DRM_SGXFIVE_OBJ_ARRAY(signal ? 2 : 1, ops),
	};
	int ret = sgxfive_submit(x->dev, &job, 1, NULL);

	if (!ret)
		x->pending = true;
	return ret;
}

/* The destination checks of fwload.fill_params (design/sgx545_fw_format.md §9.6). */
static int check_dst(const struct sgxfive_surface *s, const struct sgxfive_rect *r)
{
	if (r->x0 >= r->x1 || r->y0 >= r->y1 || r->x1 > s->width || r->y1 > s->height ||
	    r->x1 > 0x2000 || r->y1 > 0x2000)
		return -EINVAL;
	if (s->stride % 8 || s->stride < 4ull * r->x1 || s->stride > 0x8000)
		return -EINVAL;
	if (s->addr % 4 || s->addr + (uint64_t)s->stride * (r->y1 - 1) + 4ull * r->x1 >
	    1ull << 32)
		return -EINVAL;
	return 0;
}

static int run(struct sgxfive_xfer *x, const uint32_t *ids, struct sgxfive_prog_env *env,
	       const struct sgxfive_rect *rect, const struct sgxfive_rect *src_rect,
	       uint32_t signal)
{
	struct sgxfive_prog_meta m[SGXFIVE_PROG_COUNT];
	uint32_t bg[SGXFIVE_BGOBJ1_DWORDS];
	const uint32_t pri = ids == fill_progs ? SGXFIVE_PROG_PDS_PRIM
					       : SGXFIVE_PROG_PDS_PRIM_1SRC;
	struct sgxfive_xfer_regs regs;
	uint32_t P;
	int ret;

	ret = sgxfive_xfer_wait(x, WAIT_NS);	/* one job in flight: programs are reused */
	if (ret)
		return ret;
	ret = build_programs(x, ids, env, m);
	if (ret)
		return ret;
	P = (uint32_t)x->layout.pds_page;
	x->layout.bg = x->layout.stream_page;	/* offset 0: BG_ALIGN holds */
	if (src_rect) {
		sgxfive_bgobj_copy(bg, P, (uint32_t)env->addr[pri], m[pri].data_size,
				   (uint32_t)env->addr[SGXFIVE_PROG_PDS_SEC],
				   m[SGXFIVE_PROG_PDS_SEC].data_size, rect, src_rect,
				   src_rect->x1, src_rect->y1);
		memcpy(x->map + PG_STREAM * SGXFIVE_PAGE_SIZE, bg, SGXFIVE_BGOBJ1_DWORDS * 4);
	} else {
		sgxfive_bgobj_fill(bg, P, (uint32_t)env->addr[pri], m[pri].data_size,
				   (uint32_t)env->addr[SGXFIVE_PROG_PDS_SEC],
				   m[SGXFIVE_PROG_PDS_SEC].data_size, rect);
		memcpy(x->map + PG_STREAM * SGXFIVE_PAGE_SIZE, bg, SGXFIVE_BGOBJ_DWORDS * 4);
	}
	regs = (struct sgxfive_xfer_regs){
		.rect = *rect, .bg = (uint32_t)x->layout.bg,
		.req_base = (uint32_t)x->layout.bg & ~0xfffffu,
		.pixev_exec = (uint32_t)env->addr[SGXFIVE_PROG_PDS_PIXEVENT],
		.pds_exec_base = P, .layers = src_rect ? 1 : 0,
	};
	sgxfive_transfer_cmd(&x->cmd, &regs);
	return submit(x, signal);
}

int sgxfive_xfer_fill(struct sgxfive_xfer *x, const struct sgxfive_surface *dst,
		      const struct sgxfive_rect *rect, uint32_t colour, uint32_t signal)
{
	struct sgxfive_prog_env env = { 0 };
	int ret = check_dst(dst, rect);

	if (ret)
		return ret;
	env.params[SGXFIVE_PARAM_COLOUR] = colour;
	sgxfive_pbe(&env.params[SGXFIVE_PARAM_PBE0], rect, dst->stride, (uint32_t)dst->addr);
	return run(x, fill_progs, &env, rect, NULL, signal);
}

int sgxfive_xfer_copy(struct sgxfive_xfer *x, const struct sgxfive_surface *src,
		      uint32_t sx, uint32_t sy, const struct sgxfive_surface *dst,
		      const struct sgxfive_rect *rect, uint32_t signal)
{
	struct sgxfive_prog_env env = { 0 };
	struct sgxfive_rect sr;
	uint32_t w, h, t[4];
	uint64_t saddr;
	int ret = check_dst(dst, rect);

	if (ret)
		return ret;
	w = rect->x1 - rect->x0;
	h = rect->y1 - rect->y0;
	/* The source texture is the copied rectangle itself (fw_format §10.7). */
	if (w > SGXFIVE_TEX_MAX || h > SGXFIVE_TEX_MAX || sx + w > src->width ||
	    sy + h > src->height || src->stride % 4 || src->stride < 4ull * w ||
	    src->stride > 0x8000 || src->addr % 4)
		return -EINVAL;
	saddr = src->addr + (uint64_t)sy * src->stride + 4ull * sx;
	if (saddr + (uint64_t)src->stride * (h - 1) + 4ull * w > 1ull << 32)
		return -EINVAL;
	sgxfive_pbe(&env.params[SGXFIVE_PARAM_PBE0], rect, dst->stride, (uint32_t)dst->addr);
	sgxfive_tex_words(t, (uint32_t)saddr, w, h, src->stride);
	env.params[SGXFIVE_PARAM_T0] = t[0];
	env.params[SGXFIVE_PARAM_T1] = t[1];
	env.params[SGXFIVE_PARAM_SRC] = t[2];
	sr = (struct sgxfive_rect){ 0, 0, w, h };
	return run(x, copy_progs, &env, rect, &sr, signal);
}

/* Any program source: the assembled one, the fake, or one wrapping another. */
void sgxfive_prog_source_destroy(struct sgxfive_prog_source *src)
{
	if (src && src->ops->destroy)
		src->ops->destroy(src);
}
