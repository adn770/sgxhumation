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
	size = sgx_xfer_dummy(cmd, 0xf0122004, 0x1234, 0xf0122000, 0xcafe, NULL);

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

/* asDstSyncs[0]: read pending, read-complete address, write pending, address. */
static void sgx_xfer_test_dst_sync(struct kunit *test)
{
	static const struct sgx_xfer_sync dst = {
		.read_pending = 3, .read_complete = 0xf0122020,
		.write_pending = 6, .write_complete = 0xf0122024,
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	sgx_xfer_dummy(cmd, 0xf0122004, 0x1234, 0xf0122000, 0xcafe, &dst);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x00), 0u);	/* no src syncs */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x84), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x88), 3u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x8c), 0xf0122020u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x90), 6u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0x94), 0xf0122024u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0ac + 0xb8), 1u);	/* status kept */
}

static const struct sgx_xfer_rect rect64x32 = { 0, 0, 64, 32 };

/* design/sgx545_transfer.md §4.2.3: (0,0)-(64,32), primary at P, secondary P+0x20. */
static void sgx_xfer_test_bgobj(struct kunit *test)
{
	static const u32 want[SGX_XFER_BGOBJ_DWORDS] = {
		0x04000002, 0x00030000, 0x04000000, 0, 0, 0, 0, 0,
		0, 0x00100000, 0x00001000, 0, 0x10400010, 0, 0x00102000, 0x00001000,
		0,
	};
	u32 bg[SGX_XFER_BGOBJ_DWORDS];
	unsigned int i;

	memset(bg, 0xa5, sizeof(bg));
	sgx_xfer_bgobj(bg, 0xe4000000, 0xe4000000, 0x10, 0xe4000020, 0x10,
		       &rect64x32);
	for (i = 0; i < SGX_XFER_BGOBJ_DWORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, bg[i], want[i], "dword %u", i);
}

/* §4.2.5: 64 x 32 at A with a 256-byte (64-pixel) stride, ARGB8888. */
static void sgx_xfer_test_pbe(struct kunit *test)
{
	u32 pbe[SGX_XFER_PBE_WORDS];

	sgx_xfer_pbe(pbe, &rect64x32, 64, 0xf0200000, 0);
	KUNIT_EXPECT_EQ(test, pbe[0], 0x00000000u);
	KUNIT_EXPECT_EQ(test, pbe[1], 0x001f003fu);
	KUNIT_EXPECT_EQ(test, pbe[2], 0x001f8000u);
	KUNIT_EXPECT_EQ(test, pbe[3], 0xf0200000u);
	KUNIT_EXPECT_EQ(test, pbe[4], 0x00000200u);
}

/* §4.2.6 and §4.2.7 step 9. */
static void sgx_xfer_test_fill_cmd(struct kunit *test)
{
	static const struct sgx_xfer_fill f = {
		.rect = { 0, 0, 64, 32 }, .bg = 0xf0130000, .req_base = 0xf0100000,
		.pixev_exec = 0xe4000040, .pds_exec_base = 0xe4000000,
		.status_addr = 0xf0122004, .fence_id = 7,
	};
	static const u32 want[][2] = {
		{ 0x000, 0x3000 }, { 0x004, 0x200 }, { 0x008, 0x3f800000 },
		{ 0x00c, 3 }, { 0x014, 0x100 }, { 0x018, 0xf0100000 },
		{ 0x01c, 0x80000000 }, { 0x020, 0x88 }, { 0x024, 0xe4000040 },
		{ 0x028, 5 }, { 0x02c, 1 }, { 0x034, 0x00010001 }, { 0x038, 0x80 },
		{ 0x03c, 0xe4000000 }, { 0x0a4, 0x1c0 },
		{ 0x0ac + 0xb8, 1 }, { 0x0ac + 0xbc, 0xf0122004 }, { 0x0ac + 0xc0, 8 },
		{ 0x194, 0x07c00000 }, { 0x198, 7 },
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int i, off, n = 0;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgx_xfer_fill_cmd(cmd, &f), 0x1c0u);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(cmd, want[i][0]), want[i][1],
				    "offset %#x", want[i][0]);
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4)
		n += dw(cmd, off) != 0;
	KUNIT_EXPECT_EQ(test, n, (unsigned int)ARRAY_SIZE(want));	/* rest 0 */
}

