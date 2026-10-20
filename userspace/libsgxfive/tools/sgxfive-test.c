// SPDX-License-Identifier: MIT
/*
 * sgxfive-test: the debugfs checks of the SGX545 bring-up (dri/0/sgx/...)
 * redone from userspace through the sgxfive render node.
 *
 *   sgxfive-test [--mock[=rev1|harness|none]] [--device PATH] [--programs=asm|fake]
 *                <command> [options]
 *
 *   query                         DEV_QUERY: GPU, firmware, job limits, heaps
 *   dummy [-n N]                  N dummy transfers chained through one syncobj
 *   fill  [--width W --height H --x X --y Y --stride S --colour C]
 *   copy  [--width W --height H --sx X --sy Y --src-stride S --dx X --dy Y --dst-stride S]
 *   isolation                     VM separation, faults, lost contexts, read-only maps
 *
 * fill, copy and isolation take their GPU programs from the assembled source
 * (our own code, default) or the fake test double (--programs=fake, mock only).
 *
 * Exit status: 0 all checks passed, 1 a check or call failed, 2 usage.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sgxfive_mock.h"
#include "sgxfive_xfer.h"

#define SENTINEL	0xdeadbeefu
#define WAIT_NS		(5LL * 1000000000LL)
/* A fault is found by the driver's 50 ms BIF poll, not the 500 ms job timeout. */
#define FAULT_NS	(300LL * 1000000LL)

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static const char *const heap_names[DRM_SGXFIVE_HEAP_COUNT] = {
	"GENERAL", "TEXTURE", "PDS_PIXEL", "PDS_VERTEX", "USSE_PIXEL", "USSE_VERTEX",
	"KERNEL_CODE", "KERNEL_DATA",
};

struct opts {
	uint32_t width, height, x, y, stride, colour;
	uint32_t sx, sy, src_stride, dst_stride;
	uint32_t n;
};

struct tool {
	struct sgxfive_mock *mock;
	struct sgxfive_dev *dev;
	struct sgxfive_prog_source *progs;
};

static int usage(void)
{
	fprintf(stderr,
		"usage: sgxfive-test [--mock[=rev1|harness|none]] [--device PATH]\n"
		"                    [--programs=asm|fake] <command>\n"
		"       commands: query, dummy [-n N], fill [...], copy [...], isolation\n");
	return 2;
}

static int fail(const char *what, int err)
{
	fprintf(stderr, "sgxfive-test: %s: %s\n", what, strerror(-err));
	return 1;
}

/* ---- query -------------------------------------------------------------------------- */

static int cmd_query(struct tool *t)
{
	const struct sgxfive_info *i = sgxfive_dev_info(t->dev);

	printf("device          %s\n", sgxfive_dev_path(t->dev));
	printf("core_id         0x%08x\n", i->gpu.core_id);
	printf("core_revision   0x%08x\n", i->gpu.core_revision);
	if (i->gpu.flags & DRM_SGXFIVE_GPU_INFO_SW_VALID)
		printf("core_sw         %u rev 0x%x\n", i->gpu.core_id_sw,
		       i->gpu.core_revision_sw);
	printf("host_bridge_rev %u\n", i->gpu.host_bridge_rev);
	printf("quirks          0x%llx%s\n", (unsigned long long)i->gpu.quirks,
	       i->gpu.quirks & DRM_SGXFIVE_QUIRK_SAMPLE_CACHE ? " SAMPLE_CACHE" : "");
	printf("ddk             %u.%u.%u build %u options 0x%x\n", i->fw.ddk_version >> 16,
	       (i->fw.ddk_version >> 8) & 0xff, i->fw.ddk_version & 0xff, i->fw.ddk_build,
	       i->fw.build_options);
	printf("fw_format       %u\n", i->fw.fw_format);
	printf("fw_state        %u\n", i->fw.state);
	printf("job_types       0x%llx\n", (unsigned long long)i->job.job_types);
	printf("max_jobs        %u\n", i->job.max_jobs);
	printf("max_sync_ops    %u\n", i->job.max_sync_ops);
	printf("transfer_cmd    0x%x\n", i->job.transfer_cmd_size);
	printf("max_updates     %u\n", i->job.max_updates);
	printf("pixel_parts     %u\n", i->runtime.num_pixel_partitions);
	printf("tile            %ux%u\n", i->runtime.tile_width, i->runtime.tile_height);
	for (uint32_t h = 0; h < DRM_SGXFIVE_HEAP_COUNT && h < i->num_heaps; h++) {
		const struct drm_sgxfive_heap *hp = &i->heaps[h];
		char name[32];

		snprintf(name, sizeof(name), "heap %s", heap_names[h]);
		if (!hp->size) {
			printf("%-16s absent\n", name);
			continue;
		}
		printf("%-16s base 0x%08llx size 0x%08llx%s%s\n", name,
		       (unsigned long long)hp->base, (unsigned long long)hp->size,
		       hp->flags & DRM_SGXFIVE_HEAP_RESERVED ? " RESERVED" : "",
		       hp->flags & DRM_SGXFIVE_HEAP_FIXED_WINDOWS ? " FIXED_WINDOWS" : "");
	}
	return 0;
}

