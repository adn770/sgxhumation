// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* fakedrm.c — LD_PRELOAD shim over libdrm. The Cedarview usermode (libsrv_um)
 * talks to the kernel only via libdrm: drmOpen + drmCommandWrite carry the
 * PVRSRV bridge packets. This shim fakes a device fd, answers bridge calls,
 * decodes every command by name, and — crucially — makes the ALLOC_DEVICEMEM +
 * MHANDLE_TO_MMAP_DATA + mmap() dance hand back REAL host buffers, so when
 * srvinit uploads the USSE microkernel it lands in memory we own and dump.
 *
 * Command numbering (confirmed from the live trace): the bridge ID low byte is
 * the absolute command index. The closed lib is built with PDUMP+OVERLAY
 * DISABLED, placing PVRSRV_BRIDGE_SGX_CMD_BASE at 83 and INITSRV_CONNECT at 67.
 * The srvinit path we drive:
 *   CONNECT_SERVICES(12) INITSRV_CONNECT(67) ENUM_DEVICES(0)
 *   ACQUIRE_DEVICEINFO(1) SGXINFO_FOR_SRVINIT(98) -> per ukernel buffer:
 *   ALLOC_DEVICEMEM(6) MHANDLE_TO_MMAP_DATA(11) [mmap] [memcpy ukernel].
 *
 * BUILD & RUN (on cachix — CachyOS, multilib present; 32-bit is mandatory):
 *   gcc -m32 -shared -fPIC fakedrm.c -o fakedrm.so -ldl
 *   gcc -m32 loader.c -o loader -ldl
 *   # libs staged in ~/sgxhumation-uk/lib/ with unversioned-soname symlinks:
 *   ln -sf libsrv_um.so.1.7.862890   ~/sgxhumation-uk/lib/libsrv_um.so
 *   ln -sf libsrv_init.so.1.7.862890 ~/sgxhumation-uk/lib/libsrv_init.so
 *   LD_LIBRARY_PATH=~/sgxhumation-uk/lib LD_PRELOAD=./fakedrm.so ./loader
 *   cat  ~/sgxhumation-uk/drm.log       # bridge trace + ukernel copy
 *   ls   ~/sgxhumation-uk/dump/          # alloc<NN>_heap<ID>_sz<SZ>.bin
 * NOTE: both objects MUST be -m32; a 64-bit build silently ignores the shim. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define DUMPDIR "/home/user/sgxhumation-uk/dump"
static FILE *lg;

/* Device allocations we service. Each gets a unique hMappingInfo token and a
 * unique page-aligned mmap "cookie"; our mmap() interposer turns the cookie
 * into a real anonymous buffer (ptr) that the driver then fills. */
struct alloc { uint32_t hmap, size, heap, off, dva; void *ptr; };

/* Heap base device addresses presented to srvinit, by heap ID (sgxapi_km.h order:
 * GENERAL, TADATA, KERNEL_CODE, KERNEL_DATA, PIXELSHADER, VERTEXSHADER,
 * PDSPIXEL_CODEDATA; bases per sgxconfig.h). g_next is a per-heap bump allocator
 * handing out realistic, unique device addresses, so every address srvinit derives
 * (init registers, ukernel patch sites, init data) can be traced back to its buffer. */
static uint32_t HEAP_BASE[7] = { 0x08000000u, 0xD0000000u, 0xE8000000u,
    0xF0000000u, 0xF4000000u, 0xFC000000u, 0xE4000000u };
static uint32_t g_next[7];
static struct alloc g_a[64];
static int g_na;

/* fds returned by our fake drmOpen (so mmap() only hijacks those). */
static int g_fd[16], g_nfd;
static void add_fd(int fd) { if (g_nfd < 16) g_fd[g_nfd++] = fd; }
static int  is_devfd(int fd) {
    for (int i = 0; i < g_nfd; i++) if (g_fd[i] == fd) return 1;
    return 0;
}

static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int   (*real_munmap)(void *, size_t);
static long  (*real_syscall)(long, ...);
static int   g_pending = -1;   /* alloc index awaiting its mmap2 (set by MHANDLE) */

__attribute__((constructor)) static void boot(void) {
    lg = fopen("/home/user/sgxhumation-uk/drm.log", "w");
    if (!lg) lg = stderr;
    setvbuf(lg, 0, _IONBF, 0);
    real_mmap    = dlsym(RTLD_NEXT, "mmap");
    real_munmap  = dlsym(RTLD_NEXT, "munmap");
    real_syscall = dlsym(RTLD_NEXT, "syscall");
    mkdir(DUMPDIR, 0755);
    fprintf(lg, "[fakedrm] loaded (mmap=%p munmap=%p)\n",
            (void *)real_mmap, (void *)real_munmap);
}

