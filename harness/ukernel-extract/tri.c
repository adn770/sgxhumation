// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* tri.c — draw one constant-colour triangle with EGL + GLES2 into a 64x64
 * pbuffer, then glFinish and exit. Run under the fake libdrm (fakedrm.so) and
 * the fake window system (libpvrFAKEWSEGL.so) so the closed Cedarview GLES2
 * stack submits one TA + 3D render that the shim captures (README "3D capture").
 *
 * key=value arguments (triopts.h) change one piece of state at a time for
 * differential captures: size, pixel format, colours, depth, stencil, blending,
 * culling, colour mask, scissor, viewport, primitive type, vertex positions and
 * a varying colour. Without arguments it draws the original frame.
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

#include "triopts.h"
#include "ukdir.h"

/* Khronos EGL 1.4 / GLES 2.0 API constants. */
#define EGL_DEFAULT_DISPLAY     ((void *)0)
#define EGL_NO_CONTEXT          ((void *)0)
#define EGL_NONE                0x3038
#define EGL_RED_SIZE            0x3024
#define EGL_GREEN_SIZE          0x3023
#define EGL_BLUE_SIZE           0x3022
#define EGL_ALPHA_SIZE          0x3021
#define EGL_DEPTH_SIZE          0x3025
#define EGL_STENCIL_SIZE        0x3026
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
#define GL_DEPTH_BUFFER_BIT     0x0100
#define GL_STENCIL_BUFFER_BIT   0x0400
#define GL_CULL_FACE            0x0B44
#define GL_DEPTH_TEST           0x0B71
#define GL_STENCIL_TEST         0x0B90
#define GL_DITHER               0x0BD0
#define GL_BLEND                0x0BE2
#define GL_SCISSOR_TEST         0x0C11
#define GL_KEEP                 0x1E00
#define GL_REPLACE              0x1E01

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

