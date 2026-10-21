/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Josep Torra */
/* devmem.h — the fake device-memory model behind fakedrm.c.
 *
 * Heaps are indexed by the DDK heap id (sgxapi_km.h: 0 GENERAL ... 10 PERCONTEXT
 * 3DPARAMETERS). Each heap is a bump allocator: an object starts at the next
 * free address rounded up to its alignment (at least a page) and takes a whole
 * number of pages, so neighbours never share a page. Objects are never freed:
 * the harness keeps every buffer until exit so it can snapshot all of them at
 * each kick. Each object gets a mapping handle (0x4D410000 + index, "MA") that
 * the driver passes back to MHANDLE_TO_MMAP_DATA, and a page-aligned mmap
 * cookie. */
#ifndef DEVMEM_H
#define DEVMEM_H

#include <stdint.h>
#include <string.h>

#define DEVMEM_MAX_HEAPS 16
#define DEVMEM_MAX_OBJS  1024
#define DEVMEM_PAGE      0x1000u
#define DEVMEM_HANDLE    0x4D410000u
#define DEVMEM_COOKIE    0x10000000u   /* mmap offset of object 0 */
#define DEVMEM_COOKIE_STEP 0x00100000u

enum devmem_kind { DEVMEM_MEM = 0, DEVMEM_SYNC = 1, DEVMEM_CONTEXT = 2 };

struct devmem_obj {
    uint32_t handle, heap, size, mapped, dva, align, attribs, kind, cookie;
    void *ptr;
};

struct devmem_heap { uint32_t base, size, next; int present; };

struct devmem {
    struct devmem_heap heap[DEVMEM_MAX_HEAPS];
    struct devmem_obj obj[DEVMEM_MAX_OBJS];
    int n;
    uint32_t start;        /* offset of the first object in each heap */
};

static inline void devmem_init(struct devmem *m, uint32_t start)
{
    memset(m, 0, sizeof *m);
    m->start = start;
}

static inline void devmem_set_heap(struct devmem *m, unsigned id, uint32_t base, uint32_t size)
{
    if (id >= DEVMEM_MAX_HEAPS)
        return;
    m->heap[id].base = base;
    m->heap[id].size = size;
    m->heap[id].next = base + m->start;
    m->heap[id].present = 1;
}

static inline uint32_t devmem_round(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Allocate size bytes in heap id; return the object index or -1. */
static inline int devmem_alloc(struct devmem *m, unsigned id, uint32_t size, uint32_t align,
                               uint32_t kind)
{
    if (id >= DEVMEM_MAX_HEAPS || !m->heap[id].present || m->n >= DEVMEM_MAX_OBJS)
        return -1;
    struct devmem_heap *h = &m->heap[id];
    uint32_t a = align > DEVMEM_PAGE ? align : DEVMEM_PAGE;
    uint32_t dva = devmem_round(h->next, a);
    uint32_t mapped = devmem_round(size ? size : 1, DEVMEM_PAGE);
    if ((uint64_t)dva + mapped > (uint64_t)h->base + h->size)
        return -1;
    h->next = dva + mapped;
    int k = m->n++;
    struct devmem_obj *o = &m->obj[k];
    memset(o, 0, sizeof *o);
    o->handle = DEVMEM_HANDLE + (uint32_t)k;
    o->heap = id;
    o->size = size;
    o->mapped = mapped;
    o->dva = dva;
    o->align = align;
    o->kind = kind;
    o->cookie = DEVMEM_COOKIE + (uint32_t)k * DEVMEM_COOKIE_STEP;
    return k;
}

/* The object whose pages contain dva (offset in *off), or -1. */
static inline int devmem_find(const struct devmem *m, uint32_t dva, uint32_t *off)
{
    for (int k = 0; k < m->n; k++) {
        const struct devmem_obj *o = &m->obj[k];
        if (dva >= o->dva && dva - o->dva < o->mapped) {
            if (off)
                *off = dva - o->dva;
            return k;
        }
    }
    return -1;
}

static inline int devmem_by_handle(const struct devmem *m, uint32_t handle)
{
    uint32_t k = handle - DEVMEM_HANDLE;
    return k < (uint32_t)m->n ? (int)k : -1;
}

static inline int devmem_by_cookie(const struct devmem *m, uint32_t cookie)
{
    for (int k = 0; k < m->n; k++)
        if (m->obj[k].cookie == cookie)
            return k;
    return -1;
}

#endif
