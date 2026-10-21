// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* fakedrm.c — LD_PRELOAD shim over libdrm. The Cedarview usermode (libsrv_um)
 * talks to the kernel only via libdrm: drmOpen + drmCommandWrite carry the
 * PVRSRV bridge packets. This shim fakes a device fd, answers bridge calls,
 * logs every command by name with its arguments, and makes the ALLOC_DEVICEMEM
 * + MHANDLE_TO_MMAP_DATA + mmap2 dance hand back REAL host buffers, so whatever
 * the driver writes to "device memory" lands in memory we own and dump.
 *
 * Two clients use it:
 *  - loader.c runs srvinit: the microkernel upload and SGX_DEVINITPART2's init
 *    info are captured into $SGX_UK_DIR/dump/ (README "Init-info capture").
 *  - tri.c runs the closed GLES2 stack for one triangle: every SGX_DOKICK and
 *    SGX_SUBMITTRANSFER is captured with a snapshot of every live allocation
 *    into $SGX_UK_DIR/dump3d/ (README "3D capture"). The kicks are completed as
 *    the EDM would: their TA and 3D status updates are written to memory.
 *
 * Command numbering (bridgecmd.h): the bridge ID low byte is the absolute
 * command index; the closed lib is built with PDUMP+OVERLAY disabled, placing
 * PVRSRV_BRIDGE_SGX_CMD_BASE at 83 and INITSRV_CONNECT at 67. Struct layouts are
 * the DDK 1.7 i686 ones (pvr_bridge.h, sgx_bridge.h, sgxinfo.h) for the
 * Cedarview kernel configuration (SUPPORT_SGX_NEW_STATUS_VALS and
 * SUPPORT_PERCONTEXT_PB on, SUPPORT_SID_INTERFACE and SUPPORT_MEMINFO_IDS off).
 *
 * BUILD & RUN (x86 Linux with 32-bit userspace; 32-bit is mandatory). The
 * staging directory is $SGX_UK_DIR, default ~/sgxhumation-uk (ukdir.h):
 *   gcc -m32 -shared -fPIC fakedrm.c -o fakedrm.so -ldl
 *   LD_LIBRARY_PATH=$SGX_UK_DIR/lib LD_PRELOAD=./fakedrm.so ./loader   # or ./tri
 *   cat  $SGX_UK_DIR/drm.log       # bridge trace
 * NOTE: all objects MUST be -m32; a 64-bit build silently ignores the shim. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ukdir.h"
#include "bridgecmd.h"
#include "devmem.h"

static char g_dump[UK_PATH_MAX];     /* $SGX_UK_DIR/dump   (srvinit, exit dumps) */
static char g_dump3d[UK_PATH_MAX];   /* $SGX_UK_DIR/dump3d (kick captures) */
static FILE *lg;

/* Heap ids (sgxapi_km.h) and bases (sgxconfig.h, the 32-bit layout the harness has
 * always presented; SUPPORT_PERCONTEXT_PB puts the parameter buffer heap at
 * 0xC0000000). srvinit sees ids 0..6 (SGXINFO_FOR_SRVINIT); clients see all of
 * them (CREATE_DEVMEMCONTEXT / GET_DEVMEM_HEAPINFO). */
enum { H_GENERAL, H_TADATA, H_KERNEL_CODE, H_KERNEL_DATA, H_PIXELSHADER, H_VERTEXSHADER,
       H_PDSPIXEL, H_PDSVERTEX, H_SYNCINFO, H_SHARED_3DPARAM, H_PERCTX_3DPARAM, NHEAPS };
static const char *const HEAP_NAME[NHEAPS] = { "GENERAL", "TADATA", "KERNEL_CODE",
    "KERNEL_DATA", "PIXELSHADER", "VERTEXSHADER", "PDSPIXEL_CODEDATA", "PDSVERTEX_CODEDATA",
    "SYNCINFO", "SHARED_3DPARAMETERS", "PERCONTEXT_3DPARAMETERS" };
static uint32_t HEAP_BASE[NHEAPS] = { 0x08000000u, 0xD0000000u, 0xE8000000u, 0xF0000000u,
    0xF4000000u, 0xFC000000u, 0xE4000000u, 0xEC000000u, 0xE0000000u, 0, 0xC0000000u };
static const uint32_t HEAP_SIZE[NHEAPS] = { 0xB8000000u - 0x1000u, 0x0D000000u - 0x1000u,
    0x00080000u - 0x1000u, 0x03000000u - 0x1000u, 0x05000000u - 0x1000u,
    0x02000000u - 0x1000u, 0x02000000u - 0x1000u, 0x01C00000u - 0x1000u,
    0x01000000u - 0x1000u, 0, 0x10000000u - 0x1000u };
#define HEAP_TOKEN 0xBEEF0000u          /* hDevMemHeap = HEAP_TOKEN + id */
#define KMI_TOKEN  0xC0DE0000u          /* psKernelMemInfo / hKernelMemInfo = + index */
#define DEVCOOKIE  0xDEAD0001u
#define DEVMEMCTX  0xDC000001u
#define EVENT_TOKEN 0xEE000001u

static struct devmem g_m;
static uint32_t g_pdpaddr = 0x1000u;
static uint32_t g_devdata[24];
static int g_nkick, g_nxfer;

