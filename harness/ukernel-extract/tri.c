// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* tri.c — draw one constant-colour triangle with EGL + GLES2 into a 64x64
 * pbuffer, then glFinish and exit. Run under the fake libdrm (fakedrm.so) and
 * the fake window system (libpvrFAKEWSEGL.so) so the closed Cedarview GLES2
 * stack submits one TA + 3D render that the shim captures (README "3D capture").
 *
 * BUILD (i686): gcc -m32 tri.c -o tri -ldl
 * The closed libEGL/libGLESv2 are loaded with dlopen from $SGX_UK_DIR/lib, so
 * the build needs no closed headers or libraries: the few EGL/GLES2 constants
 * and prototypes used are the Khronos API values, declared below. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "ukdir.h"

/* Khronos EGL 1.4 / GLES 2.0 API constants. */
#define EGL_DEFAULT_DISPLAY     ((void *)0)
#define EGL_NO_CONTEXT          ((void *)0)
#define EGL_NONE                0x3038
#define EGL_RED_SIZE            0x3024
#define EGL_GREEN_SIZE          0x3023
#define EGL_BLUE_SIZE           0x3022
#define EGL_SURFACE_TYPE        0x3033
#define EGL_RENDERABLE_TYPE     0x3040
#define EGL_PBUFFER_BIT         0x0001
#define EGL_OPENGL_ES2_BIT      0x0004
#define EGL_WIDTH               0x3057
#define EGL_HEIGHT              0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API       0x30A0

#define GL_TRIANGLES            0x0004
#define GL_FLOAT                0x1406
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_VERTEX_SHADER        0x8B31
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401

#define W 64
#define H 64

static const char *VS =
    "attribute vec4 pos;\n"
    "void main() { gl_Position = pos; }\n";
static const char *FS =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0); }\n";

static const float TRI[] = { -0.5f, -0.5f, 0.0f, 1.0f,
                              0.5f, -0.5f, 0.0f, 1.0f,
                              0.0f,  0.5f, 0.0f, 1.0f };

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *c = (ucontext_t *)uc;
    char line[512];
    fprintf(stderr, "\n*** SIG%d at pc=%#lx accessing addr=%p ***\n", sig,
            (unsigned long)c->uc_mcontext.gregs[REG_EIP], si->si_addr);
    FILE *m = fopen("/proc/self/maps", "r");
    while (m && fgets(line, sizeof line, m))
        if (strstr(line, " r-xp ") || strstr(line, " r--p 00000000"))
            fputs(line, stderr);
    _exit(139);
}

/* fakedrm.so exports fakedrm_mark(): phase markers in its bridge log. */
static void (*mark_fn)(const char *);

static void phase(const char *what)
{
    printf("[tri] %s\n", what);
    if (mark_fn) mark_fn(what);
}

static void *sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    if (!p) {
        fprintf(stderr, "missing symbol %s\n", name);
        exit(2);
    }
    return p;
}

