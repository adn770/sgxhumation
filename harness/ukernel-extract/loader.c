/* loader.c — drive the closed Cedarview usermode init: dlopen libsrv_um +
 * libsrv_init, call PVRSRVConnect (opens the drm device via our fake libdrm)
 * then SrvInit (runs DevInitSGX, which uploads the ukernel).
 *
 * A SIGSEGV handler prints the faulting PC + accessed address and the library
 * load bases, so a crash maps straight to a libsrv_um/libsrv_init file offset:
 *   offset = PC - base  ->  r2 -a x86 -b 32 libsrv_um.so.1.7.862890
 *                           's 0x<offset>; pd 20'  to see what it dereferenced.
 *
 * BUILD (on cachix; 32-bit mandatory — see fakedrm.c header for the full flow):
 *   gcc -m32 loader.c -o loader -ldl
 *   LD_LIBRARY_PATH=~/sgxhumation-uk/lib LD_PRELOAD=./fakedrm.so ./loader   */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>

#define UM   "/home/user/sgxhumation-uk/lib/libsrv_um.so.1.7.862890"
#define INIT "/home/user/sgxhumation-uk/lib/libsrv_init.so.1.7.862890"

/* Dump the maps of the two closed libs so PC can be converted to a file
 * offset. Called on crash and once at startup. */
static void dump_lib_maps(void) {
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return;
    char line[512];
    fprintf(stderr, "--- lib load bases ---\n");
    while (fgets(line, sizeof line, m))
        if (strstr(line, "libsrv_um") || strstr(line, "libsrv_init"))
            fputs(line, stderr);
    fprintf(stderr, "----------------------\n");
    fclose(m);
}

static void on_fault(int sig, siginfo_t *si, void *uc) {
    ucontext_t *c = (ucontext_t *)uc;
    void *pc = (void *)c->uc_mcontext.gregs[REG_EIP];
    fprintf(stderr, "\n*** SIG%d at pc=%p accessing addr=%p ***\n",
            sig, pc, si->si_addr);
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
    dump_lib_maps();
    _exit(139);
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);

    void *um = dlopen(UM, RTLD_NOW | RTLD_GLOBAL);
    printf("dlopen um   = %p  (%s)\n", um, um ? "ok" : dlerror());
    if (!um) return 1;
    void *init = dlopen(INIT, RTLD_NOW | RTLD_GLOBAL);
    printf("dlopen init = %p  (%s)\n", init, init ? "ok" : dlerror());
    dump_lib_maps();

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
