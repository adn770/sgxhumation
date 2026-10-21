/* ukdir.h — the harness staging directory, shared by fakedrm.c and loader.c.
 *
 * $SGX_UK_DIR if set and non-empty, else $HOME/sgxhumation-uk (./sgxhumation-uk
 * without HOME). It holds lib/ (the closed libsrv_um + libsrv_init), and the
 * harness writes drm.log and dump/ there. */
#ifndef UKDIR_H
#define UKDIR_H

#include <stdio.h>
#include <stdlib.h>

#define UK_PATH_MAX 512

/* Write "<staging dir>/<rel>" into buf (n bytes, truncated if longer); return buf. */
static inline char *uk_path(char *buf, size_t n, const char *rel)
{
    const char *d = getenv("SGX_UK_DIR");
    if (d && *d) {
        snprintf(buf, n, "%s/%s", d, rel);
    } else {
        const char *h = getenv("HOME");
        snprintf(buf, n, "%s/sgxhumation-uk/%s", h && *h ? h : ".", rel);
    }
    return buf;
}

#endif
