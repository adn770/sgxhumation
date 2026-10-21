// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * Render contexts and render jobs (TA + 3D): the first triangle drawn from
 * userspace through libsgxfive's render engine (lib/sgxfive/, our own
 * assembled programs), every pixel checked against a CPU reference and the
 * area after the target against a sentinel; the rules on render contexts,
 * commands and the reserved render heaps; several contexts, transfers and
 * syncobjs interleaved with renders; and a faulting render recovered with its
 * context lost. Faults cost the file's budget, so they open their own file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_sgxfive.h"
#include "igt_syncobj.h"
#include "sgxfive/sgxfive_render.h"
#include "sgxfive/sgxfive_xfer.h"

#define WAIT_NS		(5LL * 1000000000LL)
#define SENTINEL	0xdeadbeefu

struct gpu {
	int fd;
	struct sgxfive_dev *dev;
};

/* A render context with its engine, in a VM of its own unless vm is given. */
struct rside {
	struct sgxfive_vm *vm;
	bool own_vm;
	uint32_t ctx;
	struct sgxfive_render *r;
};

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static struct gpu gpu_open(void)
{
	struct gpu g = { .fd = igt_sgxfive_open() };
	struct drm_sgxfive_dev_query_job_info j = { };

	igt_assert_eq(sgxfive_dev_adopt(NULL, g.fd, &g.dev), 0);
	igt_sgxfive_dev_query(g.fd, DRM_SGXFIVE_DEV_QUERY_JOB_INFO, &j, sizeof(j));
	igt_require_f(j.job_types & DRM_SGXFIVE_JOB_TYPE_RENDER_BIT, "no render jobs\n");
	return g;
}

static void gpu_close(struct gpu *g)
{
	sgxfive_dev_close(g->dev);
	drm_close_driver(g->fd);
}

static struct rside rside_init(struct gpu *g, struct sgxfive_vm *vm)
{
	struct rside s = { .vm = vm };

	if (!vm) {
		igt_assert_eq(sgxfive_vm_create(g->dev, &s.vm), 0);
		s.own_vm = true;
	}
	igt_assert_eq(sgxfive_ctx_create_type(s.vm, DRM_SGXFIVE_CTX_TYPE_RENDER, &s.ctx), 0);
	igt_assert_eq(sgxfive_render_create(s.vm, s.ctx, &s.r), 0);
	return s;
}

static void rside_fini(struct gpu *g, struct rside *s)
{
	sgxfive_render_destroy(s->r);
	sgxfive_ctx_destroy(g->dev, s->ctx);
	if (s->own_vm)
		sgxfive_vm_destroy(s->vm);
}

/* The image of p as text, for a failure report (# triangle, . background). */
static void show(const struct sgxfive_tri_params *p, const uint32_t *fb)
{
	char line[65];

	for (uint32_t y = 0; y < p->height && y < 64; y++) {
		uint32_t x;

		for (x = 0; x < p->width && x < 64; x++) {
			uint32_t v = fb[y * p->width + x];

			line[x] = v == p->colour ? '#' : v == p->bg_colour ? '.' :
				  v == SGXFIVE_TRI_SENTINEL ? 's' : '?';
		}
		line[x] = 0;
		igt_info("%s\n", line);
	}
}

/* Every pixel of the last render of s against the reference, and the guard area. */
static void check(struct rside *s, const struct sgxfive_tri_params *p)
{
	struct sgxfive_tri_counts n;
	uint32_t guard = sgxfive_render_guard_dirty(s->r, p);
	int ret = sgxfive_tri_check(p, sgxfive_render_fb(s->r), &n);

	igt_debug("%ux%u: %u triangle, %u background, %u other, %u flipped, guard %u\n",
		  p->width, p->height, n.tri, n.bg, n.other, n.flipped, guard);
	if (ret || guard)
		show(p, sgxfive_render_fb(s->r));
	igt_assert_f(!ret, "%u wrong inside, %u outside, %u at edges (%u if flipped)\n",
		     n.wrong_in, n.wrong_out, n.wrong_edge, n.flipped);
	igt_assert_eq_u32(guard, 0);
	igt_assert(n.tri && n.bg);
}

static void render(struct rside *s, const struct sgxfive_tri_params *p)
{
	igt_assert_eq(sgxfive_render_tri(s->r, p, 0, 0), 0);
	igt_assert_eq(sgxfive_render_wait(s->r, WAIT_NS), 0);
	check(s, p);
}

