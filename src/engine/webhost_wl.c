/* webhost_wl.c - a private Wayland display for the web module's page
 * windows (walpe-web.so).
 *
 * WebKitGTK runs a page only in a mapped GTK window, and GTK maps windows
 * on a display server. On X11 that window is an override-redirect window
 * moved off screen. Wayland has no such place: a client cannot position
 * its windows, every mapped toplevel is shown, listed and focusable. So
 * with this page host (webhostsel.h says when it is used) GTK is not
 * connected to the session's compositor at all, nor to an X server. It
 * connects to this: a Wayland display that lives on a thread
 * of the daemon, implements just what GTK's Wayland backend needs
 * (wl_compositor, wl_subcompositor, wl_shm, xdg_wm_base, wl_seat,
 * wl_output and a wl_data_device_manager that accepts nothing) and shows
 * nothing. The page's picture never goes through here: web.c takes it
 * from GTK's render node as before. What this display does:
 *
 *  - lets windows be "mapped": configure, wl_surface.enter, and frame
 *    callbacks on a timer, so GTK's frame clock runs and WebKit sees a
 *    visible page. Buffers are released unread (they are 1 x 1).
 *  - delivers input: the daemon's pointer events for a page become
 *    wl_pointer enter / motion / button / axis / leave events, key events
 *    wl_keyboard events with the keymap handed over. GTK reads them like
 *    any input, the page gets trusted DOM events.
 *  - keeps the page away from the user's clipboard and selections: this
 *    display has no other client. wl_data_device.set_selection and
 *    start_drag are answered with wl_data_source.cancelled; there is no
 *    primary selection global. What the user copied in another program
 *    cannot be read, replaced or cleared from a page.
 *
 * One client only: GTK in this process. The socket lives in a directory
 * made for it (mode 0700) and is removed as soon as GTK is connected; the
 * peer's credentials are checked. Everything of libwayland-server is
 * touched on the compositor thread only; the functions of webhost_wl.h
 * queue a command and wake that thread.
 *
 * A warning that belongs here: GTK ends the process when its display
 * connection breaks. This file therefore never posts a protocol error and
 * never disconnects its client. */
#include "walpe-build-config.h"
#include "webhost_wl.h"
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* evdev button codes (linux/input-event-codes.h) */
#define HOST_BTN_LEFT 0x110
#define HOST_BTN_RIGHT 0x111
#define HOST_BTN_MIDDLE 0x112

/* how long GTK has to connect after walpe_webhost_start */
#define HOST_ACCEPT_MS 30000
/* frame callbacks are answered this long after the commit that asked:
 * GTK may repaint at up to 144 Hz (web.c limits captures to the fps
 * setting by itself) */
#define HOST_FRAME_MS 7
#define HOST_REFRESH_MHZ 144000
/* frame callbacks kept at a time (GTK has one per surface) */
#define HOST_MAX_CALLBACKS 4096
/* the one output's size in pixels: what GTK takes for the monitor and a
 * page reads as window.screen. Large enough for any page until
 * walpe_webhost_output_size () says how large the pages are. */
#define HOST_OUT_MAX 16384

enum { HOST_OFF, HOST_WAITING, HOST_RUNNING, HOST_FAILED };

typedef struct {
    struct wl_list link;            /* H.surfaces */
    struct wl_resource *res;
    struct wl_resource *buffer;     /* attached since the last commit (NULL = none) */
    gboolean attached;              /* an attach was seen since the last commit */
    struct wl_listener buffer_gone;
    struct wl_list frames;          /* wl_callback resources asked for since the last commit */
    struct wl_resource *xdg, *toplevel;
    gboolean configured;            /* the first configure went out */
    gboolean mapped;
    gboolean active;                /* has the keyboard: told so in its toplevel state */
} HostSurface;

typedef enum {
    CMD_PTR_ENTER, CMD_PTR_MOTION, CMD_PTR_BUTTON, CMD_PTR_WHEEL, CMD_PTR_LEAVE,
    CMD_KEYMAP, CMD_KBD_FOCUS, CMD_KEY, CMD_MODS,
    CMD_OUTPUT,
} HostCmdKind;

typedef struct {
    HostCmdKind kind;
    guint32 surface, time;
    double x, y;
    int a, b;                       /* button + down, dx + dy, fd + size, key + down,
                                     * output width + height */
    guint32 mods[4];
} HostCmd;

static struct {
    GMutex lock;
    GCond cond;
    int state;                      /* HOST_*, under lock */
    int out_w0, out_h0;             /* output size asked for before the start, under lock */
    GThread *thread;
    struct wl_display *dpy;
    struct wl_event_loop *loop;
    struct wl_client *client;
    int listen_fd, wake_fd;
    char *sock_dir, *sock_path;
    gint abandon;
    GAsyncQueue *cmds;              /* HostCmd * */
    /* compositor thread only */
    struct wl_list surfaces;        /* HostSurface.link */
    struct wl_list pointers, keyboards, outputs;    /* resource links */
    struct wl_list frames_due;      /* wl_callback resource links */
    int n_callbacks;                /* frame callbacks alive (waiting for a commit or due) */
    struct wl_event_source *frame_timer;
    struct wl_event_source *wake_src;   /* (kept: the loop itself holds it only inside the kernel's epoll set) */
    gboolean frame_armed;
    HostSurface *ptr_focus, *kbd_focus;
    int out_w, out_h;               /* the output's mode */
    int keymap_fd;
    guint32 keymap_size;
    guint32 mods[4];
    /* numbers (atomic) */
    gint n_surfaces, n_toplevels;
    guint64 commits, frames, selections, input_events;   /* under lock */
} H = { .listen_fd = -1, .wake_fd = -1, .keymap_fd = -1 };

