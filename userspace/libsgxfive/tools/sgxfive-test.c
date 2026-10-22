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
 *   tri   [--width W --height H -n N] [--max-width W --max-height H] [--fault]
 *                                 the first triangle on a render context, N times,
 *                                 every pixel against the CPU reference; the context
 *                                 takes targets up to --max-width x --max-height
 *                                 (default: the target's size); --fault unmaps the
 *                                 scene's stream first, so the render faults
 *   contexts [--width W --height H -n N]  N render contexts for targets up to W x H
 *                                 in one VM, and the memory they pin (/proc/meminfo)
 *   q4                            client shaders that touch SGX registers (Q4)
 *   probe [--temps N] [--coords] [--pixels] FILE...
 *                                 each FILE (tools/usse-asm output) as the fill
 *                                 shader of its own fill, with N temporaries
 *                                 (default 32) and a 64 KiB scratch buffer; prints
 *                                 o0 and the scratch dwords the program changed;
 *                                 --coords: as the copy shader of a copy whose
 *                                 source texel (x, y) is (y << 16) | x, so pa0 holds
 *                                 the pixel's coordinates; --pixels: every pixel too
 *
 * fill, copy and isolation take their GPU programs from the assembled source
 * (our own code, default) or the fake test double (--programs=fake, mock only).
 *
 * A job that faults or hangs is recovered and fails: the commands report its
 * error as the wait returns it ("job faulted, context lost (...)").
 *
 * Exit status: 0 all checks passed, 1 a check or call failed, 2 usage.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sgxfive_mock.h"
#include "sgxfive_render.h"
#include "sgxfive_xfer.h"

#define SENTINEL	0xdeadbeefu
#define WAIT_NS		(5LL * 1000000000LL)
/* A fault is found by the driver's BIF poll (a fault and no progress over two 50 ms
 * polls), not the 500 ms job timeout. */
#define FAULT_NS	(300LL * 1000000LL)

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static const char *const heap_names[DRM_SGXFIVE_HEAP_COUNT] = {
	"GENERAL", "TEXTURE", "PDS_PIXEL", "PDS_VERTEX", "USSE_PIXEL", "USSE_VERTEX",
	"KERNEL_CODE", "KERNEL_DATA", "PARAMETERS", "TA_DATA",
};

struct opts {
	uint32_t width, height, x, y, stride, colour;
	uint32_t sx, sy, src_stride, dst_stride;
	uint32_t n;
	uint32_t max_width, max_height;	/* tri: the render context's; 0: the target's */
	bool fault;			/* tri: unmap the scene's stream page first */
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
		"       commands: query, dummy [-n N], fill [...], copy [...], isolation, q4,\n"
		"                 tri [--width W --height H -n N --max-width W --max-height H\n"
		"                      --fault],\n"
		"                 contexts [--width W --height H -n N],\n"
		"                 probe [--temps N] [--coords] [--pixels] FILE...\n");
	return 2;
}

static int fail(const char *what, int err)
{
	fprintf(stderr, "sgxfive-test: %s: %s\n", what, strerror(-err));
	return 1;
}

/*
 * What a wait's error says about the job (sgxfive_syncobj_wait): a job that
 * faulted or hung was recovered and failed, and its context is lost. Other
 * errors are the wait's own, as strerror.
 */
static const char *job_error(int err, char *buf, size_t len)
{
	const char *what = err == -EIO ? "job faulted" : err == -ETIMEDOUT ? "job hung" :
			   err == -ECANCELED ? "job cancelled" : NULL;

	if (what)
		snprintf(buf, len, "%s, context lost (%s)", what, strerror(-err));
	else
		snprintf(buf, len, "%s", strerror(-err));
	return buf;
}