/* ---- Shared set-up ------------------------------------------------------------------ */

struct side {
	struct sgxfive_vm *vm;
	uint32_t ctx;
	struct sgxfive_xfer *x;
};

static int side_init(struct tool *t, struct side *s)
{
	int ret;

	memset(s, 0, sizeof(*s));
	ret = sgxfive_vm_create(t->dev, &s->vm);
	if (!ret)
		ret = sgxfive_ctx_create(s->vm, &s->ctx);
	if (!ret && t->progs)
		ret = sgxfive_xfer_create(s->vm, s->ctx, t->progs, &s->x);
	return ret;
}

static void side_fini(struct tool *t, struct side *s)
{
	sgxfive_xfer_destroy(s->x);
	if (s->ctx)
		sgxfive_ctx_destroy(t->dev, s->ctx);
	sgxfive_vm_destroy(s->vm);
}

/* A CPU-visible BO of `pages` pages at addr (0: allocate in GENERAL), set to SENTINEL. */
static int surface(struct tool *t, struct sgxfive_vm *vm, uint32_t pages, uint32_t flags,
		   uint64_t *addr, struct sgxfive_bo **bo, uint32_t **map)
{
	const uint64_t size = pages * (uint64_t)SGXFIVE_PAGE_SIZE;
	void *p = NULL;
	int ret;

	ret = sgxfive_bo_create(t->dev, size, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS, bo);
	if (!ret)
		ret = sgxfive_bo_mmap(*bo, &p);
	if (!ret && !*addr)
		ret = sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, size, SGXFIVE_PAGE_SIZE, addr);
	if (!ret)
		ret = sgxfive_vm_map(vm, *addr, *bo, 0, size, flags);
	if (ret)
		return ret;
	*map = p;
	for (uint64_t i = 0; i < size / 4; i++)
		(*map)[i] = SENTINEL;
	return 0;
}

static uint32_t pages_for(uint32_t stride, uint32_t rows)
{
	const uint64_t bytes = (uint64_t)stride * rows;

	/* The surface plus one guard page. */
	return (uint32_t)((bytes + SGXFIVE_PAGE_SIZE - 1) / SGXFIVE_PAGE_SIZE) + 1;
}

/* ---- dummy -------------------------------------------------------------------------- */

static int cmd_dummy(struct tool *t, const struct opts *o)
{
	struct side s;
	uint32_t sync = 0;
	int ret = side_init(t, &s);

	if (!ret)
		ret = sgxfive_syncobj_create(t->dev, false, &sync);
	for (uint32_t i = 0; !ret && i < o->n; i++)
		ret = sgxfive_submit_transfer(t->dev, s.ctx, DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY,
					      NULL, i ? sync : 0, sync);
	if (!ret)
		ret = sgxfive_syncobj_wait(t->dev, sync, 0, WAIT_NS);
	if (sync)
		sgxfive_syncobj_destroy(t->dev, sync);
	side_fini(t, &s);
	if (ret)
		return fail("dummy", ret);
	printf("dummy: %u jobs ok\n", o->n);
	return 0;
}