static guint32 host_ms (void)
{
    return (guint32) (g_get_monotonic_time () / 1000);
}

static void count (guint64 *n)
{
    g_mutex_lock (&H.lock);
    (*n)++;
    g_mutex_unlock (&H.lock);
}

static void res_destroy (struct wl_client *c, struct wl_resource *r)
{
    (void) c;
    wl_resource_destroy (r);
}

/* a resource kept in one of the lists goes away */
static void res_unlink (struct wl_resource *r)
{
    wl_list_remove (wl_resource_get_link (r));
}

/* ================================================================ frames */

static int frame_tick (void *data)
{
    (void) data;
    H.frame_armed = FALSE;
    guint32 now = host_ms ();
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe (cb, tmp, &H.frames_due) {
        wl_callback_send_done (cb, now);
        wl_resource_destroy (cb);       /* callback_gone takes it off the list */
        count (&H.frames);
    }
    return 0;
}

static void frames_arm (void)
{
    if (H.frame_armed || wl_list_empty (&H.frames_due))
        return;
    H.frame_armed = TRUE;
    wl_event_source_timer_update (H.frame_timer, HOST_FRAME_MS);
}

/* ================================================================ wl_surface */

static void hs_configure (HostSurface *hs)
{
    if (!hs->toplevel || !hs->xdg)
        return;
    struct wl_array states;
    wl_array_init (&states);
    if (hs->active) {
        uint32_t *s = wl_array_add (&states, sizeof *s);
        if (s)
            *s = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    /* 0 x 0: the window decides its size (1 x 1; web.c gives the view the page's) */
    xdg_toplevel_send_configure (hs->toplevel, 0, 0, &states);
    wl_array_release (&states);
    xdg_surface_send_configure (hs->xdg, wl_display_next_serial (H.dpy));
}

static void hs_unmapped (HostSurface *hs)
{
    if (hs->mapped) {
        hs->mapped = FALSE;
        g_atomic_int_add (&H.n_toplevels, -1);
    }
    hs->configured = FALSE;
}

static void hs_buffer_gone (struct wl_listener *l, void *data)
{
    (void) data;
    HostSurface *hs = wl_container_of (l, hs, buffer_gone);
    wl_list_remove (&hs->buffer_gone.link);
    hs->buffer = NULL;
}

static void hs_drop_buffer (HostSurface *hs)
{
    if (hs->buffer) {
        wl_list_remove (&hs->buffer_gone.link);
        hs->buffer = NULL;
    }
}

static void surface_attach (struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer,
                            int32_t x, int32_t y)
{
    (void) c; (void) x; (void) y;
    HostSurface *hs = wl_resource_get_user_data (r);
    hs_drop_buffer (hs);
    hs->attached = TRUE;
    hs->buffer = buffer;
    if (buffer) {
        hs->buffer_gone.notify = hs_buffer_gone;
        wl_resource_add_destroy_listener (buffer, &hs->buffer_gone);
    }
}

/* a frame callback goes away: answered, or with its client */
static void callback_gone (struct wl_resource *cb)
{
    res_unlink (cb);
    H.n_callbacks--;
}

static void surface_frame (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    HostSurface *hs = wl_resource_get_user_data (r);
    struct wl_resource *cb = wl_resource_create (c, &wl_callback_interface, 1, id);
    if (!cb)
        return;     /* out of memory: the client waits; nothing to post */
    wl_resource_set_implementation (cb, NULL, NULL, callback_gone);
    wl_list_insert (hs->frames.prev, wl_resource_get_link (cb));
    /* callbacks wait for the surface's next commit. A client that keeps
     * asking and never commits would make this list grow without end:
     * past the limit a callback is answered at once instead of kept. */
    if (++H.n_callbacks > HOST_MAX_CALLBACKS) {
        wl_callback_send_done (cb, host_ms ());
        wl_resource_destroy (cb);
    }
}

static void surface_commit (struct wl_client *c, struct wl_resource *r)
{
    (void) c;
    HostSurface *hs = wl_resource_get_user_data (r);
    count (&H.commits);
    if (hs->attached) {
        gboolean has_buffer = hs->buffer != NULL;
        if (hs->buffer) {
            /* nothing here reads a buffer: the client may reuse it at once */
            wl_buffer_send_release (hs->buffer);
            hs_drop_buffer (hs);
        }
        hs->attached = FALSE;
        if (hs->toplevel) {
            if (has_buffer && hs->configured && !hs->mapped) {
                hs->mapped = TRUE;
                g_atomic_int_inc (&H.n_toplevels);
                /* "you are on the output": scale and refresh rate come from it */
                struct wl_resource *o;
                wl_resource_for_each (o, &H.outputs)
                    wl_surface_send_enter (hs->res, o);
            } else if (!has_buffer) {
                hs_unmapped (hs);
            }
        }
    }
    if (hs->toplevel && !hs->configured) {
        hs->configured = TRUE;
        hs_configure (hs);
    }
    if (!wl_list_empty (&hs->frames)) {
        wl_list_insert_list (H.frames_due.prev, &hs->frames);
        wl_list_init (&hs->frames);
        frames_arm ();
    }
}

static void surface_nop_region (struct wl_client *c, struct wl_resource *r, struct wl_resource *region)
{
    (void) c; (void) r; (void) region;
}

static void surface_nop_rect (struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w,
                              int32_t h)
{
    (void) c; (void) r; (void) x; (void) y; (void) w; (void) h;
}

static void surface_nop_int (struct wl_client *c, struct wl_resource *r, int32_t v)
{
    (void) c; (void) r; (void) v;
}

static void surface_nop_xy (struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
    (void) c; (void) r; (void) x; (void) y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = res_destroy,
    .attach = surface_attach,
    .damage = surface_nop_rect,
    .frame = surface_frame,
    .set_opaque_region = surface_nop_region,
    .set_input_region = surface_nop_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_nop_int,
    .set_buffer_scale = surface_nop_int,
    .damage_buffer = surface_nop_rect,
    .offset = surface_nop_xy,
};

static void surface_gone (struct wl_resource *r)
{
    HostSurface *hs = wl_resource_get_user_data (r);
    if (H.ptr_focus == hs)
        H.ptr_focus = NULL;
    if (H.kbd_focus == hs)
        H.kbd_focus = NULL;
    hs_drop_buffer (hs);
    hs_unmapped (hs);
    /* callbacks it still waits for are never answered: they go with the
     * surface (callback_gone takes each off the list head that is about
     * to be freed; the client is told that the id is free again, where
     * keeping the object would have held it and its id until the client
     * disconnects) */
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe (cb, tmp, &hs->frames)
        wl_resource_destroy (cb);
    /* role objects that outlive the surface must not point at it */
    if (hs->xdg)
        wl_resource_set_user_data (hs->xdg, NULL);
    if (hs->toplevel)
        wl_resource_set_user_data (hs->toplevel, NULL);
    wl_list_remove (&hs->link);
    g_atomic_int_add (&H.n_surfaces, -1);
    g_free (hs);
}

/* ---- wl_region: accepted, not looked at */

static const struct wl_region_interface region_impl = {
    .destroy = res_destroy,
    .add = surface_nop_rect,
    .subtract = surface_nop_rect,
};

/* ---- wl_compositor */

static void compositor_create_surface (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *res = wl_resource_create (c, &wl_surface_interface, wl_resource_get_version (r), id);
    if (!res)
        return;
    HostSurface *hs = g_new0 (HostSurface, 1);
    hs->res = res;
    wl_list_init (&hs->frames);
    wl_list_insert (&H.surfaces, &hs->link);
    g_atomic_int_inc (&H.n_surfaces);
    wl_resource_set_implementation (res, &surface_impl, hs, surface_gone);
}

static void compositor_create_region (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void) r;
    struct wl_resource *res = wl_resource_create (c, &wl_region_interface, 1, id);
    if (res)
        wl_resource_set_implementation (res, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &wl_compositor_interface, (int) version, id);
    if (r)
        wl_resource_set_implementation (r, &compositor_impl, NULL, NULL);
}

