// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* triopts.h — the options of tri.c, for differential 3D captures.
 *
 * Every option changes one piece of GL state against the default frame (a
 * constant-colour triangle on a blue clear, 64x64 RGB565), so the capture it
 * produces can be diffed against the default one (diff3d.py). Options are
 * key=value words on the command line:
 *
 *   size=WxH            surface size (the viewport follows unless given)
 *   format=565|8888     WSEGL pixel format (RGB565, ARGB8888)
 *   color=r,g,b,a       the fragment shader's constant colour
 *   clear=r,g,b,a       glClearColor; "noclear" skips glClear
 *   depth=FUNC          depth test on with FUNC (never less equal lequal greater
 *                       notequal gequal always); cleardepth=F, depthmask=0, z=F
 *   depthrange=N,F      glDepthRangef
 *   stencil=FUNC,REF    stencil test on; stencilmask=CMP,WRITE (hex),
 *                       stencilop=SFAIL,ZFAIL,ZPASS (keep zero replace incr decr
 *                       invert incrwrap decrwrap; default keep,keep,replace)
 *   blend=SRC,DST       blending on (zero one srccolor oneminussrccolor srcalpha
 *                       oneminussrcalpha dstalpha oneminusdstalpha dstcolor
 *                       oneminusdstcolor); blendeq=add|sub|rsub
 *   cull=front|back|both, frontface=cw|ccw
 *   mask=RGBA           colour mask as four 0/1 digits
 *   scissor=x,y,w,h     scissor test on
 *   viewport=x,y,w,h
 *   dither=0            dithering off (GL default is on)
 *   prim=tri|two|strip|fan|points|lines|linestrip
 *   scale=S, shift=dx,dy  transform the vertex positions
 *   varying             a per-vertex colour attribute, interpolated
 *
 * The values are the Khronos GLES 2.0 API constants. */
#ifndef TRIOPTS_H
#define TRIOPTS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRI_MAX_VERTS   8
#define TRI_VERT_FLOATS 8       /* position xyzw, colour rgba */

struct tri_opts {
    int w, h;
    int format;                 /* 565 or 8888 */
    float color[4], clear[4];
    int do_clear;
    unsigned depth_func;        /* 0 = depth test off */
    float clear_depth, z;
    int depth_write;
    unsigned stencil_func;      /* 0 = stencil test off */
    int stencil_ref;
    unsigned stencil_mask, stencil_write, stencil_ops[3];   /* sfail, zfail, zpass */
    float depth_range[2];
    unsigned blend_src, blend_dst, blend_eq;  /* blend_eq 0 = blending off */
    unsigned cull;              /* 0 = culling off */
    unsigned front_face;
    int mask[4];
    int scissor[4], do_scissor;
    int viewport[4], have_viewport;
    int dither;
    unsigned prim;
    int prim_kind;              /* index into TRI_PRIMS */
    float scale, shift[2];
    int varying;
};

struct tri_name { const char *name; unsigned value; };

static const struct tri_name TRI_FUNCS[] = {
    { "never", 0x0200 }, { "less", 0x0201 }, { "equal", 0x0202 }, { "lequal", 0x0203 },
    { "greater", 0x0204 }, { "notequal", 0x0205 }, { "gequal", 0x0206 },
    { "always", 0x0207 }, { 0, 0 },
};
static const struct tri_name TRI_FACTORS[] = {
    { "zero", 0 }, { "one", 1 }, { "srccolor", 0x0300 }, { "oneminussrccolor", 0x0301 },
    { "srcalpha", 0x0302 }, { "oneminussrcalpha", 0x0303 }, { "dstalpha", 0x0304 },
    { "oneminusdstalpha", 0x0305 }, { "dstcolor", 0x0306 }, { "oneminusdstcolor", 0x0307 },
    { 0, 0 },
};
static const struct tri_name TRI_STENCIL_OPS[] = {
    { "keep", 0x1E00 }, { "zero", 0 }, { "replace", 0x1E01 }, { "incr", 0x1E02 },
    { "decr", 0x1E03 }, { "invert", 0x150A }, { "incrwrap", 0x8507 }, { "decrwrap", 0x8508 },
    { 0, 0 },
};
static const struct tri_name TRI_EQS[] = {
    { "add", 0x8006 }, { "sub", 0x800A }, { "rsub", 0x800B }, { 0, 0 },
};
static const struct tri_name TRI_FACES[] = {
    { "front", 0x0404 }, { "back", 0x0405 }, { "both", 0x0408 }, { 0, 0 },
};
static const struct tri_name TRI_WINDING[] = { { "cw", 0x0900 }, { "ccw", 0x0901 }, { 0, 0 } };

