/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The SGX MMU's page tables: one 4 KiB page directory of 1024 PDEs per GPU
 * address space, each naming a 4 KiB page table of 1024 PTEs (4 KiB pages,
 * 32-bit device addresses, physical addresses below 4 GiB).
 */
#ifndef _GMA500_SGX_MMU_H_
#define _GMA500_SGX_MMU_H_

#include <linux/types.h>

struct page;
struct sgx_mmu_pd;

#define SGX_MMU_VALID	0x1
#define SGX_MMU_WO	0x2	/* write-only */
#define SGX_MMU_RO	0x4	/* read-only */
#define SGX_MMU_CACHED	0x8	/* cache-coherent (snooped) */

static inline u32 sgx_mmu_pde(u32 pt_phys)
{
	return pt_phys | SGX_MMU_VALID;
}

static inline u32 sgx_mmu_pte(u32 page_phys, u32 flags)
{
	return page_phys | flags | SGX_MMU_VALID;
}

static inline unsigned int sgx_mmu_pd_index(u32 addr)
{
	return addr >> 22;
}

static inline unsigned int sgx_mmu_pt_index(u32 addr)
{
	return (addr >> 12) & 0x3ff;
}

struct sgx_mmu_pd *sgx_mmu_pd_create(void);
void sgx_mmu_pd_destroy(struct sgx_mmu_pd *pd);
u32 sgx_mmu_pd_phys(const struct sgx_mmu_pd *pd);
int sgx_mmu_map(struct sgx_mmu_pd *pd, u32 addr, struct page **pages,
		unsigned int n, u32 flags);
void sgx_mmu_unmap(struct sgx_mmu_pd *pd, u32 addr, unsigned int n);
u32 sgx_mmu_lookup(const struct sgx_mmu_pd *pd, u32 addr);
u32 sgx_mmu_pd_entry(const struct sgx_mmu_pd *pd, unsigned int i);

#endif
