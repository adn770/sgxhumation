// SPDX-License-Identifier: MIT
/*
 * Fills and copies through the transfer engine, run by the mock, once with the
 * fake program source and once with the assembled one (our own programs): the programs reproduce the debugfs recipes word for word
 * (design/sgx545_transfer.md §4.2.7, §4.4.5) when the heaps are the harness
 * layout, the pixels land exactly, and isolation and read-only mappings hold.
 */
#include <errno.h>
#include <string.h>

#include "sgxfive_mock.h"
#include "sgxfive_xfer.h"
#include "tap.h"

#define SENTINEL 0xdeadbeefu
#define NSEC 1000000000LL

struct env {
	struct sgxfive_mock *mock;
	struct sgxfive_dev *dev;
	struct sgxfive_vm *vm;
	uint32_t ctx;
	struct sgxfive_prog_source *src;
	struct sgxfive_xfer *x;
};

/* The program source of this pass: sgxfive_prog_fake_create or _asm_create. */
static struct sgxfive_prog_source *(*make_source)(void);

static void env_init(struct env *e, enum sgxfive_mock_layout layout)
{
	const struct sgxfive_mock_config cfg = { .layout = layout };

	memset(e, 0, sizeof(*e));
	e->mock = sgxfive_mock_create(&cfg);
	CHECK_EQ(sgxfive_dev_open(sgxfive_mock_backend(e->mock), NULL, &e->dev), 0);
	CHECK_EQ(sgxfive_vm_create(e->dev, &e->vm), 0);
	CHECK_EQ(sgxfive_ctx_create(e->vm, &e->ctx), 0);
	e->src = make_source();
	CHECK_EQ(sgxfive_xfer_create(e->vm, e->ctx, e->src, &e->x), 0);
}

static void env_fini(struct env *e)
{
	sgxfive_xfer_destroy(e->x);
	sgxfive_prog_source_destroy(e->src);
	sgxfive_ctx_destroy(e->dev, e->ctx);
	sgxfive_vm_destroy(e->vm);
	sgxfive_dev_close(e->dev);
	sgxfive_mock_destroy(e->mock);
}

/* A CPU-visible surface BO of `pages` pages mapped in the GENERAL heap. */
static uint32_t *surface(struct env *e, struct sgxfive_vm *vm, uint32_t pages,
			 uint32_t map_flags, uint64_t *addr, struct sgxfive_bo **bo)
{
	void *p = NULL;

	CHECK_EQ(sgxfive_bo_create(e->dev, pages * 4096ull, DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS,
				   bo), 0);
	CHECK_EQ(sgxfive_bo_mmap(*bo, &p), 0);
	if (!*addr)
		CHECK_EQ(sgxfive_vm_alloc(vm, DRM_SGXFIVE_HEAP_GENERAL, pages * 4096ull, 4096,
					  addr), 0);
	CHECK_EQ(sgxfive_vm_map(vm, *addr, *bo, 0, pages * 4096ull, map_flags), 0);
	for (uint32_t i = 0; i < pages * 1024; i++)
		((uint32_t *)p)[i] = SENTINEL;
	return p;
}

static int words_at(const void *page, uint32_t off, const uint32_t *want, unsigned int n)
{
	const uint32_t *w = (const uint32_t *)((const uint8_t *)page + off);

	for (unsigned int i = 0; i < n; i++) {
		if (w[i] != want[i]) {
			printf("#   +%#x word %u: got %08x, expected %08x\n", off, i, w[i],
			       want[i]);
			return 0;
		}
	}
	return 1;
}

/* Every dword of the stride-wide surface: colour inside r, sentinel elsewhere. */
static int check_fill(const uint32_t *p, uint32_t words, uint32_t stride,
		      const struct sgxfive_rect *r, uint32_t colour)
{
	for (uint32_t i = 0; i < words; i++) {
		uint32_t x = i % (stride / 4), y = i / (stride / 4);
		int in = x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
		uint32_t want = in ? colour : SENTINEL;

		if (p[i] != want) {
			printf("#   dword %u (%u, %u): got %08x, expected %08x\n", i, x, y, p[i],
			       want);
			return 0;
		}
	}
	return 1;
}

