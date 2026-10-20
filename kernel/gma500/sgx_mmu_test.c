// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for sgx_mmu.c: the SGX MMU's two-level page tables (a 4 KiB
 * page directory of 1024 PDEs, each naming a page table of 1024 PTEs).
 */
#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "sgx_mmu.h"

static void sgx_mmu_test_encoding(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde(0x01234000), 0x01234001u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pte(0x05678000, 0), 0x05678001u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pte(0x05678000, SGX_MMU_RO), 0x05678005u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pte(0x05678000, SGX_MMU_CACHED), 0x05678009u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pd_index(0x32104000), 0xc8u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pt_index(0x32104000), 0x104u);
}

static void sgx_mmu_test_map_unmap(struct kunit *test)
{
	struct sgx_mmu_pd *pd = sgx_mmu_pd_create();
	struct page *pages[3];
	unsigned int i;
	u32 pde;

	KUNIT_ASSERT_NOT_NULL(test, pd);
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}
	KUNIT_EXPECT_EQ(test, sgx_mmu_lookup(pd, 0x323ff000), 0u);

	/* Three pages across a page-table boundary (0x323ff000..0x32401fff). */
	KUNIT_ASSERT_EQ(test, sgx_mmu_map(pd, 0x323ff000, pages, 3, SGX_MMU_RO), 0);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, sgx_mmu_lookup(pd, 0x323ff000 + i * PAGE_SIZE),
				sgx_mmu_pte(page_to_phys(pages[i]), SGX_MMU_RO));
	pde = sgx_mmu_pd_entry(pd, sgx_mmu_pd_index(0x32400000));
	KUNIT_EXPECT_EQ(test, pde & 0xfff, 1u);
	KUNIT_EXPECT_NE(test, sgx_mmu_pd_entry(pd, sgx_mmu_pd_index(0x323ff000)),
			pde);

	/* Mapping over a live entry is refused; unmapping clears it. */
	KUNIT_EXPECT_EQ(test, sgx_mmu_map(pd, 0x32400000, pages, 1, 0), -EBUSY);
	sgx_mmu_unmap(pd, 0x323ff000, 3);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, sgx_mmu_lookup(pd, 0x323ff000 + i * PAGE_SIZE),
				0u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_map(pd, 0x32400000, pages, 1, 0), 0);

	/* Misaligned addresses are refused. */
	KUNIT_EXPECT_EQ(test, sgx_mmu_map(pd, 0x32400800, pages, 1, 0), -EINVAL);

	sgx_mmu_pd_destroy(pd);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

static struct kunit_case sgx_mmu_test_cases[] = {
	KUNIT_CASE(sgx_mmu_test_encoding),
	KUNIT_CASE(sgx_mmu_test_map_unmap),
	{}
};

static struct kunit_suite sgx_mmu_test_suite = {
	.name = "gma500_sgx_mmu",
	.test_cases = sgx_mmu_test_cases,
};

kunit_test_suite(sgx_mmu_test_suite);
