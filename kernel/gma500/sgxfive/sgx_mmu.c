// SPDX-License-Identifier: GPL-2.0-only
/*
 * SGX MMU page tables for the SGX545 driver. The directory and table pages
 * are mapped uncached, so every entry the CPU writes is in memory when the
 * BIF next walks the tables: no cache flushes before a kick. The BIF's own
 * PD and TLB caches are invalidated through the next CCB command's cache
 * control (or the reset sequence at boot).
 */
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/set_memory.h>

#include "sgx_mmu.h"

#define SGX_MMU_ENTRIES	1024

struct sgx_mmu_pd {
	struct page *dir;
	u32 *pde;			/* the directory, uncached */
	struct page *table[SGX_MMU_ENTRIES];
};

static struct page *sgx_mmu_alloc_uc(void)
{
	struct page *p = alloc_page(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO);

	if (p && set_pages_uc(p, 1)) {
		__free_page(p);
		return NULL;
	}
	return p;
}

static void sgx_mmu_free_uc(struct page *p)
{
	set_pages_wb(p, 1);
	__free_page(p);
}

struct sgx_mmu_pd *sgx_mmu_pd_create(void)
{
	struct sgx_mmu_pd *pd = kzalloc_obj(*pd);

	if (!pd)
		return NULL;
	pd->dir = sgx_mmu_alloc_uc();
	if (!pd->dir) {
		kfree(pd);
		return NULL;
	}
	pd->pde = page_address(pd->dir);
	return pd;
}

void sgx_mmu_pd_destroy(struct sgx_mmu_pd *pd)
{
	unsigned int i;

	if (!pd)
		return;
	for (i = 0; i < SGX_MMU_ENTRIES; i++)
		if (pd->table[i])
			sgx_mmu_free_uc(pd->table[i]);
	sgx_mmu_free_uc(pd->dir);
	kfree(pd);
}

u32 sgx_mmu_pd_phys(const struct sgx_mmu_pd *pd)
{
	return page_to_phys(pd->dir);
}

u32 sgx_mmu_pd_entry(const struct sgx_mmu_pd *pd, unsigned int i)
{
	return READ_ONCE(pd->pde[i]);
}

static u32 *sgx_mmu_ptes(const struct sgx_mmu_pd *pd, u32 addr)
{
	struct page *t = pd->table[sgx_mmu_pd_index(addr)];

	return t ? page_address(t) : NULL;
}

u32 sgx_mmu_lookup(const struct sgx_mmu_pd *pd, u32 addr)
{
	u32 *pte = sgx_mmu_ptes(pd, addr);

	return pte ? READ_ONCE(pte[sgx_mmu_pt_index(addr)]) : 0;
}

/*
 * Map n pages at addr. Page tables are created on demand and kept until the
 * directory goes; a live entry is never replaced (-EBUSY), so a caller cannot
 * silently redirect a mapping the GPU may be using.
 */
int sgx_mmu_map(struct sgx_mmu_pd *pd, u32 addr, struct page **pages,
		unsigned int n, u32 flags)
{
	unsigned int i;
	u32 a;

	if (addr % PAGE_SIZE || (u64)addr + (u64)n * PAGE_SIZE > 1ull << 32)
		return -EINVAL;
	for (i = 0, a = addr; i < n; i++, a += PAGE_SIZE)
		if (sgx_mmu_lookup(pd, a))
			return -EBUSY;
	for (i = 0, a = addr; i < n; i++, a += PAGE_SIZE) {
		unsigned int d = sgx_mmu_pd_index(a);
		u32 *pte;

		if (!pd->table[d]) {
			pd->table[d] = sgx_mmu_alloc_uc();
			if (!pd->table[d]) {
				sgx_mmu_unmap(pd, addr, i);
				return -ENOMEM;
			}
			WRITE_ONCE(pd->pde[d],
				   sgx_mmu_pde(page_to_phys(pd->table[d])));
		}
		if ((u64)page_to_phys(pages[i]) >> 32) {
			sgx_mmu_unmap(pd, addr, i);
			return -EFAULT;		/* the MMU has 32-bit addresses */
		}
		pte = page_address(pd->table[d]);
		WRITE_ONCE(pte[sgx_mmu_pt_index(a)],
			   sgx_mmu_pte(page_to_phys(pages[i]), flags));
	}
	return 0;
}

void sgx_mmu_unmap(struct sgx_mmu_pd *pd, u32 addr, unsigned int n)
{
	unsigned int i;
	u32 a, *pte;

	for (i = 0, a = addr; i < n; i++, a += PAGE_SIZE) {
		pte = sgx_mmu_ptes(pd, a);
		if (pte)
			WRITE_ONCE(pte[sgx_mmu_pt_index(a)], 0);
	}
}