static void fill_recipe(void)
{
	const uint32_t pds_prim[] = { 0, 4, 0, 0, 0x07000305, 0xaf000000 };
	const uint32_t pds_sec[] = { 0x20, 4, 0, 0, 0x07000305, 0xaf000000 };
	const uint32_t pixev[] = {
		0x01000040, 0x00800000, 0, 0x01000040, 0x010000c0, 0x00c00000,
	};
	const uint32_t shader[] = { 0, 0xfa440700, 0x00136699, 0xfca7f191 };
	const uint32_t eor[] = { 0, 0xfa440700, 0, 0xfca00000, 0x80200000, 0xfb260040 };
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	struct sgxfive_xfer_layout l;
	struct sgxfive_bo *bo = NULL;
	struct sgxfive_surface dst;
	const void *use, *pds;
	uint64_t addr = 0;
	uint32_t eot[14];
	struct env e;
	uint32_t *p;

	env_init(&e, SGXFIVE_MOCK_LAYOUT_HARNESS);
	p = surface(&e, e.vm, 3, 0, &addr, &bo);
	dst = (struct sgxfive_surface){ addr, 64, 32, 256 };
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &r, 0xff336699, 0), 0);
	CHECK_EQ(sgxfive_xfer_wait(e.x, NSEC), 0);
	CHECK(check_fill(p, 3 * 1024, 256, &r, 0xff336699));

	sgxfive_xfer_get_layout(e.x, &l);
	CHECK_EQ(l.pds_page, 0xE4000000);
	CHECK_EQ(l.use_page, 0xF4000000);
	CHECK_EQ(l.prog[SGXFIVE_PROG_PDS_PIXEVENT], 0xE4000040);
	CHECK_EQ(l.prog[SGXFIVE_PROG_USE_EOR], 0xF4000060);
	pds = sgxfive_xfer_page(e.x, DRM_SGXFIVE_HEAP_PDS_PIXEL);
	use = sgxfive_xfer_page(e.x, DRM_SGXFIVE_HEAP_USSE_PIXEL);
	CHECK(words_at(pds, 0x00, pds_prim, 6));
	CHECK(words_at(pds, 0x20, pds_sec, 6));
	CHECK(words_at(pds, 0x40, pixev, 6));
	CHECK_EQ(((const uint32_t *)pds)[0x40 / 4 + 12], 0x00800000);
	CHECK(words_at(use, 0x00, shader, 4));
	CHECK(words_at(use, 0x60, eor, 6));

	/* EOT for the PBE state 00000000 001f003f 001f8000 <addr> 00000200 (§4.2.7 step 6). */
	memcpy(eot, (const uint32_t[]){ 0, 0xfa440700, 0, 0xfca00000, 0x003f003f, 0xfca00000,
					 0x005f8000, 0xfca00000, 0, 0, 0x00200080, 0xfb200100,
					 0x00208180, 0xfb240040 }, sizeof(eot));
	{
		uint64_t mov = sgxfive_limm_set((uint64_t)0xfca00000 << 32 | 3u << 21,
						(uint32_t)addr);

		eot[8] = (uint32_t)mov;
		eot[9] = (uint32_t)(mov >> 32);
	}
	CHECK(words_at(use, 0x20, eot, 14));

	{
		const struct drm_sgxfive_transfer_cmd *c = sgxfive_xfer_last_cmd(e.x);

		CHECK_EQ(c->event_pixel_pds_exec, 0xE4000040);
		CHECK_EQ(c->pds_exec_base, 0xE4000000);
		CHECK_EQ(c->bif_3d_req_base, l.bg & ~0xfffffull);
		CHECK_EQ(c->isp_bgobjtag, (l.bg - c->bif_3d_req_base) >> 4);
		CHECK_EQ(c->isp_render_box2, 0x00010001);
	}
	sgxfive_bo_destroy(bo);
	env_fini(&e);
}