static int (*real_munmap)(void *, size_t);
static long (*real_syscall)(long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static FILE *(*real_fopen)(const char *, const char *);
static int g_pending = -1;   /* object awaiting its mmap2 (set by MHANDLE) */

/* fds returned by our fake drmOpen (so mmap only hijacks those). */
static int g_fd[16], g_nfd;
static void add_fd(int fd) { if (g_nfd < 16) g_fd[g_nfd++] = fd; }
static int is_devfd(int fd)
{
    for (int i = 0; i < g_nfd; i++) if (g_fd[i] == fd) return 1;
    return 0;
}

static int env_u32(const char *name, uint32_t *dst)
{
    const char *v = getenv(name);
    if (!v) return 0;
    *dst = (uint32_t)strtoul(v, 0, 0);
    return 1;
}

/* asInitDevData from a captured SGX_DEVINITPART2 IN buffer (2068 bytes; the
 * array sits just before the 18 init memory handles at the end). The kernel
 * hands it to every client in SGX_GETCLIENTINFO. */
static void load_devdata(void)
{
    char p[UK_PATH_MAX];
    FILE *f = real_fopen(uk_path(p, sizeof p, "devinit.bin"), "rb");
    uint32_t w[517];
    if (f && fread(w, 4, 517, f) == 517)
        memcpy(g_devdata, &w[517 - 18 - 24], sizeof g_devdata);
    if (f) fclose(f);
    fprintf(lg, "[fakedrm] asDevData %s:", f ? "from devinit.bin" : "zero (no devinit.bin)");
    for (int k = 0; k < 24; k++) fprintf(lg, " %x", g_devdata[k]);
    fprintf(lg, "\n");
}

__attribute__((constructor)) static void boot(void)
{
    char logpath[UK_PATH_MAX];
    real_mmap    = dlsym(RTLD_NEXT, "mmap");
    real_munmap  = dlsym(RTLD_NEXT, "munmap");
    real_syscall = dlsym(RTLD_NEXT, "syscall");
    real_fopen   = dlsym(RTLD_NEXT, "fopen");
    uk_path(g_dump, sizeof g_dump, "dump");
    uk_path(g_dump3d, sizeof g_dump3d, "dump3d");
    lg = real_fopen(uk_path(logpath, sizeof logpath, "drm.log"), "w");
    if (!lg) lg = stderr;
    setvbuf(lg, 0, _IONBF, 0);
    mkdir(g_dump, 0755);
    mkdir(g_dump3d, 0755);
    uint32_t start = 0x1000u;
    /* MOCK_HEAP_START moves every buffer and MOCK_HEAP_BASE_<id> one heap, for
     * differential captures of address-patched fields. */
    env_u32("MOCK_HEAP_START", &start);
    env_u32("MOCK_PDPADDR", &g_pdpaddr);
    devmem_init(&g_m, start);
    for (int k = 0; k < NHEAPS; k++) {
        char name[32];
        snprintf(name, sizeof name, "MOCK_HEAP_BASE_%d", k);
        env_u32(name, &HEAP_BASE[k]);
        if (HEAP_SIZE[k]) devmem_set_heap(&g_m, k, HEAP_BASE[k], HEAP_SIZE[k]);
    }
    fprintf(lg, "[fakedrm] loaded pid=%d\n", (int)getpid());
    load_devdata();
}

/* ---- device objects -------------------------------------------------------- */

static int new_obj(unsigned heap, uint32_t size, uint32_t align, uint32_t kind, uint32_t attribs)
{
    int k = devmem_alloc(&g_m, heap, size, align, kind);
    if (k < 0) {
        fprintf(lg, "   [devmem] allocation of %#x in heap %u FAILED\n", size, heap);
        return -1;
    }
    struct devmem_obj *o = &g_m.obj[k];
    o->attribs = attribs;
    o->ptr = real_mmap(0, o->mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return k;
}

static void *dva_ptr(uint32_t dva)
{
    uint32_t off;
    int k = devmem_find(&g_m, dva, &off);
    return k >= 0 && g_m.obj[k].ptr ? (char *)g_m.obj[k].ptr + off : 0;
}

static const char *heap_name(unsigned id) { return id < NHEAPS ? HEAP_NAME[id] : "?"; }

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = real_fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

/* Old srvinit-style dump: alloc<NN>_heap<token>_sz<SZ>.bin in dump/. */
static void dump_one(int k)
{
    struct devmem_obj *o = &g_m.obj[k];
    if (!o->ptr || o->kind != DEVMEM_MEM) return;
    char path[UK_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/alloc%02d_heap%08x_sz%06x.bin", g_dump, k,
             HEAP_TOKEN + o->heap, o->mapped);
    write_file(path, o->ptr, o->mapped);
}

/* Snapshot every live object into dump3d/<tag>/ with an index file. */
static void snapshot(const char *tag)
{
    char dir[UK_PATH_MAX + 64], path[UK_PATH_MAX + 160];
    snprintf(dir, sizeof dir, "%s/%s", g_dump3d, tag);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/objects.txt", dir);
    FILE *idx = real_fopen(path, "w");
    for (int k = 0; k < g_m.n; k++) {
        struct devmem_obj *o = &g_m.obj[k];
        if (!o->ptr) continue;
        snprintf(path, sizeof path, "%s/obj%03d_%08x.bin", dir, k, o->dva);
        write_file(path, o->ptr, o->mapped);
        if (idx)
            fprintf(idx, "%3d heap=%-2u %-24s dva=%08x size=%#08x mapped=%#08x attribs=%08x "
                    "kind=%u\n", k, o->heap, heap_name(o->heap), o->dva, o->size, o->mapped,
                    o->attribs, o->kind);
    }
    if (idx) fclose(idx);
    fprintf(lg, "   [snapshot] %d objects -> %s\n", g_m.n, dir);
}

__attribute__((destructor)) static void fin(void)
{
    fprintf(lg, "[fakedrm] exit: dumping %d objects\n", g_m.n);
    for (int k = 0; k < g_m.n; k++) dump_one(k);
    if (g_nkick || g_nxfer) snapshot("exit");
}

/* ---- bridge ---------------------------------------------------------------- */

/* PVRSRV_BRIDGE_PACKAGE (32-bit): +0 ui32BridgeID +4 ui32Size +8 pvParamIn
 *  +12 ui32InBufferSize +16 pvParamOut +20 ui32OutBufferSize +24 hKernelServices */
static unsigned bridge_id(void *d) { return d ? *(uint32_t *)d : 0xffffffffu; }

static void log_words(const char *tag, const uint32_t *w, uint32_t bytes, unsigned max)
{
    unsigned nd = bytes / 4;
    if (nd > max) nd = max;
    fprintf(lg, " %s=[", tag);
    for (unsigned k = 0; k < nd; k++) fprintf(lg, "%s%08x", k ? "," : "", w[k]);
    fprintf(lg, "%s]", bytes / 4 > max ? ",..." : "");
}

#define PVRSRV_DEVICE_TYPE_SGX 7u
#define PVRSRV_MEM_NO_SYNCOBJ (1u << 3)

/* SGX_MISC_INFO answer for SGX_GETMISCINFO, for differential captures. Each
 * MISC_<FIELD> environment variable, if set, is written to its field; with none
 * set the struct is left as the caller passed it in (a zero-mock). Layout per
 * sgxapi_km.h with SUPPORT_SGX_EDM_MEMORY_DEBUG off: eRequest, padding, then the
 * uData union at dword 2 (ui32SGXClockSpeed, or PVRSRV_SGX_MISCINFO_FEATURES). */
static void miscinfo_fill(uint32_t *mi)
{
    static const char *const FEAT[7] = { "MISC_COREREV", "MISC_COREID", "MISC_DDKVER",
        "MISC_DDKBUILD", "MISC_COREIDSW", "MISC_COREREVSW", "MISC_BUILDOPTS" };
    int n = 0;
    if (mi[0] == 2 && !getenv("MISC_ZERO")) {
        /* REQUEST_DRIVER_SGXREV: what SGXGetMiscInfoKM reports, the DDK version
         * (MAJ << 16 | MIN << 8 | BRANCH = 1.7.17), build 862890, and the kernel
         * build options, which the client checks against its own (srvinit's
         * ui32ClientBuildOptions, 0x60218). MISC_ZERO=1 keeps the zero-mock. */
        mi[4] = 0x010711u; mi[5] = 862890u; mi[8] = 0x60218u;
    }
    if (mi[0] == 0)                                     /* REQUEST_CLOCKSPEED */
        n += env_u32("MISC_CLOCK", &mi[2]);
    else if (mi[0] == 1 || mi[0] == 2)                  /* REQUEST_(DRIVER_)SGXREV */
        for (int k = 0; k < 7; k++) n += env_u32(FEAT[k], &mi[2 + k]);
    fprintf(lg, "   [miscinfo] filled %d field(s) after=[", n);
    for (int k = 0; k < 10; k++) fprintf(lg, "%s%08x", k ? "," : "", mi[k]);
    fprintf(lg, "]\n");
}

/* PVRSRV_HEAP_INFO (24 B): id, hDevMemHeap, sDevVAddrBase, ui32HeapByteSize,
 * ui32Attribs, ui32XTileStride. Returns the number written. */
static unsigned fill_heaps(uint32_t *h, unsigned max, unsigned nheaps)
{
    unsigned n = 0;
    for (unsigned k = 0; k < nheaps && n < max; k++) {
        if (!HEAP_SIZE[k]) continue;
        uint32_t *e = h + 6 * n++;
        e[0] = k; e[1] = HEAP_TOKEN + k; e[2] = HEAP_BASE[k]; e[3] = HEAP_SIZE[k];
        e[4] = 0; e[5] = 0;
    }
    for (unsigned k = n; k < max; k++) h[6 * k] = 0xFFFFFFFFu;   /* UNDEFINED */
    return n;
}

/* A kernel meminfo handle as the client passes it back: the devmem handle, or
 * the psKernelMemInfo token. */
static int kmi_index(uint32_t h)
{
    uint32_t k = h - KMI_TOKEN;
    return k < (uint32_t)g_m.n ? (int)k : devmem_by_handle(&g_m, h);
}

static void describe_handle(const char *name, uint32_t h)
{
    int k = kmi_index(h);
    if (k >= 0)
        fprintf(lg, "     %-34s %08x -> obj %d %s dva=%08x size=%#x\n", name, h, k,
                heap_name(g_m.obj[k].heap), g_m.obj[k].dva, g_m.obj[k].size);
    else
        fprintf(lg, "     %-34s %08x\n", name, h);
}

/* SGX_CCB_KICK (sgxinfo.h) for this build, in dwords after ui32BridgeFlags and
 * hDevCookie: sCommand[8], hCCBKernelMemInfo, ui32NumDstSyncObjects,
 * hKernelHWSyncListMemInfo, pahDstSyncHandles, ui32NumTAStatusVals,
 * ui32Num3DStatusVals, asTAStatusUpdate[32] and as3DStatusUpdate[4] of
 * {sStatusDevAddr, ui32StatusValue, hKernelMemInfo}, bFirstKickOrResume,
 * bLastInScene, ui32CCBOffset, ui32NumSrcSyncs, ahSrcKernelSyncInfo[8],
 * bTADependency, hTA3DSyncInfo, hTASyncInfo, h3DSyncInfo, hDevMemContext. */
#define KICK_TA_STATUS 14
#define KICK_3D_STATUS (KICK_TA_STATUS + 32 * 3)
#define KICK_AFTER_STATUS (KICK_3D_STATUS + 4 * 3)
#define KICK_DWORDS (KICK_AFTER_STATUS + 3 + 1 + 8 + 5)

static void apply_status(const char *what, const uint32_t *u, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        uint32_t addr = u[3 * k], val = u[3 * k + 1];
        uint32_t *p = dva_ptr(addr);
        fprintf(lg, "     %s status[%u] *%08x = %08x (kmi %08x)%s\n", what, k, addr, val,
                u[3 * k + 2], p ? "" : "  <unmapped>");
        if (p) *p = val;
    }
}

/* A kernel sync info handle as the client passes it: our sync object, whose
 * buffer is its PVRSRV_SYNC_DATA {WriteOpsPending, WriteOpsComplete,
 * ReadOpsPending, read_ops_complete, ...}. */
static uint32_t *sync_data(uint32_t h, uint32_t *dva)
{
    int k = h ? devmem_by_handle(&g_m, h) : -1;
    if (k < 0 || g_m.obj[k].kind != DEVMEM_SYNC) return 0;
    if (dva) *dva = g_m.obj[k].dva;
    return g_m.obj[k].ptr;
}

/* SGXMKIF_CMDTA_SHARED (sgx_mkif_km.h, 0x1e0 bytes for this build), dwords:
 * 0 ui32CtrlFlags, 1 ui32NumTAStatusVals, 2 ui32Num3DStatusVals, 3..6 TA TQ sync
 * {WriteOpsPendingVal, WriteOpsCompleteDevVAddr, ReadOpsPendingVal,
 * ReadOpsCompleteDevVAddr}, 7..10 the same for the 3D TQ sync, 11
 * ui32NumSrcSyncs, 12..43 asSrcSyncs[8], 44..47 sTA3DDependency, 48..111
 * sCtlTAStatusInfo[32], 112..119 sCtl3DStatusInfo[4]. A PVRSRV_DEVICE_SYNC_OBJECT
 * is {ReadOpsPendingVal, sReadOpsCompleteDevVAddr, WriteOpsPendingVal,
 * sWriteOpsCompleteDevVAddr}. */
enum { SH_FLAGS = 0, SH_NTA = 1, SH_N3D = 2, SH_TATQ = 3, SH_3DTQ = 7, SH_NSRC = 11,
       SH_SRC = 12, SH_TA3D = 44, SH_TASTAT = 48, SH_3DSTAT = 112, SH_DWORDS = 120 };

/* What sgxkick.c:SGXDoKickKM writes before it queues the command: the sync and
 * status fields of the shared block in the client CCB, the destination sync
 * list on the first kick of a scene, and the READY flag. Pending counters move
 * as the kernel moves them. */
static void kick_km(const uint32_t *k, uint32_t *sh)
{
    const uint32_t *a = k + KICK_AFTER_STATUS;
    uint32_t dva, *y;
    if ((y = sync_data(a[13], &dva))) {                 /* hTA3DSyncInfo */
        sh[SH_TA3D + 3] = dva + 4;
        sh[SH_TA3D + 2] = y[0];
        if (a[12]) y[0]++;                              /* bTADependency */
    }
    if ((y = sync_data(a[14], &dva))) {                 /* hTASyncInfo */
        sh[SH_TATQ + 3] = dva + 12; sh[SH_TATQ + 1] = dva + 4;
        sh[SH_TATQ + 2] = y[2]++;   sh[SH_TATQ + 0] = y[0];
    }
    if ((y = sync_data(a[15], &dva))) {                 /* h3DSyncInfo */
        sh[SH_3DTQ + 3] = dva + 12; sh[SH_3DTQ + 1] = dva + 4;
        sh[SH_3DTQ + 2] = y[2]++;   sh[SH_3DTQ + 0] = y[0];
    }
    sh[SH_NTA] = k[12];
    for (uint32_t n = 0; n < k[12] && n < 32; n++) {
        sh[SH_TASTAT + 2 * n] = k[KICK_TA_STATUS + 3 * n];
        sh[SH_TASTAT + 2 * n + 1] = k[KICK_TA_STATUS + 3 * n + 1];
    }
    sh[SH_N3D] = k[13];
    for (uint32_t n = 0; n < k[13] && n < 4; n++) {
        sh[SH_3DSTAT + 2 * n] = k[KICK_3D_STATUS + 3 * n];
        sh[SH_3DSTAT + 2 * n + 1] = k[KICK_3D_STATUS + 3 * n + 1];
    }
    sh[SH_NSRC] = a[3];
    for (uint32_t n = 0; n < a[3] && n < 8; n++)
        if ((y = sync_data(a[4 + n], &dva))) {
            uint32_t *d = sh + SH_SRC + 4 * n;
            d[0] = y[2]++; d[1] = dva + 12; d[2] = y[0]; d[3] = dva + 4;
        }
    /* SGXMKIF_HWDEVICE_SYNC_LIST {sAccessDevAddr, ui32NumSyncObjects, asSyncData[]} */
    int sl = devmem_by_handle(&g_m, k[10]);
    const uint32_t *dst = (const uint32_t *)(uintptr_t)k[11];
    if (a[0] && k[9] && sl >= 0 && dst) {
        uint32_t *l = g_m.obj[sl].ptr;
        l[1] = k[9];
        for (uint32_t n = 0; n < k[9]; n++) {
            uint32_t *d = l + 2 + 4 * n;
            if ((y = sync_data(dst[n], &dva))) {
                d[0] = y[2]; d[1] = dva + 12; d[2] = y[0]++; d[3] = dva + 4;
            } else {
                d[0] = d[2] = 0;
            }
            fprintf(lg, "     dst sync[%u] %08x -> list entry {%x %08x %x %08x}\n", n, dst[n],
                    d[0], d[1], d[2], d[3]);
        }
    }
    sh[SH_FLAGS] |= 1u;                                 /* CTRLFLAGS_READY */
}

/* Complete a kick as the EDM would once the TA and the render are done: write
 * the TA and 3D status values, and bump the complete counters of every sync the
 * command waits on or updates (TQ syncs, sources, destination list). */
static void complete_kick(const uint32_t *sh, const uint32_t *k)
{
    apply_status("TA", k + KICK_TA_STATUS, k[12] <= 32 ? k[12] : 32);
    apply_status("3D", k + KICK_3D_STATUS, k[13] <= 4 ? k[13] : 4);
    uint32_t *p;
    for (int t = 0; t < 2; t++) {
        const uint32_t *q = sh + (t ? SH_3DTQ : SH_TATQ);
        if (q[1] && (p = dva_ptr(q[1]))) *p = q[0] + 1;
        if (q[3] && (p = dva_ptr(q[3]))) *p = q[2] + 1;
    }
    for (uint32_t n = 0; n < sh[SH_NSRC] && n < 8; n++) {
        const uint32_t *d = sh + SH_SRC + 4 * n;
        if (d[1] && (p = dva_ptr(d[1]))) *p = d[0] + 1;
    }
    int sl = devmem_by_handle(&g_m, k[10]);
    if (sl >= 0 && k[KICK_AFTER_STATUS + 1]) {          /* bLastInScene */
        const uint32_t *l = g_m.obj[sl].ptr;
        for (uint32_t n = 0; n < l[1] && n < 64; n++) {
            const uint32_t *d = l + 2 + 4 * n;
            if (d[3] && (p = dva_ptr(d[3]))) *p = d[2] + 1;
        }
    }
}

static void do_kick(const uint32_t *in, uint32_t insz)
{
    int n = g_nkick++;
    char tag[32], path[UK_PATH_MAX + 64];
    snprintf(tag, sizeof tag, "kick%02d", n);
    snprintf(path, sizeof path, "%s/%s_in.bin", g_dump3d, tag);
    write_file(path, in, insz);
    if (insz / 4 < 2 + KICK_DWORDS) {
        fprintf(lg, "   [kick] unexpected IN size %u\n", insz);
        snapshot(tag);
        return;
    }
    const uint32_t *k = in + 2;
    fprintf(lg, "   [kick %d] sCommand=[%08x %08x | %08x %08x %08x %08x %08x %08x]\n", n,
            k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7]);
    describe_handle("hCCBKernelMemInfo", k[8]);
    fprintf(lg, "     ui32NumDstSyncObjects=%u hKernelHWSyncListMemInfo=%08x pahDst=%08x\n",
            k[9], k[10], k[11]);
    const uint32_t *a = k + KICK_AFTER_STATUS;
    fprintf(lg, "     NumTAStatus=%u Num3DStatus=%u bFirstKickOrResume=%u bLastInScene=%u "
            "ui32CCBOffset=%#x NumSrcSyncs=%u bTADependency=%u\n", k[12], k[13], a[0], a[1],
            a[2], a[3], a[12]);
    describe_handle("hTA3DSyncInfo", a[13]);
    describe_handle("hTASyncInfo", a[14]);
    describe_handle("h3DSyncInfo", a[15]);
    int ccb = kmi_index(k[8]);
    uint32_t *sh = 0;
    if (ccb >= 0 && a[2] + SH_DWORDS * 4 <= g_m.obj[ccb].mapped) {
        sh = (uint32_t *)((char *)g_m.obj[ccb].ptr + a[2]);
        fprintf(lg, "     CMDTA_SHARED at dva %08x\n", g_m.obj[ccb].dva + a[2]);
        kick_km(k, sh);
    }
    snapshot(tag);
    if (sh) complete_kick(sh, k);
}

