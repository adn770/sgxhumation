// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for sgxfive_job.c's pure parts: turning the UAPI transfer
 * register block into the firmware's SGXMKIF_TRANSFERCMD, and its checks.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_xfer.h"
#include "sgxfive_priv.h"

static u32 dw(const u8 *p, unsigned int off)
{
	return p[off] | p[off + 1] << 8 | p[off + 2] << 16 | (u32)p[off + 3] << 24;
}

/* A fill's register block in the REV1 layout (design/sgx545_transfer.md §4.2). */
static void fill_regs(struct drm_sgxfive_transfer_cmd *c)
{
	memset(c, 0, sizeof(*c));
	c->isp_bgobjtag = 0x3000;
	c->isp_bgobj = 0x200;
	c->isp_bgobjdepth = 0x3f800000;
	c->isp_render = 3;
	c->isp_ipfmisc = 0x100;
	c->bif_3d_req_base = 0x3e100000;
	c->aa_mode_3d = 0x80000000;
	c->isp_multisamplectl = 0x88;
	c->event_pixel_pds_exec = 0x2e000040;
	c->event_pixel_pds_data = 5;
	c->event_pixel_pds_info = 1;
	c->isp_render_box2 = 0x00010001;
	c->pixelbe = 0x80;
	c->pds_exec_base = 0x2e000000;
}

static void sgxfive_job_test_build(struct kunit *test)
{
	struct drm_sgxfive_transfer_cmd c;
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int off, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	fill_regs(&c);
	c.load_flags = DRM_SGXFIVE_TRANSFER_LOAD_FIR;
	c.csc0_coeff[0] = 0x11;			/* lands at 0x44 */
	c.fir_hfilter[8] = 0x22;		/* lands at 0xa0 */
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgxfive_job_build(cmd, &c, false, 0x32400200, 7),
			0x1c0u);

	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0x3000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x018), 0x3e100000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x024), 0x2e000040u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x03c), 0x2e000000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x040), 0u);	/* double pixel partitions */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x044), 0x11u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0a0), 0x22u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_SIZE), 0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_STATUS), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 7u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x188), 0u);	/* load_yuv_coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x18c), 0u);	/* load_luma_key_coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x190), 1u);	/* load_fir_coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x194), 0x07c00000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x198), 7u);	/* fence id */
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_UPDATES), 0u);
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4)
		nonzero += dw(cmd, off) != 0;
	/* 14 register words + 2 coefficients + size, status x3, FIR, parts, id */
	KUNIT_EXPECT_EQ(test, nonzero, 23u);
}

static void sgxfive_job_test_dummy(struct kunit *test)
{
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_EXPECT_EQ(test, sgxfive_job_build(cmd, NULL, true, 0x32400200, 3),
			0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), (u32)SGX_XFER_FLAG_DUMMY);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 3u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x194), 0u);
}

/* Addresses the pixel pipe fetches or executes must be in a user heap. */
static void sgxfive_job_test_check(struct kunit *test)
{
	struct drm_sgxfive_transfer_cmd c;

	fill_regs(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), 0);

	c.pds_exec_base = 0x30000000;		/* the reserved firmware code */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.event_pixel_pds_exec = 0x32000000;	/* the reserved firmware data */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.bif_3d_req_base = 0x3e180000;		/* not 1 MiB aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.isp_rgn_base = 0x10000000;		/* outside every heap */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.double_pixel_partitions = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.load_flags = 1u << 3;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
}

static struct kunit_case sgxfive_job_test_cases[] = {
	KUNIT_CASE(sgxfive_job_test_build),
	KUNIT_CASE(sgxfive_job_test_dummy),
	KUNIT_CASE(sgxfive_job_test_check),
	{}
};

static struct kunit_suite sgxfive_job_test_suite = {
	.name = "gma500_sgxfive_job",
	.test_cases = sgxfive_job_test_cases,
};

kunit_test_suite(sgxfive_job_test_suite);