static void fill_rects(enum sgxfive_mock_layout layout)
{
	const struct {
		uint32_t stride;
		struct sgxfive_rect r;
	} cases[] = {
		{ 320, { 8, 4, 40, 20 } },	/* 32 x 16 inset, padded stride */
		{ 320, { 8, 4, 41, 20 } },	/* 33 x 16 across tiles */
		{ 256, { 5, 7, 6, 8 } },	/* one pixel */
	};
	struct env e;

	env_init(&e, layout);
	for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct sgxfive_bo *bo = NULL;
		uint64_t addr = 0;
		uint32_t *p = surface(&e, e.vm, 4, 0, &addr, &bo);
		struct sgxfive_surface dst = { addr, cases[i].stride / 4, 32, cases[i].stride };
		uint32_t colour = 0xff000000 | i * 0x10101;

		CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &cases[i].r, colour, 0), 0);
		CHECK_EQ(sgxfive_xfer_wait(e.x, NSEC), 0);
		CHECK(check_fill(p, 4 * 1024, cases[i].stride, &cases[i].r, colour));
		sgxfive_bo_destroy(bo);
	}
	env_fini(&e);
}

static void fill_errors(void)
{
	const struct sgxfive_rect r = { 0, 0, 64, 32 }, out = { 0, 0, 65, 32 };
	struct sgxfive_surface dst = { 0x3e000000, 64, 32, 256 };
	struct env e;

	env_init(&e, SGXFIVE_MOCK_LAYOUT_REV1);
	dst.stride = 260;			/* odd number of pixels */
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &r, 0, 0), -EINVAL);
	dst.stride = 256;
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &out, 0, 0), -EINVAL);
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &(struct sgxfive_rect){ 4, 4, 4, 8 }, 0, 0),
		 -EINVAL);			/* empty */
	dst.addr = 0x3e000002;
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &r, 0, 0), -EINVAL);
	env_fini(&e);
}

static uint32_t pattern(uint32_t x, uint32_t y)
{
	return 0xa5000000 | (y & 0xff) << 16 | (x & 0xff) << 8 | ((x ^ y) & 0xff);
}

static void copy_case(struct env *e, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy,
		      uint32_t sstride, uint32_t dx, uint32_t dy, uint32_t dstride)
{
	struct sgxfive_bo *sbo = NULL, *dbo = NULL;
	uint64_t saddr = 0, daddr = 0;
	uint32_t *s = surface(e, e->vm, 4, DRM_SGXFIVE_VM_MAP_READ_ONLY, &saddr, &sbo);
	uint32_t *d = surface(e, e->vm, 4, 0, &daddr, &dbo);
	const uint32_t sw = sstride / 4, dw = dstride / 4;
	struct sgxfive_surface src = { saddr, sw, 4096 / sstride * 4, sstride };
	struct sgxfive_surface dst = { daddr, dw, 4096 / dstride * 4, dstride };
	const struct sgxfive_rect r = { dx, dy, dx + w, dy + h };
	int ok = 1;

	for (uint32_t i = 0; i < 4 * 1024; i++)
		s[i] = pattern(i % sw, i / sw);
	CHECK_EQ(sgxfive_xfer_copy(e->x, &src, sx, sy, &dst, &r, 0), 0);
	CHECK_EQ(sgxfive_xfer_wait(e->x, NSEC), 0);
	for (uint32_t i = 0; i < 4 * 1024 && ok; i++) {
		uint32_t x = i % dw, y = i / dw;
		int in = x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
		uint32_t want = in ? pattern(sx + x - dx, sy + y - dy) : SENTINEL;

		if (d[i] != want) {
			printf("#   dst (%u, %u): got %08x, expected %08x\n", x, y, d[i], want);
			ok = 0;
		}
	}
	CHECK(ok);
	CHECK_EQ(s[5], pattern(5, 0));		/* the source is unchanged */
	sgxfive_bo_destroy(sbo);
	sgxfive_bo_destroy(dbo);
}