static void do_transfer(const uint32_t *in, uint32_t insz)
{
    int n = g_nxfer++;
    char tag[32], path[UK_PATH_MAX + 64];
    snprintf(tag, sizeof tag, "xfer%02d", n);
    snprintf(path, sizeof path, "%s/%s_in.bin", g_dump3d, tag);
    write_file(path, in, insz);
    if (insz >= 20) {
        describe_handle("hCCBMemInfo", in[2]);
        fprintf(lg, "     ui32SharedCmdCCBOffset=%#x sHWTransferContextDevVAddr=%08x\n",
                in[3], in[4]);
    }
    snapshot(tag);
}

/* REGISTER_HW_{RENDER,TRANSFER}_CONTEXT: the kernel copies the client's context
 * image into KERNEL_DATA and patches the page-directory address into it. */
static void register_context(const char *what, const uint32_t *i, uint32_t *o)
{
    const void *img = (const void *)(uintptr_t)i[2];
    uint32_t size = i[3], pdoff = i[4];
    int k = new_obj(H_KERNEL_DATA, size, 32, DEVMEM_CONTEXT, 0);
    if (k < 0) { o[0] = 1; return; }
    struct devmem_obj *c = &g_m.obj[k];
    if (img) memcpy(c->ptr, img, size);
    if (pdoff + 4 <= size) *(uint32_t *)((char *)c->ptr + pdoff) = g_pdpaddr;
    o[1] = c->handle;
    o[2] = c->dva;
    fprintf(lg, "   [respond] %s size=%#x pdoff=%#x hDevMemContext=%08x -> obj %d dva=%08x\n",
            what, size, pdoff, i[5], k, c->dva);
}