/* ---- fill and copy ------------------------------------------------------------------ */

static int cmd_fill(struct tool *t, const struct opts *o)
{
	const struct sgxfive_rect r = { o->x, o->y, o->x + o->width, o->y + o->height };
	const uint32_t rows = o->y + o->height, pages = pages_for(o->stride, rows);
	struct sgxfive_bo *bo = NULL;
	struct sgxfive_surface dst;
	uint32_t *p = NULL, bad = 0;
	uint64_t addr = 0;
	struct side s;
	int ret = side_init(t, &s);

	if (!ret)
		ret = surface(t, s.vm, pages, 0, &addr, &bo, &p);
	dst = (struct sgxfive_surface){ addr, o->stride / 4, rows, o->stride };
	if (!ret)
		ret = sgxfive_xfer_fill(s.x, &dst, &r, o->colour, 0);
	if (!ret)
		ret = sgxfive_xfer_wait(s.x, WAIT_NS);
	for (uint32_t i = 0; !ret && i < pages * 1024; i++) {
		uint32_t x = i % (o->stride / 4), y = i / (o->stride / 4);
		bool in = x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;

		if (p[i] != (in ? o->colour : SENTINEL) && !bad++)
			printf("fill: dword %u (%u, %u) = 0x%08x\n", i, x, y, p[i]);
	}
	sgxfive_bo_destroy(bo);
	side_fini(t, &s);
	if (ret)
		return fail("fill", ret);
	if (bad) {
		printf("fill: FAIL, %u wrong dwords\n", bad);
		return 1;
	}
	printf("fill: ok, %ux%u at (%u, %u), stride %u, colour 0x%08x\n", o->width, o->height,
	       o->x, o->y, o->stride, o->colour);
	return 0;
}

static uint32_t pattern(uint32_t x, uint32_t y)
{
	return 0xa5000000 | (y & 0xff) << 16 | (x & 0xff) << 8 | ((x ^ y) & 0xff);
}

static int cmd_copy(struct tool *t, const struct opts *o)
{
	const struct sgxfive_rect r = { o->x, o->y, o->x + o->width, o->y + o->height };
	const uint32_t srows = o->sy + o->height, drows = o->y + o->height;
	const uint32_t spages = pages_for(o->src_stride, srows);
	const uint32_t dpages = pages_for(o->dst_stride, drows);
	struct sgxfive_bo *sbo = NULL, *dbo = NULL;
	struct sgxfive_surface src, dst;
	uint32_t *s = NULL, *d = NULL, bad = 0;
	const uint32_t sw = o->src_stride / 4, dw = o->dst_stride / 4;
	uint64_t saddr = 0, daddr = 0;
	struct side sd;
	int ret = side_init(t, &sd);

	if (!ret)
		ret = surface(t, sd.vm, spages, DRM_SGXFIVE_VM_MAP_READ_ONLY, &saddr, &sbo, &s);
	if (!ret)
		ret = surface(t, sd.vm, dpages, 0, &daddr, &dbo, &d);
	for (uint32_t i = 0; !ret && i < spages * 1024; i++)
		s[i] = pattern(i % sw, i / sw);
	src = (struct sgxfive_surface){ saddr, sw, srows, o->src_stride };
	dst = (struct sgxfive_surface){ daddr, dw, drows, o->dst_stride };
	if (!ret)
		ret = sgxfive_xfer_copy(sd.x, &src, o->sx, o->sy, &dst, &r, 0);
	if (!ret)
		ret = sgxfive_xfer_wait(sd.x, WAIT_NS);
	for (uint32_t i = 0; !ret && i < dpages * 1024; i++) {
		uint32_t x = i % dw, y = i / dw;
		bool in = x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
		uint32_t want = in ? pattern(o->sx + x - r.x0, o->sy + y - r.y0) : SENTINEL;

		if (d[i] != want && !bad++)
			printf("copy: dword %u (%u, %u) = 0x%08x, expected 0x%08x\n", i, x, y,
			       d[i], want);
	}
	sgxfive_bo_destroy(sbo);
	sgxfive_bo_destroy(dbo);
	side_fini(t, &sd);
	if (ret)
		return fail("copy", ret);
	if (bad) {
		printf("copy: FAIL, %u wrong dwords\n", bad);
		return 1;
	}
	printf("copy: ok, %ux%u from (%u, %u) to (%u, %u)\n", o->width, o->height, o->sx,
	       o->sy, o->x, o->y);
	return 0;
}