/* Write one allocation's buffer to dump/, labelled by heap + size, then null
 * the ptr so we don't dump freed memory twice. */
static void dump_one(int i) {
    if (!g_a[i].ptr) return;
    char path[256];
    snprintf(path, sizeof path, DUMPDIR "/alloc%02d_heap%08x_sz%06x.bin",
             i, g_a[i].heap, g_a[i].size);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(g_a[i].ptr, 1, g_a[i].size, f); fclose(f); }
    fprintf(lg, "[dump] %s (%u bytes)\n", path, g_a[i].size);
    g_a[i].ptr = 0;
}
__attribute__((destructor)) static void fin(void) {
    fprintf(lg, "[fakedrm] dumping %d surviving allocations at exit\n", g_na);
    for (int i = 0; i < g_na; i++) dump_one(i);
}

/* PVRSRV_BRIDGE_PACKAGE (32-bit): +0 ui32BridgeID +4 ui32Size +8 pvParamIn
 *  +12 ui32InBufferSize +16 pvParamOut +20 ui32OutBufferSize +24 hKernelServices */
static unsigned bridge_id(void *d) { return d ? *(uint32_t *)d : 0xffffffffu; }

static const char *cmd_name(unsigned nr) {
    switch (nr) {
    case 0:  return "ENUM_DEVICES";        case 1:  return "ACQUIRE_DEVICEINFO";
    case 2:  return "RELEASE_DEVICEINFO";  case 3:  return "CREATE_DEVMEMCONTEXT";
    case 5:  return "GET_DEVMEM_HEAPINFO"; case 6:  return "ALLOC_DEVICEMEM";
    case 7:  return "FREE_DEVICEMEM";      case 11: return "MHANDLE_TO_MMAP_DATA";
    case 12: return "CONNECT_SERVICES";    case 13: return "DISCONNECT_SERVICES";
    case 27: return "RELEASE_MMAP_DATA";   case 67: return "INITSRV_CONNECT";
    case 68: return "INITSRV_DISCONNECT";  case 83: return "SGX_GETCLIENTINFO";
    case 85: return "SGX_GETINTERNALDEVINFO"; case 97: return "SGX_GETMISCINFO";
    case 98: return "SGXINFO_FOR_SRVINIT"; case 99: return "SGX_DEVINITPART2";
    default: return "?";
    }
}

#define PVRSRV_DEVICE_TYPE_SGX 7u
/* SGX_MISC_INFO answer for SGX_GETMISCINFO, for differential captures. Each
 * MISC_<FIELD> environment variable, if set, is written to its field; with none
 * set the struct is left as srvinit passed it in (a zero-mock). Layout per
 * sgxapi_km.h with SUPPORT_SGX_EDM_MEMORY_DEBUG off: eRequest, padding, then the
 * uData union at dword 2 (ui32SGXClockSpeed, or PVRSRV_SGX_MISCINFO_FEATURES). */
static int misc_env(const char *name, uint32_t *dst) {
    const char *v = getenv(name);
    if (!v) return 0;
    *dst = (uint32_t)strtoul(v, 0, 0);
    return 1;
}

static void miscinfo_fill(uint32_t *mi) {
    static const char *const FEAT[7] = { "MISC_COREREV", "MISC_COREID", "MISC_DDKVER",
        "MISC_DDKBUILD", "MISC_COREIDSW", "MISC_COREREVSW", "MISC_BUILDOPTS" };
    int n = 0;
    if (mi[0] == 0)                                     /* REQUEST_CLOCKSPEED */
        n += misc_env("MISC_CLOCK", &mi[2]);
    else if (mi[0] == 1 || mi[0] == 2)                  /* REQUEST_(DRIVER_)SGXREV */
        for (int k = 0; k < 7; k++) n += misc_env(FEAT[k], &mi[2 + k]);
    fprintf(lg, "   [miscinfo] filled %d field(s) after=[", n);
    for (int k = 0; k < 10; k++) fprintf(lg, "%s%08x", k ? "," : "", mi[k]);
    fprintf(lg, "]\n");
}

