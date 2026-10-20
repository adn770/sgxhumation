// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * Transfers with real GPU programs: fills and copies through libsgxfive's
 * engine (lib/sgxfive/, our own assembled programs), every destination dword
 * checked against a sentinel, and the isolation rules between address
 * spaces. A fault costs the faulting context and counts against its file's
 * budget, so each isolation subtest opens its own file.
 */
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "igt.h"
#include "igt_sgxfive.h"
#include "sgxfive/sgxfive_xfer.h"

#define SENTINEL	0xdeadbeefu
#define WAIT_NS		(5LL * 1000000000LL)
#define FAULT_MS	300		/* a fault is found within two 50 ms polls */

struct gpu {
	int fd;
	struct sgxfive_dev *dev;
	struct sgxfive_prog_source *progs;
};

struct side {
	struct sgxfive_vm *vm;
	uint32_t ctx;
	struct sgxfive_xfer *x;
};

static struct gpu gpu_open(void)
{
	struct gpu g = { .fd = igt_sgxfive_open() };

	igt_assert_eq(sgxfive_dev_adopt(NULL, g.fd, &g.dev), 0);
	g.progs = sgxfive_prog_asm_create();
	igt_assert(g.progs);
	return g;
}

static void gpu_close(struct gpu *g)
{
	sgxfive_prog_source_destroy(g->progs);
	sgxfive_dev_close(g->dev);
	drm_close_driver(g->fd);
}

static struct side side_init(struct gpu *g)
{
	struct side s = { };

	igt_assert_eq(sgxfive_vm_create(g->dev, &s.vm), 0);
	igt_assert_eq(sgxfive_ctx_create(s.vm, &s.ctx), 0);
	igt_assert_eq(sgxfive_xfer_create(s.vm, s.ctx, g->progs, &s.x), 0);
	return s;
}

static void side_fini(struct gpu *g, struct side *s)
{
	sgxfive_xfer_destroy(s->x);
	sgxfive_ctx_destroy(g->dev, s->ctx);
	sgxfive_vm_destroy(s->vm);
}

/* pages of sentinel at *addr in vm (0: allocated in GENERAL). */
static uint32_t *surface(struct gpu *g, struct sgxfive_vm *vm, uint32_t pages, uint32_t flags,
			 uint64_t *addr, struct sgxfive_bo **bo)
{
	const uint64_t size = pages * (uint64_t)SGXFIVE_PAGE_SIZE;
	uint32_t *p = NULL;

	igt_assert_eq(sgxfive_bo_create(g->dev, size, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, bo), 0);
	igt_assert_eq(sgxfive_bo_mmap(*bo, (void **)&p), 0);
	if (!*addr)
		igt_assert_eq(sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, size,
					       SGXFIVE_PAGE_SIZE, addr), 0);
	igt_assert_eq(sgxfive_vm_map(vm, *addr, *bo, 0, size, flags), 0);
	for (uint64_t i = 0; i < size / 4; i++)
		p[i] = SENTINEL;
	return p;
}

static uint32_t pages_for(uint32_t stride, uint32_t rows)
{
	return (stride * rows + SGXFIVE_PAGE_SIZE - 1) / SGXFIVE_PAGE_SIZE + 1;	/* + guard */
}

static void fill_case(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t stride,
		      uint32_t colour)
{
	const struct sgxfive_rect r = { x0, y0, x0 + w, y0 + h };
	const uint32_t rows = y0 + h, pages = pages_for(stride, rows), sw = stride / 4;
	struct gpu g = gpu_open();
	struct side s = side_init(&g);
	struct sgxfive_bo *bo = NULL;
	uint64_t addr = 0;
	uint32_t *p = surface(&g, s.vm, pages, 0, &addr, &bo), bad = 0;
	struct sgxfive_surface dst = { addr, sw, rows, stride };

	igt_assert_eq(sgxfive_xfer_fill(s.x, &dst, &r, colour, 0), 0);
	igt_assert_eq(sgxfive_xfer_wait(s.x, WAIT_NS), 0);
	for (uint32_t i = 0; i < pages * 1024; i++) {
		uint32_t x = i % sw, y = i / sw;
		bool in = x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;

		if (p[i] != (in ? colour : SENTINEL) && !bad++)
			igt_info("dword %u (%u, %u) = 0x%08x\n", i, x, y, p[i]);
	}
	igt_assert_eq_u32(bad, 0);
	sgxfive_bo_destroy(bo);
	side_fini(&g, &s);
	gpu_close(&g);
}

