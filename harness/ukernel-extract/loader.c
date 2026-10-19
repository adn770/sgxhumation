/* loader.c — drive the closed Cedarview usermode init: dlopen libsrv_um +
 * libsrv_init, call PVRSRVConnect (opens the drm device via our fake libdrm)
 * then SrvInit (runs DevInitSGX, which uploads the ukernel).
 *
 * BUILD (on cachix; 32-bit mandatory — see fakedrm.c header for the full flow):
 *   gcc -m32 loader.c -o loader -ldl
 *   LD_LIBRARY_PATH=~/sgxhumation-uk/lib LD_PRELOAD=./fakedrm.so ./loader   */
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

    void *conn = 0;
    if (PVRSRVConnect) {
        printf(">> calling PVRSRVConnect...\n");
        fflush(stdout);
        int r = PVRSRVConnect(&conn, 0);
        printf("<< PVRSRVConnect returned %d, conn=%p\n", r, conn);
    }

    /* SrvInit = the services initialisation entry; runs SGX DevInit which
     * uploads the ukernel -> our PVRSRVMemCopy interposer should catch it. */
    if (SrvInit) {
        typedef int (*srvinit_t)(void);
        printf(">> calling SrvInit...\n");
        fflush(stdout);
        int r = ((srvinit_t)SrvInit)();
        printf("<< SrvInit returned %d\n", r);
    }
    printf("done.\n");
    return 0;
}