static void ok_out(void *d) {
    if (!d) return;
    unsigned nr   = (*(uint32_t *)d) & 0xffu;
    void    *in   = *(void **)((char *)d + 8);
    uint32_t insz = *(uint32_t *)((char *)d + 12);
    void    *out  = *(void **)((char *)d + 16);
    uint32_t sz   = *(uint32_t *)((char *)d + 20);
    uint32_t *i   = (uint32_t *)in;

    fprintf(lg, "  -> nr=%u (%s) insz=%u outsz=%u", nr, cmd_name(nr), insz, sz);
    if (in && insz >= 4) {
        unsigned nd = insz / 4; if (nd > 16) nd = 16;
        fprintf(lg, " IN=[");
        for (unsigned k = 0; k < nd; k++) fprintf(lg, "%s%08x", k ? "," : "", i[k]);
        fprintf(lg, "]");
    }
    fprintf(lg, "\n");

    /* SGX_DEVINITPART2's IN is the whole SGX_BRIDGE_INIT_INFO srvinit builds:
     * host kick addresses, the register init scripts, EDM task / clock-gating
     * values. Capture it verbatim for offline decoding. */
    if (nr == 99 && in && insz) {
        FILE *f = fopen(DUMPDIR "/devinitpart2_in.bin", "wb");
        if (f) { fwrite(in, 1, insz, f); fclose(f); }
        fprintf(lg, "[dump] devinitpart2_in.bin (%u bytes)\n", insz);
    }

    /* SGX_GETMISCINFO: IN = bridge id, hDevCookie, psMiscInfo (a pointer into
     * srvinit's own memory). The answer goes back through that struct, not OUT. */
    if (nr == 97 && in && insz >= 12 && i[2]) {
        uint32_t *mi = (uint32_t *)(uintptr_t)i[2];
        fprintf(lg, "   [miscinfo] eRequest=%u before=[", mi[0]);
        for (int k = 0; k < 10; k++) fprintf(lg, "%s%08x", k ? "," : "", mi[k]);
        fprintf(lg, "]\n");
        miscinfo_fill(mi);
    }

    if (!out || !sz || sz >= 0x10000) return;
    __builtin_memset(out, 0, sz);
    uint32_t *o = (uint32_t *)out;

    if (nr == 0) {                 /* ENUM_DEVICES: 1x SGX (type 7) */
        o[1] = 1; o[2] = PVRSRV_DEVICE_TYPE_SGX;
        fprintf(lg, "   [respond] ENUM_DEVICES -> 1x SGX(type 7)\n");
    } else if (nr == 1) {          /* ACQUIRE_DEVICEINFO -> hDevCookie */
        o[1] = 0xDEAD0001u;
    } else if (nr == 98) {         /* SGXINFO_FOR_SRVINIT: present 7 heaps */
        /* OUT = u32 eError; u32 sPDDevPAddr; PVRSRV_HEAP_INFO asHeapInfo[32].
         * srvinit switches on (ui32HeapID & 0xffffff); IDs 0..6 must exist or
         * it faults reading sDevVAddrBase. Heap IDs per sgxapi_km.h; bases per
         * sgxconfig.h. ID 2 = KERNEL_CODE (where the ukernel is uploaded).
         * PVRSRV_HEAP_INFO (24B): +0 id +4 hDevMemHeap +8 base +12 size ... */
        static const uint32_t size[7] = { 0xB8000000u-0x1000u, 0x0D000000u-0x1000u,
            0x00080000u-0x1000u, 0x03000000u-0x1000u, 0x05000000u-0x1000u,
            0x02000000u-0x1000u, 0x02000000u-0x1000u };
        o[1] = 0x1000u;            /* sPDDevPAddr (nonzero, page-aligned) */
        misc_env("MOCK_PDPADDR", &o[1]);   /* override, for differential captures */
        uint32_t *h = o + 2;       /* &asHeapInfo[0] */
        for (int k = 0; k < 7; k++) {  /* MOCK_HEAP_BASE_<id> overrides, likewise */
            char name[32];
            snprintf(name, sizeof name, "MOCK_HEAP_BASE_%d", k);
            misc_env(name, &HEAP_BASE[k]);
        }
        for (int k = 0; k < 32; k++, h += 6) {
            if (k < 7) { h[0] = k; h[1] = 0xBEEF0000u + k; h[2] = HEAP_BASE[k]; h[3] = size[k]; }
            else       { h[0] = 0xFFFFFFFFu; }   /* UNDEFINED -> skipped */
        }
        fprintf(lg, "   [respond] SGXINFO_FOR_SRVINIT -> 7 heaps\n");
    } else if (nr == 6) {          /* ALLOC_DEVICEMEM */
        /* IN = flags,hDevCookie,hDevMemHeap,ui32Attribs,ui32Size,ui32Alignment.
         * OUT (76B, offsets pinned empirically; sCpuPAddr is 64-bit):
         *   o0 eError  o1 psKernelMemInfo  o2 pvLinAddr .. o9 uAllocSize
         *   o11 hMappingInfo  o12 hKernelMemInfo. */
        uint32_t heap = (insz >= 24) ? i[2] : 0;
        uint32_t size = (insz >= 24) ? i[4] : 0;
        int idx = (g_na < 64) ? g_na++ : 63;
        uint32_t hmap = 0x4D410000u + idx;             /* 'MA' token */
        g_a[idx].hmap = hmap; g_a[idx].size = size; g_a[idx].heap = heap;
        g_a[idx].off  = 0x10000000u + (uint32_t)idx * 0x100000u;  /* page-aligned cookie */
        g_a[idx].ptr  = 0;
        /* sDevVAddr (o4): next free, aligned slot in the requested heap (token
         * 0xBEEF0000+id), at least page-granular so neighbours stay distinct. */
        uint32_t hid = heap - 0xBEEF0000u, dva = 0;
        if (hid < 7) {
            uint32_t align = (insz >= 24 && i[5] > 0x1000u) ? i[5] : 0x1000u;
            if (!g_next[hid]) {
                /* First allocation's offset in its heap; MOCK_HEAP_START moves every
                 * buffer, for differential captures of the address-patched fields. */
                uint32_t start = 0x1000u;
                misc_env("MOCK_HEAP_START", &start);
                g_next[hid] = HEAP_BASE[hid] + start;
            }
            dva = (g_next[hid] + align - 1) & ~(align - 1);
            g_next[hid] = dva + ((size + 0xFFFu) & ~0xFFFu);
        }
        g_a[idx].dva = dva;
        o[4]  = dva;
        o[1]  = 0xC0DE0000u + idx;  /* psKernelMemInfo (opaque, non-null) */
        o[9]  = size;               /* uAllocSize */
        o[11] = hmap;               /* hMappingInfo -> used by MHANDLE */
        o[12] = 0xC0DE0000u + idx;  /* hKernelMemInfo */
        fprintf(lg, "   [respond] ALLOC #%d heap=%08x size=%#x dva=%08x hMap=%08x off=%08x\n",
                idx, heap, size, dva, hmap, g_a[idx].off);
    } else if (nr == 11) {         /* MHANDLE_TO_MMAP_DATA */
        /* IN = flags, hMHandle. OUT (20B): eError, MMapOffset, ByteOffset,
         * RealByteSize, UserVAddr. Return our cookie + page-rounded size so the
         * driver mmap()s it; UserVAddr=0 forces the mmap path. */
        uint32_t hmh = (insz >= 8) ? i[1] : 0;
        int idx = -1;
        for (int k = 0; k < g_na; k++) if (g_a[k].hmap == hmh) { idx = k; break; }
        if (idx >= 0) {
            uint32_t rsz = (g_a[idx].size + 0xfffu) & ~0xfffu;
            if (!rsz) rsz = 0x1000u;
            g_a[idx].size = rsz;        /* dump the whole mapped region */
            o[1] = g_a[idx].off;        /* ui32MMapOffset (our cookie) */
            o[3] = rsz;                 /* ui32RealByteSize */
            g_pending = idx;            /* next mmap2 on the device fd is this buffer */
            fprintf(lg, "   [respond] MHANDLE #%d -> off=%08x rsz=%#x\n", idx, g_a[idx].off, rsz);
        } else {
            fprintf(lg, "   [respond] MHANDLE: unknown hMHandle=%08x\n", hmh);
        }
    }
    /* Everything else: eError=0 (zeroed OUT). */
}