/* ================================================================ xdg-shell */

static void nop_r (struct wl_client *c, struct wl_resource *r)
{
    (void) c; (void) r;
}

static void nop_res (struct wl_client *c, struct wl_resource *r, struct wl_resource *o);

/* ---- wl_subcompositor: there so that a toolkit which lists it among the
 * interfaces it expects finds it. A subsurface made here is an object
 * that does nothing (nothing is shown, so there is nothing to stack). */

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = res_destroy,
    .set_position = surface_nop_xy,
    .place_above = nop_res,
    .place_below = nop_res,
    .set_sync = nop_r,
    .set_desync = nop_r,
};

static void subcompositor_get_subsurface (struct wl_client *c, struct wl_resource *r, uint32_t id,
                                          struct wl_resource *surface, struct wl_resource *parent)
{
    (void) r; (void) surface; (void) parent;
    struct wl_resource *s = wl_resource_create (c, &wl_subsurface_interface, 1, id);
    if (s)
        wl_resource_set_implementation (s, &subsurface_impl, NULL, NULL);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = res_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &wl_subcompositor_interface, (int) version, id);
    if (r)
        wl_resource_set_implementation (r, &subcompositor_impl, NULL, NULL);
}

static void nop_u (struct wl_client *c, struct wl_resource *r, uint32_t v)
{
    (void) c; (void) r; (void) v;
}

static void nop_res (struct wl_client *c, struct wl_resource *r, struct wl_resource *o)
{
    (void) c; (void) r; (void) o;
}

static void nop_str (struct wl_client *c, struct wl_resource *r, const char *s)
{
    (void) c; (void) r; (void) s;
}

static void nop_seat_serial (struct wl_client *c, struct wl_resource *r, struct wl_resource *seat,
                             uint32_t serial)
{
    (void) c; (void) r; (void) seat; (void) serial;
}

static void nop_seat_serial_u (struct wl_client *c, struct wl_resource *r, struct wl_resource *seat,
                               uint32_t serial, uint32_t v)
{
    (void) c; (void) r; (void) seat; (void) serial; (void) v;
}

static void nop_window_menu (struct wl_client *c, struct wl_resource *r, struct wl_resource *seat,
                             uint32_t serial, int32_t x, int32_t y)
{
    (void) c; (void) r; (void) seat; (void) serial; (void) x; (void) y;
}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = res_destroy,
    .set_size = surface_nop_xy,
    .set_anchor_rect = surface_nop_rect,
    .set_anchor = nop_u,
    .set_gravity = nop_u,
    .set_constraint_adjustment = nop_u,
    .set_offset = surface_nop_xy,
    .set_reactive = nop_r,
    .set_parent_size = surface_nop_xy,
    .set_parent_configure = nop_u,
};

