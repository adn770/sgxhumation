// SPDX-License-Identifier: MIT
/*
 * The assembled program source, sgxfive_prog_asm_create(): our own programs
 * (tools/usse-asm/progs/) with the TQ_META records and TQ_PATCHES of
 * design/sgx545_fw_format.md §9-§11. Here: its metadata equals the fake's, every
 * word the fake knows is the same, and the bodies the fake leaves zero are
 * there. tests/test_prog_asm.py compares every byte with fwload.build_tq().
 */
#include <errno.h>
#include <string.h>

#include "sgxfive_xfer.h"
#include "tap.h"

#define USE_BASE	0x37000000u
#define PDS_BASE	0x36000000u

static uint32_t rng = 545;

static uint32_t next(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng ^ rng >> 15;
}

/* Every program placed in its page, as the engine does, with random parameters. */
static void random_env(struct sgxfive_prog_source *src, struct sgxfive_prog_env *env)
{
	uint32_t cursor[2] = { 0, 0 };

	memset(env, 0, sizeof(*env));
	env->heap_base[DRM_SGXFIVE_HEAP_USSE_PIXEL] = USE_BASE;
	env->heap_base[DRM_SGXFIVE_HEAP_PDS_PIXEL] = PDS_BASE;
	for (uint32_t id = 0; id < SGXFIVE_PROG_COUNT; id++) {
		struct sgxfive_prog_meta m;
		int use;

		src->ops->meta(src, id, &m);
		use = m.heap == DRM_SGXFIVE_HEAP_USSE_PIXEL;
		env->addr[id] = (use ? USE_BASE : PDS_BASE) + 0x1000 * (next() % 64) + cursor[use];
		cursor[use] += (m.size + 15) & ~15u;
		if (use)
			sgxfive_doutu(env->addr[id], USE_BASE, m.temps, &env->doutu[id]);
	}
	for (int i = 0; i < SGXFIVE_PARAM_COUNT; i++)
		env->params[i] = next();
	env->params[SGXFIVE_PARAM_SRC] &= ~3u;
}

static void metadata(struct sgxfive_prog_source *as, struct sgxfive_prog_source *fake)
{
	struct sgxfive_prog_meta a, f;
	int same = 1;

	CHECK(!strcmp(as->name, "asm"));
	for (uint32_t id = 0; id < SGXFIVE_PROG_COUNT; id++) {
		CHECK_EQ(as->ops->meta(as, id, &a), 0);
		CHECK_EQ(fake->ops->meta(fake, id, &f), 0);
		same &= !memcmp(&a, &f, sizeof(a));
	}
	CHECK(same);
	CHECK_EQ(as->ops->meta(as, SGXFIVE_PROG_COUNT, &a), -ENOENT);
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_COUNT, &(struct sgxfive_prog_env){ 0 }, &a),
		 -ENOENT);
}

/* Every dword the fake writes non-zero, the assembled program has too. */
static void agrees_with_fake(struct sgxfive_prog_source *as, struct sgxfive_prog_source *fake)
{
	uint32_t a[0x100 / 4], f[0x100 / 4];
	struct sgxfive_prog_env env;
	int ok = 1;

	for (int round = 0; round < 200; round++) {
		random_env(as, &env);
		for (uint32_t id = 0; id < SGXFIVE_PROG_COUNT; id++) {
			struct sgxfive_prog_meta m;

			as->ops->meta(as, id, &m);
			memset(a, 0x5a, sizeof(a));
			memset(f, 0, sizeof(f));
			ok &= as->ops->build(as, id, &env, a) == 0;
			ok &= fake->ops->build(fake, id, &env, f) == 0;
			for (uint32_t i = 0; i < m.size / 4; i++) {
				if (f[i] && f[i] != a[i]) {
					printf("#   prog %u dword %u: asm %08x, fake %08x\n", id, i,
					       a[i], f[i]);
					ok = 0;
				}
			}
			ok &= a[m.size / 4] == 0x5a5a5a5a;	/* writes size bytes only */
		}
	}
	CHECK(ok);
}

/* The bodies the fake does not know. */
static void closed_bodies_are_ours(struct sgxfive_prog_source *as)
{
	const uint32_t pixev_const[] = { 8, 2, 0x100, 0xff8, 4 };
	struct sgxfive_prog_env env;
	uint32_t w[0xc4 / 4];
	int nonzero = 1;

	random_env(as, &env);
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_USE_SEC, &env, w), 0);
	CHECK_EQ(w[2], 0x00000000);			/* nop.end */
	CHECK_EQ(w[3], 0xf8040140);
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_PDS_PIXEVENT, &env, w), 0);
	CHECK(!memcmp(&w[6], pixev_const, sizeof(pixev_const)));
	CHECK_EQ(w[13], 0x003ff000);
	CHECK_EQ(w[16], 0x00400000);
	for (int i = 20; i < 0xc4 / 4; i++)		/* 29 instructions */
		nonzero &= w[i] != 0;
	CHECK(nonzero);
	CHECK_EQ(w[0xc4 / 4 - 1], 0xaf000000);		/* halt */
}

static void errors(struct sgxfive_prog_source *as)
{
	struct sgxfive_prog_env env;
	uint32_t w[0xc4 / 4];

	random_env(as, &env);
	env.addr[SGXFIVE_PROG_USE_FILL] = 0;		/* the DOUTU target not placed */
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_PDS_PRIM, &env, w), -ENOENT);
	env.addr[SGXFIVE_PROG_USE_FILL] = USE_BASE + 0x80000;	/* outside the window */
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_PDS_PRIM, &env, w), -EINVAL);
	env.addr[SGXFIVE_PROG_USE_FILL] = USE_BASE + 4;		/* not an instruction */
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_PDS_PRIM, &env, w), -EINVAL);
	CHECK_EQ(as->ops->build(as, SGXFIVE_PROG_USE_FILL, &env, w), 0);
}

int main(void)
{
	struct sgxfive_prog_source *as = sgxfive_prog_asm_create();
	struct sgxfive_prog_source *fake = sgxfive_prog_fake_create();

	CHECK(as != NULL);
	if (!as)
		return tap_done();
	metadata(as, fake);
	agrees_with_fake(as, fake);
	closed_bodies_are_ours(as);
	errors(as);
	sgxfive_prog_source_destroy(as);
	sgxfive_prog_source_destroy(fake);
	return tap_done();
}