static uint32_t pattern(uint32_t x, uint32_t y)
{
	return 0xa5000000 | (y & 0xff) << 16 | (x & 0xff) << 8 | ((x ^ y) & 0xff);
}

static void copy_case(uint32_t sx, uint32_t sy, uint32_t src_stride, uint32_t x0, uint32_t y0,
		      uint32_t w, uint32_t h, uint32_t dst_stride)
{
	const struct sgxfive_rect r = { x0, y0, x0 + w, y0 + h };
	const uint32_t srows = sy + h, drows = y0 + h;
	const uint32_t spages = pages_for(src_stride, srows), dpages = pages_for(dst_stride, drows);
	const uint32_t sw = src_stride / 4, dw = dst_stride / 4;
	struct gpu g = gpu_open();
	struct side sd = side_init(&g);
	struct sgxfive_bo *sbo = NULL, *dbo = NULL;
	uint64_t saddr = 0, daddr = 0;
	uint32_t *s = surface(&g, sd.vm, spages, DRM_SGXFIVE_VM_MAP_READ_ONLY, &saddr, &sbo);
	uint32_t *d = surface(&g, sd.vm, dpages, 0, &daddr, &dbo), bad = 0;
	struct sgxfive_surface src = { saddr, sw, srows, src_stride };
	struct sgxfive_surface dst = { daddr, dw, drows, dst_stride };

	for (uint32_t i = 0; i < spages * 1024; i++)
		s[i] = pattern(i % sw, i / sw);
	igt_assert_eq(sgxfive_xfer_copy(sd.x, &src, sx, sy, &dst, &r, 0), 0);
	igt_assert_eq(sgxfive_xfer_wait(sd.x, WAIT_NS), 0);
	for (uint32_t i = 0; i < dpages * 1024; i++) {
		uint32_t x = i % dw, y = i / dw;
		bool in = x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
		uint32_t want = in ? pattern(sx + x - r.x0, sy + y - r.y0) : SENTINEL;

		if (d[i] != want && !bad++)
			igt_info("dword %u (%u, %u) = 0x%08x, expected 0x%08x\n", i, x, y, d[i],
				 want);
	}
	igt_assert_eq_u32(bad, 0);
	sgxfive_bo_destroy(sbo);
	sgxfive_bo_destroy(dbo);
	side_fini(&g, &sd);
	gpu_close(&g);
}