static void copy_recipe(void)
{
	struct sgxfive_xfer_layout l;
	const void *pds, *use;
	struct env e;

	env_init(&e, SGXFIVE_MOCK_LAYOUT_HARNESS);
	copy_case(&e, 64, 32, 0, 0, 256, 0, 0, 256);
	sgxfive_xfer_get_layout(e.x, &l);
	pds = sgxfive_xfer_page(e.x, DRM_SGXFIVE_HEAP_PDS_PIXEL);
	use = sgxfive_xfer_page(e.x, DRM_SGXFIVE_HEAP_USSE_PIXEL);
	{
		const uint32_t src = ((const uint32_t *)pds)[6];
		const uint32_t prim[] = {
			0, 0x02000004, 0, 0, 0x40012001, 0, src, 0xa0800000,
			0x000e0e00, 0xcc03f01f, 0, 0, 0x07000305, 0x07040202, 0x07082364,
			0xaf000000,
		};
		const uint32_t sec[] = { 0x20, 4, 0, 0, 0x07000305, 0xaf000000 };
		const uint32_t shader[] = { 0, 0xfa440700, 0xa0000000, 0x28851001 };

		CHECK(words_at(pds, 0, prim, 16));
		CHECK(words_at(pds, 0x40, sec, 6));
		CHECK(words_at(use, 0, shader, 4));
	}
	CHECK_EQ(l.prog[SGXFIVE_PROG_PDS_PIXEVENT], 0xE4000060);
	CHECK_EQ(sgxfive_xfer_last_cmd(e.x)->event_pixel_pds_exec, 0xE4000060);
	CHECK_EQ(sgxfive_xfer_last_cmd(e.x)->isp_bgobjtag >> 24, 2);	/* TSPDATASIZE */
	env_fini(&e);
}

static void copies(void)
{
	struct sgxfive_surface s = { 0x3e000000, 64, 32, 256 }, d = s;
	struct env e;

	env_init(&e, SGXFIVE_MOCK_LAYOUT_REV1);
	copy_case(&e, 32, 16, 5, 3, 320, 8, 4, 288);
	copy_case(&e, 33, 16, 0, 0, 256, 0, 0, 256);
	copy_case(&e, 1, 1, 7, 9, 256, 3, 2, 256);
	CHECK_EQ(sgxfive_xfer_copy(e.x, &s, 0, 0, &d,
				   &(struct sgxfive_rect){ 0, 0, 4097, 1 }, 0), -EINVAL);
	s.stride = 258;
	CHECK_EQ(sgxfive_xfer_copy(e.x, &s, 0, 0, &d,
				   &(struct sgxfive_rect){ 0, 0, 8, 8 }, 0), -EINVAL);
	s.stride = 256;
	CHECK_EQ(sgxfive_xfer_copy(e.x, &s, 60, 0, &d,
				   &(struct sgxfive_rect){ 0, 0, 8, 8 }, 0), -EINVAL);
	env_fini(&e);
}

