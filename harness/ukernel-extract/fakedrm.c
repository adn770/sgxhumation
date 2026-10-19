/* fakedrm.c — LD_PRELOAD shim over libdrm, Phase 1: LOG the PVR bridge sequence.
 * The Cedarview usermode (libsrv_um) talks to the kernel only via libdrm:
 * drmOpen + drmCommandWrite/WriteRead carry the PVRSRV bridge packets.
 * Here we just fake a device fd and log every call so we learn the exact
 * bridge-command sequence of the init path (recon for the real mock). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static FILE *lg;
__attribute__((constructor)) static void boot(void) {
    lg = fopen("/home/user/sgxhumation-uk/drm.log", "w");
    if (!lg) lg = stderr;
    setvbuf(lg, 0, _IONBF, 0);
    fprintf(lg, "[fakedrm] loaded\n");
}

/* PVRSRV_BRIDGE_PACKAGE layout (32-bit):
 *  +0 ui32BridgeID  +4 ui32Size  +8 pvParamIn  +12 ui32InBufferSize
 *  +16 pvParamOut   +20 ui32OutBufferSize  +24 hKernelServices           */
static unsigned bridge_id(void *d) { return d ? *(uint32_t *)d : 0xffffffffu; }

/* Make a bridge call "succeed": zero the OUT buffer so its leading eError
 * field reads PVRSRV_OK (0) and the rest is null/benign. */
static void ok_out(void *d) {
    if (!d) return;
    void  *out = *(void **)((char *)d + 16);
    uint32_t sz = *(uint32_t *)((char *)d + 20);
    if (out && sz && sz < 0x10000) __builtin_memset(out, 0, sz);
}

/* Interpose PVRSRVMemCopy (exported by libsrv_um, also used internally):
 * log every copy and flag large ones — the ukernel upload is a big copy. */
extern void *memcpy(void *, const void *, unsigned long);
void PVRSRVMemCopy(void *dst, const void *src, unsigned long n) {
    if (lg) fprintf(lg, "[PVRSRVMemCopy] dst=%p src=%p size=%lu%s\n",
                    dst, src, n, n >= 1024 ? "   <== LARGE (ukernel?)" : "");
    memcpy(dst, src, n);
}

/* Return a REAL fd (dummy /dev/null) so the driver's fcntl()/close() on it
 * succeed; only the drm* command ops are faked below. */
static int fake_fd(void) {
    int fd = open("/dev/null", O_RDWR);
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

/* The connect helper checks drmCommandNone(21)'s return as a boolean and treats
 * 0 as failure (b9d0: test eax,eax; je FAIL) — so return non-zero to proceed. */
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
int drmDropMaster(int fd) { fprintf(lg, "[drmDropMaster]\n"); return 0; }
int drmSetMaster(int fd) { fprintf(lg, "[drmSetMaster]\n"); return 0; }
int drmGetMagic(int fd, unsigned int *magic) { if (magic) *magic = 0; return 0; }
int drmAuthMagic(int fd, unsigned int magic) { return 0; }