static void ok_out(void *d)
{
    if (!d) return;
    unsigned nr   = (*(uint32_t *)d) & 0xffu;
    void    *in   = *(void **)((char *)d + 8);
    uint32_t insz = *(uint32_t *)((char *)d + 12);
    void    *out  = *(void **)((char *)d + 16);
    uint32_t sz   = *(uint32_t *)((char *)d + 20);
    uint32_t *i   = (uint32_t *)in;

    fprintf(lg, "  -> nr=%u (%s) insz=%u outsz=%u", nr, bridge_cmd_name(nr), insz, sz);
    if (in && insz >= 4) log_words("IN", i, insz, 24);
    fprintf(lg, "\n");
    if (!i) insz = 0;

    /* SGX_DEVINITPART2's IN is the whole SGX_BRIDGE_INIT_INFO srvinit builds. */
    if (nr == BR_SGX_DEVINITPART2 && insz) {
        char path[UK_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/devinitpart2_in.bin", g_dump);
        write_file(path, in, insz);
        fprintf(lg, "[dump] devinitpart2_in.bin (%u bytes)\n", insz);
    }
    /* SGX_GETMISCINFO: IN = flags, hDevCookie, psMiscInfo (a pointer into the
     * caller's memory). The answer goes back through that struct, not OUT. */
    if (nr == BR_SGX_GETMISCINFO && insz >= 12 && i[2]) {
        uint32_t *mi = (uint32_t *)(uintptr_t)i[2];
        fprintf(lg, "   [miscinfo] eRequest=%u before=[", mi[0]);
        for (int k = 0; k < 10; k++) fprintf(lg, "%s%08x", k ? "," : "", mi[k]);
        fprintf(lg, "]\n");
        miscinfo_fill(mi);
    }
    if (nr == BR_SGX_DOKICK && insz) do_kick(i, insz);
    if (nr == BR_SGX_SUBMITTRANSFER && insz) do_transfer(i, insz);

    if (!out || !sz || sz >= 0x10000) return;
    memset(out, 0, sz);
    uint32_t *o = (uint32_t *)out;

    switch (nr) {
    case BR_ENUM_DEVICES:              /* 1x SGX (type 7) */
        o[1] = 1; o[2] = PVRSRV_DEVICE_TYPE_SGX;
        break;
    case BR_ACQUIRE_DEVICEINFO:
        o[1] = DEVCOOKIE;
        break;
    case BR_SGXINFO_FOR_SRVINIT:       /* srvinit: sPDDevPAddr + 7 heaps */
        o[1] = g_pdpaddr;
        fill_heaps(o + 2, 32, 7);
        fprintf(lg, "   [respond] SGXINFO_FOR_SRVINIT -> 7 heaps\n");
        break;
    case BR_CREATE_DEVMEMCONTEXT:      /* eError, hDevMemContext, count, heaps[32] */
        if (sz >= 12 + 32 * 24) {
            o[1] = DEVMEMCTX;
            o[2] = fill_heaps(o + 3, 32, NHEAPS);
            fprintf(lg, "   [respond] CREATE_DEVMEMCONTEXT -> ctx %08x, %u heaps\n", o[1], o[2]);
        }
        break;
    case BR_GET_DEVMEM_HEAPINFO:       /* eError, count, heaps[32] */
        if (sz >= 8 + 32 * 24) o[1] = fill_heaps(o + 2, 32, NHEAPS);
        break;
    case BR_ALLOC_DEVICEMEM: {
        /* IN = flags, hDevCookie, hDevMemHeap, ui32Attribs, ui32Size, ui32Alignment.
         * OUT (19 dwords, i686 with a 32-bit sCpuPAddr): o0 eError, o1
         * psKernelMemInfo, sClientMemInfo {o2 pvLinAddr, o3 pvLinAddrKM, o4
         * sDevVAddr, o5 sCpuPAddr, o6 ui32Flags, o7 ui32ClientFlags, o8 uAllocSize,
         * o9 psClientSyncInfo, o10 hMappingInfo, o11 hKernelMemInfo, o12 hResItem,
         * o13 psNext}, sClientSyncInfo {o14 psSyncData, o15
         * sWriteOpsCompleteDevVAddr, o16 sReadOpsCompleteDevVAddr, o17 hMappingInfo,
         * o18 hKernelSyncInfo}. The client maps a buffer by passing its kernel
         * handle (hKernelMemInfo, hKernelSyncInfo) to MHANDLE_TO_MMAP_DATA, so both
         * handles of an object are its devmem handle. */
        uint32_t heap = insz >= 24 ? i[2] - HEAP_TOKEN : 0xffu;
        uint32_t attr = insz >= 24 ? i[3] : 0, size = insz >= 24 ? i[4] : 0;
        uint32_t align = insz >= 24 ? i[5] : 0;
        int k = new_obj(heap, size, align, DEVMEM_MEM, attr);
        if (k < 0) { o[0] = 1; break; }
        struct devmem_obj *m = &g_m.obj[k];
        o[1] = KMI_TOKEN + k; o[4] = m->dva; o[6] = attr; o[8] = size;
        o[10] = m->handle; o[11] = m->handle;
        int s = -1;
        if (!(attr & PVRSRV_MEM_NO_SYNCOBJ) && sz >= 19 * 4) {
            /* PVRSRV_SYNC_DATA: WriteOpsPending, WriteOpsComplete,
             * ReadOpsPending, read_ops_complete, LastOpDumpVal, LastReadOpDumpVal. */
            s = new_obj(H_SYNCINFO, 24, 0, DEVMEM_SYNC, 0);
            if (s >= 0) {
                o[15] = g_m.obj[s].dva + 4; o[16] = g_m.obj[s].dva + 12;
                o[17] = g_m.obj[s].handle; o[18] = g_m.obj[s].handle;
            }
        }
        fprintf(lg, "   [respond] ALLOC obj %d heap=%s attribs=%08x size=%#x align=%#x "
                "dva=%08x hMap=%08x sync=%d\n", k, heap_name(heap), attr, size, align,
                m->dva, m->handle, s);
        break;
    }
    case BR_MHANDLE_TO_MMAP_DATA: {
        /* IN = flags, hMHandle. OUT (20B): eError, ui32MMapOffset,
         * ui32ByteOffset, ui32RealByteSize, ui32UserVAddr. */
        int k = insz >= 8 ? devmem_by_handle(&g_m, i[1]) : -1;
        if (k < 0) {
            fprintf(lg, "   [respond] MHANDLE: unknown hMHandle=%08x\n", insz >= 8 ? i[1] : 0);
            o[0] = 1;
            break;
        }
        o[1] = g_m.obj[k].cookie;
        o[3] = g_m.obj[k].mapped;
        g_pending = k;
        fprintf(lg, "   [respond] MHANDLE obj %d -> off=%08x rsz=%#x\n", k, o[1], o[3]);
        break;
    }
    case BR_GET_MISC_INFO: {
        /* IN = flags, PVRSRV_MISC_INFO (136 B); OUT = eError, PVRSRV_MISC_INFO.
         * Misc info dwords: 0 ui32StateRequest, 1 ui32StatePresent, 10..22
         * sGlobalEventObject {szName[50], hOSEventKM @23}, 24 hOSGlobalEvent,
         * 25..28 aui32DDKVersion. Answer the global event object, the DDK version
         * and CPU cache operations (a no-op here). */
        if (insz < 4 + 136 || sz < 4 + 136) break;
        uint32_t *mi = o + 1;
        memcpy(mi, i + 1, 136);
        uint32_t req = mi[0];
        mi[1] = req & ((1u << 3) | (1u << 4) | (1u << 5));
        if (req & (1u << 3)) {
            strcpy((char *)&mi[10], "PVRSRV_GLOBAL_EVENTOBJECT");
            mi[23] = EVENT_TOKEN; mi[24] = EVENT_TOKEN;
        }
        if (req & (1u << 4)) { mi[25] = 1; mi[26] = 7; mi[27] = 17; mi[28] = 862890; }
        fprintf(lg, "   [respond] GET_MISC_INFO request=%#x present=%#x\n", req, mi[1]);
        break;
    }
    case BR_EVENT_OBJECT_OPEN:         /* OUT = hOSEvent, eError */
        o[0] = EVENT_TOKEN + 1;
        break;
    case BR_EVENT_OBJECT_WAIT: {       /* nothing to wait for: the GPU is instant */
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, 0);
        break;
    }
    case BR_SGX_GETCLIENTINFO:
        /* OUT = SGX_CLIENT_INFO {ui32ProcessID, pvProcess, sMiscInfo, asDevData[24]},
         * eError: the device data sits just before eError. */
        o[0] = (uint32_t)getpid();
        if (sz >= 4 * 26) memcpy(o + sz / 4 - 25, g_devdata, sizeof g_devdata);
        break;
    case BR_SGX_GETINTERNALDEVINFO: {
        /* OUT = {ui32Flags, hHostCtlKernelMemInfoHandle, bForcePTOff}, eError. The
         * client maps the host control block (SGXMKIF_HOST_CTL, 0x44 bytes). */
        static int hostctl = -1;
        if (hostctl < 0) hostctl = new_obj(H_KERNEL_DATA, 0x44, 0, DEVMEM_MEM, 0);
        if (hostctl >= 0) o[1] = g_m.obj[hostctl].handle;
        break;
    }
    case BR_SGX_FINDSHAREDPBDESC:      /* none yet: all handles 0, eError OK */
        if (insz >= 16)
            fprintf(lg, "   [respond] FINDSHAREDPBDESC bLockOnFailure=%u ui32TotalPBSize=%#x"
                    " -> not found\n", i[2], i[3]);
        break;
    case BR_SGX_ADDSHAREDPBDESC: {
        /* IN = flags, ui32TotalPBSize, hDevCookie, hSharedPBDescKernelMemInfo,
         * hHWPBDescKernelMemInfo, hBlockKernelMemInfo, hHWBlockKernelMemInfo,
         * phKernelMemInfoHandles, ui32KernelMemInfoHandlesCount, sHWPBDescDevVAddr. */
        if (insz < 40) break;
        fprintf(lg, "   [respond] ADDSHAREDPBDESC ui32TotalPBSize=%#x sHWPBDescDevVAddr=%08x\n",
                i[1], i[9]);
        describe_handle("hSharedPBDescKernelMemInfo", i[3]);
        describe_handle("hHWPBDescKernelMemInfo", i[4]);
        describe_handle("hBlockKernelMemInfo", i[5]);
        describe_handle("hHWBlockKernelMemInfo", i[6]);
        const uint32_t *sub = (const uint32_t *)(uintptr_t)i[7];
        for (uint32_t k = 0; sub && k < i[8] && k < 16; k++) {
            char name[48];
            snprintf(name, sizeof name, "ahKernelMemInfoHandles[%u]", k);
            describe_handle(name, sub[k]);
        }
        o[1] = 0x50420001u;           /* hSharedPBDesc ("PB") */
        break;
    }
    case BR_SGX_REGISTER_HW_RENDER_CONTEXT:
        if (insz >= 24 && sz >= 12) register_context("HW_RENDER_CONTEXT", i, o);
        break;
    case BR_SGX_REGISTER_HW_TRANSFER_CONTEXT:
        if (insz >= 24 && sz >= 12) register_context("HW_TRANSFER_CONTEXT", i, o);
        break;
    default:                           /* everything else: eError = 0 */
        break;
    }
    fprintf(lg, "  <-");
    log_words("OUT", o, sz, 16);
    fprintf(lg, "\n");
}

