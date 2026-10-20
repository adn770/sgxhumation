// SPDX-License-Identifier: GPL-2.0-only
/*
 * sgxfive VM contexts: one GPU address space (an SGX page directory) per
 * context, managed by the client through VM_MAP and VM_UNMAP. Mappings are
 * exact ranges: a map may not overlap another, and an unmap removes exactly
 * one whole mapping, so a drm_mm of reserved nodes tracks them. Client VMs
 * hold only the client's own buffers: without memory updates the EDM never
 * runs through them, so they carry no alias of the firmware's buffers.
 */
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>

#include "sgx_mmu.h"
#include "sgx_seq.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

struct sgxfive_vm {
	struct kref ref;
	struct sgxfive *sgx;
	struct mutex lock;		/* the range tracker and the page tables */
	struct sgx_mmu_pd *pd;
	struct drm_mm mm;		/* one reserved node per mapping */
	struct sgxfive_deferred release;	/* of the page directory */
};

struct sgxfive_mapping {
	struct drm_mm_node node;
	struct drm_gem_shmem_object *shmem;	/* pinned while mapped */
	struct sgxfive_deferred release;	/* of the pages */
};

u64 sgxfive_ccb_done(u64 sent, u32 woff, u32 roff)
{
	return sent - (woff - roff) % SGX_CCB_ENTRIES;
}

/* Move the items of list whose command is done, in order, to due. */
unsigned int sgxfive_reap(struct list_head *list, u64 done, struct list_head *due)
{
	struct sgxfive_deferred *d, *next;
	unsigned int n = 0;

	list_for_each_entry_safe(d, next, list, link) {
		if (d->after > done)
			break;
		list_move_tail(&d->link, due);
		n++;
	}
	return n;
}

void sgxfive_free_all(struct list_head *due)
{
	struct sgxfive_deferred *d, *next;

	list_for_each_entry_safe(d, next, due, link) {
		list_del(&d->link);
		d->free(d);
	}
}

/* A VM_MAP range: page aligned, non-empty, inside one present user heap. */
int sgxfive_vm_check_range(const struct drm_sgxfive_heap *heaps, unsigned int n,
			   u64 addr, u64 size)
{
	unsigned int i;

	if (!size || (addr | size) & (PAGE_SIZE - 1) || addr + size < addr)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		const struct drm_sgxfive_heap *h = &heaps[i];

		if (!h->size || (h->flags & DRM_SGXFIVE_HEAP_RESERVED))
			continue;
		if (addr >= h->base && addr + size <= h->base + h->size)
			return 0;
	}
	return -EINVAL;
}

static void sgxfive_mapping_release(struct sgxfive_deferred *d)
{
	struct sgxfive_mapping *m = container_of(d, struct sgxfive_mapping, release);

	drm_gem_shmem_unpin(m->shmem);
	drm_gem_object_put(&m->shmem->base);
	kfree(m);
}

/*
 * The range is free for new mappings at once; the pages stay pinned until
 * the GPU's stale TLB entries for it are gone (design §3.3).
 */
static void sgxfive_mapping_free(struct sgxfive_vm *vm, struct sgxfive_mapping *m)
{
	sgx_mmu_unmap(vm->pd, m->node.start, m->node.size >> PAGE_SHIFT);
	drm_mm_remove_node(&m->node);
	m->release.free = sgxfive_mapping_release;
	sgxfive_defer(vm->sgx, &m->release);
}

static void sgxfive_vm_pd_release(struct sgxfive_deferred *d)
{
	struct sgxfive_vm *vm = container_of(d, struct sgxfive_vm, release);

	sgx_mmu_pd_destroy(vm->pd);
	kfree(vm);
}

static void sgxfive_vm_release(struct kref *ref)
{
	struct sgxfive_vm *vm = container_of(ref, struct sgxfive_vm, ref);
	struct drm_mm_node *node, *next;

	drm_mm_for_each_node_safe(node, next, &vm->mm)
		sgxfive_mapping_free(vm, container_of(node, struct sgxfive_mapping,
						      node));
	drm_mm_takedown(&vm->mm);
	mutex_destroy(&vm->lock);
	/*
	 * The EDM keeps directory-list assignments by PD address and its BIF
	 * caches what it read through them, so a later VM whose PD landed on
	 * this page would run on stale entries (seen: the tiling engine and
	 * cache requestors fault at page 0 and the job stalls). The directory
	 * and its tables go only after a command has made the BIF drop them.
	 */
	vm->release.free = sgxfive_vm_pd_release;
	sgxfive_defer(vm->sgx, &vm->release);
}

struct sgxfive_vm *sgxfive_vm_get(struct sgxfive_file *f, u32 handle)
{
	struct sgxfive_vm *vm;

	xa_lock(&f->vms);
	vm = xa_load(&f->vms, handle);
	if (vm)
		kref_get(&vm->ref);
	xa_unlock(&f->vms);
	return vm;
}

void sgxfive_vm_put(struct sgxfive_vm *vm)
{
	kref_put(&vm->ref, sgxfive_vm_release);
}

u32 sgxfive_vm_pd_phys(const struct sgxfive_vm *vm)
{
	return sgx_mmu_pd_phys(vm->pd);
}

int sgxfive_ioctl_create_vm(struct sgxfive *sgx, void *data,
			    struct drm_file *file)
{
	struct drm_sgxfive_create_vm_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_vm *vm;
	int ret;