static void popup_reposition (struct wl_client *c, struct wl_resource *r, struct wl_resource *positioner,
                              uint32_t token)
{
    (void) c; (void) r; (void) positioner; (void) token;
}

static const struct xdg_popup_interface popup_impl = {
    .destroy = res_destroy,
    .grab = nop_seat_serial,
    .reposition = popup_reposition,
};

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = res_destroy,
    .set_parent = nop_res,
    .set_title = nop_str,
    .set_app_id = nop_str,
    .show_window_menu = nop_window_menu,
    .move = nop_seat_serial,
    .resize = nop_seat_serial_u,
    .set_max_size = surface_nop_xy,
    .set_min_size = surface_nop_xy,
    .set_maximized = nop_r,
    .unset_maximized = nop_r,
    .set_fullscreen = nop_res,
    .unset_fullscreen = nop_r,
    .set_minimized = nop_r,
};

static void toplevel_gone (struct wl_resource *r)
{
    HostSurface *hs = wl_resource_get_user_data (r);
    if (!hs || hs->toplevel != r)
        return;
    hs->toplevel = NULL;
    hs_unmapped (hs);
}

static void xdg_surface_get_toplevel (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    HostSurface *hs = wl_resource_get_user_data (r);
    struct wl_resource *t = wl_resource_create (c, &xdg_toplevel_interface, wl_resource_get_version (r), id);
    if (!t)
        return;
    /* One toplevel per surface. A second one (a protocol violation GTK
     * does not commit) gets an object that does nothing: a role object
     * keeps a pointer to the surface's record, and that record knows of
     * one toplevel only - it could not clear the pointer of another when
     * the surface goes, and the other would then write into freed memory. */
    if (hs && hs->toplevel)
        hs = NULL;
    wl_resource_set_implementation (t, &toplevel_impl, hs, toplevel_gone);
    if (hs) {
        hs->toplevel = t;
        hs->configured = FALSE;
    }
}

/* popups (menus, choosers) are not wanted on a wallpaper and web.c keeps
 * WebKit from opening them; one that is asked for anyway is dismissed */
static void xdg_surface_get_popup (struct wl_client *c, struct wl_resource *r, uint32_t id,
                                   struct wl_resource *parent, struct wl_resource *positioner)
{
    (void) parent; (void) positioner;
    struct wl_resource *p = wl_resource_create (c, &xdg_popup_interface, wl_resource_get_version (r), id);
    if (!p)
        return;
    wl_resource_set_implementation (p, &popup_impl, NULL, NULL);
    xdg_popup_send_popup_done (p);
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = res_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = surface_nop_rect,
    .ack_configure = nop_u,
};

static void xdg_surface_gone (struct wl_resource *r)
{
    HostSurface *hs = wl_resource_get_user_data (r);
    if (!hs || hs->xdg != r)
        return;
    hs->xdg = NULL;
    /* a toplevel the client left behind (it destroys the toplevel first)
     * has lost its surface: it is detached, so that the surface can be
     * given an xdg_surface and a toplevel again */
    if (hs->toplevel) {
        wl_resource_set_user_data (hs->toplevel, NULL);
        hs->toplevel = NULL;
        hs_unmapped (hs);
    }
}

static void wm_base_create_positioner (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *p = wl_resource_create (c, &xdg_positioner_interface, wl_resource_get_version (r), id);
    if (p)
        wl_resource_set_implementation (p, &positioner_impl, NULL, NULL);
}

static void wm_base_get_xdg_surface (struct wl_client *c, struct wl_resource *r, uint32_t id,
                                     struct wl_resource *surface)
{
    HostSurface *hs = wl_resource_get_user_data (surface);
    struct wl_resource *x = wl_resource_create (c, &xdg_surface_interface, wl_resource_get_version (r), id);
    if (!x)
        return;
    /* one xdg_surface per surface; a second one is an object that does
     * nothing (see xdg_surface_get_toplevel: the surface's record can
     * detach only the role objects it knows of when it is freed) */
    if (hs && hs->xdg)
        hs = NULL;
    wl_resource_set_implementation (x, &xdg_surface_impl, hs, xdg_surface_gone);
    if (hs) {
        hs->xdg = x;
        hs->configured = FALSE;
    }
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = res_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = nop_u,
};

static void wm_base_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &xdg_wm_base_interface, (int) version, id);
    if (r)
        wl_resource_set_implementation (r, &wm_base_impl, NULL, NULL);
}

/* ================================================================ wl_output */

static const struct wl_output_interface output_impl = { .release = res_destroy };

static void output_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &wl_output_interface, (int) version, id);
    if (!r)
        return;
    wl_resource_set_implementation (r, &output_impl, NULL, res_unlink);
    wl_list_insert (&H.outputs, wl_resource_get_link (r));
    /* scale 1: a page pixel is a wallpaper pixel */
    wl_output_send_geometry (r, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "WalPE", "page host",
                             WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode (r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, H.out_w, H.out_h,
                         HOST_REFRESH_MHZ);
    if (version >= 2) {
        wl_output_send_scale (r, 1);
        wl_output_send_done (r);
    }
}

/* ================================================================ wl_seat */

static void pointer_set_cursor (struct wl_client *c, struct wl_resource *r, uint32_t serial,
                                struct wl_resource *surface, int32_t hx, int32_t hy)
{
    (void) c; (void) r; (void) serial; (void) surface; (void) hx; (void) hy;
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = res_destroy,
};