static int fail_job(const char *what, int err)
{
	char buf[96];

	fprintf(stderr, "sgxfive-test: %s: %s\n", what, job_error(err, buf, sizeof(buf)));
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
	printf("render_cmd      0x%x pb 0x%x pages max %ux%u\n", i->job.render_cmd_size,
	       i->job.render_pb_pages, i->job.render_max_width, i->job.render_max_height);
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
	struct sgxfive_vm *banned_vm = NULL;
	uint32_t banned_ctx = 0;
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
	/* The job is recovered and fails: its fence carries the fault. */
	check(&failed, !ret && sgxfive_xfer_wait(A.x, WAIT_NS) == -EIO && untouched(po, 2048),
	      "other-vm-address-faults");
	t0 = now_ns() - t0;
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

	/*
	 * 6. A job queued right behind a faulting one, on another context, still
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
		ret = sgxfive_xfer_wait(D.x, WAIT_NS);	/* re-run: no error */
	check(&failed, !ret && filled(pm, 2048, 0xff0000ff) && untouched(pq, 2048) &&
		       sgxfive_xfer_wait(C.x, WAIT_NS) == -EIO &&
		       sgxfive_submit_transfer(t->dev, C.ctx,
			       DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0) == -ECANCELED,
	      "queued-innocent-job-reruns");

	/* 7. A read-only mapping cannot be written by the GPU. */
	ret = surface(t, B.vm, 2, DRM_SGXFIVE_VM_MAP_READ_ONLY, &ro, &bro, &pro);
	dst.addr = ro;
	if (!ret)
		ret = sgxfive_xfer_fill(B.x, &dst, &r, 0xff0000dd, 0);
	check(&failed, !ret && sgxfive_xfer_wait(B.x, WAIT_NS) == -EIO && untouched(pro, 2048) &&
		       sgxfive_xfer_fill(B.x, &dst, &r, 0xff0000dd, 0) == -ECANCELED,
	      "read-only-faults");

	/*
	 * 8. That was the file's third fault within a minute: it may neither
	 * create contexts nor submit, even to a context that never faulted.
	 */
	ret = sgxfive_vm_create(t->dev, &banned_vm);
	check(&failed, !ret && sgxfive_ctx_create(banned_vm, &banned_ctx) == -ECANCELED &&
		       sgxfive_submit_transfer(t->dev, D.ctx,
			       DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY, NULL, 0, 0) == -ECANCELED,
	      "faulting-file-banned");
	if (banned_ctx)
		sgxfive_ctx_destroy(t->dev, banned_ctx);
	sgxfive_vm_destroy(banned_vm);

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

/* ---- q4: can a client's shader reach SGX registers? ---------------------------------- */

/*
 * Probe Q4 (design/sgx545_render_uapi.md §7): fill shaders that touch SGX
 * registers with the instructions the EDM uses for MMIO (sgx545_hw_spec.md
 * §13.6), run as an ordinary client fill. The words are tools/usse-asm output
 * for the source beside them (tests/test_cli.py checks that).
 */
static const uint64_t probe_ldr[] = {
	0xFA44070000000000ull,	/* phas #0, #0, pixel, end, parallel */
	0xFE21000020000007ull,	/* ldr r0, #7, drc0 (CORE_ID, 0x1c) */
	0xF920000000000000ull,	/* wdf drc0 */
	0x2885100120000000ull,	/* mov.skipinv.end o0, r0 */
};

static const uint64_t probe_str[] = {
	0xFA44070000000000ull,	/* phas #0, #0, pixel, end, parallel */
	0xFCA16170001E0001ull,	/* mov.skipinv r0, #0x5AFE0001 */
	0xFE2900002001401Full,	/* str #671, r0 (USE_G0, 0xa7c) */
	0xFCA7F1810000AA55ull,	/* mov.skipinv.end o0, #0xFF00AA55 */
};

/* A page directory's physical address (BIF_DIR_LIST_BASE0, 0xc84). */
static const uint64_t probe_ldr_dirlist[] = {
	0xFA44070000000000ull,	/* phas #0, #0, pixel, end, parallel */
	0xFE21000020018021ull,	/* ldr r0, #801, drc0 */
	0xF920000000000000ull,	/* wdf drc0 */
	0x2885100120000000ull,	/* mov.skipinv.end o0, r0 */
};

/* As probe_ldr, with r0 set first: does the ldr write r0 at all? */
static const uint64_t probe_ldr_marked[] = {
	0xFA44070000000000ull,	/* phas #0, #0, pixel, end, parallel */
	0xFCA0408000111111ull,	/* mov.skipinv r0, #0x11111111 */
	0xFE21000020000007ull,	/* ldr r0, #7, drc0 (CORE_ID, 0x1c) */
	0xF920000000000000ull,	/* wdf drc0 */
	0x2885100120000000ull,	/* mov.skipinv.end o0, r0 */
};

/* A store, then a load of the same register: did the store land? */
static const uint64_t probe_str_ldr[] = {
	0xFA44070000000000ull,	/* phas #0, #0, pixel, end, parallel */
	0xFCA16170001E0002ull,	/* mov.skipinv r0, #0x5AFE0002 */
	0xFE2900002001401Full,	/* str #671, r0 (USE_G0, 0xa7c) */
	0xFE2100002021401Full,	/* ldr r1, #671, drc0 */
	0xF920000000000000ull,	/* wdf drc0 */
	0x2885100120000080ull,	/* mov.skipinv.end o0, r1 */
};

/* A temporary the program never writes: what did earlier tasks leave? */
static const uint64_t probe_uninit[] = {
	0xFA44070000000000ull,	/* phas #0, #0, pixel, end, parallel */
	0x2885100120000000ull,	/* mov.skipinv.end o0, r0 */
};

/* A program source that is inner's, but with words for the fill shader. */
struct probe_source {
	struct sgxfive_prog_source base;
	struct sgxfive_prog_source *inner;
	const uint64_t *words;
	uint32_t n;
	bool cbase15;		/* the PDS primary kicks code base 15 instead */
	uint32_t id;		/* the program the words replace: USE_FILL or USE_COPY */
	uint32_t temps;		/* at least this many temporaries */
	uint64_t scratch;	/* 0, or where SCRATCH_IMM immediates point */
};

/*
 * A limm "mov d, #imm" whose immediate is SCRATCH_IMM | k (k < 0x10000) loads
 * the scratch buffer's address + k instead: probes of loads and stores address
 * a buffer the host fills and reads back.
 */
#define LIMM_MASK	0xFF30000000000000ull
#define LIMM_VAL	0xFC20000000000000ull
#define SCRATCH_IMM	0x5CAF0000u
#define SCRATCH_PAGES	16			/* 64 KiB */
#define SCRATCH_FILL(i)	(0xD0000000u | (i))	/* dword i of the scratch buffer, before */

static int probe_meta(struct sgxfive_prog_source *src, uint32_t id, struct sgxfive_prog_meta *m)
{
	struct probe_source *p = (struct probe_source *)src;
	int ret = p->inner->ops->meta(p->inner, id, m);

	if (!ret && id == p->id) {
		m->size = p->n * 8;
		if (m->temps < 4)
			m->temps = 4;		/* r0 */
		if (m->temps < p->temps)
			m->temps = p->temps;
	}
	return ret;
}

static int probe_build(struct sgxfive_prog_source *src, uint32_t id,
		       const struct sgxfive_prog_env *env, void *out)
{
	struct probe_source *p = (struct probe_source *)src;
	int ret;

	if (id == p->id) {
		uint64_t *w = out;

		memcpy(out, p->words, p->n * 8);
		for (uint32_t i = 0; p->scratch && i < p->n; i++) {
			uint32_t imm = sgxfive_limm_get(w[i]);

			if ((w[i] & LIMM_MASK) == LIMM_VAL && (imm & ~0xffffu) == SCRATCH_IMM)
				w[i] = sgxfive_limm_set(w[i],
							(uint32_t)p->scratch + (imm & 0xffff));
		}
		return 0;
	}
	ret = p->inner->ops->build(p->inner, id, env, out);
	if (!ret && id == SGXFIVE_PROG_PDS_PRIM && p->cbase15) {
		uint32_t u0;

		/* Its DOUTU word U0 (the KICKONLY data, byte 0): EXE 0, CBASE 15. */
		memcpy(&u0, out, 4);
		u0 = (u0 & 0xff000000u) | 0xf;
		memcpy(out, &u0, 4);
	}
	return ret;
}

static void probe_destroy(struct sgxfive_prog_source *src)
{
}

static const struct sgxfive_prog_source_ops probe_ops = {
	.meta = probe_meta, .build = probe_build, .destroy = probe_destroy,
};

/* Fill 64x32 with the probe as the fill shader; report what reached memory. */
static int run_probe(struct tool *t, const char *name, const uint64_t *words, uint32_t n,
		     bool cbase15)
{
	struct probe_source src = {
		{ &probe_ops, name }, t->progs, words, n, cbase15, SGXFIVE_PROG_USE_FILL, 0, 0,
	};
	struct sgxfive_prog_source *saved = t->progs;
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	struct sgxfive_surface dst = { 0, 64, 32, 256 };
	struct sgxfive_bo *bo = NULL;
	uint32_t *p = NULL, same = 0;
	uint64_t addr = 0;
	struct side s;
	char why[96];
	int ret, job = 0;

	t->progs = &src.base;
	ret = side_init(t, &s);
	t->progs = saved;
	if (!ret)
		ret = surface(t, s.vm, 2, 0, &addr, &bo, &p);
	dst.addr = addr;
	if (!ret)
		ret = sgxfive_xfer_fill(s.x, &dst, &r, 0, 0);
	if (!ret)
		job = sgxfive_xfer_wait(s.x, WAIT_NS);
	if (p)
		for (uint32_t i = 0; i < 2048; i++)
			same += p[i] == p[0];
	if (ret) {
		printf("%s: failed: %s\n", name, strerror(-ret));
	} else if (job) {
		printf("%s: job failed: %s\n", name, job_error(job, why, sizeof(why)));
	} else {
		uint32_t val[8], cnt[8], nv = 0, other = 0;

		printf("%s: job completed, dword 0 = 0x%08x, %u of 2048 equal\n", name, p[0], same);
		for (uint32_t i = 0; i < 2048; i++) {
			uint32_t k = 0;

			while (k < nv && val[k] != p[i])
				k++;
			if (k < nv)
				cnt[k]++;
			else if (nv < 8)
				val[nv] = p[i], cnt[nv++] = 1;
			else
				other++;
		}
		for (uint32_t k = 0; k < nv; k++)
			printf("  0x%08x x%u\n", val[k], cnt[k]);
		if (other)
			printf("  (%u dwords with other values)\n", other);
		for (uint32_t y = 0; y < 4; y++)
			printf("  row %u: %08x %08x %08x %08x ... %08x\n", y, p[y * 64], p[y * 64 + 1],
			       p[y * 64 + 2], p[y * 64 + 3], p[y * 64 + 63]);
	}
	sgxfive_bo_destroy(bo);
	side_fini(t, &s);
	return 0;
}

static int cmd_q4(struct tool *t)
{
	run_probe(t, "uninit-temps", probe_uninit, 2, false);
	run_probe(t, "ldr-core-id", probe_ldr, 4, false);
	run_probe(t, "ldr-marked", probe_ldr_marked, 5, false);
	run_probe(t, "ldr-dir-list-base0", probe_ldr_dirlist, 4, false);
	run_probe(t, "str-use-g0", probe_str, 4, false);
	run_probe(t, "str-then-ldr-use-g0", probe_str_ldr, 6, false);
	run_probe(t, "kick-code-base-15", probe_ldr, 4, true);
	return 0;
}

/* ---- probe: our own programs as the fill shader --------------------------------------- */

#define PROBE_MAX_WORDS	256

struct probe_file {
	const char *name;	/* the file's base name without .bin */
	char label[64];
	uint64_t words[PROBE_MAX_WORDS];
	uint32_t n;
};

/* Read a raw little-endian USSE program (tools/usse-asm -o) into f. */
static int probe_read(const char *path, struct probe_file *f)
{
	const char *base = strrchr(path, '/');
	uint8_t buf[PROBE_MAX_WORDS * 8 + 1];
	size_t len, dot;
	FILE *fp = fopen(path, "rb");

	if (!fp) {
		fprintf(stderr, "sgxfive-test: %s: %s\n", path, strerror(errno));
		return -1;
	}
	len = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if (!len || len % 8 || len > PROBE_MAX_WORDS * 8) {
		fprintf(stderr, "sgxfive-test: %s: not 1 to %u USSE words\n", path,
			PROBE_MAX_WORDS);
		return -1;
	}
	f->n = (uint32_t)(len / 8);
	for (uint32_t i = 0; i < f->n; i++) {
		f->words[i] = 0;
		for (unsigned int b = 0; b < 8; b++)
			f->words[i] |= (uint64_t)buf[i * 8 + b] << (8 * b);
	}
	base = base ? base + 1 : path;
	dot = strcspn(base, ".");
	snprintf(f->label, sizeof(f->label), "%.*s", (int)dot, base);
	f->name = f->label;
	return 0;
}

static bool pixels_all(const uint32_t *p, uint32_t n, uint32_t v)
{
	for (uint32_t i = 0; i < n; i++)
		if (p[i] != v)
			return false;
	return true;
}

struct probe_opts {
	uint32_t temps;		/* at least this many temporaries */
	bool coords;		/* the copy shader of a copy of the coordinates */
	bool pixels;		/* print every pixel */
};

/* The texel (x, y) of a --coords probe's source. */
static uint32_t coord_texel(uint32_t x, uint32_t y)
{
	return y << 16 | x;
}

/*
 * One probe: a 64x32 fill whose shader is the file's program, with temps
 * temporaries and a 64 KiB scratch buffer (dword i = SCRATCH_FILL(i)) its
 * SCRATCH_IMM immediates point at; with coords, a 64x32 copy whose shader
 * is the program and whose source texel (x, y) is coord_texel(x, y). Prints
 * o0 as the surface holds it (the four most frequent values when the pixels
 * differ; every pixel, row by row, with pixels) and every scratch dword the
 * program changed; a failed job is reported and recovered from.
 */
static void probe_one(struct tool *t, const struct probe_file *f, const struct probe_opts *po)
{
	struct probe_source src = {
		{ &probe_ops, f->name }, t->progs, f->words, f->n, false,
		po->coords ? SGXFIVE_PROG_USE_COPY : SGXFIVE_PROG_USE_FILL, po->temps, 0,
	};
	struct sgxfive_prog_source *saved = t->progs;
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	struct sgxfive_surface dst = { 0, 64, 32, 256 };
	struct sgxfive_bo *bo = NULL, *sbo = NULL, *cbo = NULL;
	uint32_t *p = NULL, *sp = NULL, *cp = NULL;
	uint64_t addr = 0, saddr = 0, caddr = 0;
	struct side s;
	char why[96];
	int ret;

	t->progs = &src.base;
	ret = side_init(t, &s);
	t->progs = saved;
	if (!ret)
		ret = surface(t, s.vm, 2, 0, &addr, &bo, &p);
	if (!ret)
		ret = surface(t, s.vm, SCRATCH_PAGES, 0, &saddr, &sbo, &sp);
	if (!ret && po->coords)
		ret = surface(t, s.vm, 3, DRM_SGXFIVE_VM_MAP_READ_ONLY, &caddr, &cbo, &cp);
	for (uint32_t i = 0; !ret && po->coords && i < 2048; i++)
		cp[i] = coord_texel(i % 64, i / 64);
	if (!ret) {
		for (uint32_t i = 0; i < SCRATCH_PAGES * SGXFIVE_PAGE_SIZE / 4; i++)
			sp[i] = SCRATCH_FILL(i);
		src.scratch = saddr;
		printf("probe %s: scratch at 0x%08llx (%u KiB)\n", f->name,
		       (unsigned long long)saddr, SCRATCH_PAGES * SGXFIVE_PAGE_SIZE / 1024);
		dst.addr = addr;
		if (po->coords) {
			const struct sgxfive_surface csrc = { caddr, 64, 32, 256 };

			ret = sgxfive_xfer_copy(s.x, &csrc, 0, 0, &dst, &r, 0);
		} else {
			ret = sgxfive_xfer_fill(s.x, &dst, &r, 0, 0);
		}
	}
	if (ret) {
		printf("probe %s: failed: %s\n", f->name, strerror(-ret));
	} else if ((ret = sgxfive_xfer_wait(s.x, WAIT_NS))) {
		/* faulted or hung: recovered, the job failed and the context is lost */
		printf("probe %s: failed: %s\n", f->name, job_error(ret, why, sizeof(why)));
	} else if (pixels_all(p, 2048, SENTINEL)) {
		/* the job completed without writing a pixel */
		printf("probe %s: failed: no pixel written\n", f->name);
	} else {
		uint32_t val[64], cnt[64], nv = 0, other = 0, shown = 0;

		for (uint32_t i = 0; i < 2048; i++) {
			uint32_t k = 0;

			while (k < nv && val[k] != p[i])
				k++;
			if (k < nv)
				cnt[k]++;
			else if (nv < 64)
				val[nv] = p[i], cnt[nv++] = 1;
			else
				other++;
		}
		/* the most frequent first */
		for (uint32_t a = 1; a < nv; a++)
			for (uint32_t b = a; b > 0 && cnt[b] > cnt[b - 1]; b--) {
				uint32_t tv = val[b], tc = cnt[b];

				val[b] = val[b - 1], cnt[b] = cnt[b - 1];
				val[b - 1] = tv, cnt[b - 1] = tc;
			}
		if (nv == 1) {
			printf("probe %s: o0 0x%08x (2048 of 2048)\n", f->name, val[0]);
		} else {
			printf("probe %s: o0 mixed", f->name);
			for (uint32_t k = 0; k < nv; k++) {
				if (k < 4)
					printf(" 0x%08x x%u", val[k], cnt[k]);
				else
					other += cnt[k];
			}
			if (other)
				printf(" other x%u", other);
			printf("\n");
		}
		for (uint32_t y = 0; po->pixels && y < 32; y++) {
			printf("probe %s: row %u:", f->name, y);
			for (uint32_t x = 0; x < 64; x++)
				printf(" %08x", p[y * 64 + x]);
			printf("\n");
		}
		for (uint32_t i = 0; i < SCRATCH_PAGES * SGXFIVE_PAGE_SIZE / 4; i++) {
			if (sp[i] == SCRATCH_FILL(i))
				continue;
			if (shown++ < 32)
				printf("probe %s: scratch +0x%03x 0x%08x\n", f->name, i * 4, sp[i]);
		}
		if (shown > 32)
			printf("probe %s: scratch %u more changed\n", f->name, shown - 32);
	}
	sgxfive_bo_destroy(cbo);
	sgxfive_bo_destroy(sbo);
	sgxfive_bo_destroy(bo);
	side_fini(t, &s);
}

static int cmd_probe(struct tool *t, struct probe_file *files, uint32_t n,
		     const struct probe_opts *po)
{
	for (uint32_t i = 0; i < n; i++)
		probe_one(t, &files[i], po);
	return 0;
}

/* ---- main --------------------------------------------------------------------------- */

/* ---- tri ---------------------------------------------------------------------------- */

/* The image as text, one character a pixel: # triangle, . background, ? other. */
static void tri_show(const struct sgxfive_tri_params *p, const uint32_t *fb)
{
	for (uint32_t y = 0; y < p->height && y < 128; y++) {
		for (uint32_t x = 0; x < p->width && x < 128; x++) {
			uint32_t v = fb[y * p->width + x];

			putchar(v == p->colour ? '#' : v == p->bg_colour ? '.' :
				v == SGXFIVE_TRI_SENTINEL ? 's' : '?');
		}
		putchar('\n');
	}
}

/*
 * The first triangle (design/sgx545_3d.md §14) from userspace: a render
 * context, our programs and scene in the VM, n renders with alternating
 * colours, every pixel against the CPU reference and the guard area intact.
 */
static int cmd_tri(struct tool *t, const struct opts *o)
{
	static const uint32_t colours[2][2] = {
		{ 0xff00ff00, 0xff203040 }, { 0xffff0000, 0xff000080 },
	};
	struct sgxfive_render *r = NULL;
	struct sgxfive_tri_params p;
	struct sgxfive_vm *vm = NULL;
	uint32_t ctx = 0, ok = 0;
	const uint32_t max_w = o->max_width == UINT32_MAX ? o->width : o->max_width;
	const uint32_t max_h = o->max_height == UINT32_MAX ? o->height : o->max_height;
	int ret;

	ret = sgxfive_vm_create(t->dev, &vm);
	if (!ret)
		ret = sgxfive_render_ctx_create(vm, max_w, max_h, &ctx);
	if (!ret)
		ret = sgxfive_render_create(vm, ctx, &r);
	if (ret) {
		fail("render context", ret);
		goto out;
	}
	printf("tri: render context for targets up to %ux%u\n", max_w, max_h);
	if (o->fault) {
		struct sgxfive_tri_layout l;

		sgxfive_render_get_layout(r, &l);
		ret = sgxfive_vm_unmap(vm, l.gen, SGXFIVE_PAGE_SIZE);
		if (ret) {
			fail("unmap the stream", ret);
			goto out;
		}
	}
	for (uint32_t i = 0; i < o->n; i++) {
		struct sgxfive_tri_counts n;
		long long t0 = now_ns();
		uint32_t guard;

		sgxfive_tri_default(&p, o->width, o->height);
		p.colour = colours[i % 2][0];
		p.bg_colour = colours[i % 2][1];
		ret = sgxfive_render_tri(r, &p, 0, 0);
		if (ret) {
			fail("render", ret);
			break;
		}
		ret = sgxfive_render_wait(r, WAIT_NS);
		if (ret) {
			fail_job("render", ret);
			break;
		}
		ret = sgxfive_tri_check(&p, sgxfive_render_fb(r), &n);
		guard = sgxfive_render_guard_dirty(r, &p);
		printf("tri %ux%u #%u: %u triangle, %u background, %u other; wrong %u inside, "
		       "%u outside, %u at edges; %u if flipped; guard %u\n", p.width, p.height,
		       i + 1, n.tri, n.bg, n.other, n.wrong_in, n.wrong_out, n.wrong_edge,
		       n.flipped, guard);
		printf("tri %ux%u #%u: %lld us\n", p.width, p.height, i + 1,
		       (now_ns() - t0) / 1000);
		if (ret || guard) {
			tri_show(&p, sgxfive_render_fb(r));
			ret = ret ? ret : -EIO;
			break;
		}
		ok++;
	}
	if (!ret)
		printf("tri: %u renders ok\n", ok);
	else
		printf("tri: FAILED after %u renders\n", ok);
out:
	sgxfive_render_destroy(r);
	if (ctx)
		sgxfive_ctx_destroy(t->dev, ctx);
	sgxfive_vm_destroy(vm);
	return ret ? 1 : 0;
}

/* ---- contexts ----------------------------------------------------------------------- */

/* MemFree and, on a 32-bit kernel with highmem, LowFree in KiB; -1 if absent. */
static void meminfo(long long *mem_free, long long *low_free)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];

	*mem_free = *low_free = -1;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "MemFree: %lld kB", mem_free);
		sscanf(line, "LowFree: %lld kB", low_free);
	}
	fclose(f);
}