	if (a->_padding_4)
		return -EINVAL;
	vm = kzalloc_obj(*vm);
	if (!vm)
		return -ENOMEM;
	vm->pd = sgx_mmu_pd_create();
	if (!vm->pd) {
		kfree(vm);
		return -ENOMEM;
	}
	kref_init(&vm->ref);
	vm->sgx = sgx;
	mutex_init(&vm->lock);
	drm_mm_init(&vm->mm, 0, 1ull << 32);
	ret = xa_alloc(&f->vms, &a->handle, vm, xa_limit_32b, GFP_KERNEL);
	if (ret)
		sgxfive_vm_put(vm);
	return ret;
}

int sgxfive_ioctl_destroy_vm(struct sgxfive *sgx, void *data,
			     struct drm_file *file)
{
	struct drm_sgxfive_destroy_vm_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_vm *vm;

	if (a->_padding_4)
		return -EINVAL;
	vm = xa_erase(&f->vms, a->handle);
	if (!vm)
		return -EINVAL;
	sgxfive_vm_put(vm);	/* contexts using it hold their own reference */
	return 0;
}

int sgxfive_ioctl_vm_map(struct sgxfive *sgx, void *data, struct drm_file *file)
{
	struct drm_sgxfive_vm_map_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct drm_gem_shmem_object *shmem;
	struct sgxfive_mapping *m = NULL;
	struct drm_gem_object *obj;
	struct sgxfive_vm *vm;
	int ret;

	if (a->_padding_14 || (a->flags & ~DRM_SGXFIVE_VM_MAP_FLAGS_MASK) ||
	    (a->offset & (PAGE_SIZE - 1)))
		return -EINVAL;
	ret = sgxfive_vm_check_range(sgxfive_rev1_heaps, DRM_SGXFIVE_HEAP_COUNT,
				     a->device_addr, a->size);
	if (ret)
		return ret;
	obj = drm_gem_object_lookup(file, a->handle);
	if (!obj)
		return -ENOENT;
	if (!sgxfive_is_bo(obj) || a->offset > obj->size ||
	    a->size > obj->size - a->offset) {
		drm_gem_object_put(obj);
		return -EINVAL;
	}
	vm = sgxfive_vm_get(f, a->vm_context_handle);
	if (!vm) {
		drm_gem_object_put(obj);
		return -EINVAL;
	}
	shmem = to_drm_gem_shmem_obj(obj);
	m = kzalloc_obj(*m);
	ret = m ? drm_gem_shmem_pin(shmem) : -ENOMEM;
	if (ret)
		goto err;
	m->shmem = shmem;
	m->node.start = a->device_addr;
	m->node.size = a->size;

	mutex_lock(&vm->lock);
	ret = drm_mm_reserve_node(&vm->mm, &m->node);
	if (ret == -ENOSPC)
		ret = -EEXIST;		/* overlaps a mapping */
	if (!ret) {
		ret = sgx_mmu_map(vm->pd, a->device_addr,
				  shmem->pages + (a->offset >> PAGE_SHIFT),
				  a->size >> PAGE_SHIFT,
				  a->flags & DRM_SGXFIVE_VM_MAP_READ_ONLY ?
				  SGX_MMU_RO : 0);
		if (ret)
			drm_mm_remove_node(&m->node);
	}
	mutex_unlock(&vm->lock);
	if (ret) {
		drm_gem_shmem_unpin(shmem);
		goto err;
	}
	/* The BIF must refetch the page tables (next command's cache control). */
	sgxfive_need_bif_inval(sgx);
	sgxfive_vm_put(vm);
	return 0;		/* the mapping keeps the object reference */
err:
	kfree(m);
	sgxfive_vm_put(vm);
	drm_gem_object_put(obj);
	return ret;
}

int sgxfive_ioctl_vm_unmap(struct sgxfive *sgx, void *data,
			   struct drm_file *file)
{
	struct drm_sgxfive_vm_unmap_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct drm_mm_node *node;
	struct sgxfive_vm *vm;
	int ret = -ENOENT;

	if (a->_padding_4)
		return -EINVAL;
	vm = sgxfive_vm_get(f, a->vm_context_handle);
	if (!vm)
		return -EINVAL;
	mutex_lock(&vm->lock);
	drm_mm_for_each_node(node, &vm->mm) {
		if (node->start == a->device_addr && node->size == a->size) {
			struct sgxfive_mapping *m;

			m = container_of(node, struct sgxfive_mapping, node);
			sgxfive_mapping_free(vm, m);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&vm->lock);
	if (!ret)
		sgxfive_need_bif_inval(sgx);
	sgxfive_vm_put(vm);
	return ret;
}

int sgxfive_open(struct sgxfive *sgx, struct drm_file *file)
{
	struct sgxfive_file *f = kzalloc_obj(*f);

	if (!f)
		return -ENOMEM;
	f->client = sgxfive_client_create();
	if (!f->client) {
		kfree(f);
		return -ENOMEM;
	}
	xa_init_flags(&f->vms, XA_FLAGS_ALLOC1);
	xa_init_flags(&f->ctxs, XA_FLAGS_ALLOC1);
	file->driver_priv = f;
	return 0;
}

void sgxfive_postclose(struct sgxfive *sgx, struct drm_file *file)
{
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_vm *vm;
	unsigned long i;

	if (!f)
		return;
	sgxfive_ctx_close_all(f);	/* contexts hold their VMs */
	xa_for_each(&f->vms, i, vm)
		sgxfive_vm_put(vm);
	xa_destroy(&f->vms);
	sgxfive_client_put(f->client);
	kfree(f);
	file->driver_priv = NULL;
}