/* Two VMs, two contexts: a context reaches only its own VM's mappings. */
static void isolation(void)
{
	struct sgxfive_bo *ba = NULL, *bb = NULL, *bro = NULL;
	const struct sgxfive_rect r = { 0, 0, 64, 32 };
	struct sgxfive_xfer *xb = NULL;
	struct sgxfive_mock_stats st;
	struct sgxfive_vm *vmb = NULL;
	uint64_t addr = 0, only_b = 0, ro = 0x60000000;
	uint32_t ctxb = 0, *pa, *pb, *pro, *pob;
	struct sgxfive_surface dst;
	struct env e;

	env_init(&e, SGXFIVE_MOCK_LAYOUT_REV1);
	CHECK_EQ(sgxfive_vm_create(e.dev, &vmb), 0);
	CHECK_EQ(sgxfive_ctx_create(vmb, &ctxb), 0);
	CHECK_EQ(sgxfive_xfer_create(vmb, ctxb, e.src, &xb), 0);

	/* The same address in both VMs, backed by different BOs. */
	pa = surface(&e, e.vm, 2, 0, &addr, &ba);
	pb = surface(&e, vmb, 2, 0, &addr, &bb);
	dst = (struct sgxfive_surface){ addr, 64, 32, 256 };
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &r, 0xff0000aa, 0), 0);
	CHECK_EQ(sgxfive_xfer_wait(e.x, NSEC), 0);
	CHECK(check_fill(pa, 2048, 256, &r, 0xff0000aa));
	CHECK(check_fill(pb, 2048, 256, &(struct sgxfive_rect){ 0 }, 0));

	/* An address mapped only in B: A's fill faults and writes nothing. */
	only_b = 0x50000000;
	pob = surface(&e, vmb, 2, 0, &only_b, &bro);
	dst.addr = only_b;
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &r, 0xff0000bb, 0), 0);
	sgxfive_mock_get_stats(e.mock, &st);
	CHECK_EQ(st.faults, 1);
	CHECK_EQ(st.last_fault_addr >> 12, only_b >> 12);
	CHECK(check_fill(pob, 2048, 256, &(struct sgxfive_rect){ 0 }, 0));
	/* The faulting context is lost; B still works. */
	CHECK_EQ(sgxfive_xfer_fill(e.x, &dst, &r, 0xff0000bb, 0), -ECANCELED);
	CHECK_EQ(sgxfive_submit_transfer(e.dev, e.ctx, DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY,
					 NULL, 0, 0), -ECANCELED);
	CHECK_EQ(sgxfive_xfer_fill(xb, &dst, &r, 0xff0000cc, 0), 0);
	CHECK_EQ(sgxfive_xfer_wait(xb, NSEC), 0);
	CHECK(check_fill(pob, 2048, 256, &r, 0xff0000cc));
	sgxfive_bo_destroy(bro);

	/* A read-only destination faults too (the xfer_ro check) and stays untouched. */
	pro = surface(&e, vmb, 2, DRM_SGXFIVE_VM_MAP_READ_ONLY, &ro, &bro);
	dst.addr = ro;
	CHECK_EQ(sgxfive_xfer_fill(xb, &dst, &r, 0xff0000dd, 0), 0);
	sgxfive_mock_get_stats(e.mock, &st);
	CHECK_EQ(st.faults, 2);
	CHECK(check_fill(pro, 2048, 256, &(struct sgxfive_rect){ 0 }, 0));
	CHECK_EQ(sgxfive_xfer_fill(xb, &dst, &r, 0xff0000dd, 0), -ECANCELED);

	sgxfive_bo_destroy(bro);
	sgxfive_bo_destroy(ba);
	sgxfive_bo_destroy(bb);
	sgxfive_xfer_destroy(xb);
	sgxfive_ctx_destroy(e.dev, ctxb);
	sgxfive_vm_destroy(vmb);
	env_fini(&e);
}

int main(void)
{
	struct sgxfive_prog_source *(*const sources[])(void) = {
		sgxfive_prog_fake_create, sgxfive_prog_asm_create,
	};

	for (unsigned int i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		make_source = sources[i];
		printf("# program source %s\n", i ? "asm" : "fake");
		fill_recipe();
		fill_rects(SGXFIVE_MOCK_LAYOUT_HARNESS);
		fill_rects(SGXFIVE_MOCK_LAYOUT_REV1);
		fill_errors();
		copy_recipe();
		copies();
		isolation();
	}
	return tap_done();
}