/* ---- isolation ---------------------------------------------------------------------- */

static int check(int *failed, bool ok, const char *name)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	*failed += !ok;
	return ok;
}

static bool untouched(const uint32_t *p, uint32_t words)
{
	for (uint32_t i = 0; i < words; i++)
		if (p[i] != SENTINEL)
			return false;
	return true;
}

static bool filled(const uint32_t *p, uint32_t words, uint32_t colour)
{
	for (uint32_t i = 0; i < words; i++)
		if (p[i] != colour)
			return false;
	return true;
}

/*
 * The xfer_iso / alias_ro / xfer_ro checks: two VMs, a transfer context in
 * each. Order matters: the fault checks lose context A, then context B.
 */
static int cmd_isolation(struct tool *t)
{
	const struct sgxfive_info *info = sgxfive_dev_info(t->dev);
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	struct sgxfive_bo *ba = NULL, *bb = NULL, *bo = NULL, *bro = NULL, *bk = NULL;
	struct sgxfive_bo *bq = NULL, *bm = NULL;
	uint32_t *pa = NULL, *pb = NULL, *po = NULL, *pro = NULL, *pq = NULL, *pm = NULL;
	uint64_t shared = 0, only_b = 0, ro = 0, only_d = 0, mine = 0;
	struct sgxfive_surface dst = { 0, 64, 32, 256 };
	struct side A, B, C = { 0 }, D = { 0 };
	long long t0;
	int failed = 0, ret;

	ret = side_init(t, &A);
	if (!ret)
		ret = side_init(t, &B);
	if (ret)
		return fail("isolation set-up", ret);

	/* 1. The kernel heaps can never be mapped. */
	ret = sgxfive_bo_create(t->dev, SGXFIVE_PAGE_SIZE, 0, &bk);
	check(&failed, !ret && sgxfive_vm_map(A.vm,
		info->heaps[DRM_SGXFIVE_HEAP_KERNEL_CODE].base, bk, 0, SGXFIVE_PAGE_SIZE, 0) ==
		-EINVAL && sgxfive_vm_map(A.vm, info->heaps[DRM_SGXFIVE_HEAP_KERNEL_DATA].base,
		bk, 0, SGXFIVE_PAGE_SIZE, DRM_SGXFIVE_VM_MAP_READ_ONLY) == -EINVAL,
	      "kernel-heap-unmappable");

	/* 2. One address, two VMs, two BOs: a fill through A lands only in A's BO. */
	ret = surface(t, A.vm, 2, 0, &shared, &ba, &pa);
	if (!ret)
		ret = surface(t, B.vm, 2, 0, &shared, &bb, &pb);
	dst.addr = shared;
	if (!ret)
		ret = sgxfive_xfer_fill(A.x, &dst, &r, 0xff0000aa, 0);
	if (!ret)
		ret = sgxfive_xfer_wait(A.x, WAIT_NS);
	check(&failed, !ret && filled(pa, 2048, 0xff0000aa) && untouched(pb, 2048),
	      "same-address-separate-vms");

	/* 3. An address mapped only in B: A's fill faults and writes nothing. */
	if (!ret)
		ret = sgxfive_vm_alloc(B.vm, DRM_SGXFIVE_HEAP_GENERAL, 0x100000, 0x100000,
				       &only_b);
	if (!ret)
		ret = surface(t, B.vm, 2, 0, &only_b, &bo, &po);
	dst.addr = only_b;
	if (!ret)
		ret = sgxfive_xfer_fill(A.x, &dst, &r, 0xff0000bb, 0);
	t0 = now_ns();
	if (!ret)
		sgxfive_xfer_wait(A.x, WAIT_NS);	/* completes with an error fence */
	t0 = now_ns() - t0;
	check(&failed, !ret && untouched(po, 2048), "other-vm-address-faults");
	printf("fault recovered after %lld ms\n", t0 / 1000000);
	check(&failed, !ret && t0 < FAULT_NS, "fault-recovered-quickly");

	/* 4. The faulting context is lost. */
	check(&failed, sgxfive_submit_transfer(t->dev, A.ctx,
		DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0) == -ECANCELED,
	      "lost-context-cancelled");

	/* 5. Context B is unaffected. */
	ret = sgxfive_xfer_fill(B.x, &dst, &r, 0xff0000cc, 0);
	if (!ret)
		ret = sgxfive_xfer_wait(B.x, WAIT_NS);
	check(&failed, !ret && filled(po, 2048, 0xff0000cc), "other-context-survives");

	/* 6. A read-only mapping cannot be written by the GPU. */
	ret = surface(t, B.vm, 2, DRM_SGXFIVE_VM_MAP_READ_ONLY, &ro, &bro, &pro);
	dst.addr = ro;
	if (!ret)
		ret = sgxfive_xfer_fill(B.x, &dst, &r, 0xff0000dd, 0);
	if (!ret)
		sgxfive_xfer_wait(B.x, WAIT_NS);
	check(&failed, !ret && untouched(pro, 2048) &&
		       sgxfive_xfer_fill(B.x, &dst, &r, 0xff0000dd, 0) == -ECANCELED,
	      "read-only-faults");

	/*
	 * 7. A job queued right behind a faulting one, on another context, still
	 * runs: the recovery reset re-runs it.
	 */
	ret = side_init(t, &C);
	if (!ret)
		ret = side_init(t, &D);
	if (!ret)
		ret = surface(t, D.vm, 2, 0, &only_d, &bq, &pq);
	if (!ret)
		ret = surface(t, D.vm, 2, 0, &mine, &bm, &pm);
	if (!ret) {
		dst.addr = only_d;
		ret = sgxfive_xfer_fill(C.x, &dst, &r, 0xff0000ee, 0);
	}
	if (!ret) {
		dst.addr = mine;
		ret = sgxfive_xfer_fill(D.x, &dst, &r, 0xff0000ff, 0);
	}
	if (!ret)
		ret = sgxfive_xfer_wait(D.x, WAIT_NS);
	check(&failed, !ret && filled(pm, 2048, 0xff0000ff) && untouched(pq, 2048) &&
		       sgxfive_submit_transfer(t->dev, C.ctx,
			       DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0) == -ECANCELED,
	      "queued-innocent-job-reruns");

	sgxfive_bo_destroy(bk);
	sgxfive_bo_destroy(ba);
	sgxfive_bo_destroy(bb);
	sgxfive_bo_destroy(bo);
	sgxfive_bo_destroy(bro);
	sgxfive_bo_destroy(bq);
	sgxfive_bo_destroy(bm);
	side_fini(t, &A);
	side_fini(t, &B);
	side_fini(t, &C);
	side_fini(t, &D);
	return failed ? 1 : 0;
}