static bool all(const uint32_t *p, uint32_t words, uint32_t v)
{
	for (uint32_t i = 0; i < words; i++)
		if (p[i] != v)
			return false;
	return true;
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static const struct sgxfive_rect rect = { 0, 0, 64, 32 };

/*
 * A fill by a's context into an address only b's VM maps: it faults, writes
 * nothing, and its context is lost. Returns b's pages there.
 */
static uint32_t *fault_into(struct gpu *g, struct side *a, struct side *b,
			    struct sgxfive_bo **bo, int64_t *ms)
{
	uint64_t addr = 0;
	uint32_t *p = surface(g, b->vm, 2, 0, &addr, bo);
	struct sgxfive_surface dst = { addr, 64, 32, 256 };
	int64_t t0;

	igt_assert_eq(sgxfive_xfer_fill(a->x, &dst, &rect, 0xff0000bb, 0), 0);
	t0 = now_ms();
	sgxfive_xfer_wait(a->x, WAIT_NS);		/* signals with an error */
	if (ms)
		*ms = now_ms() - t0;
	return p;
}

int igt_main()
{
	igt_fixture() {
		struct gpu g = gpu_open();	/* skips without sgxfive */

		gpu_close(&g);
	}

	igt_describe("A solid fill of a whole 64x32 surface.");
	igt_subtest("fill")
		fill_case(0, 0, 64, 32, 256, 0xff336699);

	igt_describe("A fill of an inset rectangle on a padded stride.");
	igt_subtest("fill-inset")
		fill_case(8, 4, 32, 16, 320, 0xff00ff00);

	igt_describe("A fill crossing tile boundaries.");
	igt_subtest("fill-tile-crossing")
		fill_case(16, 8, 33, 16, 256, 0xffff0000);

	igt_describe("A single-pixel fill.");
	igt_subtest("fill-pixel")
		fill_case(5, 7, 1, 1, 256, 0xff0000ff);

	igt_describe("A copy of a whole 64x32 surface.");
	igt_subtest("copy")
		copy_case(0, 0, 256, 0, 0, 64, 32, 256);

	igt_describe("A copy between offsets and different strides.");
	igt_subtest("copy-offset")
		copy_case(3, 2, 320, 8, 4, 32, 16, 384);

	igt_subtest("same-address-separate-vms") {
		struct gpu g = gpu_open();
		struct side a = side_init(&g), b = side_init(&g);
		struct sgxfive_bo *ba = NULL, *bb = NULL;
		uint64_t addr = 0;
		uint32_t *pa = surface(&g, a.vm, 2, 0, &addr, &ba);
		uint32_t *pb = surface(&g, b.vm, 2, 0, &addr, &bb);	/* same address */
		struct sgxfive_surface dst = { addr, 64, 32, 256 };

		igt_assert_eq(sgxfive_xfer_fill(a.x, &dst, &rect, 0xff0000aa, 0), 0);
		igt_assert_eq(sgxfive_xfer_wait(a.x, WAIT_NS), 0);
		igt_assert(all(pa, 2048, 0xff0000aa));
		igt_assert(all(pb, 2048, SENTINEL));
		sgxfive_bo_destroy(ba);
		sgxfive_bo_destroy(bb);
		side_fini(&g, &a);
		side_fini(&g, &b);
		gpu_close(&g);
	}

	igt_subtest("other-vm-address-faults") {
		struct gpu g = gpu_open();
		struct side a = side_init(&g), b = side_init(&g);
		struct sgxfive_bo *bo = NULL;
		int64_t ms;
		uint32_t *p = fault_into(&g, &a, &b, &bo, &ms);

		igt_info("fault recovered after %" PRId64 " ms\n", ms);
		igt_assert(all(p, 2048, SENTINEL));
		igt_assert(ms < FAULT_MS);
		/* The faulting context is lost. */
		igt_assert_eq(sgxfive_submit_transfer(g.dev, a.ctx,
				DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0), -ECANCELED);
		sgxfive_bo_destroy(bo);
		side_fini(&g, &a);
		side_fini(&g, &b);
		gpu_close(&g);
	}

	igt_subtest("other-context-survives") {
		struct gpu g = gpu_open();
		struct side a = side_init(&g), b = side_init(&g);
		struct sgxfive_bo *bo = NULL, *bm = NULL;
		uint64_t mine = 0;
		uint32_t *pm;
		struct sgxfive_surface dst;

		fault_into(&g, &a, &b, &bo, NULL);
		pm = surface(&g, b.vm, 2, 0, &mine, &bm);
		dst = (struct sgxfive_surface){ mine, 64, 32, 256 };
		igt_assert_eq(sgxfive_xfer_fill(b.x, &dst, &rect, 0xff0000cc, 0), 0);
		igt_assert_eq(sgxfive_xfer_wait(b.x, WAIT_NS), 0);
		igt_assert(all(pm, 2048, 0xff0000cc));
		sgxfive_bo_destroy(bo);
		sgxfive_bo_destroy(bm);
		side_fini(&g, &a);
		side_fini(&g, &b);
		gpu_close(&g);
	}

	igt_describe("A job queued right behind a faulting one, on another context, is re-run "
		     "by the recovery reset.");
	igt_subtest("queued-innocent-job-reruns") {
		struct gpu g = gpu_open();
		struct side c = side_init(&g), d = side_init(&g);
		struct sgxfive_bo *bq = NULL, *bm = NULL;
		uint64_t only_d = 0, mine = 0;
		uint32_t *pq = surface(&g, d.vm, 2, 0, &only_d, &bq);
		uint32_t *pm = surface(&g, d.vm, 2, 0, &mine, &bm);
		struct sgxfive_surface bad = { only_d, 64, 32, 256 }, good = { mine, 64, 32, 256 };

		igt_assert_eq(sgxfive_xfer_fill(c.x, &bad, &rect, 0xff0000ee, 0), 0);
		igt_assert_eq(sgxfive_xfer_fill(d.x, &good, &rect, 0xff0000ff, 0), 0);
		igt_assert_eq(sgxfive_xfer_wait(d.x, WAIT_NS), 0);
		igt_assert(all(pm, 2048, 0xff0000ff));
		igt_assert(all(pq, 2048, SENTINEL));
		igt_assert_eq(sgxfive_submit_transfer(g.dev, c.ctx,
				DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0), -ECANCELED);
		sgxfive_bo_destroy(bq);
		sgxfive_bo_destroy(bm);
		side_fini(&g, &c);
		side_fini(&g, &d);
		gpu_close(&g);
	}

	igt_subtest("read-only-faults") {
		struct gpu g = gpu_open();
		struct side a = side_init(&g);
		struct sgxfive_bo *bo = NULL;
		uint64_t ro = 0;
		uint32_t *p = surface(&g, a.vm, 2, DRM_SGXFIVE_VM_MAP_READ_ONLY, &ro, &bo);
		struct sgxfive_surface dst = { ro, 64, 32, 256 };

		igt_assert_eq(sgxfive_xfer_fill(a.x, &dst, &rect, 0xff0000dd, 0), 0);
		sgxfive_xfer_wait(a.x, WAIT_NS);
		igt_assert(all(p, 2048, SENTINEL));
		igt_assert_eq(sgxfive_xfer_fill(a.x, &dst, &rect, 0xff0000dd, 0), -ECANCELED);
		sgxfive_bo_destroy(bo);
		side_fini(&g, &a);
		gpu_close(&g);
	}

	igt_describe("Three faults within a minute ban the file; other files carry on.");
	igt_subtest("faulting-file-banned") {
		struct gpu g = gpu_open(), other;
		struct sgxfive_bo *bo[3] = { };
		struct sgxfive_vm *vm = NULL;
		struct side s[4];
		uint32_t ctx = 0;

		for (int i = 0; i < 4; i++)
			s[i] = side_init(&g);
		for (int i = 0; i < 3; i++)
			fault_into(&g, &s[i], &s[3], &bo[i], NULL);
		igt_assert_eq(sgxfive_vm_create(g.dev, &vm), 0);
		igt_assert_eq(sgxfive_ctx_create(vm, &ctx), -ECANCELED);
		igt_assert_eq(sgxfive_submit_transfer(g.dev, s[3].ctx,
				DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0), -ECANCELED);
		sgxfive_vm_destroy(vm);
		for (int i = 0; i < 3; i++)
			sgxfive_bo_destroy(bo[i]);
		for (int i = 0; i < 4; i++)
			side_fini(&g, &s[i]);
		gpu_close(&g);

		other = gpu_open();
		s[0] = side_init(&other);		/* a new file is not banned */
		side_fini(&other, &s[0]);
		gpu_close(&other);
	}
}