/* name, GL primitive, vertex count; the vertices come from TRI_SHAPE[kind]. */
static const struct { const char *name; unsigned prim; int n; } TRI_PRIMS[] = {
    { "tri", 0x0004, 3 }, { "two", 0x0004, 6 }, { "strip", 0x0005, 4 },
    { "fan", 0x0006, 4 }, { "points", 0x0000, 3 }, { "lines", 0x0001, 4 },
    { "linestrip", 0x0003, 3 },
};
#define TRI_NPRIMS ((int)(sizeof TRI_PRIMS / sizeof TRI_PRIMS[0]))

/* x, y of each vertex (counter-clockwise triangles). */
static const float TRI_SHAPE[][TRI_MAX_VERTS][2] = {
    { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } },
    { { -0.5f, -0.5f }, { 0.0f, -0.5f }, { -0.25f, 0.0f },
      { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 0.25f, 0.5f } },
    { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } },
    { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } },
    { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } },
    { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } },
    { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } },
};
static const float TRI_COLOURS[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };

static int tri_lookup(const struct tri_name *t, const char *s, unsigned *out)
{
    for (; t->name; t++)
        if (!strcmp(t->name, s)) { *out = t->value; return 0; }
    return -1;
}

static int tri_floats(const char *s, float *out, int n)
{
    char *end;
    for (int i = 0; i < n; i++) {
        out[i] = strtof(s, &end);
        if (end == s) return -1;
        s = end;
        if (i + 1 < n) {
            if (*s != ',') return -1;
            s++;
        }
    }
    return *s ? -1 : 0;
}

static int tri_ints(const char *s, int *out, int n)
{
    float f[4];
    if (n > 4 || tri_floats(s, f, n)) return -1;
    for (int i = 0; i < n; i++) out[i] = (int)f[i];
    return 0;
}

static void tri_defaults(struct tri_opts *o)
{
    static const struct tri_opts d = {
        .w = 64, .h = 64, .format = 565, .color = { 1.0f, 0.5f, 0.25f, 1.0f },
        .clear = { 0.0f, 0.0f, 1.0f, 1.0f }, .do_clear = 1, .clear_depth = 1.0f,
        .depth_write = 1, .front_face = 0x0901, .mask = { 1, 1, 1, 1 }, .dither = 1,
        .prim = 0x0004, .scale = 1.0f, .stencil_mask = 0xff, .stencil_write = 0xff,
        .stencil_ops = { 0x1E00, 0x1E00, 0x1E01 }, .depth_range = { 0.0f, 1.0f },
    };
    *o = d;
}

