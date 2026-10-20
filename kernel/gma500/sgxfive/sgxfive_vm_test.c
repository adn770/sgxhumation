// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for sgxfive_vm.c's range rules: a VM_MAP must be page aligned,
 * non-empty, and lie inside one heap that is present and not reserved.
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgxfive_priv.h"

static const struct drm_sgxfive_heap heaps[3] = {
	{ 0x3e000000, 0x10000000, 0, 12 },			/* user */
	{ 0x30000000, 0x00080000, DRM_SGXFIVE_HEAP_RESERVED, 12 },
	{ 0xf0000000, 0, 0, 12 },				/* absent */
};

static void sgxfive_vm_test_range(struct kunit *test)
{
	/* Inside a user heap, at its start and up to its end. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000, 0x1000), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x4dfff000, 0x1000), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000,
						     0x10000000), 0);
	/* Misaligned, empty, crossing the heap end, outside every heap. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000800, 0x1000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000, 0x800),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x4dfff000, 0x2000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x10000000, 0x1000),
			-EINVAL);
	/* The reserved firmware heap and an absent heap are not mappable. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x30000000, 0x1000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0xf0000000, 0x1000),
			-EINVAL);
	/* No wrap-around past 4 GiB. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0xfffff000,
						     0x100002000ull), -EINVAL);
}

static struct kunit_case sgxfive_vm_test_cases[] = {
	KUNIT_CASE(sgxfive_vm_test_range),
	{}
};

static struct kunit_suite sgxfive_vm_test_suite = {
	.name = "gma500_sgxfive_vm",
	.test_cases = sgxfive_vm_test_cases,
};

kunit_test_suite(sgxfive_vm_test_suite);