/* Phase markers from the test program (tri.c looks this up with dlsym). */
__attribute__((visibility("default"))) void fakedrm_mark(const char *what)
{
    fprintf(lg, "==== %s (objects so far: %d, kicks: %d) ====\n", what, g_m.n, g_nkick);
}

/* syscall interposer: libsrv_um maps device memory via raw syscall(SYS_mmap2)
 * (it imports `syscall` + `munmap` but NOT `mmap`). Catch mmap2 (i386 #192) on
 * our device fd and return the object's buffer: by its cookie (the page offset),
 * else the object whose MHANDLE_TO_MMAP_DATA just ran. Grab 6 args
 * unconditionally (benign on the i386 cdecl ABI) and pass other calls through. */
#define SYS_MMAP2_I386 192
long syscall(long n, ...)
{
    va_list ap; va_start(ap, n);
    long a0 = va_arg(ap, long), a1 = va_arg(ap, long), a2 = va_arg(ap, long),
         a3 = va_arg(ap, long), a4 = va_arg(ap, long), a5 = va_arg(ap, long);
    va_end(ap);
    if (n == SYS_MMAP2_I386 && is_devfd((int)a4)) {
        uint32_t pg = (uint32_t)a5;
        int k = devmem_by_cookie(&g_m, pg);
        if (k < 0) k = devmem_by_cookie(&g_m, pg << 12);
        if (k < 0) k = g_pending;
        g_pending = -1;
        void *p = k >= 0 ? g_m.obj[k].ptr : MAP_FAILED;
        fprintf(lg, "[mmap2] devfd=%d len=%#lx pgoff=%#x -> %p (obj %d %s)\n", (int)a4,
                (unsigned long)a1, pg, p, k, k >= 0 ? heap_name(g_m.obj[k].heap) : "-");
        return (long)p;
    }
    return real_syscall(n, a0, a1, a2, a3, a4, a5);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (is_devfd(fd)) {
        int k = devmem_by_cookie(&g_m, (uint32_t)off);
        fprintf(lg, "[mmap] devfd=%d off=%08lx len=%zu -> obj %d\n", fd, (unsigned long)off,
                len, k);
        if (k >= 0) return g_m.obj[k].ptr;
    }
    return real_mmap(addr, len, prot, flags, fd, off);
}

