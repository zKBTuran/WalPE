/* webhost_wl_test.c - the web module's private Wayland display
 * (src/engine/webhost_wl.c, compiled into the test) against a plain
 * libwayland client that does what GTK's Wayland backend does, without
 * GTK or WebKit:
 *   - the socket is in a directory of its own, is gone once the client is
 *     connected, and nobody connects a second time;
 *   - the globals GTK needs are there, a primary-selection or
 *     data-control global is not;
 *   - a window can be mapped: configure, buffer release, wl_surface.enter,
 *     frame callbacks that keep coming; unmapped and mapped again (what a
 *     paused page does);
 *   - pointer enter / motion / buttons / wheel / leave and keyboard
 *     keymap / focus / key / modifiers arrive as sent, on the right
 *     surface, and stop when the surface is gone;
 *   - a selection or a drag offered by the client is cancelled at once.
 * No compositor, no display, no GPU. */
#include "engine/webhost_wl.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-test-client-protocol.h"   /* the client side, generated for this test */

static int fails;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fails++;                                                          \
            fprintf (stderr, "FAIL %s:%d: (%s) ", __FILE__, __LINE__, #cond); \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

/* ---- what the client saw */
static struct {
    struct wl_display *dpy;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_output *output;
    struct wl_seat *seat;
    struct wl_data_device_manager *ddm;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    guint32 v_compositor, v_wm_base, v_output, v_seat, v_ddm;
    gboolean forbidden_global;      /* primary selection / data control / anything of that kind */
    char forbidden_name[80];
    /* output */
    int refresh, scale;
    int out_w, out_h, out_modes;
    gboolean output_done;
    /* seat */
    guint32 caps;
    /* window */
    int configures, top_configures;
    guint32 last_serial;
    gboolean activated;
    int released, entered_output, frames;
    /* pointer */
    int p_enter, p_leave, p_motion, p_button, p_axis, p_discrete, p_frame;
    struct wl_surface *p_surface;
    double p_x, p_y;
    guint32 p_btn, p_btn_state;
    guint32 p_axis_which;
    double p_axis_value;
    int p_discrete_value;
    /* keyboard */
    int k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat;
    guint32 k_keymap_size;
    char k_keymap_head[16];
    struct wl_surface *k_surface;
    guint32 k_code, k_state, k_dep, k_group;
    /* data */
    int cancelled;
} C;

/* ---- listeners */

static void reg_global (void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void) d;
    if (!strcmp (iface, "wl_compositor")) {
        C.v_compositor = v;
        C.compositor = wl_registry_bind (r, name, &wl_compositor_interface, MIN (v, 4u));
    } else if (!strcmp (iface, "wl_shm")) {
        C.shm = wl_registry_bind (r, name, &wl_shm_interface, 1);
    } else if (!strcmp (iface, "xdg_wm_base")) {
        C.v_wm_base = v;
        C.wm_base = wl_registry_bind (r, name, &xdg_wm_base_interface, MIN (v, 2u));
    } else if (!strcmp (iface, "wl_output")) {
        C.v_output = v;
        C.output = wl_registry_bind (r, name, &wl_output_interface, MIN (v, 2u));
    } else if (!strcmp (iface, "wl_seat")) {
        C.v_seat = v;
        C.seat = wl_registry_bind (r, name, &wl_seat_interface, MIN (v, 5u));
    } else if (!strcmp (iface, "wl_data_device_manager")) {
        C.v_ddm = v;
        C.ddm = wl_registry_bind (r, name, &wl_data_device_manager_interface, MIN (v, 3u));
    } else if (strstr (iface, "primary_selection") || strstr (iface, "data_control") ||
               strstr (iface, "clipboard")) {
        C.forbidden_global = TRUE;
        g_strlcpy (C.forbidden_name, iface, sizeof C.forbidden_name);
    }
}

