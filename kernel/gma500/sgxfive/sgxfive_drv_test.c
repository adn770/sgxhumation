// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the render-node gate (sgxfive_ioctl_allowed): with the
 * render node off, only DEV_QUERY may reach the device; everything that
 * allocates, maps or runs on the GPU is for trusted users only.
 */
#include <kunit/test.h>
#include <linux/module.h>

#include <drm/sgxfive_drm.h>

#include "sgxfive.h"

static void sgxfive_drv_test_gate_on(struct kunit *test)
{
	unsigned int nr;

	for (nr = DRM_SGXFIVE_DEV_QUERY; nr <= DRM_SGXFIVE_SUBMIT_JOBS; nr++)
		KUNIT_EXPECT_TRUE_MSG(test, sgxfive_ioctl_allowed(true, nr), "nr %u", nr);
}

static void sgxfive_drv_test_gate_off(struct kunit *test)
{
	unsigned int nr;

	KUNIT_EXPECT_TRUE(test, sgxfive_ioctl_allowed(false, DRM_SGXFIVE_DEV_QUERY));
	for (nr = DRM_SGXFIVE_CREATE_BO; nr <= DRM_SGXFIVE_SUBMIT_JOBS; nr++)
		KUNIT_EXPECT_FALSE_MSG(test, sgxfive_ioctl_allowed(false, nr), "nr %u", nr);
	/* Off, unknown numbers are refused too. */
	KUNIT_EXPECT_FALSE(test, sgxfive_ioctl_allowed(false, DRM_SGXFIVE_SUBMIT_JOBS + 1));
}

static struct kunit_case sgxfive_drv_test_cases[] = {
	KUNIT_CASE(sgxfive_drv_test_gate_on),
	KUNIT_CASE(sgxfive_drv_test_gate_off),
	{}
};

static struct kunit_suite sgxfive_drv_test_suite = {
	.name = "gma500_sgxfive_drv",
	.test_cases = sgxfive_drv_test_cases,
};

kunit_test_suite(sgxfive_drv_test_suite);