/* Device buffers stay alive until exit (they are snapshotted at every kick). */
int munmap(void *addr, size_t len)
{
    for (int k = 0; k < g_m.n; k++)
        if (g_m.obj[k].ptr == addr) {
            fprintf(lg, "[munmap] obj %d kept\n", k);
            return 0;
        }
    return real_munmap(addr, len);
}

/* The app hints are read from /etc/powervr.ini; take ours from the staging dir. */
FILE *fopen(const char *path, const char *mode)
{
    if (path && !strcmp(path, "/etc/powervr.ini")) {
        char p[UK_PATH_MAX];
        uk_path(p, sizeof p, "powervr.ini");
        if (lg) fprintf(lg, "[fopen] /etc/powervr.ini -> %s\n", p);
        return real_fopen(p, mode);
    }
    return real_fopen(path, mode);
}

/* Interpose PVRSRVMemCopy: log every copy; a large one whose src is inside
 * libsrv_* is the ukernel upload — src reveals its offset in the binary. */
void PVRSRVMemCopy(void *dst, const void *src, unsigned long n)
{
    if (lg) fprintf(lg, "[PVRSRVMemCopy] dst=%p src=%p size=%lu%s\n",
                    dst, src, n, n >= 512 ? "   <== LARGE" : "");
    memcpy(dst, src, n);
}