/* Parse key=value words; 0 on success, else -1 with a message in err. */
static int tri_parse(struct tri_opts *o, int argc, char **argv, char *err, size_t errn)
{
    tri_defaults(o);
    for (int i = 0; i < argc; i++) {
        char key[32];
        const char *a = argv[i], *eq = strchr(a, '=');
        const char *v = eq ? eq + 1 : "";
        size_t kn = eq ? (size_t)(eq - a) : strlen(a);
        if (kn >= sizeof key) kn = sizeof key - 1;
        memcpy(key, a, kn);
        key[kn] = 0;
        int bad = 0;
        if (!strcmp(key, "size")) {
            bad = sscanf(v, "%dx%d", &o->w, &o->h) != 2 || o->w <= 0 || o->h <= 0;
        } else if (!strcmp(key, "format")) {
            o->format = atoi(v);
            bad = o->format != 565 && o->format != 8888;
        } else if (!strcmp(key, "color")) {
            bad = tri_floats(v, o->color, 4);
        } else if (!strcmp(key, "clear")) {
            bad = tri_floats(v, o->clear, 4);
        } else if (!strcmp(key, "noclear")) {
            o->do_clear = 0;
        } else if (!strcmp(key, "depth")) {
            bad = tri_lookup(TRI_FUNCS, v, &o->depth_func);
        } else if (!strcmp(key, "cleardepth")) {
            bad = tri_floats(v, &o->clear_depth, 1);
        } else if (!strcmp(key, "depthmask")) {
            o->depth_write = atoi(v) != 0;
        } else if (!strcmp(key, "z")) {
            bad = tri_floats(v, &o->z, 1);
        } else if (!strcmp(key, "stencil")) {
            char f[16] = "";
            bad = sscanf(v, "%15[a-z],%d", f, &o->stencil_ref) != 2 ||
                  tri_lookup(TRI_FUNCS, f, &o->stencil_func);
        } else if (!strcmp(key, "stencilmask")) {
            bad = sscanf(v, "%x,%x", &o->stencil_mask, &o->stencil_write) != 2;
        } else if (!strcmp(key, "stencilop")) {
            char f[3][16] = { "", "", "" };
            bad = sscanf(v, "%15[a-z],%15[a-z],%15[a-z]", f[0], f[1], f[2]) != 3;
            for (int k = 0; !bad && k < 3; k++)
                bad = tri_lookup(TRI_STENCIL_OPS, f[k], &o->stencil_ops[k]);
        } else if (!strcmp(key, "depthrange")) {
            bad = tri_floats(v, o->depth_range, 2);
        } else if (!strcmp(key, "blend")) {
            char s[24] = "", d[24] = "";
            bad = sscanf(v, "%23[a-z],%23[a-z]", s, d) != 2 ||
                  tri_lookup(TRI_FACTORS, s, &o->blend_src) ||
                  tri_lookup(TRI_FACTORS, d, &o->blend_dst);
            if (!o->blend_eq) o->blend_eq = 0x8006;
        } else if (!strcmp(key, "blendeq")) {
            bad = tri_lookup(TRI_EQS, v, &o->blend_eq);
        } else if (!strcmp(key, "cull")) {
            bad = tri_lookup(TRI_FACES, v, &o->cull);
        } else if (!strcmp(key, "frontface")) {
            bad = tri_lookup(TRI_WINDING, v, &o->front_face);
        } else if (!strcmp(key, "mask")) {
            bad = strlen(v) != 4 || strspn(v, "01") != 4;
            for (int k = 0; !bad && k < 4; k++) o->mask[k] = v[k] == '1';
        } else if (!strcmp(key, "scissor")) {
            bad = tri_ints(v, o->scissor, 4);
            o->do_scissor = 1;
        } else if (!strcmp(key, "viewport")) {
            bad = tri_ints(v, o->viewport, 4);
            o->have_viewport = 1;
        } else if (!strcmp(key, "dither")) {
            o->dither = atoi(v) != 0;
        } else if (!strcmp(key, "prim")) {
            bad = 1;
            for (int k = 0; k < TRI_NPRIMS; k++)
                if (!strcmp(TRI_PRIMS[k].name, v)) {
                    o->prim_kind = k;
                    o->prim = TRI_PRIMS[k].prim;
                    bad = 0;
                }
        } else if (!strcmp(key, "scale")) {
            bad = tri_floats(v, &o->scale, 1);
        } else if (!strcmp(key, "shift")) {
            bad = tri_floats(v, o->shift, 2);
        } else if (!strcmp(key, "varying")) {
            o->varying = 1;
        } else {
            snprintf(err, errn, "unknown option %s", a);
            return -1;
        }
        if (bad) {
            snprintf(err, errn, "bad %s: %s", key, v);
            return -1;
        }
    }
    if (!o->have_viewport) {
        o->viewport[0] = o->viewport[1] = 0;
        o->viewport[2] = o->w;
        o->viewport[3] = o->h;
    }
    return 0;
}