static void triangle(uint32_t w, uint32_t h, uint32_t colour, uint32_t bg)
{
	struct gpu g = gpu_open();
	struct rside s = rside_init(&g, NULL);
	struct sgxfive_tri_params p;

	sgxfive_tri_default(&p, w, h);
	p.colour = colour;
	p.bg_colour = bg;
	render(&s, &p);
	rside_fini(&g, &s);
	gpu_close(&g);
}

/* A render job of cmd on ctx, raw, for the -EINVAL rules. */
static int submit_raw(int fd, uint32_t ctx, uint32_t type, uint32_t flags, const void *cmd,
		      uint32_t len)
{
	struct drm_sgxfive_job job = {
		.type = type, .context_handle = ctx, .flags = flags,
		.cmd_stream = (uintptr_t)cmd, .cmd_stream_len = len,
	};

	return __igt_sgxfive_submit(fd, &job, 1, NULL);
}

#define DEVCD_ROOT	"/sys/class/devcoredump"
#define DEVCD_MAX	(64 * 1024)

/* The data file of fd's GPU's devcoredump, into path; false if there is none. */
static bool devcd_find(int fd, char *path, size_t len)
{
	char gpu[PATH_MAX], link[PATH_MAX + 32], dev[PATH_MAX];
	struct dirent *e;
	struct stat st;
	bool found = false;
	DIR *dir;

	igt_assert_eq(fstat(fd, &st), 0);
	snprintf(link, sizeof(link), "/sys/dev/char/%u:%u/device",
		 major(st.st_rdev), minor(st.st_rdev));
	igt_assert(realpath(link, gpu));
	dir = opendir(DEVCD_ROOT);
	if (!dir)
		return false;
	while (!found && (e = readdir(dir))) {
		if (strncmp(e->d_name, "devcd", 5))
			continue;
		snprintf(link, sizeof(link), DEVCD_ROOT "/%s/failing_device", e->d_name);
		if (realpath(link, dev) && !strcmp(dev, gpu)) {
			snprintf(path, len, DEVCD_ROOT "/%s/data", e->d_name);
			found = true;
		}
	}
	closedir(dir);
	return found;
}

static bool devcd_wait(int fd, char *path, size_t len, bool gone, int ms)
{
	int64_t end = now_ms() + ms;

	do {
		if (devcd_find(fd, path, len) != gone)
			return true;
		usleep(10000);
	} while (now_ms() < end);
	return false;
}

static void devcd_free(int fd, const char *path)
{
	char again[PATH_MAX];
	int dfd = open(path, O_WRONLY);

	igt_assert_lte(0, dfd);
	igt_assert_eq(write(dfd, "1", 1), 1);
	close(dfd);
	igt_assert(devcd_wait(fd, again, sizeof(again), true, 2000));
}

static char *devcd_read(const char *path)
{
	char *buf = calloc(1, DEVCD_MAX + 1);
	size_t len = 0;
	ssize_t n;
	int dfd = open(path, O_RDONLY);

	igt_assert(buf);
	igt_assert_lte(0, dfd);
	while (len < DEVCD_MAX && (n = read(dfd, buf + len, DEVCD_MAX - len)) > 0)
		len += n;
	close(dfd);
	return buf;
}

