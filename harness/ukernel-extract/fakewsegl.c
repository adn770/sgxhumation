// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
/* fakewsegl.c — a window-system module for the closed Cedarview libIMGegl that
 * needs no display: libpvrFAKEWSEGL.so. libIMGegl loads the module named by the
 * "WindowSystem" app hint (default libpvrPVR2D_DRIWSEGL.so; fakedrm.c redirects
 * /etc/powervr.ini to $SGX_UK_DIR/powervr.ini, which names this one) and calls
 * its exported WSEGL_GetFunctionTablePointer().
 *
 * The interface is the DDK's WSEGL interface, version 2, as the closed
 * libpvrPVR2D_*WSEGL modules export it: a version word followed by 15 entry
 * points. Our module offers one display with one RGB565 configuration and no
 * window or pixmap drawables, so EGL works only with pbuffers, which libIMGegl
 * allocates itself. Every call is logged to stderr.
 *
 * BUILD (i686): gcc -m32 -shared -fPIC fakewsegl.c -o libpvrFAKEWSEGL.so */
#include <stdio.h>

#define WSEGL_INTERFACE_VERSION 2

enum ws_error { WS_SUCCESS = 0, WS_CANNOT_INITIALISE = 1, WS_BAD_NATIVE_WINDOW = 3,
                WS_BAD_NATIVE_PIXMAP = 4, WS_BAD_DRAWABLE = 6 };
enum ws_cap { WS_NO_CAPS = 0, WS_CAP_MIN_SWAP_INTERVAL = 1, WS_CAP_MAX_SWAP_INTERVAL = 2 };
enum ws_format { WS_RGB565 = 0, WS_ARGB8888 = 2 };
#define WS_NO_DRAWABLE     0x0
#define WS_DRAWABLE_WINDOW 0x1

struct ws_caps { unsigned long type, value; };

/* A display configuration: drawable types (bit mask), pixel format, native
 * renderable, frame-buffer level, native visual id, native visual, transparency
 * type, transparent colour. An entry with drawable type 0 ends the list. */
struct ws_config {
    unsigned long drawable_type, pixel_format, native_renderable, fb_level;
    unsigned long visual_id;
    void *visual;
    unsigned long transparent_type, transparent_colour;
};

static const struct ws_caps CAPS[] = {
    { WS_CAP_MIN_SWAP_INTERVAL, 1 }, { WS_CAP_MAX_SWAP_INTERVAL, 1 }, { WS_NO_CAPS, 0 },
};
static struct ws_config CONFIGS[] = {
    { WS_DRAWABLE_WINDOW, WS_RGB565, 0, 0, 0, 0, 0, 0 },
    { WS_NO_DRAWABLE, 0, 0, 0, 0, 0, 0, 0 },
};
static int g_display = 0x57534547;     /* the display handle we hand out */

#define LOG(...) fprintf(stderr, "[wsegl] " __VA_ARGS__)

static int is_display_valid(void *native)
{
    LOG("is_display_valid(%p)\n", native);
    return WS_SUCCESS;
}

static int initialise_display(void *native, void **display, const struct ws_caps **caps,
                              struct ws_config **configs)
{
    LOG("InitialiseDisplay(%p)\n", native);
    *display = &g_display;
    *caps = CAPS;
    *configs = CONFIGS;
    return WS_SUCCESS;
}

static int close_display(void *display)
{
    LOG("CloseDisplay(%p)\n", display);
    return WS_SUCCESS;
}

static int create_window_drawable(void *d, void *c, void **dr, void *w, void *rot, void *conn)
{
    LOG("CreateWindowDrawable(window=%p) -> unsupported\n", w);
    (void)d; (void)c; (void)dr; (void)rot; (void)conn;
    return WS_BAD_NATIVE_WINDOW;
}

static int create_pixmap_drawable(void *d, void *c, void **dr, void *p, void *rot)
{
    LOG("CreatePixmapDrawable(pixmap=%p) -> unsupported\n", p);
    (void)d; (void)c; (void)dr; (void)rot;
    return WS_BAD_NATIVE_PIXMAP;
}

static int delete_drawable(void *dr)            { LOG("DeleteDrawable(%p)\n", dr); return 0; }
static int swap_drawable(void *dr, unsigned long f) { LOG("SwapDrawable(%p,%lu)\n", dr, f); return 0; }
static int swap_interval(void *dr, unsigned long i) { LOG("SwapControlInterval(%p,%lu)\n", dr, i); return 0; }
static int wait_native(void *dr, unsigned long e) { LOG("WaitNative(%p,%lu)\n", dr, e); return 0; }
static int copy_from_drawable(void *dr, void *p) { LOG("CopyFromDrawable(%p,%p)\n", dr, p); return 0; }

static int copy_from_pbuffer(void *addr, unsigned long w, unsigned long h, unsigned long stride,
                             unsigned long fmt, void *pixmap)
{
    LOG("CopyFromPBuffer(%p %lux%lu stride=%lu fmt=%lu pixmap=%p)\n", addr, w, h, stride,
        fmt, pixmap);
    return WS_SUCCESS;
}

static int get_drawable_parameters(void *dr, void *src, void *render)
{
    LOG("GetDrawableParameters(%p) -> bad drawable\n", dr);
    (void)src; (void)render;
    return WS_BAD_DRAWABLE;
}

static int connect_drawable(void *dr)    { LOG("ConnectDrawable(%p)\n", dr); return 0; }
static int disconnect_drawable(void *dr) { LOG("DisconnectDrawable(%p)\n", dr); return 0; }

static const struct {
    unsigned long version;
    void *fn[15];
} TABLE = { WSEGL_INTERFACE_VERSION, {
    (void *)is_display_valid, (void *)initialise_display, (void *)close_display,
    (void *)create_window_drawable, (void *)create_pixmap_drawable, (void *)delete_drawable,
    (void *)swap_drawable, (void *)swap_interval, (void *)wait_native,
    (void *)copy_from_drawable, (void *)copy_from_pbuffer, (void *)get_drawable_parameters,
    (void *)connect_drawable, (void *)disconnect_drawable, 0 } };

__attribute__((visibility("default"))) const void *WSEGL_GetFunctionTablePointer(void)
{
    LOG("GetFunctionTablePointer -> version %lu\n", TABLE.version);
    return &TABLE;
}