static const struct wl_keyboard_interface keyboard_impl = { .release = res_destroy };
static const struct wl_touch_interface touch_impl = { .release = res_destroy };

static void seat_get_pointer (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *p = wl_resource_create (c, &wl_pointer_interface, wl_resource_get_version (r), id);
    if (!p)
        return;
    wl_resource_set_implementation (p, &pointer_impl, NULL, res_unlink);
    wl_list_insert (&H.pointers, wl_resource_get_link (p));
}

static void keyboard_send_keymap (struct wl_resource *k)
{
    if (H.keymap_fd >= 0)
        wl_keyboard_send_keymap (k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, H.keymap_fd, H.keymap_size);
}

static void seat_get_keyboard (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *k = wl_resource_create (c, &wl_keyboard_interface, wl_resource_get_version (r), id);
    if (!k)
        return;
    wl_resource_set_implementation (k, &keyboard_impl, NULL, res_unlink);
    wl_list_insert (&H.keyboards, wl_resource_get_link (k));
    keyboard_send_keymap (k);
    /* key repeat is the client's job on Wayland; the usual desktop values */
    if (wl_resource_get_version (k) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info (k, 25, 600);
}

static void seat_get_touch (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *t = wl_resource_create (c, &wl_touch_interface, wl_resource_get_version (r), id);
    if (t)
        wl_resource_set_implementation (t, &touch_impl, NULL, NULL);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = res_destroy,
};

static void seat_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &wl_seat_interface, (int) version, id);
    if (!r)
        return;
    wl_resource_set_implementation (r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities (r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name (r, "walpe-page-host");
}

/* ================================================================ data device
 * GTK only sets a seat up when a wl_data_device_manager exists, so there
 * is one. It never offers anything and refuses everything: a selection or
 * a drag started from a page is cancelled on the spot. */

static void source_offer (struct wl_client *c, struct wl_resource *r, const char *mime)
{
    (void) c; (void) r; (void) mime;
}

static const struct wl_data_source_interface source_impl = {
    .offer = source_offer,
    .destroy = res_destroy,
    .set_actions = nop_u,
};

static void device_start_drag (struct wl_client *c, struct wl_resource *r, struct wl_resource *source,
                               struct wl_resource *origin, struct wl_resource *icon, uint32_t serial)
{
    (void) c; (void) r; (void) origin; (void) icon; (void) serial;
    count (&H.selections);
    if (source)
        wl_data_source_send_cancelled (source);
}

static void device_set_selection (struct wl_client *c, struct wl_resource *r, struct wl_resource *source,
                                  uint32_t serial)
{
    (void) c; (void) r; (void) serial;
    if (!source)
        return;     /* "no selection": that is the state anyway */
    count (&H.selections);
    wl_data_source_send_cancelled (source);
}

static const struct wl_data_device_interface device_impl = {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = res_destroy,
};

static void ddm_create_source (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *s = wl_resource_create (c, &wl_data_source_interface, wl_resource_get_version (r), id);
    if (s)
        wl_resource_set_implementation (s, &source_impl, NULL, NULL);
}

static void ddm_get_device (struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat)
{
    (void) seat;
    struct wl_resource *d = wl_resource_create (c, &wl_data_device_interface, wl_resource_get_version (r), id);
    if (d)
        wl_resource_set_implementation (d, &device_impl, NULL, NULL);
}

static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source,
    .get_data_device = ddm_get_device,
};

static void ddm_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &wl_data_device_manager_interface, (int) version, id);
    if (r)
        wl_resource_set_implementation (r, &ddm_impl, NULL, NULL);
}

/* ================================================================ input */

/* the surface GTK names by the id of its proxy; NULL when it is gone */
static HostSurface *surface_by_id (guint32 id)
{
    if (!H.client || !id)
        return NULL;
    struct wl_resource *r = wl_client_get_object (H.client, id);
    if (!r || !wl_resource_instance_of (r, &wl_surface_interface, &surface_impl))
        return NULL;
    return wl_resource_get_user_data (r);
}