static void reg_remove (void *d, struct wl_registry *r, uint32_t name)
{
    (void) d; (void) r; (void) name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void out_geometry (void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                          int32_t sp, const char *make, const char *model, int32_t tr)
{
    (void) d; (void) o; (void) x; (void) y; (void) pw; (void) ph; (void) sp; (void) make; (void) model; (void) tr;
}
static void out_mode (void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    (void) d; (void) o; (void) flags;
    C.refresh = refresh;
    C.out_w = w;
    C.out_h = h;
    C.out_modes++;
}
static void out_done (void *d, struct wl_output *o)
{
    (void) d; (void) o;
    C.output_done = TRUE;
}
static void out_scale (void *d, struct wl_output *o, int32_t f)
{
    (void) d; (void) o;
    C.scale = f;
}
static void out_name (void *d, struct wl_output *o, const char *n)
{
    (void) d; (void) o; (void) n;
}
static const struct wl_output_listener out_listener = { .geometry = out_geometry, .mode = out_mode,
                                                        .done = out_done, .scale = out_scale,
                                                        .name = out_name, .description = out_name };

static void seat_caps (void *d, struct wl_seat *s, uint32_t caps)
{
    (void) d; (void) s;
    C.caps = caps;
}
static void seat_name (void *d, struct wl_seat *s, const char *n)
{
    (void) d; (void) s; (void) n;
}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void p_enter (void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x,
                     wl_fixed_t y)
{
    (void) d; (void) p; (void) serial;
    C.p_enter++;
    C.p_surface = s;
    C.p_x = wl_fixed_to_double (x);
    C.p_y = wl_fixed_to_double (y);
}
static void p_leave (void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void) d; (void) p; (void) serial; (void) s;
    C.p_leave++;
    C.p_surface = NULL;
}
static void p_motion (void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void) d; (void) p; (void) t;
    C.p_motion++;
    C.p_x = wl_fixed_to_double (x);
    C.p_y = wl_fixed_to_double (y);
}
static void p_button (void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st)
{
    (void) d; (void) p; (void) serial; (void) t;
    C.p_button++;
    C.p_btn = b;
    C.p_btn_state = st;
}
static void p_axis (void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void) d; (void) p; (void) t;
    C.p_axis++;
    C.p_axis_which = axis;
    C.p_axis_value = wl_fixed_to_double (v);
}
static void p_frame (void *d, struct wl_pointer *p)
{
    (void) d; (void) p;
    C.p_frame++;
}
static void p_axis_source (void *d, struct wl_pointer *p, uint32_t src)
{
    (void) d; (void) p; (void) src;
}
static void p_axis_stop (void *d, struct wl_pointer *p, uint32_t t, uint32_t axis)
{
    (void) d; (void) p; (void) t; (void) axis;
}
static void p_axis_discrete (void *d, struct wl_pointer *p, uint32_t axis, int32_t n)
{
    (void) d; (void) p; (void) axis;
    C.p_discrete++;
    C.p_discrete_value = n;
}
/* (by name: later versions of the listener have more members) */
static const struct wl_pointer_listener pointer_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
    .frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop,
    .axis_discrete = p_axis_discrete,
};

static void k_keymap (void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    (void) d; (void) k; (void) fmt;
    C.k_keymap++;
    C.k_keymap_size = size;
    memset (C.k_keymap_head, 0, sizeof C.k_keymap_head);
    void *m = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m != MAP_FAILED) {
        memcpy (C.k_keymap_head, m, MIN (size, sizeof C.k_keymap_head - 1));
        munmap (m, size);
    }
    close (fd);
}
static void k_enter (void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
    (void) d; (void) k; (void) serial; (void) keys;
    C.k_enter++;
    C.k_surface = s;
}
static void k_leave (void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void) d; (void) k; (void) serial; (void) s;
    C.k_leave++;
    C.k_surface = NULL;
}
static void k_key (void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st)
{
    (void) d; (void) k; (void) serial; (void) t;
    C.k_key++;
    C.k_code = key;
    C.k_state = st;
}
static void k_mods (void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock,
                    uint32_t group)
{
    (void) d; (void) k; (void) serial; (void) lat; (void) lock;
    C.k_mods++;
    C.k_dep = dep;
    C.k_group = group;
}
static void k_repeat (void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void) d; (void) k; (void) rate; (void) delay;
    C.k_repeat++;
}
static const struct wl_keyboard_listener keyboard_listener = { k_keymap, k_enter, k_leave, k_key, k_mods,
                                                               k_repeat };