/* ---- main --------------------------------------------------------------------------- */

static bool parse_u32(const char *s, uint32_t *v)
{
	char *end;
	unsigned long long x;

	errno = 0;
	x = strtoull(s, &end, 0);
	if (errno || !*s || *end || x > 0xffffffffull)
		return false;
	*v = (uint32_t)x;
	return true;
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	static const struct { const char *name; size_t off; } names[] = {
		{ "--width", offsetof(struct opts, width) },
		{ "--height", offsetof(struct opts, height) },
		{ "--x", offsetof(struct opts, x) }, { "--y", offsetof(struct opts, y) },
		{ "--dx", offsetof(struct opts, x) }, { "--dy", offsetof(struct opts, y) },
		{ "--stride", offsetof(struct opts, stride) },
		{ "--dst-stride", offsetof(struct opts, dst_stride) },
		{ "--src-stride", offsetof(struct opts, src_stride) },
		{ "--sx", offsetof(struct opts, sx) }, { "--sy", offsetof(struct opts, sy) },
		{ "--colour", offsetof(struct opts, colour) },
		{ "-n", offsetof(struct opts, n) },
	};

	for (int i = 0; i < argc; i += 2) {
		bool ok = false;

		for (size_t k = 0; k < sizeof(names) / sizeof(names[0]) && !ok; k++)
			if (!strcmp(argv[i], names[k].name) && i + 1 < argc)
				ok = parse_u32(argv[i + 1],
					       (uint32_t *)((char *)o + names[k].off));
		if (!ok) {
			fprintf(stderr, "sgxfive-test: bad option %s\n", argv[i]);
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct opts o = {
		.width = 64, .height = 32, .stride = 256, .colour = 0xff336699,
		.src_stride = 256, .dst_stride = 256, .n = 8,
	};
	struct sgxfive_mock_config mcfg = { 0 };
	const char *device = NULL, *cmd;
	bool use_mock = false, needs_programs, fake = false;
	struct tool t = { 0 };
	int i = 1, ret;

	for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
		if (!strcmp(argv[i], "--mock") || !strcmp(argv[i], "--mock=rev1")) {
			use_mock = true;
		} else if (!strcmp(argv[i], "--mock=harness")) {
			use_mock = true;
			mcfg.layout = SGXFIVE_MOCK_LAYOUT_HARNESS;
		} else if (!strcmp(argv[i], "--mock=none")) {
			use_mock = true;
			mcfg.no_sgxfive = true;
		} else if (!strcmp(argv[i], "--programs=asm")) {
			fake = false;
		} else if (!strcmp(argv[i], "--programs=fake")) {
			fake = true;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			device = argv[++i];
		} else {
			return usage();
		}
	}
	if (i >= argc)
		return usage();
	cmd = argv[i++];
	if (strcmp(cmd, "query") && strcmp(cmd, "dummy") && strcmp(cmd, "fill") &&
	    strcmp(cmd, "copy") && strcmp(cmd, "isolation"))
		return usage();
	if (parse_opts(argc - i, argv + i, &o))
		return 2;

	/*
	 * Programs: our assembled ones by default. The fake source's images are
	 * not runnable; refuse to send them to a real GPU.
	 */
	needs_programs = !strcmp(cmd, "fill") || !strcmp(cmd, "copy") ||
			 !strcmp(cmd, "isolation");
	if (needs_programs && fake && !use_mock) {
		fprintf(stderr, "sgxfive-test: the fake program source is for the mock only "
			"(use --mock, or the default --programs=asm)\n");
		return 2;
	}
	if (needs_programs) {
		t.progs = fake ? sgxfive_prog_fake_create() : sgxfive_prog_asm_create();
		if (!t.progs)
			return fail("programs", -ENOMEM);
	}

	if (use_mock)
		t.mock = sgxfive_mock_create(&mcfg);
	ret = sgxfive_dev_open(use_mock ? sgxfive_mock_backend(t.mock) : NULL, device, &t.dev);
	if (!ret && needs_programs)
		printf("programs %s\n", t.progs->name);
	if (ret) {
		if (ret == -ENODEV && !device)
			fprintf(stderr, "sgxfive-test: no sgxfive render node found\n");
		else
			fail(device ? device : "open", ret);
		ret = 1;
	} else if (!strcmp(cmd, "query")) {
		ret = cmd_query(&t);
	} else if (!strcmp(cmd, "dummy")) {
		ret = cmd_dummy(&t, &o);
	} else if (!strcmp(cmd, "fill")) {
		ret = cmd_fill(&t, &o);
	} else if (!strcmp(cmd, "copy")) {
		ret = cmd_copy(&t, &o);
	} else {
		ret = cmd_isolation(&t);
	}
	sgxfive_dev_close(t.dev);
	sgxfive_prog_source_destroy(t.progs);
	sgxfive_mock_destroy(t.mock);
	return ret;
}
