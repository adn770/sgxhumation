// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for sgx_seq.c: the SGX545 boot register sequence must be exactly
 * the DDK's (design/sgx545_boot_sequence.md, rows C1-C4, R1-R22, C8, C12).
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgx_seq.h"

#define MAX_OPS		128
#define OP_DELAY	0xd0000000	/* delay marker: reg = microseconds */
#define OP_PREKICK	0xd1000000	/* pre_kick() marker */
#define OP_READ		0xd2000000	/* read marker: reg = register */

struct rec {
	u32 op[MAX_OPS][2];
	unsigned int n;
	u32 int_stat;		/* what BIF_INT_STAT reads as */
};

static void rec_add(struct rec *r, u32 a, u32 b)
{
	if (r->n < MAX_OPS) {
		r->op[r->n][0] = a;
		r->op[r->n][1] = b;
	}
	r->n++;
}

static u32 rec_rd(void *ctx, u32 reg)
{
	struct rec *r = ctx;

	rec_add(r, OP_READ, reg);
	return reg == SGX_BIF_INT_STAT ? r->int_stat : 0xffffffff;
}

static void rec_wr(void *ctx, u32 reg, u32 val)
{
	rec_add(ctx, reg, val);
}

static void rec_udelay(void *ctx, unsigned int us)
{
	rec_add(ctx, OP_DELAY, us);
}

static void rec_pre_kick(void *ctx)
{
	rec_add(ctx, OP_PREKICK, 0);
}

static const struct sgx_fw_write part1[] = { { 0xa58, 0 }, { 0xacc, 0 } };
static const struct sgx_fw_write part2[] = { { 0x818, 0x7ff }, { 0xa58, 0x2204b00f } };

static void seq(struct sgx_seq *s, struct rec *r)
{
	*s = (struct sgx_seq) {
		.rd = rec_rd, .wr = rec_wr, .udelay = rec_udelay,
		.pre_kick = rec_pre_kick, .ctx = r,
		.clkgatectl = 0xa6aa, .clkgatectl2 = 0xaaaaaa,
		.part1 = part1, .npart1 = ARRAY_SIZE(part1),
		.part2 = part2, .npart2 = ARRAY_SIZE(part2),
		.reset_pd = 0x1000, .edm_pd = 0x3ebf000,
	};
}

static void sgx_seq_test_boot(struct kunit *test)
{
	static const u32 want[][2] = {
		{ 0x000, 0xa6aa }, { 0x004, 0xaaaaaa },		/* C1-C2 */
		{ 0xa58, 0 }, { 0xacc, 0 },			/* C3-C4 */
		{ 0x080, 0x3f }, { OP_DELAY, 1 },		/* R1-R2 */
		{ 0xc00, 0 }, { 0xc74, 0 }, { 0xc78, 0 },	/* R3-R5 */
		{ 0xc84, 0 },					/* R6 */
		{ 0xc38, 0 }, { 0xc3c, 0 }, { 0xc40, 0 }, { 0xc44, 0 },
		{ 0xc48, 0 }, { 0xc4c, 0 }, { 0xc50, 0 }, { 0xc54, 0 },
		{ 0xc58, 0 }, { 0xc5c, 0 }, { 0xc60, 0 }, { 0xc64, 0 },
		{ 0xc68, 0 }, { 0xc6c, 0 }, { 0xc70, 0 },	/* R7 */
		{ 0xc84, 0x1000 }, { OP_DELAY, 1 },		/* R8-R9 */
		{ 0x080, 0x3e }, { OP_DELAY, 1 },		/* R10-R11 */
		{ 0xc00, 0x8 }, { 0xc00, 0 }, { OP_DELAY, 1 },	/* R12-R14 */
		{ OP_READ, 0xc04 },				/* R15 */
		{ 0xc78, 0xf }, { 0xc70, 0x3ebf000 },		/* R16-R17 */
		{ 0xc00, 0x8 }, { 0xc00, 0 }, { OP_DELAY, 1 },	/* R18-R20 */
		{ 0x080, 0 }, { OP_DELAY, 1 },			/* R21-R22 */
		{ 0x818, 0x7ff }, { 0xa58, 0x2204b00f },	/* C8 */
		{ OP_PREKICK, 0 },				/* C7, C9-C11 */
		{ 0xae8, 1 },					/* C12 */
	};
	struct rec *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	struct sgx_seq s;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, r);
	seq(&s, r);
	KUNIT_ASSERT_EQ(test, sgx_seq_boot(&s), 0);
	KUNIT_ASSERT_EQ(test, r->n, (unsigned int)ARRAY_SIZE(want));
	for (i = 0; i < ARRAY_SIZE(want); i++) {
		KUNIT_EXPECT_EQ_MSG(test, r->op[i][0], want[i][0], "op %u", i);
		KUNIT_EXPECT_EQ_MSG(test, r->op[i][1], want[i][1], "op %u", i);
	}
}

/* A BIF fault after the reset PD goes in: stop with the BIF still in reset. */
static void sgx_seq_test_bif_fault(struct kunit *test)
{
	struct rec *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	struct sgx_seq s;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, r);
	r->int_stat = 0x1;
	seq(&s, r);
	KUNIT_EXPECT_EQ(test, sgx_seq_boot(&s), -EIO);
	KUNIT_EXPECT_EQ(test, r->op[r->n - 1][0], (u32)OP_READ);
	for (i = 0; i < r->n; i++) {
		KUNIT_EXPECT_NE(test, r->op[i][0], (u32)SGX_EVENT_KICK2);
		KUNIT_EXPECT_NE(test, r->op[i][0], (u32)OP_PREKICK);
	}
}

static struct kunit_case sgx_seq_test_cases[] = {
	KUNIT_CASE(sgx_seq_test_boot),
	KUNIT_CASE(sgx_seq_test_bif_fault),
	{}
};

static struct kunit_suite sgx_seq_test_suite = {
	.name = "gma500_sgx_seq",
	.test_cases = sgx_seq_test_cases,
};

kunit_test_suite(sgx_seq_test_suite);