/* syscall interposer: libsrv_um maps device memory via raw syscall(SYS_mmap2)
 * (it imports `syscall` + `munmap` but NOT `mmap`). Catch mmap2 (i386 #192) on
 * our device fd and serve a real anonymous RW buffer, associating it with the
 * allocation whose MHANDLE_TO_MMAP_DATA just ran (init is strictly sequential:
 * ALLOC #k -> MHANDLE #k -> mmap2). Grab 6 args unconditionally (benign on the
 * i386 cdecl ABI) and pass non-mmap2 calls straight through. */
#define SYS_MMAP2_I386 192
long syscall(long n, ...) {
    va_list ap; va_start(ap, n);
    long a0 = va_arg(ap, long), a1 = va_arg(ap, long), a2 = va_arg(ap, long),
         a3 = va_arg(ap, long), a4 = va_arg(ap, long), a5 = va_arg(ap, long);
    va_end(ap);
    if (n == SYS_MMAP2_I386 && is_devfd((int)a4)) {
        size_t len = (size_t)a1;
        int idx = g_pending;
        void *p = real_mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (idx >= 0 && idx < g_na) { g_a[idx].ptr = p; g_pending = -1; }
        fprintf(lg, "[mmap2] devfd=%d len=%zu pgoff=%#lx -> anon %p (alloc #%d heap %08x)\n",
                (int)a4, len, (unsigned long)a5, p, idx,
                (idx >= 0 && idx < g_na) ? g_a[idx].heap : 0);
        return (long)p;
    }
    return real_syscall(n, a0, a1, a2, a3, a4, a5);
}

