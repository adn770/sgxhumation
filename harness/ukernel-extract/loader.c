/* loader.c — drive the closed Cedarview usermode init far enough to reveal
 * the bridge sequence (Phase 1). dlopen libsrv_um + libsrv_init, then call
 * PVRSRVConnect (which opens the drm device via our fake libdrm) and probe
 * the init entry points. Runs under LD_PRELOAD=./fakedrm.so. */
#include <stdio.h>
#include <dlfcn.h>

#define UM   "/home/user/sgxhumation-uk/lib/libsrv_um.so.1.7.862890"
#define INIT "/home/user/sgxhumation-uk/lib/libsrv_init.so.1.7.862890"

int main(void) {
    void *um = dlopen(UM, RTLD_NOW | RTLD_GLOBAL);
    printf("dlopen um   = %p  (%s)\n", um, um ? "ok" : dlerror());
    if (!um) return 1;
    void *init = dlopen(INIT, RTLD_NOW | RTLD_GLOBAL);
    printf("dlopen init = %p  (%s)\n", init, init ? "ok" : dlerror());

    /* PVRSRVConnect(PVRSRV_CONNECTION **ppsConnection, IMG_UINT32 flags) */
    typedef int (*connect_t)(void **, unsigned int);
    connect_t PVRSRVConnect = (connect_t)dlsym(um, "PVRSRVConnect");
    printf("sym PVRSRVConnect = %p\n", (void *)PVRSRVConnect);
    void *SrvInit = init ? dlsym(init, "SrvInit") : 0;
    printf("sym SrvInit       = %p\n", SrvInit);

    if (PVRSRVConnect) {
        void *conn = 0;
        printf(">> calling PVRSRVConnect...\n");
        fflush(stdout);
        int r = PVRSRVConnect(&conn, 0);
        printf("<< PVRSRVConnect returned %d, conn=%p\n", r, conn);
    }
    printf("done.\n");
    return 0;
}