/* Fill v (TRI_MAX_VERTS x TRI_VERT_FLOATS) and return the vertex count. */
static int tri_geometry(const struct tri_opts *o, float *v)
{
    int n = TRI_PRIMS[o->prim_kind].n;
    for (int i = 0; i < n; i++) {
        float *p = v + i * TRI_VERT_FLOATS;
        p[0] = TRI_SHAPE[o->prim_kind][i][0] * o->scale + o->shift[0];
        p[1] = TRI_SHAPE[o->prim_kind][i][1] * o->scale + o->shift[1];
        p[2] = o->z;
        p[3] = 1.0f;
        for (int c = 0; c < 4; c++) p[4 + c] = o->varying ? TRI_COLOURS[i % 3][c] : 0.0f;
    }
    return n;
}

/* The default is the original frame's shader; points also set a point size. */
static const char *tri_vertex_shader(const struct tri_opts *o)
{
    if (o->varying)
        return "attribute vec4 pos;\nattribute vec4 colour;\nvarying vec4 col;\n"
               "void main() { gl_Position = pos; col = colour; }\n";
    if (o->prim == 0x0000)
        return "attribute vec4 pos;\nvoid main() { gl_Position = pos; gl_PointSize = 4.0; }\n";
    return "attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n";
}

static const char *tri_fragment_shader(const struct tri_opts *o)
{
    static char fs[256];
    if (o->varying)
        return "precision mediump float;\nvarying vec4 col;\n"
               "void main() { gl_FragColor = col; }\n";
    snprintf(fs, sizeof fs, "precision mediump float;\nvoid main() { gl_FragColor = "
             "vec4(%.4f, %.4f, %.4f, %.4f); }\n",
             o->color[0], o->color[1], o->color[2], o->color[3]);
    return fs;
}

static void tri_dump(FILE *f, const struct tri_opts *o)
{
    fprintf(f, "size %dx%d\nformat %d\n", o->w, o->h, o->format);
    fprintf(f, "color %g %g %g %g\n", o->color[0], o->color[1], o->color[2], o->color[3]);
    if (o->do_clear)
        fprintf(f, "clear %g %g %g %g\n", o->clear[0], o->clear[1], o->clear[2], o->clear[3]);
    else
        fprintf(f, "clear none\n");
    if (o->depth_func)
        fprintf(f, "depth %#x clear=%g write=%d\n", o->depth_func, o->clear_depth,
                o->depth_write);
    else
        fprintf(f, "depth off\n");
    if (o->stencil_func)
        fprintf(f, "stencil %#x ref=%d mask=%#x write=%#x ops=%#x,%#x,%#x\n", o->stencil_func,
                o->stencil_ref, o->stencil_mask, o->stencil_write, o->stencil_ops[0],
                o->stencil_ops[1], o->stencil_ops[2]);
    else
        fprintf(f, "stencil off\n");
    fprintf(f, "stencilops %#x %#x %#x\n", o->stencil_ops[0], o->stencil_ops[1],
            o->stencil_ops[2]);
    fprintf(f, "depthrange %g %g\n", o->depth_range[0], o->depth_range[1]);
    if (o->blend_eq)
        fprintf(f, "blend %#x %#x eq=%#x\n", o->blend_src, o->blend_dst, o->blend_eq);
    else
        fprintf(f, "blend off\n");
    if (o->cull)
        fprintf(f, "cull %#x front=%#x\n", o->cull, o->front_face);
    else
        fprintf(f, "cull off\n");
    fprintf(f, "mask %d%d%d%d\n", o->mask[0], o->mask[1], o->mask[2], o->mask[3]);
    if (o->do_scissor)
        fprintf(f, "scissor %d %d %d %d\n", o->scissor[0], o->scissor[1], o->scissor[2],
                o->scissor[3]);
    else
        fprintf(f, "scissor off\n");
    fprintf(f, "viewport %d %d %d %d\n", o->viewport[0], o->viewport[1], o->viewport[2],
            o->viewport[3]);
    fprintf(f, "dither %d\nprim 0x%x\nvarying %d\n", o->dither, o->prim, o->varying);
}

#endif
