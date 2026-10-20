// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for sgxfive_vm.c's range rules: a VM_MAP must be page aligned,
 * non-empty, and lie inside one heap that is present and not reserved. And
 * for the deferred release of what the GPU may still reach: freed only once
 * the kernel-CCB command carrying the BIF invalidation has been processed.
 */
#include <kunit/test.h>
#include <linux/list.h>
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

/* Commands done: those sent minus those still between read and write offset. */
static void sgxfive_vm_test_ccb_done(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(10, 10, 10), 10);
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(10, 12, 10), 8);
	/* The offsets count 256 entries and wrap. */
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(300, 1, 255), 298);
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(0, 0, 0), 0);
}

struct fake_item {
	struct sgxfive_deferred d;
	int *freed;
};

static void fake_free(struct sgxfive_deferred *d)
{
	(*container_of(d, struct fake_item, d)->freed)++;
}

/* In order, every item whose command is done, and only those. */
static void sgxfive_vm_test_reap(struct kunit *test)
{
	struct fake_item it[3];
	LIST_HEAD(list);
	LIST_HEAD(due);
	int i, freed = 0;

	for (i = 0; i < 3; i++) {
		it[i] = (struct fake_item){ .d = { .after = 5 + i, .free = fake_free },
					    .freed = &freed };
		list_add_tail(&it[i].d.link, &list);
	}
	KUNIT_EXPECT_EQ(test, sgxfive_reap(&list, 4, &due), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&due));
	KUNIT_EXPECT_EQ(test, sgxfive_reap(&list, 6, &due), 2);
	KUNIT_EXPECT_EQ(test, freed, 0);		/* moved, not freed yet */
	sgxfive_free_all(&due);
	KUNIT_EXPECT_EQ(test, freed, 2);
	KUNIT_EXPECT_TRUE(test, list_empty(&due));
	KUNIT_EXPECT_FALSE(test, list_empty(&list));
	KUNIT_EXPECT_EQ(test, sgxfive_reap(&list, U64_MAX, &due), 1);	/* halted: all */
	sgxfive_free_all(&due);
	KUNIT_EXPECT_EQ(test, freed, 3);
	KUNIT_EXPECT_TRUE(test, list_empty(&list));
}

static struct kunit_case sgxfive_vm_test_cases[] = {
	KUNIT_CASE(sgxfive_vm_test_range),
	KUNIT_CASE(sgxfive_vm_test_ccb_done),
	KUNIT_CASE(sgxfive_vm_test_reap),
	{}
};

static struct kunit_suite sgxfive_vm_test_suite = {
	.name = "gma500_sgxfive_vm",
	.test_cases = sgxfive_vm_test_cases,
};

kunit_test_suite(sgxfive_vm_test_suite);