static void pointer_frame (void)
{
    struct wl_resource *p;
    wl_resource_for_each (p, &H.pointers)
        if (wl_resource_get_version (p) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame (p);
}

static void pointer_leave_now (void)
{
    if (!H.ptr_focus)
        return;
    uint32_t serial = wl_display_next_serial (H.dpy);
    struct wl_resource *p;
    wl_resource_for_each (p, &H.pointers)
        wl_pointer_send_leave (p, serial, H.ptr_focus->res);
    pointer_frame ();
    H.ptr_focus = NULL;
}

static void keyboard_focus_now (HostSurface *hs)
{
    if (H.kbd_focus == hs)
        return;
    struct wl_resource *k;
    if (H.kbd_focus) {
        uint32_t serial = wl_display_next_serial (H.dpy);
        wl_resource_for_each (k, &H.keyboards)
            wl_keyboard_send_leave (k, serial, H.kbd_focus->res);
        H.kbd_focus->active = FALSE;
        if (H.kbd_focus->configured)
            hs_configure (H.kbd_focus);
    }
    H.kbd_focus = hs;
    if (!hs)
        return;
    hs->active = TRUE;
    if (hs->configured)
        hs_configure (hs);
    uint32_t serial = wl_display_next_serial (H.dpy);
    struct wl_array none;
    wl_array_init (&none);
    wl_resource_for_each (k, &H.keyboards) {
        wl_keyboard_send_enter (k, serial, hs->res, &none);
        wl_keyboard_send_modifiers (k, wl_display_next_serial (H.dpy), H.mods[0], H.mods[1], H.mods[2],
                                    H.mods[3]);
    }
    wl_array_release (&none);
}

static void host_cmd (const HostCmd *m)
{
    struct wl_resource *p, *k;
    switch (m->kind) {
    case CMD_OUTPUT: {
        if (m->a == H.out_w && m->b == H.out_h)
            return;
        H.out_w = m->a;
        H.out_h = m->b;
        struct wl_resource *o;
        wl_resource_for_each (o, &H.outputs) {
            wl_output_send_mode (o, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, H.out_w, H.out_h,
                                 HOST_REFRESH_MHZ);
            if (wl_resource_get_version (o) >= WL_OUTPUT_DONE_SINCE_VERSION)
                wl_output_send_done (o);
        }
        return;     /* not an input event */
    }
    case CMD_PTR_ENTER: {
        HostSurface *hs = surface_by_id (m->surface);
        if (H.ptr_focus && H.ptr_focus != hs)
            pointer_leave_now ();
        if (!hs || H.ptr_focus == hs)
            break;
        H.ptr_focus = hs;
        uint32_t serial = wl_display_next_serial (H.dpy);
        wl_resource_for_each (p, &H.pointers)
            wl_pointer_send_enter (p, serial, hs->res, wl_fixed_from_double (m->x),
                                   wl_fixed_from_double (m->y));
        pointer_frame ();
        break;
    }
    case CMD_PTR_MOTION:
        if (!H.ptr_focus)
            break;
        wl_resource_for_each (p, &H.pointers)
            wl_pointer_send_motion (p, m->time, wl_fixed_from_double (m->x), wl_fixed_from_double (m->y));
        pointer_frame ();
        break;
    case CMD_PTR_BUTTON: {
        if (!H.ptr_focus)
            break;
        uint32_t code = m->a == 3 ? HOST_BTN_RIGHT : m->a == 2 ? HOST_BTN_MIDDLE : HOST_BTN_LEFT;
        uint32_t serial = wl_display_next_serial (H.dpy);
        wl_resource_for_each (p, &H.pointers)
            wl_pointer_send_button (p, serial, m->time, code,
                                    m->b ? WL_POINTER_BUTTON_STATE_PRESSED
                                         : WL_POINTER_BUTTON_STATE_RELEASED);
        pointer_frame ();
        break;
    }
    case CMD_PTR_WHEEL:
        if (!H.ptr_focus)
            break;
        wl_resource_for_each (p, &H.pointers) {
            int v = wl_resource_get_version (p);
            if (v >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
                wl_pointer_send_axis_source (p, WL_POINTER_AXIS_SOURCE_WHEEL);
            /* one wheel click = 10 units (what libinput reports), and
             * the click count for clients that scroll by clicks */
            if (m->b) {
                if (v >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                    wl_pointer_send_axis_discrete (p, WL_POINTER_AXIS_VERTICAL_SCROLL, m->b);
                wl_pointer_send_axis (p, m->time, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                      wl_fixed_from_int (m->b * 10));
            }
            if (m->a) {
                if (v >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                    wl_pointer_send_axis_discrete (p, WL_POINTER_AXIS_HORIZONTAL_SCROLL, m->a);
                wl_pointer_send_axis (p, m->time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                      wl_fixed_from_int (m->a * 10));
            }
        }
        pointer_frame ();
        break;
    case CMD_PTR_LEAVE:
        pointer_leave_now ();
        break;
    case CMD_KEYMAP:
        if (H.keymap_fd >= 0)
            close (H.keymap_fd);
        H.keymap_fd = m->a;
        H.keymap_size = (guint32) m->b;
        wl_resource_for_each (k, &H.keyboards)
            keyboard_send_keymap (k);
        break;
    case CMD_KBD_FOCUS:
        keyboard_focus_now (surface_by_id (m->surface));
        break;
    case CMD_KEY: {
        if (!H.kbd_focus)
            break;
        uint32_t serial = wl_display_next_serial (H.dpy);
        wl_resource_for_each (k, &H.keyboards)
            wl_keyboard_send_key (k, serial, m->time, (uint32_t) m->a,
                                  m->b ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        break;
    }
    case CMD_MODS:
        memcpy (H.mods, m->mods, sizeof H.mods);
        if (!H.kbd_focus)
            break;
        wl_resource_for_each (k, &H.keyboards)
            wl_keyboard_send_modifiers (k, wl_display_next_serial (H.dpy), H.mods[0], H.mods[1],
                                        H.mods[2], H.mods[3]);
        break;
    }
    count (&H.input_events);
}

static int host_wake (int fd, uint32_t mask, void *data)
{
    (void) mask; (void) data;
    guint64 v;
    if (read (fd, &v, sizeof v) < 0 && errno != EAGAIN)
        return 0;
    HostCmd *m;
    while ((m = g_async_queue_try_pop (H.cmds))) {
        if (H.client)
            host_cmd (m);
        else if (m->kind == CMD_KEYMAP && m->a >= 0)
            close (m->a);
        g_free (m);
    }
    return 0;
}

static void host_send (HostCmd *m)
{
    g_mutex_lock (&H.lock);
    gboolean up = H.state == HOST_RUNNING;
    g_mutex_unlock (&H.lock);
    if (!up) {
        if (m->kind == CMD_KEYMAP && m->a >= 0)
            close (m->a);
        g_free (m);
        return;
    }
    g_async_queue_push (H.cmds, m);
    guint64 one = 1;
    if (write (H.wake_fd, &one, sizeof one) < 0) {
        /* EAGAIN: the counter is full, the thread is awake anyway */
    }
}

static HostCmd *cmd_new (HostCmdKind k)
{
    HostCmd *m = g_new0 (HostCmd, 1);
    m->kind = k;
    return m;
}

void walpe_webhost_output_size (int w, int h)
{
    if (w < 1 || h < 1)
        return;
    w = MIN (w, HOST_OUT_MAX);
    h = MIN (h, HOST_OUT_MAX);
    g_mutex_lock (&H.lock);
    gboolean off = H.state == HOST_OFF, up = H.state == HOST_RUNNING;
    if (off) {
        /* the size the output is first announced with */
        H.out_w0 = w;
        H.out_h0 = h;
    }
    g_mutex_unlock (&H.lock);
    if (!up)
        return;
    HostCmd *m = cmd_new (CMD_OUTPUT);
    m->a = w;
    m->b = h;
    host_send (m);
}

void walpe_webhost_pointer_enter (guint32 surface, double x, double y)
{
    HostCmd *m = cmd_new (CMD_PTR_ENTER);
    m->surface = surface;
    m->x = x;
    m->y = y;
    host_send (m);
}

void walpe_webhost_pointer_motion (guint32 time, double x, double y)
{
    HostCmd *m = cmd_new (CMD_PTR_MOTION);
    m->time = time;
    m->x = x;
    m->y = y;
    host_send (m);
}

void walpe_webhost_pointer_button (guint32 time, int button, gboolean down)
{
    HostCmd *m = cmd_new (CMD_PTR_BUTTON);
    m->time = time;
    m->a = button;
    m->b = down;
    host_send (m);
}

void walpe_webhost_pointer_wheel (guint32 time, int dx, int dy)
{
    HostCmd *m = cmd_new (CMD_PTR_WHEEL);
    m->time = time;
    m->a = dx;
    m->b = dy;
    host_send (m);
}

void walpe_webhost_pointer_leave (void)
{
    host_send (cmd_new (CMD_PTR_LEAVE));
}

void walpe_webhost_keymap (int fd, guint32 size)
{
    HostCmd *m = cmd_new (CMD_KEYMAP);
    m->a = fd;
    m->b = (int) size;
    host_send (m);
}

void walpe_webhost_keyboard_focus (guint32 surface)
{
    HostCmd *m = cmd_new (CMD_KBD_FOCUS);
    m->surface = surface;
    host_send (m);
}

void walpe_webhost_key (guint32 time, guint32 evdev_key, gboolean down)
{
    HostCmd *m = cmd_new (CMD_KEY);
    m->time = time;
    m->a = (int) evdev_key;
    m->b = down;
    host_send (m);
}

void walpe_webhost_modifiers (guint32 depressed, guint32 latched, guint32 locked, guint32 group)
{
    HostCmd *m = cmd_new (CMD_MODS);
    m->mods[0] = depressed;
    m->mods[1] = latched;
    m->mods[2] = locked;
    m->mods[3] = group;
    host_send (m);
}

/* ================================================================ life */

static void host_set_state (int st)
{
    g_mutex_lock (&H.lock);
    H.state = st;
    g_cond_broadcast (&H.cond);
    g_mutex_unlock (&H.lock);
}

static void host_drop_socket (void)
{
    if (H.listen_fd >= 0) {
        close (H.listen_fd);
        H.listen_fd = -1;
    }
    if (H.sock_path)
        g_unlink (H.sock_path);
    if (H.sock_dir)
        g_rmdir (H.sock_dir);
}

static void client_gone (struct wl_listener *l, void *data)
{
    (void) l; (void) data;
    /* GTK closed its display (it does not while pages exist): nothing
     * can be hosted any more */
    H.client = NULL;
    H.ptr_focus = H.kbd_focus = NULL;
    host_set_state (HOST_FAILED);
    g_message ("web: the page host's display lost its client");
}

static struct wl_listener client_gone_listener = { .notify = client_gone };

/* wait for GTK, take exactly that one connection */
static gboolean host_accept (void)
{
    gint64 end = g_get_monotonic_time () + (gint64) HOST_ACCEPT_MS * 1000;
    for (;;) {
        if (g_atomic_int_get (&H.abandon))
            return FALSE;
        gint64 left = (end - g_get_monotonic_time ()) / 1000;
        if (left <= 0)
            return FALSE;
        struct pollfd pfd[2] = { { H.listen_fd, POLLIN, 0 }, { H.wake_fd, POLLIN, 0 } };
        int n = poll (pfd, 2, (int) MIN (left, 1000));
        if (n < 0 && errno != EINTR)
            return FALSE;
        if (n <= 0 || !(pfd[0].revents & POLLIN))
            continue;
        int fd = accept4 (H.listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        /* the directory is ours alone; all the same, only this process */
        struct ucred cr;
        socklen_t len = sizeof cr;
        if (getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || cr.uid != getuid () ||
            cr.pid != getpid ()) {
            close (fd);
            continue;
        }
        H.client = wl_client_create (H.dpy, fd);
        if (!H.client) {
            close (fd);
            return FALSE;
        }
        wl_client_add_destroy_listener (H.client, &client_gone_listener);
        return TRUE;
    }
}

static gpointer host_thread (gpointer user)
{
    (void) user;
    gboolean ok = host_accept ();
    /* nobody else gets in: the socket and its directory go now */
    host_drop_socket ();
    if (!ok) {
        host_set_state (HOST_FAILED);
        return NULL;
    }
    host_set_state (HOST_RUNNING);
    for (;;) {
        wl_display_flush_clients (H.dpy);
        if (wl_event_loop_dispatch (H.loop, -1) < 0 && errno != EINTR)
            break;
    }
    host_set_state (HOST_FAILED);
    return NULL;
}

gboolean walpe_webhost_start (char **socket_path, char **why)
{
    g_mutex_lock (&H.lock);
    int st = H.state;
    g_mutex_unlock (&H.lock);
    if (st != HOST_OFF) {
        /* one display, one client: it cannot be started a second time */
        if (why)
            *why = g_strdup (st == HOST_FAILED ? "the private page host has stopped"
                                               : "the private page host is already in use");
        return FALSE;
    }
    /* a directory nobody else can enter, below the runtime directory
     * (GLib falls back to the cache directory when there is none) */
    char *tmpl = g_build_filename (g_get_user_runtime_dir (), "walpe-web-XXXXXX", NULL);
    g_mkdir_with_parents (g_get_user_runtime_dir (), 0700);
    if (!g_mkdtemp_full (tmpl, 0700)) {
        if (why)
            *why = g_strdup_printf ("cannot make a directory for the page host's socket in %s (%s)",
                                    g_get_user_runtime_dir (), g_strerror (errno));
        g_free (tmpl);
        return FALSE;
    }
    H.sock_dir = tmpl;
    H.sock_path = g_build_filename (tmpl, "wl", NULL);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    const char *fail = NULL;
    if (strlen (H.sock_path) >= sizeof addr.sun_path)
        fail = "the runtime directory's path is too long for a socket";
    if (!fail) {
        g_strlcpy (addr.sun_path, H.sock_path, sizeof addr.sun_path);
        H.listen_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (H.listen_fd < 0 || bind (H.listen_fd, (struct sockaddr *) &addr, sizeof addr) != 0 ||
            listen (H.listen_fd, 1) != 0)
            fail = g_strerror (errno);
    }
    if (!fail) {
        H.wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
        H.dpy = wl_display_create ();
        if (H.wake_fd < 0 || !H.dpy)
            fail = "cannot create the display";
    }
    if (fail) {
        if (why)
            *why = g_strdup_printf ("the private page host cannot start: %s", fail);
        host_drop_socket ();
        if (H.wake_fd >= 0)
            close (H.wake_fd);
        H.wake_fd = -1;
        if (H.dpy)
            wl_display_destroy (H.dpy);
        H.dpy = NULL;
        g_clear_pointer (&H.sock_path, g_free);
        g_clear_pointer (&H.sock_dir, g_free);
        return FALSE;
    }
    H.loop = wl_display_get_event_loop (H.dpy);
    wl_list_init (&H.surfaces);
    wl_list_init (&H.pointers);
    wl_list_init (&H.keyboards);
    wl_list_init (&H.outputs);
    wl_list_init (&H.frames_due);
    H.out_w = H.out_w0 > 0 ? H.out_w0 : HOST_OUT_MAX;
    H.out_h = H.out_h0 > 0 ? H.out_h0 : HOST_OUT_MAX;
    H.cmds = g_async_queue_new ();
    /* the versions offered are the lowest that give GTK what it uses:
     * every request of a higher version would be one more to answer */
    wl_global_create (H.dpy, &wl_compositor_interface, 4, NULL, compositor_bind);
    wl_global_create (H.dpy, &wl_subcompositor_interface, 1, NULL, subcompositor_bind);
    wl_display_init_shm (H.dpy);
    wl_global_create (H.dpy, &xdg_wm_base_interface, 2, NULL, wm_base_bind);
    wl_global_create (H.dpy, &wl_output_interface, 2, NULL, output_bind);
    wl_global_create (H.dpy, &wl_data_device_manager_interface, 3, NULL, ddm_bind);
    wl_global_create (H.dpy, &wl_seat_interface, 5, NULL, seat_bind);
    H.frame_timer = wl_event_loop_add_timer (H.loop, frame_tick, NULL);
    H.wake_src = wl_event_loop_add_fd (H.loop, H.wake_fd, WL_EVENT_READABLE, host_wake, NULL);
    g_atomic_int_set (&H.abandon, 0);
    H.state = HOST_WAITING;
    H.thread = g_thread_new ("walpe-web-host", host_thread, NULL);
    if (socket_path)
        *socket_path = g_strdup (H.sock_path);
    return TRUE;
}

void walpe_webhost_abandon (void)
{
    g_mutex_lock (&H.lock);
    gboolean waiting = H.state == HOST_WAITING;
    g_mutex_unlock (&H.lock);
    if (!waiting)
        return;
    g_atomic_int_set (&H.abandon, 1);
    guint64 one = 1;
    if (write (H.wake_fd, &one, sizeof one) < 0) {
        /* the accept loop looks at the flag once a second anyway */
    }
}

gboolean walpe_webhost_running (void)
{
    g_mutex_lock (&H.lock);
    gboolean r = H.state == HOST_RUNNING;
    g_mutex_unlock (&H.lock);
    return r;
}

void walpe_webhost_stats (WalpeWebhostStats *out)
{
    g_mutex_lock (&H.lock);
    out->running = H.state == HOST_RUNNING;
    out->commits = H.commits;
    out->frames = H.frames;
    out->selections = H.selections;
    out->input_events = H.input_events;
    g_mutex_unlock (&H.lock);
    out->surfaces = (guint) MAX (g_atomic_int_get (&H.n_surfaces), 0);
    out->toplevels = (guint) MAX (g_atomic_int_get (&H.n_toplevels), 0);
}