/* Return a REAL fd (dummy /dev/null) so fcntl()/close() work; track it so our
 * mmap interposer knows which fd is the "device". */
static int fake_fd(void)
{
    int fd = open("/dev/null", O_RDWR);
    add_fd(fd);
    fprintf(lg, "[drmOpen] -> real fd=%d (/dev/null)\n", fd);
    return fd;
}
int drmOpen(const char *name, const char *busid)
{
    fprintf(lg, "[drmOpen] name=%s busid=%s\n", name ? name : "(null)", busid ? busid : "(null)");
    return fake_fd();
}
int drmOpenWithType(const char *name, const char *busid, int type)
{
    fprintf(lg, "[drmOpenWithType] name=%s type=%d\n", name ? name : "(null)", type);
    (void)busid;
    return fake_fd();
}
int drmClose(int fd) { fprintf(lg, "[drmClose] fd=%d\n", fd); if (fd >= 0) close(fd); return 0; }

/* The connect helper tests drmCommandNone(21)'s return as a boolean (treats 0
 * as failure), so return non-zero to proceed. */
int drmCommandNone(int fd, unsigned long idx)
{
    fprintf(lg, "[drmCommandNone] fd=%d idx=%lu -> 1\n", fd, idx);
    return 1;
}
int drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{
    fprintf(lg, "[drmCommandWrite] idx=%lu size=%lu bridgeID=%#x\n", idx, size, bridge_id(data));
    (void)fd;
    ok_out(data);
    return 0;
}
int drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{
    fprintf(lg, "[drmCommandWriteRead] idx=%lu size=%lu bridgeID=%#x\n", idx, size,
            bridge_id(data));
    (void)fd;
    ok_out(data);
    return 0;
}
int drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{
    fprintf(lg, "[drmCommandRead] idx=%lu size=%lu\n", idx, size);
    (void)fd; (void)data;
    return 0;
}
int drmDropMaster(int fd) { (void)fd; return 0; }
int drmSetMaster(int fd) { (void)fd; return 0; }
int drmGetMagic(int fd, unsigned int *magic) { (void)fd; if (magic) *magic = 0; return 0; }
int drmAuthMagic(int fd, unsigned int magic) { (void)fd; (void)magic; return 0; }