/*
 * The memory render contexts pin (design/sgx545_render_uapi.md §11.10, R13):
 * n render contexts for targets up to W x H in one VM, /proc/meminfo before
 * and after creating them. Other activity on the machine shows up too, so
 * compare runs, not single numbers.
 */
static int cmd_contexts(struct tool *t, const struct opts *o)
{
	uint32_t ctxs[64], n = 0;
	long long free0, low0, free1, low1;
	struct sgxfive_vm *vm = NULL;
	int ret;

	if (o->n > sizeof(ctxs) / sizeof(ctxs[0]))
		return fail("contexts", -E2BIG);
	ret = sgxfive_vm_create(t->dev, &vm);
	if (ret)
		return fail("vm", ret);
	meminfo(&free0, &low0);
	for (; n < o->n; n++) {
		ret = sgxfive_render_ctx_create(vm, o->width, o->height, &ctxs[n]);
		if (ret) {
			char what[32];

			snprintf(what, sizeof(what), "render context %u", n + 1);
			fail(what, ret);
			break;
		}
	}
	meminfo(&free1, &low1);
	if (!ret) {
		printf("contexts: %u render contexts for targets up to %ux%u: ", n, o->width,
		       o->height);
		if (free0 < 0 || free1 < 0)
			printf("no /proc/meminfo\n");
		else
			printf("MemFree -%lld KiB (%lld KiB each)%s", free0 - free1,
			       n ? (free0 - free1) / n : 0, low0 < 0 || low1 < 0 ? "\n" : ", ");
		if (free0 >= 0 && free1 >= 0 && low0 >= 0 && low1 >= 0)
			printf("LowFree -%lld KiB (%lld KiB each)\n", low0 - low1,
			       n ? (low0 - low1) / n : 0);
	}
	while (n)
		sgxfive_ctx_destroy(t->dev, ctxs[--n]);
	sgxfive_vm_destroy(vm);
	if (!ret)
		printf("contexts: destroyed\n");
	return ret ? 1 : 0;
}

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
		{ "--max-width", offsetof(struct opts, max_width) },
		{ "--max-height", offsetof(struct opts, max_height) },
	};

	for (int i = 0; i < argc; i += 2) {
		bool ok = false;

		if (!strcmp(argv[i], "--fault")) {
			o->fault = true;
			i--;		/* a flag: no value */
			continue;
		}
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
		.max_width = UINT32_MAX, .max_height = UINT32_MAX,
	};
	struct sgxfive_mock_config mcfg = { 0 };
	const char *device = NULL, *cmd;
	bool use_mock = false, needs_programs, fake = false;
	struct tool t = { 0 };
	struct probe_file *probes = NULL;
	struct probe_opts popts = { .temps = 32 };
	uint32_t nprobes = 0;
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
	    strcmp(cmd, "copy") && strcmp(cmd, "isolation") && strcmp(cmd, "q4") &&
	    strcmp(cmd, "tri") && strcmp(cmd, "contexts") && strcmp(cmd, "probe"))
		return usage();
	if (!strcmp(cmd, "probe")) {
		for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
			char *end;
			unsigned long v;

			if (!strcmp(argv[i], "--coords")) {
				popts.coords = true;
				continue;
			}
			if (!strcmp(argv[i], "--pixels")) {
				popts.pixels = true;
				continue;
			}
			if (strcmp(argv[i], "--temps") || i + 1 >= argc)
				return usage();
			v = strtoul(argv[++i], &end, 0);
			if (*end || v < 1 || v > 128)
				return usage();
			popts.temps = (uint32_t)v;
		}
		if (i >= argc)
			return usage();
		nprobes = (uint32_t)(argc - i);
		probes = calloc(nprobes, sizeof(*probes));
		if (!probes)
			return fail("probe", -ENOMEM);
		for (uint32_t k = 0; k < nprobes; k++) {
			if (probe_read(argv[i + k], &probes[k])) {
				free(probes);
				return 1;
			}
		}
		i = argc;
	}
	if (!strcmp(cmd, "tri")) {
		o.height = 64;			/* the first triangle's 64 x 64 */
		o.n = 1;
	}
	if (!strcmp(cmd, "contexts")) {
		o.height = 64;			/* 32 contexts for 64 x 64 targets */
		o.n = 32;
	}
	if (parse_opts(argc - i, argv + i, &o))
		return 2;

	/*
	 * Programs: our assembled ones by default. The fake source's images are
	 * not runnable; refuse to send them to a real GPU.
	 */
	needs_programs = !strcmp(cmd, "fill") || !strcmp(cmd, "copy") ||
			 !strcmp(cmd, "isolation") || !strcmp(cmd, "q4") || !strcmp(cmd, "probe");
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
	} else if (!strcmp(cmd, "q4")) {
		ret = cmd_q4(&t);
	} else if (!strcmp(cmd, "probe")) {
		ret = cmd_probe(&t, probes, nprobes, &popts);
	} else if (!strcmp(cmd, "tri")) {
		ret = cmd_tri(&t, &o);
	} else if (!strcmp(cmd, "contexts")) {
		ret = cmd_contexts(&t, &o);
	} else {
		ret = cmd_isolation(&t);
	}
	sgxfive_dev_close(t.dev);
	sgxfive_prog_source_destroy(t.progs);
	sgxfive_mock_destroy(t.mock);
	free(probes);
	return ret;
}