int main(int argc, char **argv)
{
    struct tri_opts o;
    char err[128];
    if (tri_parse(&o, argc - 1, argv + 1, err, sizeof err)) {
        fprintf(stderr, "tri: %s\n", err);
        return 2;
    }
    tri_dump(stdout, &o);
    if (o.format == 8888) setenv("FAKEWSEGL_FORMAT", "8888", 1);   /* fakewsegl.c */
    const int W = o.w, H = o.h;
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
    void (*glFlush)(void) = sym(gl, "glFlush");
    void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *) =
        sym(gl, "glReadPixels");
    unsigned (*glGetError)(void) = sym(gl, "glGetError");
    void (*glEnable)(unsigned) = sym(gl, "glEnable");
    void (*glDisable)(unsigned) = sym(gl, "glDisable");
    void (*glDepthFunc)(unsigned) = sym(gl, "glDepthFunc");
    void (*glDepthMask)(unsigned char) = sym(gl, "glDepthMask");
    void (*glClearDepthf)(float) = sym(gl, "glClearDepthf");
    void (*glStencilFunc)(unsigned, int, unsigned) = sym(gl, "glStencilFunc");
    void (*glStencilOp)(unsigned, unsigned, unsigned) = sym(gl, "glStencilOp");
    void (*glStencilMask)(unsigned) = sym(gl, "glStencilMask");
    void (*glDepthRangef)(float, float) = sym(gl, "glDepthRangef");
    void (*glBlendFunc)(unsigned, unsigned) = sym(gl, "glBlendFunc");
    void (*glBlendEquation)(unsigned) = sym(gl, "glBlendEquation");
    void (*glCullFace)(unsigned) = sym(gl, "glCullFace");
    void (*glFrontFace)(unsigned) = sym(gl, "glFrontFace");
    void (*glColorMask)(unsigned char, unsigned char, unsigned char, unsigned char) =
        sym(gl, "glColorMask");
    void (*glScissor)(int, int, int, int) = sym(gl, "glScissor");

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

    const int c8 = o.format == 8888;
    const int cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, c8 ? 8 : 5,
        EGL_GREEN_SIZE, c8 ? 8 : 6, EGL_BLUE_SIZE, c8 ? 8 : 5, EGL_ALPHA_SIZE, c8 ? 8 : 0,
        EGL_DEPTH_SIZE, o.depth_func ? 16 : 0, EGL_STENCIL_SIZE, o.stencil_func ? 8 : 0,
        EGL_NONE };
    void *cfg = 0;
    int ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        printf("[tri] eglChooseConfig failed: %#x n=%d\n", eglGetError(), ncfg);
        return 1;
    }
    const int pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
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
    const char *VS = tri_vertex_shader(&o), *FS = tri_fragment_shader(&o);
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
    if (o.varying) glBindAttribLocation(prog, 1, "colour");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    printf("[tri] program linked=%d\n", ok);
    glUseProgram(prog);

    phase("glClear");
    glViewport(o.viewport[0], o.viewport[1], o.viewport[2], o.viewport[3]);
    if (!o.dither) glDisable(GL_DITHER);
    if (o.depth_range[0] != 0.0f || o.depth_range[1] != 1.0f)
        glDepthRangef(o.depth_range[0], o.depth_range[1]);
    unsigned clear_bits = GL_COLOR_BUFFER_BIT;
    if (o.depth_func) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(o.depth_func);
        glDepthMask(o.depth_write);
        glClearDepthf(o.clear_depth);
        clear_bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (o.stencil_func) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(o.stencil_func, o.stencil_ref, o.stencil_mask);
        glStencilMask(o.stencil_write);
        glStencilOp(o.stencil_ops[0], o.stencil_ops[1], o.stencil_ops[2]);
        clear_bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (o.do_clear)
        glClearColor(o.clear[0], o.clear[1], o.clear[2], o.clear[3]);
    if (o.blend_eq) {
        glEnable(GL_BLEND);
        glBlendFunc(o.blend_src, o.blend_dst);
        glBlendEquation(o.blend_eq);
    }
    if (o.cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(o.cull);
    }
    if (o.front_face != 0x0901) glFrontFace(o.front_face);
    if (!(o.mask[0] && o.mask[1] && o.mask[2] && o.mask[3]))
        glColorMask(o.mask[0], o.mask[1], o.mask[2], o.mask[3]);
    if (o.do_scissor) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(o.scissor[0], o.scissor[1], o.scissor[2], o.scissor[3]);
    }
    /* Positions and colours in two packed arrays, as the original frame. */
    static float v[TRI_MAX_VERTS * TRI_VERT_FLOATS], pos[TRI_MAX_VERTS * 4],
        col[TRI_MAX_VERTS * 4];
    int nv = tri_geometry(&o, v);
    for (int i = 0; i < nv; i++)
        for (int c = 0; c < 4; c++) {
            pos[4 * i + c] = v[i * TRI_VERT_FLOATS + c];
            col[4 * i + c] = v[i * TRI_VERT_FLOATS + 4 + c];
        }
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, pos);
    glEnableVertexAttribArray(0);
    if (o.varying) {
        glVertexAttribPointer(1, 4, GL_FLOAT, 0, 0, col);
        glEnableVertexAttribArray(1);
    }
    for (int f = 0; f < o.frames; f++) {
        if (o.do_clear && (!o.clear_once || f == 0)) {
            phase("glClear");
            glClear(clear_bits);
        }
        phase("glDrawArrays");
        for (int k = 0; k < o.draws; k++) {
            /* the client array is read at the draw call: move it for each draw */
            float dx, dy;
            tri_draw_shift(&o, k, &dx, &dy);
            for (int i = 0; i < nv; i++) {
                pos[4 * i] = v[i * TRI_VERT_FLOATS] + dx;
                pos[4 * i + 1] = v[i * TRI_VERT_FLOATS + 1] + dy;
            }
            glDrawArrays(o.prim, 0, nv);
            if (o.flush && k + 1 < o.draws)
                glFlush();
        }
        phase("glFinish");
        glFinish();
        unsigned char px[4] = { 0 };
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("[tri] frame %d centre pixel %02x%02x%02x%02x glGetError=%#x\n", f,
               px[0], px[1], px[2], px[3], glGetError());
        phase("eglSwapBuffers");
        eglSwapBuffers(dpy, surf);
    }
    phase("eglTerminate");
    eglMakeCurrent(dpy, 0, 0, EGL_NO_CONTEXT);
    eglTerminate(dpy);
    printf("[tri] done\n");
    return 0;
}