/* §2.4: the render box in 32 x 16 tiles, inclusive. */
static void sgx_xfer_test_render_box(struct kunit *test)
{
	static const struct sgx_xfer_fill f = {
		.rect = { 40, 20, 100, 33 },
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	sgx_xfer_fill_cmd(cmd, &f);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x030), 1u | 1u << 16);	/* y 20/16, x 40/32 */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x034), 2u | 3u << 16);	/* y 47/16-1, x 131/32-1 */
}

/* §4.4.2: a whole 64 x 32 copy, primary 0x30 data bytes at P, secondary P+0x20. */
static void sgx_xfer_test_bgobj_copy(struct kunit *test)
{
	static const u32 want[SGX_XFER_BGOBJ1_DWORDS] = {
		0x04000002, 0x00030001, 0x0c000000,
		0x00000000, 0x00000000, 0x3f800000, 0x00000000, 0x00000000, 0x3f800000,
		0x00000000, 0x00000202, 0, 0, 0, 0,
		0x00100000, 0x00001000, 0x00000000, 0x10400010, 0x00000000,
		0x00102000, 0x00001000, 0x00000000,
	};
	static const struct sgx_xfer_rect src = { 0, 0, 64, 32 };
	u32 bg[SGX_XFER_BGOBJ1_DWORDS];
	unsigned int i;

	memset(bg, 0xa5, sizeof(bg));
	sgx_xfer_bgobj_copy(bg, 0xe4000000, 0xe4000000, 0x30, 0xe4000020, 0x10,
			    &rect64x32, &src, 64, 32);
	for (i = 0; i < SGX_XFER_BGOBJ1_DWORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, bg[i], want[i], "dword %u", i);
}

/*
 * §4.4.2: the source rectangle (8,4)-(40,20) of a 48 x 32 surface fixes the
 * order (U0 V0, U1 V0, U0 V1) and the truncation to 20 fraction bits.
 */
static void sgx_xfer_test_bgobj_copy_coords(struct kunit *test)
{
	static const u32 want[6] = {
		0x3e2aaa80, 0x3e000000, 0x3f555550, 0x3e000000, 0x3e2aaa80, 0x3f200000,
	};
	static const struct sgx_xfer_rect src = { 8, 4, 40, 20 };
	u32 bg[SGX_XFER_BGOBJ1_DWORDS];
	unsigned int i;

	sgx_xfer_bgobj_copy(bg, 0xe4000000, 0xe4000000, 0x30, 0xe4000020, 0x10,
			    &rect64x32, &src, 48, 32);
	for (i = 0; i < 6; i++)
		KUNIT_EXPECT_EQ_MSG(test, bg[3 + i], want[i], "coord %u", i);
}

/* §4.4.4: the copy's command differs from the fill only in TSPDATASIZE. */
static void sgx_xfer_test_copy_cmd(struct kunit *test)
{
	struct sgx_xfer_fill f = {
		.rect = { 0, 0, 64, 32 }, .bg = 0xf0130000, .req_base = 0xf0100000,
		.pixev_exec = 0xe4000040, .pds_exec_base = 0xe4000000,
		.status_addr = 0xf0122004, .fence_id = 7, .layers = 1,
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	sgx_xfer_fill_cmd(cmd, &f);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0x02003000u);
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
	KUNIT_CASE(sgx_xfer_test_dst_sync),
	KUNIT_CASE(sgx_xfer_test_ctx),
	KUNIT_CASE(sgx_xfer_test_bgobj),
	KUNIT_CASE(sgx_xfer_test_pbe),
	KUNIT_CASE(sgx_xfer_test_fill_cmd),
	KUNIT_CASE(sgx_xfer_test_render_box),
	KUNIT_CASE(sgx_xfer_test_bgobj_copy),
	KUNIT_CASE(sgx_xfer_test_bgobj_copy_coords),
	KUNIT_CASE(sgx_xfer_test_copy_cmd),
	{}
};

static struct kunit_suite sgx_xfer_test_suite = {
	.name = "gma500_sgx_xfer",
	.test_cases = sgx_xfer_test_cases,
};

kunit_test_suite(sgx_xfer_test_suite);