static void xs_configure (void *d, struct xdg_surface *x, uint32_t serial)
{
    (void) d;
    C.configures++;
    C.last_serial = serial;
    xdg_surface_ack_configure (x, serial);
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure (void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    (void) d; (void) t; (void) w; (void) h;
    C.top_configures++;
    C.activated = FALSE;
    const uint32_t *s;
    wl_array_for_each (s, states)
        if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
            C.activated = TRUE;
}
static void top_close (void *d, struct xdg_toplevel *t)
{
    (void) d; (void) t;
}
static const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = top_close };

static void buf_release (void *d, struct wl_buffer *b)
{
    (void) d; (void) b;
    C.released++;
}
static const struct wl_buffer_listener buf_listener = { buf_release };

static void surf_enter (void *d, struct wl_surface *s, struct wl_output *o)
{
    (void) d; (void) s; (void) o;
    C.entered_output++;
}
static void surf_leave (void *d, struct wl_surface *s, struct wl_output *o)
{
    (void) d; (void) s; (void) o;
}
static const struct wl_surface_listener surf_listener = { .enter = surf_enter, .leave = surf_leave };

static void frame_done (void *d, struct wl_callback *cb, uint32_t t)
{
    (void) d; (void) t;
    C.frames++;
    wl_callback_destroy (cb);
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void src_target (void *d, struct wl_data_source *s, const char *m)
{
    (void) d; (void) s; (void) m;
}
static void src_send (void *d, struct wl_data_source *s, const char *m, int32_t fd)
{
    (void) d; (void) s; (void) m;
    close (fd);
}
static void src_cancelled (void *d, struct wl_data_source *s)
{
    (void) d; (void) s;
    C.cancelled++;
}
static void src_nop (void *d, struct wl_data_source *s)
{
    (void) d; (void) s;
}
static void src_action (void *d, struct wl_data_source *s, uint32_t a)
{
    (void) d; (void) s; (void) a;
}
static const struct wl_data_source_listener src_listener = { src_target, src_send, src_cancelled, src_nop,
                                                             src_nop, src_action };

/* ---- helpers */

/* the host works on its own thread: round trips until cond holds */
static gboolean until (int *counter, int at_least)
{
    gint64 end = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
    while (*counter < at_least) {
        if (wl_display_roundtrip (C.dpy) < 0 || g_get_monotonic_time () > end)
            return FALSE;
        if (*counter < at_least)
            g_usleep (2000);
    }
    return TRUE;
}

static struct wl_buffer *make_buffer (void)
{
    int fd = memfd_create ("walpe-webhost-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate (fd, 4) != 0)
        return NULL;
    struct wl_shm_pool *pool = wl_shm_create_pool (C.shm, fd, 4);
    struct wl_buffer *b = wl_shm_pool_create_buffer (pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy (pool);
    close (fd);
    wl_buffer_add_listener (b, &buf_listener, NULL);
    return b;
}

typedef struct {
    struct wl_surface *surface;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
} Win;

static void win_map (Win *w, struct wl_buffer *buf)
{
    if (!w->surface) {
        w->surface = wl_compositor_create_surface (C.compositor);
        wl_surface_add_listener (w->surface, &surf_listener, NULL);
    }
    w->xs = xdg_wm_base_get_xdg_surface (C.wm_base, w->surface);
    xdg_surface_add_listener (w->xs, &xs_listener, NULL);
    w->top = xdg_surface_get_toplevel (w->xs);
    xdg_toplevel_add_listener (w->top, &top_listener, NULL);
    xdg_toplevel_set_title (w->top, "walpe web (offscreen)");
    int before = C.configures;
    wl_surface_commit (w->surface);     /* the first, empty commit asks for the configure */
    CHECK (until (&C.configures, before + 1), "a new toplevel gets its first configure");
    wl_surface_attach (w->surface, buf, 0, 0);
    wl_surface_damage (w->surface, 0, 0, 1, 1);
    wl_callback_add_listener (wl_surface_frame (w->surface), &frame_listener, NULL);
    wl_surface_commit (w->surface);
}

static void win_unmap (Win *w)
{
    xdg_toplevel_destroy (w->top);
    xdg_surface_destroy (w->xs);
    w->top = NULL;
    w->xs = NULL;
    wl_surface_attach (w->surface, NULL, 0, 0);
    wl_surface_commit (w->surface);
    wl_display_roundtrip (C.dpy);
}

int main (void)
{
    /* a runtime directory of the test's own */
    char *dir = g_dir_make_tmp ("walpe-webhost-XXXXXX", NULL);
    if (!dir) {
        fprintf (stderr, "webhost-wl: no temporary directory\n");
        return 1;
    }
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);

    WalpeWebhostStats st;
    walpe_webhost_stats (&st);
    CHECK (!st.running && !walpe_webhost_running (), "not running before start");
    /* input before the display exists goes nowhere (and must not crash) */
    walpe_webhost_pointer_motion (1, 1, 1);
    walpe_webhost_keymap (-1, 0);
    /* the output's size (what a page reads as window.screen), given
     * before the start: the size it is announced with */
    walpe_webhost_output_size (2560, 1440);

    char *sock = NULL, *why = NULL;
    CHECK (walpe_webhost_start (&sock, &why), "start: %s", why ? why : "");
    if (!sock)
        return 1;
    CHECK (g_str_has_prefix (sock, dir), "the socket is below the runtime directory: %s", sock);
    {
        char *sdir = g_path_get_dirname (sock);
        GStatBuf sb;
        CHECK (g_stat (sdir, &sb) == 0 && (sb.st_mode & 0777) == 0700, "its directory is for this user only");
        g_free (sdir);
    }
    char *again = NULL, *why2 = NULL;
    CHECK (!walpe_webhost_start (&again, &why2) && why2, "a second start is refused with a reason");
    g_free (why2);

    C.dpy = wl_display_connect (sock);
    CHECK (C.dpy != NULL, "the client connects by the socket's path");
    if (!C.dpy)
        return 1;
    struct wl_registry *reg = wl_display_get_registry (C.dpy);
    wl_registry_add_listener (reg, &reg_listener, NULL);
    wl_display_roundtrip (C.dpy);
    CHECK (walpe_webhost_running (), "running once the client is connected");
    CHECK (!g_file_test (sock, G_FILE_TEST_EXISTS), "the socket is removed after the first connection");
    {
        char *sdir = g_path_get_dirname (sock);
        CHECK (!g_file_test (sdir, G_FILE_TEST_EXISTS), "and so is its directory");
        g_free (sdir);
    }
    CHECK (wl_display_connect (sock) == NULL, "nobody connects a second time");

    /* ---- globals */
    CHECK (C.compositor && C.shm && C.wm_base && C.output && C.seat && C.ddm,
           "the globals GTK's Wayland backend needs: compositor %p shm %p wm_base %p output %p seat %p ddm %p",
           (void *) C.compositor, (void *) C.shm, (void *) C.wm_base, (void *) C.output, (void *) C.seat,
           (void *) C.ddm);
    CHECK (C.v_compositor >= 3 && C.v_wm_base >= 1 && C.v_seat >= 5 && C.v_ddm >= 1, "versions");
    CHECK (!C.forbidden_global, "no global through which a selection could travel: %s", C.forbidden_name);
    if (!(C.compositor && C.shm && C.wm_base && C.output && C.seat && C.ddm))
        return 1;
    wl_output_add_listener (C.output, &out_listener, NULL);
    wl_seat_add_listener (C.seat, &seat_listener, NULL);
    wl_display_roundtrip (C.dpy);
    CHECK (C.output_done && C.scale == 1 && C.refresh >= 60000, "the output: scale %d, refresh %d mHz", C.scale,
           C.refresh);
    CHECK (C.out_w == 2560 && C.out_h == 1440, "the output has the size asked for before the start (%d x %d)",
           C.out_w, C.out_h);
    {
        int modes = C.out_modes;
        walpe_webhost_output_size (3840, 2160);     /* a larger page came */
        CHECK (until (&C.out_modes, modes + 1) && C.out_w == 3840 && C.out_h == 2160,
               "a new size arrives as a mode change (%d x %d)", C.out_w, C.out_h);
        walpe_webhost_output_size (0, -5);          /* nonsense is ignored */
        walpe_webhost_output_size (3840, 2160);     /* and so is the size it has */
        wl_display_roundtrip (C.dpy);
        g_usleep (30000);
        wl_display_roundtrip (C.dpy);
        CHECK (C.out_modes == modes + 1, "no mode event without a change (%d)", C.out_modes - modes);
    }
    CHECK ((C.caps & WL_SEAT_CAPABILITY_POINTER) && (C.caps & WL_SEAT_CAPABILITY_KEYBOARD),
           "the seat has a pointer and a keyboard");
    C.pointer = wl_seat_get_pointer (C.seat);
    wl_pointer_add_listener (C.pointer, &pointer_listener, NULL);
    C.keyboard = wl_seat_get_keyboard (C.seat);
    wl_keyboard_add_listener (C.keyboard, &keyboard_listener, NULL);
    wl_display_roundtrip (C.dpy);
    CHECK (C.k_keymap == 0, "no keymap is sent before one was handed over");
    CHECK (C.k_repeat == 1, "key repeat settings are sent");

    /* ---- a window */
    Win w = { 0 };
    struct wl_buffer *buf = make_buffer ();
    CHECK (buf != NULL, "a 1 x 1 shm buffer");
    win_map (&w, buf);
    CHECK (until (&C.released, 1), "the buffer is released (nothing reads it)");
    CHECK (until (&C.entered_output, 1), "the mapped window is told its output");
    CHECK (until (&C.frames, 1), "the frame callback is answered");
    CHECK (!C.activated, "a page window is not active by itself");
    walpe_webhost_stats (&st);
    CHECK (st.running && st.toplevels == 1 && st.surfaces >= 1 && st.commits >= 2 && st.frames >= 1,
           "stats: %u toplevels, %u surfaces, %" G_GUINT64_FORMAT " commits, %" G_GUINT64_FORMAT " frames",
           st.toplevels, st.surfaces, st.commits, st.frames);
    /* frames keep coming: ten repaints in well under a second */
    {
        gint64 t0 = g_get_monotonic_time ();
        for (int i = 0; i < 10; i++) {
            int f = C.frames;
            wl_surface_attach (w.surface, buf, 0, 0);
            wl_callback_add_listener (wl_surface_frame (w.surface), &frame_listener, NULL);
            wl_surface_commit (w.surface);
            if (!until (&C.frames, f + 1))
                break;
        }
        double ms = (g_get_monotonic_time () - t0) / 1000.0;
        CHECK (C.frames >= 11, "ten more frames (%d)", C.frames);
        CHECK (ms < 1500, "frame callbacks pace repaints, they do not stall them (%.0f ms for ten)", ms);
    }
    guint32 sid = wl_proxy_get_id ((struct wl_proxy *) w.surface);

    /* ---- pointer */
    walpe_webhost_pointer_motion (5, 1, 1);     /* not on any window: dropped */
    wl_display_roundtrip (C.dpy);
    CHECK (C.p_motion == 0, "motion without an enter goes nowhere");
    walpe_webhost_pointer_enter (sid, 10.5, 20.25);
    CHECK (until (&C.p_enter, 1) && C.p_surface == w.surface, "enter on the right surface");
    CHECK (C.p_x > 10.4 && C.p_x < 10.6 && C.p_y > 20.2 && C.p_y < 20.3, "enter position %.2f, %.2f", C.p_x, C.p_y);
    walpe_webhost_pointer_motion (100, 300.0, 4000.75);    /* far outside the 1 x 1 window: page coordinates */
    CHECK (until (&C.p_motion, 1) && C.p_x == 300.0 && C.p_y > 4000.7, "motion %.2f, %.2f", C.p_x, C.p_y);
    walpe_webhost_pointer_button (110, 1, TRUE);
    CHECK (until (&C.p_button, 1) && C.p_btn == 0x110 && C.p_btn_state == WL_POINTER_BUTTON_STATE_PRESSED,
           "left press (code %#x)", C.p_btn);
    walpe_webhost_pointer_button (120, 1, FALSE);
    CHECK (until (&C.p_button, 2) && C.p_btn_state == WL_POINTER_BUTTON_STATE_RELEASED, "left release");
    walpe_webhost_pointer_button (130, 3, TRUE);
    CHECK (until (&C.p_button, 3) && C.p_btn == 0x111, "right press (code %#x)", C.p_btn);
    walpe_webhost_pointer_button (131, 3, FALSE);
    walpe_webhost_pointer_wheel (140, 0, 2);
    CHECK (until (&C.p_axis, 1) && C.p_axis_which == WL_POINTER_AXIS_VERTICAL_SCROLL && C.p_axis_value == 20.0,
           "wheel: two clicks down = +20 on the vertical axis (%.1f)", C.p_axis_value);
    CHECK (C.p_discrete >= 1 && C.p_discrete_value == 2, "and as a click count");
    walpe_webhost_pointer_wheel (141, -1, 0);
    CHECK (until (&C.p_axis, 2) && C.p_axis_which == WL_POINTER_AXIS_HORIZONTAL_SCROLL && C.p_axis_value == -10.0,
           "wheel left");
    CHECK (C.p_frame >= 6, "every group of pointer events ends with a frame (%d)", C.p_frame);
    walpe_webhost_pointer_leave ();
    CHECK (until (&C.p_leave, 1), "leave");
    walpe_webhost_pointer_motion (150, 5, 5);
    walpe_webhost_pointer_button (151, 1, TRUE);
    wl_display_roundtrip (C.dpy);
    g_usleep (30000);
    wl_display_roundtrip (C.dpy);
    CHECK (C.p_motion == 1 && C.p_button == 4, "after the leave nothing arrives");
    walpe_webhost_pointer_enter (4242, 1, 1);   /* no such surface */
    wl_display_roundtrip (C.dpy);
    g_usleep (30000);
    wl_display_roundtrip (C.dpy);
    CHECK (C.p_enter == 1, "an enter for a surface that does not exist is dropped");

    /* ---- keyboard */
    {
        static const char km[] = "xkb_keymap { not a real one };";
        int fd = memfd_create ("walpe-keymap", MFD_CLOEXEC);
        CHECK (fd >= 0 && write (fd, km, sizeof km) == (ssize_t) sizeof km, "a keymap file");
        walpe_webhost_keymap (fd, sizeof km);   /* the host takes the descriptor */
        CHECK (until (&C.k_keymap, 1) && C.k_keymap_size == sizeof km && !strncmp (C.k_keymap_head, "xkb_keymap", 10),
               "the keymap is passed on as it is (%u bytes)", C.k_keymap_size);
    }
    walpe_webhost_key (200, 30, TRUE);
    wl_display_roundtrip (C.dpy);
    g_usleep (30000);
    wl_display_roundtrip (C.dpy);
    CHECK (C.k_key == 0, "a key without keyboard focus goes nowhere");
    int tc = C.top_configures;
    walpe_webhost_modifiers (1, 0, 0, 0);       /* remembered for the next focus */
    walpe_webhost_keyboard_focus (sid);
    CHECK (until (&C.k_enter, 1) && C.k_surface == w.surface, "keyboard focus on the window");
    CHECK (until (&C.top_configures, tc + 1) && C.activated, "the focused window is told it is active");
    CHECK (until (&C.k_mods, 1) && C.k_dep == 1, "the modifier state follows the focus");
    walpe_webhost_key (210, 30, TRUE);
    CHECK (until (&C.k_key, 1) && C.k_code == 30 && C.k_state == WL_KEYBOARD_KEY_STATE_PRESSED, "key down");
    walpe_webhost_key (220, 30, FALSE);
    CHECK (until (&C.k_key, 2) && C.k_state == WL_KEYBOARD_KEY_STATE_RELEASED, "key up");
    walpe_webhost_modifiers (0, 0, 0, 1);
    CHECK (until (&C.k_mods, 2) && C.k_dep == 0 && C.k_group == 1, "modifiers");
    tc = C.top_configures;
    walpe_webhost_keyboard_focus (0);
    CHECK (until (&C.k_leave, 1), "keyboard focus taken away");
    CHECK (until (&C.top_configures, tc + 1) && !C.activated, "and the window is no longer active");

    /* ---- selections and drags */
    struct wl_data_device *dev = wl_data_device_manager_get_data_device (C.ddm, C.seat);
    struct wl_data_source *src = wl_data_device_manager_create_data_source (C.ddm);
    wl_data_source_add_listener (src, &src_listener, NULL);
    wl_data_source_offer (src, "text/plain;charset=utf-8");
    wl_data_device_set_selection (dev, src, C.last_serial);
    CHECK (until (&C.cancelled, 1), "a selection offered by the client is cancelled at once");
    struct wl_data_source *drag = wl_data_device_manager_create_data_source (C.ddm);
    wl_data_source_add_listener (drag, &src_listener, NULL);
    wl_data_source_offer (drag, "text/uri-list");
    wl_data_device_start_drag (dev, drag, w.surface, NULL, C.last_serial);
    CHECK (until (&C.cancelled, 2), "so is a drag");
    wl_data_device_set_selection (dev, NULL, C.last_serial);    /* "no selection" is not a claim */
    wl_display_roundtrip (C.dpy);
    walpe_webhost_stats (&st);
    CHECK (st.selections == 2, "two claims counted (%" G_GUINT64_FORMAT ")", st.selections);

    /* ---- hidden and shown again (a paused page) */
    walpe_webhost_pointer_enter (sid, 1, 1);
    CHECK (until (&C.p_enter, 2), "pointer on the window again");
    win_unmap (&w);
    walpe_webhost_stats (&st);
    CHECK (st.toplevels == 0, "unmapped (%u)", st.toplevels);
    int rel = C.released, fr = C.frames, ent = C.entered_output;
    win_map (&w, buf);
    CHECK (until (&C.released, rel + 1) && until (&C.frames, fr + 1), "mapped again: release and frame");
    CHECK (until (&C.entered_output, ent + 1), "and told its output again");
    walpe_webhost_stats (&st);
    CHECK (st.toplevels == 1, "one mapped window again");

    /* ---- the window goes away under the pointer */
    wl_callback_add_listener (wl_surface_frame (w.surface), &frame_listener, NULL);  /* never answered */
    xdg_toplevel_destroy (w.top);
    xdg_surface_destroy (w.xs);
    wl_surface_destroy (w.surface);
    wl_display_roundtrip (C.dpy);
    int pm = C.p_motion;
    walpe_webhost_pointer_motion (300, 7, 7);
    walpe_webhost_pointer_button (301, 1, TRUE);
    walpe_webhost_keyboard_focus (sid);
    walpe_webhost_key (302, 30, TRUE);
    wl_display_roundtrip (C.dpy);
    g_usleep (50000);
    wl_display_roundtrip (C.dpy);
    CHECK (C.p_motion == pm && C.k_enter == 1, "input for a destroyed window is dropped");
    CHECK (wl_display_get_error (C.dpy) == 0, "the connection is still healthy (the host never posts an error)");
    walpe_webhost_stats (&st);
    CHECK (st.running && st.toplevels == 0 && st.input_events > 10, "final stats");

    /* (the host has no shutdown: it lives as long as the process, like the
     * module it belongs to; the client connection is simply left open) */
    g_free (sock);
    g_rmdir (dir);
    g_free (dir);
    if (fails) {
        fprintf (stderr, "webhost-wl: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("webhost-wl: ok\n");
    return 0;
}