int igt_main()
{
	igt_fixture() {
		struct gpu g = gpu_open();	/* skips without sgxfive or render jobs */

		gpu_close(&g);
	}

	igt_describe("Render contexts come and go; a VM holds at most 32, and a freed "
		     "slot is reusable.");
	igt_subtest("create-destroy") {
		struct gpu g = gpu_open();
		uint32_t vm = igt_sgxfive_create_vm(g.fd), ctx[32], extra;
		struct drm_sgxfive_create_context_args a = {
			.type = DRM_SGXFIVE_CTX_TYPE_RENDER, .vm_context_handle = vm,
		};

		for (int i = 0; i < 32; i++) {
			a.handle = 0;
			igt_assert_eq(__igt_sgxfive_create_ctx(g.fd, &a), 0);
			ctx[i] = a.handle;
		}
		igt_assert_eq(__igt_sgxfive_create_ctx(g.fd, &a), -ENOSPC);
		igt_sgxfive_destroy_ctx(g.fd, ctx[7]);
		igt_assert_eq(__igt_sgxfive_create_ctx(g.fd, &a), 0);
		extra = a.handle;
		igt_sgxfive_destroy_ctx(g.fd, extra);
		for (int i = 0; i < 32; i++)
			if (i != 7)
				igt_sgxfive_destroy_ctx(g.fd, ctx[i]);
		/* Another VM has its own 32. */
		a.vm_context_handle = igt_sgxfive_create_vm(g.fd);
		igt_assert_eq(__igt_sgxfive_create_ctx(g.fd, &a), 0);
		igt_sgxfive_destroy_ctx(g.fd, a.handle);
		igt_sgxfive_destroy_vm(g.fd, a.vm_context_handle);
		igt_sgxfive_destroy_vm(g.fd, vm);
		gpu_close(&g);
	}

	igt_describe("The render heaps are reported reserved, and no client can map them.");
	igt_subtest("reserved-heaps") {
		struct gpu g = gpu_open();
		const uint32_t ids[] = { DRM_SGXFIVE_HEAP_PARAMETERS, DRM_SGXFIVE_HEAP_TA_DATA };
		uint32_t vm = igt_sgxfive_create_vm(g.fd);
		uint32_t bo = igt_sgxfive_create_bo(g.fd, 4096, 0);

		for (int i = 0; i < 2; i++) {
			struct drm_sgxfive_heap h = igt_sgxfive_heap(g.fd, ids[i]);

			igt_assert(h.size);
			igt_assert(h.flags & DRM_SGXFIVE_HEAP_RESERVED);
			igt_assert_eq(__igt_sgxfive_vm_map(g.fd, vm, h.base, bo, 0, 4096, 0),
				      -EINVAL);
		}
		gem_close(g.fd, bo);
		igt_sgxfive_destroy_vm(g.fd, vm);
		gpu_close(&g);
	}

	igt_describe("Render commands that break the kernel's rules are refused.");
	igt_subtest("bad-args") {
		struct gpu g = gpu_open();
		struct rside s = rside_init(&g, NULL);
		struct drm_sgxfive_render_cmd good, c __attribute__((aligned(8)));
		struct sgxfive_tri_params p;
		uint32_t xctx;

		sgxfive_tri_default(&p, 64, 64);
		render(&s, &p);
		good = *sgxfive_render_last_cmd(s.r);
		igt_assert_eq(sgxfive_ctx_create(s.vm, &xctx), 0);
#define BAD(field, value) do {						\
		c = good;						\
		c.field = (value);					\
		igt_assert_eq(submit_raw(g.fd, s.ctx, DRM_SGXFIVE_JOB_TYPE_RENDER, 0,	\
					 &c, sizeof(c)), -EINVAL);	\
	} while (0)
		BAD(vdm_ctrl_stream_base, (uint32_t)igt_sgxfive_heap(g.fd,
						DRM_SGXFIVE_HEAP_KERNEL_DATA).base);
		BAD(vdm_ctrl_stream_base, (uint32_t)igt_sgxfive_heap(g.fd,
						DRM_SGXFIVE_HEAP_PARAMETERS).base);
		BAD(vdm_ctrl_stream_base, good.vdm_ctrl_stream_base + 2);
		BAD(event_pixel_pds_exec, (uint32_t)igt_sgxfive_heap(g.fd,
						DRM_SGXFIVE_HEAP_KERNEL_CODE).base);
		BAD(event_pixel_pds_exec, good.event_pixel_pds_exec + 8);
		BAD(mte_screen, 511 | 63 << 16);
		BAD(mte_screen, 127 | 191 << 16);	/* the 64x64 tiling, 12 tile rows */
		BAD(te_screen, 15 | 3 << 12);
		BAD(te_mtile1, 0x82010018);
		BAD(te_mtile2, 0x02008008);
		BAD(te_mtile_stride, 0x20);
		BAD(isp_bgobj, 0x1300);
		/* Beyond the device's largest target, with the tiling of that size. */
		c = good;
		c.mte_screen = 4096 | 63 << 16;
		sgxfive_tiling_cmd(&c, 4097, 64);
		igt_assert_eq(submit_raw(g.fd, s.ctx, DRM_SGXFIVE_JOB_TYPE_RENDER, 0, &c,
					 sizeof(c)), -EINVAL);
#undef BAD
		c = good;
		/* Flags (none defined), a short command, job and context types apart. */
		igt_assert_eq(submit_raw(g.fd, s.ctx, DRM_SGXFIVE_JOB_TYPE_RENDER, 1, &c,
					 sizeof(c)), -EINVAL);
		igt_assert_eq(submit_raw(g.fd, s.ctx, DRM_SGXFIVE_JOB_TYPE_RENDER, 0, &c,
					 sizeof(c) - 8), -EINVAL);
		igt_assert_eq(submit_raw(g.fd, xctx, DRM_SGXFIVE_JOB_TYPE_RENDER, 0, &c,
					 sizeof(c)), -EINVAL);
		igt_assert_eq(submit_raw(g.fd, s.ctx, DRM_SGXFIVE_JOB_TYPE_TRANSFER,
					 DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0), -EINVAL);
		/* After all that the context still renders. */
		render(&s, &p);
		igt_sgxfive_destroy_ctx(g.fd, xctx);
		rside_fini(&g, &s);
		gpu_close(&g);
	}

	igt_describe("The first triangle, 64x64, every pixel against the CPU reference.");
	igt_subtest("triangle")
		triangle(64, 64, 0xff00ff00, 0xff203040);

	igt_describe("The triangle at other render-target sizes, up to the largest the "
		     "kernel reports: two and four macrotiles, non-power-of-two sizes.");
	igt_subtest_with_dynamic("triangle-sizes") {
		static const uint32_t sizes[][2] = {
			{ 32, 16 }, { 96, 64 }, { 128, 64 }, { 64, 128 }, { 256, 64 }, { 256, 128 },
			{ 128, 192 }, { 128, 256 }, { 256, 256 }, { 100, 60 }, { 512, 16 },
			{ 32, 512 }, { 334, 778 }, { 640, 474 }, { 640, 480 }, { 800, 600 },
			{ 1024, 600 }, { 1024, 768 }, { 1366, 768 }, { 2048, 2048 }, { 0, 0 },
		};
		struct drm_sgxfive_dev_query_job_info j = { };
		struct gpu g = gpu_open();

		igt_sgxfive_dev_query(g.fd, DRM_SGXFIVE_DEV_QUERY_JOB_INFO, &j, sizeof(j));
		gpu_close(&g);
		igt_assert_eq_u32(j.render_max_width, 4096);
		igt_assert_eq_u32(j.render_max_height, 4096);
		for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
			uint32_t w = sizes[i][0] ?: j.render_max_width;
			uint32_t h = sizes[i][1] ?: j.render_max_height;

			igt_dynamic_f("%ux%u", w, h)
				triangle(w, h, 0xffff8000, 0xff102030);
		}
	}

	igt_describe("One context renders targets of changing sizes, larger and smaller, "
		     "each exact.");
	igt_subtest("triangle-resize") {
		static const uint32_t sizes[][2] = {
			{ 64, 64 }, { 1024, 768 }, { 64, 64 }, { 256, 256 }, { 1366, 768 },
			{ 334, 222 }, { 32, 16 }, { 640, 480 }, { 640, 474 }, { 64, 64 },
		};
		struct gpu g = gpu_open();
		struct rside s = rside_init(&g, NULL);
		struct sgxfive_tri_params p;

		for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
			sgxfive_tri_default(&p, sizes[i][0], sizes[i][1]);
			p.colour = i % 2 ? 0xffff0000 : 0xff0000ff;
			p.bg_colour = i % 2 ? 0xff000000 : 0xffffffff;
			render(&s, &p);
		}
		rside_fini(&g, &s);
		gpu_close(&g);
	}

	igt_describe("Twenty renders in a row on one context, alternating colours.");
	igt_subtest("triangle-repeat") {
		struct gpu g = gpu_open();
		struct rside s = rside_init(&g, NULL);
		struct sgxfive_tri_params p;

		for (int i = 0; i < 20; i++) {
			sgxfive_tri_default(&p, 64, 64);
			p.colour = i % 2 ? 0xffff0000 : 0xff0000ff;
			p.bg_colour = i % 2 ? 0xff000000 : 0xffffffff;
			render(&s, &p);
		}
		rside_fini(&g, &s);
		gpu_close(&g);
	}

	igt_describe("Render contexts in one VM and in two, queued together: the "
		     "firmware switches parameter buffers between them.");
	igt_subtest("two-contexts") {
		struct gpu g = gpu_open();
		struct rside a = rside_init(&g, NULL), b = rside_init(&g, a.vm);
		struct rside c = rside_init(&g, NULL);
		struct sgxfive_tri_params pa, pb, pc;

		sgxfive_tri_default(&pa, 64, 64);
		sgxfive_tri_default(&pb, 128, 64);
		sgxfive_tri_default(&pc, 64, 128);
		pb.colour = 0xffff00ff;
		pc.colour = 0xff00ffff;
		for (int round = 0; round < 5; round++) {
			igt_assert_eq(sgxfive_render_tri(a.r, &pa, 0, 0), 0);
			igt_assert_eq(sgxfive_render_tri(b.r, &pb, 0, 0), 0);
			igt_assert_eq(sgxfive_render_tri(c.r, &pc, 0, 0), 0);
			igt_assert_eq(sgxfive_render_wait(a.r, WAIT_NS), 0);
			igt_assert_eq(sgxfive_render_wait(b.r, WAIT_NS), 0);
			igt_assert_eq(sgxfive_render_wait(c.r, WAIT_NS), 0);
			check(&a, &pa);
			check(&b, &pb);
			check(&c, &pc);
		}
		rside_fini(&g, &c);
		rside_fini(&g, &b);
		rside_fini(&g, &a);
		gpu_close(&g);
	}

	igt_describe("Transfers and renders queued together in one VM: fills, the "
		     "triangle, and a transfer engine whose PDS page is not the heap base.");
	igt_subtest("interleave-transfer") {
		struct gpu g = gpu_open();
		struct rside s = rside_init(&g, NULL);
		struct sgxfive_prog_source *src = sgxfive_prog_asm_create();
		const struct sgxfive_rect rect = { 8, 4, 56, 28 };
		struct sgxfive_xfer *x1, *x2;
		struct sgxfive_tri_params p;
		uint32_t xctx, bo, *dst, bad = 0;
		uint64_t addr;

		igt_assert(src);
		igt_assert_eq(sgxfive_ctx_create(s.vm, &xctx), 0);
		igt_assert_eq(sgxfive_xfer_create(s.vm, xctx, src, &x1), 0);
		igt_assert_eq(sgxfive_xfer_create(s.vm, xctx, src, &x2), 0);
		bo = igt_sgxfive_create_bo(g.fd, 3 * 4096, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS);
		dst = igt_sgxfive_mmap_bo(g.fd, bo, 3 * 4096);
		igt_assert_eq(sgxfive_vm_alloc(s.vm, DRM_SGXFIVE_HEAP_GENERAL, 3 * 4096, 4096,
					       &addr), 0);
		igt_sgxfive_vm_map(g.fd, sgxfive_vm_handle(s.vm), addr, bo, 0, 3 * 4096, 0);
		sgxfive_tri_default(&p, 64, 64);
		for (int round = 0; round < 6; round++) {
			const struct sgxfive_surface d = { addr, 64, 32, 256 };
			struct sgxfive_xfer *x = round % 2 ? x2 : x1;
			uint32_t colour = 0xff000000 | (0x10203 * (round + 1));

			for (int i = 0; i < 3 * 1024; i++)
				dst[i] = SENTINEL;
			p.colour = colour ^ 0x00ffffff;
			igt_assert_eq(sgxfive_render_tri(s.r, &p, 0, 0), 0);
			igt_assert_eq(sgxfive_xfer_fill(x, &d, &rect, colour, 0), 0);
			igt_assert_eq(sgxfive_xfer_wait(x, WAIT_NS), 0);
			igt_assert_eq(sgxfive_render_wait(s.r, WAIT_NS), 0);
			for (uint32_t i = 0; i < 3 * 1024; i++) {
				uint32_t xx = i % 64, y = i / 64;
				bool in = y < 32 && xx >= rect.x0 && xx < rect.x1 &&
					  y >= rect.y0 && y < rect.y1;

				bad += dst[i] != (in ? colour : SENTINEL);
			}
			igt_assert_eq_u32(bad, 0);
			check(&s, &p);
		}
		sgxfive_xfer_destroy(x2);
		sgxfive_xfer_destroy(x1);
		igt_sgxfive_vm_unmap(g.fd, sgxfive_vm_handle(s.vm), addr, 3 * 4096);
		gem_close(g.fd, bo);
		sgxfive_prog_source_destroy(src);
		igt_sgxfive_destroy_ctx(g.fd, xctx);
		rside_fini(&g, &s);
		gpu_close(&g);
	}

	igt_describe("A render waits for a transfer fill's syncobj and a transfer for the "
		     "render's; a render signals a timeline point.");
	igt_subtest("syncobj-chain") {
		struct gpu g = gpu_open();
		struct rside s = rside_init(&g, NULL);
		struct sgxfive_prog_source *src = sgxfive_prog_asm_create();
		const struct sgxfive_rect rect = { 0, 0, 64, 32 };
		struct sgxfive_tri_params p;
		struct sgxfive_xfer *x;
		uint32_t xctx, s1, s2, s3, tl, bo, *dst, bad = 0;
		uint64_t addr;

		igt_assert(src);
		igt_assert_eq(sgxfive_ctx_create(s.vm, &xctx), 0);
		igt_assert_eq(sgxfive_xfer_create(s.vm, xctx, src, &x), 0);
		bo = igt_sgxfive_create_bo(g.fd, 2 * 4096, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS);
		dst = igt_sgxfive_mmap_bo(g.fd, bo, 2 * 4096);
		igt_assert_eq(sgxfive_vm_alloc(s.vm, DRM_SGXFIVE_HEAP_GENERAL, 2 * 4096, 4096,
					       &addr), 0);
		igt_sgxfive_vm_map(g.fd, sgxfive_vm_handle(s.vm), addr, bo, 0, 2 * 4096, 0);
		for (int i = 0; i < 2 * 1024; i++)
			dst[i] = SENTINEL;
		s1 = syncobj_create(g.fd, 0);
		s2 = syncobj_create(g.fd, 0);
		s3 = syncobj_create(g.fd, 0);
		tl = syncobj_create(g.fd, 0);
		sgxfive_tri_default(&p, 64, 64);
		/* fill -> s1 -> render -> s2 -> dummy transfer -> s3, queued at once. */
		{
			const struct sgxfive_surface d = { addr, 64, 32, 256 };

			igt_assert_eq(sgxfive_xfer_fill(x, &d, &rect, 0xff445566, s1), 0);
		}
		igt_assert_eq(sgxfive_render_tri(s.r, &p, s1, s2), 0);
		igt_sgxfive_submit_dummy(g.fd, xctx, s2, s3);
		igt_assert_eq(igt_sgxfive_wait(g.fd, s3, 0, 5000), 0);
		igt_assert_eq(igt_sgxfive_wait(g.fd, s2, 0, 0), 0);
		igt_assert_eq(igt_sgxfive_wait(g.fd, s1, 0, 0), 0);
		igt_assert_eq(sgxfive_render_wait(s.r, WAIT_NS), 0);
		igt_assert_eq(sgxfive_xfer_wait(x, WAIT_NS), 0);
		check(&s, &p);
		for (int i = 0; i < 2 * 1024; i++)
			bad += dst[i] != 0xff445566;
		igt_assert_eq_u32(bad, 0);
		/* A timeline point signalled by a render. */
		{
			const struct drm_sgxfive_render_cmd *last = sgxfive_render_last_cmd(s.r);
			struct drm_sgxfive_sync_op op = {
				.handle = tl, .value = 3,
				.flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL |
					 DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ,
			};
			struct drm_sgxfive_job job = {
				.type = DRM_SGXFIVE_JOB_TYPE_RENDER, .context_handle = s.ctx,
				.cmd_stream = (uintptr_t)last,
				.cmd_stream_len = sizeof(struct drm_sgxfive_render_cmd),
				.sync_ops = DRM_SGXFIVE_OBJ_ARRAY(1, &op),
			};

			igt_assert_eq(__igt_sgxfive_submit(g.fd, &job, 1, NULL), 0);
			igt_assert_eq(igt_sgxfive_wait(g.fd, tl, 3, 5000), 0);
		}
		syncobj_destroy(g.fd, tl);
		syncobj_destroy(g.fd, s3);
		syncobj_destroy(g.fd, s2);
		syncobj_destroy(g.fd, s1);
		sgxfive_xfer_destroy(x);
		igt_sgxfive_vm_unmap(g.fd, sgxfive_vm_handle(s.vm), addr, 2 * 4096);
		gem_close(g.fd, bo);
		sgxfive_prog_source_destroy(src);
		igt_sgxfive_destroy_ctx(g.fd, xctx);
		rside_fini(&g, &s);
		gpu_close(&g);
	}

	igt_describe("Destroying a render context with a job queued, then rendering on a "
		     "new one (the firmware drops the old context's state).");
	igt_subtest("destroy-busy") {
		struct gpu g = gpu_open();
		struct sgxfive_tri_params p;

		sgxfive_tri_default(&p, 64, 64);
		for (int i = 0; i < 4; i++) {
			struct rside s = rside_init(&g, NULL);
			uint32_t ctx2;

			igt_assert_eq(sgxfive_render_tri(s.r, &p, 0, 0), 0);
			igt_assert_eq(sgxfive_submit_render(g.dev, s.ctx,
							    sgxfive_render_last_cmd(s.r), 0, 0),
				      0);
			igt_assert_eq(sgxfive_ctx_create_type(s.vm, DRM_SGXFIVE_CTX_TYPE_RENDER,
							      &ctx2), 0);
			sgxfive_ctx_destroy(g.dev, s.ctx);	/* two jobs queued */
			s.ctx = ctx2;
			sgxfive_render_wait(s.r, WAIT_NS);
			sgxfive_render_destroy(s.r);
			igt_assert_eq(sgxfive_render_create(s.vm, s.ctx, &s.r), 0);
			render(&s, &p);
			rside_fini(&g, &s);
		}
		gpu_close(&g);
	}

	igt_describe("A render whose control stream is mapped nowhere faults: it is "
		     "recovered quickly, its context is lost, others render on, and the "
		     "error state names a render context.");
	igt_subtest("fault-recovered") {
		struct gpu g = gpu_open();
		struct rside s = rside_init(&g, NULL), t = rside_init(&g, NULL);
		struct drm_sgxfive_render_cmd c __attribute__((aligned(8)));
		struct sgxfive_tri_params p;
		char path[PATH_MAX];
		uint32_t done = syncobj_create(g.fd, 0);
		uint64_t hole;
		int64_t t0, ms;
		char *text;

		if (devcd_find(g.fd, path, sizeof(path)))
			devcd_free(g.fd, path);	/* only one dump is kept */
		sgxfive_tri_default(&p, 64, 64);
		render(&s, &p);
		c = *sgxfive_render_last_cmd(s.r);
		igt_assert_eq(sgxfive_vm_alloc(s.vm, DRM_SGXFIVE_HEAP_GENERAL, 4096, 4096,
					       &hole), 0);
		c.vdm_ctrl_stream_base = (uint32_t)hole;	/* never mapped */
		t0 = now_ms();
		igt_assert_eq(sgxfive_submit_render(g.dev, s.ctx, &c, 0, done), 0);
		igt_assert_eq(igt_sgxfive_wait(g.fd, done, 0, 5000), 0);
		ms = now_ms() - t0;
		igt_info("render fault recovered after %" PRId64 " ms\n", ms);
		igt_assert_lt(ms, 2000);
		igt_assert_eq(sgxfive_submit_render(g.dev, s.ctx, &c, 0, 0), -ECANCELED);
		render(&t, &p);
		igt_assert(devcd_wait(g.fd, path, sizeof(path), false, 2000));
		text = devcd_read(path);
		igt_assert(strstr(text, " guilty "));
		igt_assert(strstr(text, " render ta "));
		free(text);
		devcd_free(g.fd, path);
		syncobj_destroy(g.fd, done);
		rside_fini(&g, &t);
		rside_fini(&g, &s);
		gpu_close(&g);
	}

	igt_describe("A transfer must keep PDS_EXEC_BASE at the PDS_PIXEL heap base, which "
		     "render jobs rely on.");
	igt_subtest("transfer-pds-exec-base") {
		struct gpu g = gpu_open();
		uint32_t vm = igt_sgxfive_create_vm(g.fd), ctx = igt_sgxfive_create_ctx(g.fd, vm);
		struct drm_sgxfive_transfer_cmd c __attribute__((aligned(8))) = { };
		uint32_t base = (uint32_t)igt_sgxfive_heap(g.fd, DRM_SGXFIVE_HEAP_PDS_PIXEL).base;

		c.bif_3d_req_base = (uint32_t)igt_sgxfive_heap(g.fd, DRM_SGXFIVE_HEAP_GENERAL).base;
		c.event_pixel_pds_exec = base + 0x1000;
		c.pds_exec_base = base + 0x1000;
		igt_assert_eq(submit_raw(g.fd, ctx, DRM_SGXFIVE_JOB_TYPE_TRANSFER, 0, &c,
					 sizeof(c)), -EINVAL);
		igt_sgxfive_destroy_ctx(g.fd, ctx);
		igt_sgxfive_destroy_vm(g.fd, vm);
		gpu_close(&g);
	}
}