/* mmap interposer: only hijack our fake device fd at a known cookie offset;
 * hand back a fresh anonymous RW buffer the driver will fill with the ukernel.
 * All other mmap()s (malloc arenas, dlopen, ...) pass through untouched. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (is_devfd(fd)) {
        for (int k = 0; k < g_na; k++) if (g_a[k].off == (uint32_t)off) {
            void *p = real_mmap(0, len, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            g_a[k].ptr = p;
            fprintf(lg, "[mmap] devfd=%d off=%08lx len=%zu -> anon %p (alloc #%d heap %08x)\n",
                    fd, (unsigned long)off, len, p, k, g_a[k].heap);
            return p;
        }
        fprintf(lg, "[mmap] devfd=%d off=%08lx len=%zu (no cookie match)\n",
                fd, (unsigned long)off, len);
    }
    return real_mmap(addr, len, prot, flags, fd, off);
}
int munmap(void *addr, size_t len) {
    for (int k = 0; k < g_na; k++) if (g_a[k].ptr == addr) {
        fprintf(lg, "[munmap] alloc #%d -> dumping before unmap\n", k);
        dump_one(k);
    }
    return real_munmap(addr, len);
}

/* Interpose PVRSRVMemCopy: log every copy; a large one whose src is inside
 * libsrv_* is the ukernel upload — src reveals its offset in the binary. */
void PVRSRVMemCopy(void *dst, const void *src, unsigned long n) {
    if (lg) fprintf(lg, "[PVRSRVMemCopy] dst=%p src=%p size=%lu%s\n",
                    dst, src, n, n >= 512 ? "   <== LARGE (ukernel?)" : "");
    memcpy(dst, src, n);
}

/* Return a REAL fd (dummy /dev/null) so fcntl()/close() work; track it so our
 * mmap interposer knows which fd is the "device". */
static int fake_fd(void) {
    int fd = open("/dev/null", O_RDWR);
    add_fd(fd);
    fprintf(lg, "[drmOpen] -> real fd=%d (/dev/null)\n", fd);
    return fd;
}
int drmOpen(const char *name, const char *busid) {
    fprintf(lg, "[drmOpen] name=%s busid=%s\n", name ? name : "(null)", busid ? busid : "(null)");
    return fake_fd();
}
int drmOpenWithType(const char *name, const char *busid, int type) {
    fprintf(lg, "[drmOpenWithType] name=%s type=%d\n", name ? name : "(null)", type);
    return fake_fd();
}
int drmClose(int fd) { fprintf(lg, "[drmClose] fd=%d\n", fd); if (fd >= 0) close(fd); return 0; }

/* The connect helper tests drmCommandNone(21)'s return as a boolean (treats 0
 * as failure), so return non-zero to proceed. */
int drmCommandNone(int fd, unsigned long idx) {
    fprintf(lg, "[drmCommandNone] fd=%d idx=%lu -> 1\n", fd, idx);
    return 1;
}
int drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size) {
    fprintf(lg, "[drmCommandWrite] idx=%lu size=%lu bridgeID=%u\n", idx, size, bridge_id(data));
    ok_out(data);
    return 0;
}
int drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size) {
    fprintf(lg, "[drmCommandWriteRead] idx=%lu size=%lu bridgeID=%u\n", idx, size, bridge_id(data));
    ok_out(data);
    return 0;
}
int drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size) {
    fprintf(lg, "[drmCommandRead] idx=%lu size=%lu\n", idx, size);
    return 0;
}
int drmDropMaster(int fd) { return 0; }
int drmSetMaster(int fd) { return 0; }
int drmGetMagic(int fd, unsigned int *magic) { if (magic) *magic = 0; return 0; }
int drmAuthMagic(int fd, unsigned int magic) { return 0; }