int main(void)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    setvbuf(stdout, 0, _IONBF, 0);

    mark_fn = (void (*)(const char *))dlsym(RTLD_DEFAULT, "fakedrm_mark");
    char path[UK_PATH_MAX];
    void *egl = dlopen(uk_path(path, sizeof path, "lib/libEGL.so.1"), RTLD_NOW | RTLD_GLOBAL);
    void *gl = egl ? dlopen(uk_path(path, sizeof path, "lib/libGLESv2.so.2"),
                            RTLD_NOW | RTLD_GLOBAL) : 0;
    if (!egl || !gl) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    void *(*eglGetDisplay)(void *) = sym(egl, "eglGetDisplay");
    unsigned (*eglInitialize)(void *, int *, int *) = sym(egl, "eglInitialize");
    unsigned (*eglBindAPI)(unsigned) = sym(egl, "eglBindAPI");
    unsigned (*eglChooseConfig)(void *, const int *, void **, int, int *) =
        sym(egl, "eglChooseConfig");
    void *(*eglCreatePbufferSurface)(void *, void *, const int *) =
        sym(egl, "eglCreatePbufferSurface");
    void *(*eglCreateContext)(void *, void *, void *, const int *) =
        sym(egl, "eglCreateContext");
    unsigned (*eglMakeCurrent)(void *, void *, void *, void *) = sym(egl, "eglMakeCurrent");
    unsigned (*eglSwapBuffers)(void *, void *) = sym(egl, "eglSwapBuffers");
    unsigned (*eglTerminate)(void *) = sym(egl, "eglTerminate");
    int (*eglGetError)(void) = sym(egl, "eglGetError");

    unsigned (*glCreateShader)(unsigned) = sym(gl, "glCreateShader");
    void (*glShaderSource)(unsigned, int, const char **, const int *) = sym(gl, "glShaderSource");
    void (*glCompileShader)(unsigned) = sym(gl, "glCompileShader");
    void (*glGetShaderiv)(unsigned, unsigned, int *) = sym(gl, "glGetShaderiv");
    unsigned (*glCreateProgram)(void) = sym(gl, "glCreateProgram");
    void (*glAttachShader)(unsigned, unsigned) = sym(gl, "glAttachShader");
    void (*glBindAttribLocation)(unsigned, unsigned, const char *) =
        sym(gl, "glBindAttribLocation");
    void (*glLinkProgram)(unsigned) = sym(gl, "glLinkProgram");
    void (*glGetProgramiv)(unsigned, unsigned, int *) = sym(gl, "glGetProgramiv");
    void (*glUseProgram)(unsigned) = sym(gl, "glUseProgram");
    void (*glViewport)(int, int, int, int) = sym(gl, "glViewport");
    void (*glClearColor)(float, float, float, float) = sym(gl, "glClearColor");
    void (*glClear)(unsigned) = sym(gl, "glClear");
    void (*glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *) =
        sym(gl, "glVertexAttribPointer");
    void (*glEnableVertexAttribArray)(unsigned) = sym(gl, "glEnableVertexAttribArray");
    void (*glDrawArrays)(unsigned, int, int) = sym(gl, "glDrawArrays");
    void (*glFinish)(void) = sym(gl, "glFinish");
    void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *) =
        sym(gl, "glReadPixels");
    unsigned (*glGetError)(void) = sym(gl, "glGetError");

    phase("eglGetDisplay");
    void *dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    int major = 0, minor = 0;
    phase("eglInitialize");
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("[tri] eglInitialize failed: %#x\n", eglGetError());
        return 1;
    }
    printf("[tri] EGL %d.%d\n", major, minor);
    eglBindAPI(EGL_OPENGL_ES_API);

    static const int cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6,
        EGL_BLUE_SIZE, 5, EGL_NONE };
    void *cfg = 0;
    int ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        printf("[tri] eglChooseConfig failed: %#x n=%d\n", eglGetError(), ncfg);
        return 1;
    }
    static const int pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    phase("eglCreatePbufferSurface");
    void *surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (!surf) {
        printf("[tri] eglCreatePbufferSurface failed: %#x\n", eglGetError());
        return 1;
    }
    static const int ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    phase("eglCreateContext");
    void *ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!ctx) {
        printf("[tri] eglCreateContext failed: %#x\n", eglGetError());
        return 1;
    }
    phase("eglMakeCurrent");
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("[tri] eglMakeCurrent failed: %#x\n", eglGetError());
        return 1;
    }

    phase("compile and link");
    unsigned vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    int ok = 0;
    glShaderSource(vs, 1, &VS, 0);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    printf("[tri] vertex shader compiled=%d\n", ok);
    glShaderSource(fs, 1, &FS, 0);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    printf("[tri] fragment shader compiled=%d\n", ok);
    unsigned prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    printf("[tri] program linked=%d\n", ok);
    glUseProgram(prog);

    phase("glClear");
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, TRI);
    glEnableVertexAttribArray(0);
    phase("glDrawArrays");
    glDrawArrays(GL_TRIANGLES, 0, 3);
    phase("glFinish");
    glFinish();
    unsigned char px[4] = { 0 };
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("[tri] centre pixel %02x%02x%02x%02x glGetError=%#x\n",
           px[0], px[1], px[2], px[3], glGetError());
    phase("eglSwapBuffers");
    eglSwapBuffers(dpy, surf);
    phase("eglTerminate");
    eglMakeCurrent(dpy, 0, 0, EGL_NO_CONTEXT);
    eglTerminate(dpy);
    printf("[tri] done\n");
    return 0;
}
