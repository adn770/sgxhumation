// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for sgx_xfer.c: SGXMKIF_TRANSFERCMD and SGXMKIF_HWTRANSFERCONTEXT
 * as built for a dummy transfer (design/sgx545_transfer.md §2, §4.1).
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgx_xfer.h"

static u32 dw(const u8 *p, unsigned int off)
{
	return p[off] | p[off + 1] << 8 | p[off + 2] << 16 | (u32)p[off + 3] << 24;
}

static void sgx_xfer_test_dummy_cmd(struct kunit *test)
{
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int off;
	u32 size;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	size = sgx_xfer_dummy(cmd, 0xf0122004, 0x1234, 0xf0122000, 0xcafe);

	KUNIT_EXPECT_EQ(test, size, 0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0a4), 0x1c0u);		/* ui32Size */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0a8), 0x20u);		/* DUMMYTRANSFER */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x00), 0u);	/* no src syncs */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x84), 0u);	/* no dst syncs */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x9c), 0u);	/* no TA sync */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0xac), 0u);	/* no 3D sync */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0xb8), 1u);	/* one status */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0xbc), 0xf0122004u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0xc0), 0x1234u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x19c), 1u);		/* one update */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x1a0), 0xf0122000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x1a4), 0xcafeu);
	/* Everything else is zero, including the register block. */
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4) {
		if (off == 0x0a4 || off == 0x0a8 || off == 0x0ac + 0xb8 ||
		    off == 0x0ac + 0xbc || off == 0x0ac + 0xc0 || off == 0x19c ||
		    off == 0x1a0 || off == 0x1a4)
			continue;
		KUNIT_EXPECT_EQ_MSG(test, dw(cmd, off), 0u, "offset %#x", off);
	}
}

static void sgx_xfer_test_ctx(struct kunit *test)
{
	u8 *ctx = kunit_kzalloc(test, SGX_XFER_CTX_SIZE, GFP_KERNEL);
	unsigned int off;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	memset(ctx, 0xa5, SGX_XFER_CTX_SIZE);
	sgx_xfer_ctx_init(ctx, 0x39ea000, 0xf0100000, 0xf0120000);

	KUNIT_EXPECT_EQ(test, dw(ctx, 0x00), 0u);		/* flags */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x04), 0u);		/* priority 0 */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x08), 0x39ea000u);	/* PD */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x0c), 0xf0100000u);	/* CCB base */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x10), 0xf0120000u);	/* CCB control */
	for (off = 0x14; off < SGX_XFER_CTX_SIZE; off += 4)
		KUNIT_EXPECT_EQ_MSG(test, dw(ctx, off), 0u, "offset %#x", off);
}

static struct kunit_case sgx_xfer_test_cases[] = {
	KUNIT_CASE(sgx_xfer_test_dummy_cmd),
	KUNIT_CASE(sgx_xfer_test_ctx),
	{}
};

static struct kunit_suite sgx_xfer_test_suite = {
	.name = "gma500_sgx_xfer",
	.test_cases = sgx_xfer_test_cases,
};

kunit_test_suite(sgx_xfer_test_suite);
