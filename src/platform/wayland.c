/* wayland.c - the Wayland backend of the daemon (platform/impl.h).
 *
 * One connection. Each output gets one surface with the layer-shell role
 * (zwlr_layer_shell_v1) on the background layer (on KWin: the bottom
 * layer, see wl_layer_choose () in wayland_logic.h): anchored to all four
 * edges, size 0 x 0 (the compositor decides: the whole output), exclusive
 * zone -1 (panels do not push it), no keyboard. The daemon renders into it
 * through a wl_egl_window; frames are paced by the compositor's frame
 * callbacks, not by the EGL swap interval, which is always 0 here.
 *
 * THREADS AND EVENT QUEUES
 * libwayland delivers an object's events on the event queue the object is
 * assigned to, to whichever thread dispatches that queue. This file uses
 *
 *   the default queue, dispatched by the MAIN thread only (the GSource
 *   attached in start (), and the waits inside open / outputs / sync):
 *   registry, outputs, xdg-output, seat and pointer, the role objects once
 *   a surface is set up, fractional scale, xdg_wm_base;
 *
 *   one private queue per surface ("rq"), for what a frame needs: the
 *   frame callback, the presentation feedback and - only while a role is
 *   being set up - the role object. It is dispatched by the surface's
 *   render thread inside pre_swap () / swapped (), and by the main thread
 *   when the render thread is not waiting on it (so that a paused
 *   wallpaper still learns that it is shown again). Whoever dispatches rq
 *   holds the surface's rq_lock; the main thread only ever tries that
 *   lock, it never waits for it.
 *
 * Objects that deliver to rq are created through proxy wrappers assigned
 * to rq, so that a new object is on the right queue from the moment it
 * exists. Every thread that reads the socket does it with
 * wl_display_prepare_read_queue / poll / wl_display_read_events (or
 * wl_display_cancel_read): the protocol libwayland prescribes for several
 * readers. No thread calls the blocking wl_display_dispatch.
 *
 * Listeners may therefore run on either thread. They only change the
 * surface's state under its lock (and destroy the object the event
 * belongs to); whatever follows for the daemon - an event callback, a
 * pause signal - is done by the main thread from the event source
 * (wl_attention () wakes it). The render thread never waits for the main
 * thread: its only waits are a bounded poll on the socket.
 *
 * Lock order: rq_lock, then lock. Requests are sent with the lock held
 * when they name an object the other thread may destroy (the role, the
 * output); a request never blocks.
 *
 * A surface's state (role object, logical size, input region, viewport)
 * is double-buffered in the protocol and applied with the commit that the
 * EGL library sends inside eglSwapBuffers, on the render thread. So the
 * render thread sends those requests itself, right before the swap
 * (pre_swap); the main thread only records what it wants. It commits by
 * itself only when no swap is in progress (in_swap), for a change that has
 * to arrive although no frame is being drawn. */
#include "walpe-build-config.h"
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <epoxy/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#ifdef HAVE_WL_CURSOR
#include <wayland-cursor.h>
#endif

#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "platform/impl.h"
#include "platform/wayland_logic.h"

#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PRESENT_OPAQUE_EXT
#define EGL_PRESENT_OPAQUE_EXT 0x31DF
#endif

/* DRM fourcc of XRGB8888, the EGL_NATIVE_VISUAL_ID of an opaque config */
#define WL_VISUAL_XRGB8888 0x34325258ul

/* how long surface_new waits for the first configure, and how long a
 * re-creation inside pre_swap may hold the render thread */
#define WL_FIRST_CONFIGURE_US (3 * (gint64) G_USEC_PER_SEC)
#define WL_RECREATE_WAIT_US   200000
/* presentation feedbacks that may be outstanding per surface */
#define WL_FEEDBACK_MAX 8
/* wheel clicks handed on from one group of axis events */
#define WL_WHEEL_BURST_MAX 8

/* ------------------------------------------------------------------ types */

typedef struct {
    guint32 name, version;
    char *interface;
} WlGlobal;

typedef struct {
    guint32 name, version;
    struct wl_output *wo;
    struct zxdg_output_v1 *xo;
    guint32 xo_version;
    /* double-buffered as the protocol sends it: `pend` collects events,
     * a done event makes it `cur` */
    WlOutProps pend, cur;
    char *pend_name, *pend_desc, *pend_make, *pend_model;
    char *cur_name, *cur_desc, *cur_make, *cur_model;
    gboolean name_from_wl;      /* wl_output.name was told (it wins over xdg-output's) */
    gboolean desc_from_wl;
    gboolean cur_valid;         /* a done was seen */
} WlOut;

typedef struct {
    guint32 name, version;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    /* its pointer: the surface of ours it is on (NULL = on none) and the
     * position on it the protocol told last */
    struct WlSurf *over;
    wl_fixed_t px, py;
    /* one group of axis events (ended by wl_pointer.frame) */
    WlAxisAcc acc;
    double ax_value[2];
    int ax_discrete[2], ax_v120[2];
    gboolean ax_stop[2], ax_any;
    guint32 ax_time;
} WlSeat;

typedef enum { WL_ROLE_NONE = 0, WL_ROLE_LAYER, WL_ROLE_TOPLEVEL } WlRole;

typedef struct WlSurf {
    /* ---- fixed after surface_new (read by any thread without the lock) */
    WalpeSurfaceKind kind;
    WlRole role;
    gpointer user;
    struct wl_surface *surface;
    struct wl_egl_window *egl_window;
    EGLDisplay egl_dpy;
    EGLSurface egl_surface;
    struct wl_event_queue *rq;
    struct wl_surface *surface_rq;              /* wrapper on rq: frame callbacks */
    struct wp_presentation *presentation_rq;    /* wrapper on rq; NULL = no feedback */
    struct zwlr_layer_shell_v1 *shell_rq;       /* wrapper on rq: the role while it is set up */
    struct xdg_wm_base *wm_base_rq;             /* wrapper on rq (window) */
    struct wp_viewport *viewport;               /* NULL = the compositor has no viewporter */
    struct wp_fractional_scale_v1 *frac;
    struct wl_region *empty_region;
    guint32 out_id;
    char *out_name;
    int out_x, out_y, out_w, out_h;             /* the output's rectangle, pixels */
    char *title;

    GMutex rq_lock;             /* held by the thread that dispatches rq */
    GMutex lock;                /* everything below */
    struct wl_output *wl_out;   /* its output; NULL once that is gone */
    struct zwlr_layer_surface_v1 *layer;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    gboolean role_on_rq;        /* the role object still delivers to rq */
    WlLayerState ls;
    int top_w, top_h;           /* xdg_toplevel.configure, applied by xdg_surface.configure */
    WlSizeIn size_in;
    WlSizeOut size;
    gboolean size_dirty;        /* buffer scale / viewport destination to send */
    gboolean want_pointer, input_dirty;
    gboolean in_swap;           /* between pre_swap and swapped: the EGL library commits */
    int interval;
    double refresh_hz;
    struct wl_callback *frame_cb;
    WlFrameState fs;
    GSList *feedbacks;          /* outstanding wp_presentation_feedback */
    int n_feedback;
    WalpePresentStats stats;
    guint todo;                 /* WL_LS_ACT_* the main thread has to follow up */

    /* ---- main thread only */
    gboolean hidden_reported;
    gboolean hidden_known;
    guint lost_timer;
    int lost_ticks;
    /* wp_presentation_feedback.presented of the last frame: its flags (the
     * protocol's kind bits: 1 vsync, 2 hw clock, 4 hw completion, 8 zero
     * copy), -1 = none yet. For status only. On KWin the zero-copy bit goes
     * when something is composited over the surface - also a panel, also
     * Plasma's desktop: a hint for a bug report, nothing to act on. */
    int pres_flags;
} WlSurf;

/* what the main thread still has to tell the daemon */
typedef enum { WL_EVK_BUTTON, WL_EVK_CLOSED } WlEvKind;
typedef struct {
    WlEvKind kind;
    gpointer surface_user;
    int button, x, y;
    gboolean down;
    guint32 time_ms;
} WlEv;

typedef enum { WL_HOW_LEGACY = 0, WL_HOW_PLATFORM, WL_HOW_PLATFORM_EXT } WlEglHow;

/* The two EGL 1.5 functions used here are looked up by name and called
 * through these pointers, not through the dispatch library's own entry
 * points: libepoxy decides whether an EGL 1.5 function exists by asking
 * the display that is current on the calling thread for its version, and
 * there is none when the display is made, nor on the main thread when the
 * surfaces are made. It then finds "no provider" and ends the process.
 * eglGetProcAddress is EGL 1.0 and has no such condition. */
#ifndef EGLAPIENTRY
#define EGLAPIENTRY
#endif
typedef EGLDisplay (EGLAPIENTRY *WlEglGetDisplayFn) (EGLenum platform, void *native_display,
                                                    const EGLAttrib *attribs);
typedef EGLSurface (EGLAPIENTRY *WlEglMakeWindowFn) (EGLDisplay dpy, EGLConfig conf,
                                                    void *native_window, const EGLAttrib *attribs);

static struct {
    gboolean is_open;
    WalpePlatformOpts opts;
    struct wl_display *dpy;
    int fd;
    struct wl_registry *registry;
    GPtrArray *globals;         /* WlGlobal */

    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    guint32 layer_shell_version;
    gboolean layer_shell_gone;
    WlLayer layer;              /* of every wallpaper surface; set in open (), then only read */
    WlLayerWhy layer_why;
    gboolean kwin;              /* the compositor is KWin; set in open (), then only read */
    struct zxdg_output_manager_v1 *xdg_output_mgr;
    guint32 xdg_output_version;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *frac_mgr;
    struct wp_presentation *presentation;
    clockid_t pres_clock;
    struct xdg_wm_base *wm_base;
    gboolean wm_base_gone;
    /* registry names of the globals bound above (0 = none) */
    guint32 layer_shell_name, xdg_output_name, viewporter_name, frac_name, presentation_name,
            wm_base_name;
    guint32 dmabuf_name, dmabuf_version;
    char *display_node;

    GList *outputs;             /* WlOut */
    GList *seats;               /* WlSeat */
    GList *surfaces;            /* WlSurf */

    /* pointer (main thread). The daemon knows one pointer; with several
     * seats it is the one that entered, moved or clicked last on a surface
     * of ours (`seat`; NULL when none is on one). */
    struct {
        gboolean seen, inside;
        WlSurf *surf;
        WlSeat *seat;
        int x, y;
    } ptr;
    gboolean sel_buttons, sel_motion;
#ifdef HAVE_WL_CURSOR
    struct wl_cursor_theme *cursor_theme;
    int cursor_theme_scale;
    struct wl_surface *cursor_surface;
#endif
    char *cursor_theme_name;
    int cursor_size;
    char *ignore;               /* WALPE_WL_IGNORE: globals to behave as absent (testing) */

    /* event source (main thread) */
    GSource *src;
    GPollFD pfd;
    gboolean src_reading, pfd_added;
    gboolean started, layout_events;
    GArray *evq;                /* WlEv */
    gboolean outputs_dirty, motion_dirty;
    gboolean lost_reported;
    gboolean prov_started;
    int recreate_max;

    gint dead;                  /* atomic: the connection is gone */
    gint no_feedback;           /* atomic: wp_presentation was withdrawn */
    gint attention;             /* atomic: a surface has something for the main thread */

    EGLDisplay egl_dpy;
    WlEglHow egl_how;
    WlEglMakeWindowFn egl_make_window;  /* set with WL_HOW_PLATFORM */
    gboolean alpha_warned, interval_warned;
} W;

static const WalpeSignalProvider wl_frame_provider;

static void wl_surface_role_gone_locked (WlSurf *s);
static void wl_out_changed (WlOut *o);
static void wl_surfaces_output_gone (WlOut *o);
static void wl_surfaces_shell_gone (void);

/* --------------------------------------------------------------- helpers */

static gboolean wl_is_dead (void)
{
    return g_atomic_int_get (&W.dead) != 0;
}

/* any thread: the main thread has something to do for a surface */
static void wl_attention (void)
{
    g_atomic_int_set (&W.attention, 1);
    g_main_context_wakeup (NULL);
}

/* any thread: the connection failed. Logged once, with the compositor's
 * reason when it sent one; the main thread reports it to the daemon. */
static void wl_mark_dead (const char *where)
{
    if (!g_atomic_int_compare_and_exchange (&W.dead, 0, 1))
        return;
    int e = W.dpy ? wl_display_get_error (W.dpy) : 0;
    if (e == EPROTO) {
        const struct wl_interface *itf = NULL;
        uint32_t id = 0;
        uint32_t code = wl_display_get_protocol_error (W.dpy, &itf, &id);
        g_warning ("wayland: protocol error %u on %s@%u: the compositor closed the connection (%s)",
                   code, itf ? itf->name : "?", id, where);
    } else {
        g_warning ("wayland: the connection to the compositor is gone (%s: %s)", where,
                   e ? g_strerror (e) : "closed");
    }
    g_main_context_wakeup (NULL);
}

/* Read and dispatch one queue until done (data) says so or the time is
 * up. q = NULL: the default queue (main thread only). deadline_us in
 * g_get_monotonic_time (); a deadline in the past reads what is there
 * without waiting. FALSE = the connection failed. The caller is the only
 * thread dispatching q for the duration. */
static gboolean wl_wait_queue (struct wl_event_queue *q, gboolean (*done) (gpointer), gpointer data,
                               gint64 deadline_us)
{
    for (;;) {
        for (;;) {
            int r = q ? wl_display_prepare_read_queue (W.dpy, q) : wl_display_prepare_read (W.dpy);
            if (r == 0)
                break;
            r = q ? wl_display_dispatch_queue_pending (W.dpy, q) : wl_display_dispatch_pending (W.dpy);
            if (r < 0)
                return FALSE;
        }
        /* prepared: this thread now has to read or to cancel */
        if (done && done (data)) {
            wl_display_cancel_read (W.dpy);
            return TRUE;
        }
        if (wl_display_flush (W.dpy) < 0 && errno != EAGAIN) {
            wl_display_cancel_read (W.dpy);
            return FALSE;
        }
        struct pollfd pfd = { .fd = W.fd, .events = POLLIN };
        int pr;
        do {
            /* the time that is left - also after a signal ended the wait
             * early, or signals that keep coming would start the whole
             * wait again each time and the deadline would never be reached */
            gint64 now = g_get_monotonic_time ();
            int ms = deadline_us > now ? (int) ((deadline_us - now + 999) / 1000) : 0;
            pfd.revents = 0;
            pr = poll (&pfd, 1, ms);
        } while (pr < 0 && errno == EINTR);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events (W.dpy) < 0)
                return FALSE;
        } else {
            wl_display_cancel_read (W.dpy);
            if (pr < 0 || (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))))
                return FALSE;
        }
        int r = q ? wl_display_dispatch_queue_pending (W.dpy, q) : wl_display_dispatch_pending (W.dpy);
        if (r < 0)
            return FALSE;
        if (done && done (data))
            return TRUE;
        if (g_get_monotonic_time () >= deadline_us)
            return TRUE;
    }
}

static gboolean wl_pred_flag (gpointer data)
{
    return *(gboolean *) data;
}

static void wl_sync_done (void *data, struct wl_callback *cb, uint32_t serial)
{
    *(gboolean *) data = TRUE;
    wl_callback_destroy (cb);
}

static const struct wl_callback_listener wl_sync_listener = { wl_sync_done };

/* main thread: a round trip on the default queue that gives up after a
 * second (a compositor that hangs must not hang the daemon with it) */
static void wl_roundtrip (void)
{
    if (!W.dpy || wl_is_dead ())
        return;
    gboolean done = FALSE;
    struct wl_callback *cb = wl_display_sync (W.dpy);
    if (!cb)
        return;
    wl_callback_add_listener (cb, &wl_sync_listener, &done);
    if (!wl_wait_queue (NULL, wl_pred_flag, &done, g_get_monotonic_time () + G_USEC_PER_SEC))
        wl_mark_dead ("round trip");
    if (!done)
        wl_callback_destroy (cb);
}

/* How long the compositor may take to answer during the start (the first
 * look at it and the backend's own first round trips). A compositor that
 * is busy at login answers late, so this is far above the second a round
 * trip gets afterwards; one that accepted the connection and stays silent
 * must not hold the daemon's start for ever. */
#define WL_START_ANSWER_S 10

static void wl_start_sync_done (void *data, struct wl_callback *cb, uint32_t serial)
{
    *(gboolean *) data = TRUE;
}

static const struct wl_callback_listener wl_start_sync_listener = { wl_start_sync_done };

/* A round trip on one queue (NULL: the default queue) of any connection,
 * given up at deadline_us (g_get_monotonic_time ()). 1 = answered, 0 = no
 * answer in time, -1 = the connection failed. For the start, before W is
 * set up; the caller is the only thread using dpy. */
static int wl_sync_until (struct wl_display *dpy, struct wl_event_queue *q, gint64 deadline_us)
{
    struct wl_callback *cb;
    if (q) {
        struct wl_display *wrap = wl_proxy_create_wrapper (dpy);
        if (!wrap)
            return -1;
        wl_proxy_set_queue ((struct wl_proxy *) wrap, q);
        cb = wl_display_sync (wrap);
        wl_proxy_wrapper_destroy (wrap);
    } else {
        cb = wl_display_sync (dpy);
    }
    if (!cb)
        return -1;
    gboolean done = FALSE;
    int rc = 1;
    wl_callback_add_listener (cb, &wl_start_sync_listener, &done);
    while (!done) {
        if ((q ? wl_display_prepare_read_queue (dpy, q) : wl_display_prepare_read (dpy)) != 0) {
            /* events read earlier wait to be handed out */
            if ((q ? wl_display_dispatch_queue_pending (dpy, q)
                   : wl_display_dispatch_pending (dpy)) < 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (wl_display_flush (dpy) < 0 && errno != EAGAIN) {
            wl_display_cancel_read (dpy);
            rc = -1;
            break;
        }
        gint64 now = g_get_monotonic_time ();
        int ms = deadline_us > now ? (int) MIN ((deadline_us - now + 999) / 1000, 3600 * 1000) : 0;
        struct pollfd pfd = { .fd = wl_display_get_fd (dpy), .events = POLLIN };
        int pr = poll (&pfd, 1, ms);
        if (pr < 0 && errno == EINTR) {
            wl_display_cancel_read (dpy);
            continue;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events (dpy) < 0) {
                rc = -1;
                break;
            }
        } else {
            wl_display_cancel_read (dpy);
            rc = pr == 0 ? 0 : -1;      /* time is up / the socket failed */
            break;
        }
    }
    /* (an answer that comes after this is dropped with the object) */
    wl_callback_destroy (cb);
    if (rc > 0 && wl_display_get_error (dpy))
        rc = -1;
    return rc;
}

static void wl_push_event (const WlEv *e)
{
    if (W.evq)
        g_array_append_val (W.evq, *e);
}

static WlGlobal *wl_global_find (const char *interface)
{
    for (guint i = 0; W.globals && i < W.globals->len; i++) {
        WlGlobal *g = g_ptr_array_index (W.globals, i);
        if (!strcmp (g->interface, interface))
            return g;
    }
    return NULL;
}

static void wl_global_free (gpointer p)
{
    WlGlobal *g = p;
    g_free (g->interface);
    g_free (g);
}

static void wl_set_str (char **dst, const char *src)
{
    if (g_strcmp0 (*dst, src)) {
        g_free (*dst);
        *dst = g_strdup (src);
    }
}

/* text the compositor sent (an output's make, model): stored made harmless
 * (wl_text_clean), NULL when nothing usable is left */
static void wl_set_text (char **dst, const char *src, gsize max_bytes)
{
    char *clean = wl_text_clean (src, max_bytes);
    wl_set_str (dst, clean);
    g_free (clean);
}

/* ---------------------------------------------------------------- outputs */

static gboolean wl_out_listed (const WlOut *o)
{
    if (!o->cur_valid || !wl_out_usable (&o->cur))
        return FALSE;
    /* with xdg-output the logical size is part of the rectangle: an output
     * is listed when that has arrived, so that two reads of the same
     * layout cannot differ by it */
    return !o->xo || o->cur.have_xdg_size;
}

static gboolean wl_props_equal (const WlOutProps *a, const WlOutProps *b)
{
    return a->geo_x == b->geo_x && a->geo_y == b->geo_y && a->mode_w == b->mode_w &&
           a->mode_h == b->mode_h && a->refresh_mhz == b->refresh_mhz &&
           a->int_scale == b->int_scale && a->transform == b->transform &&
           a->have_xdg_pos == b->have_xdg_pos && a->have_xdg_size == b->have_xdg_size &&
           a->xdg_x == b->xdg_x && a->xdg_y == b->xdg_y && a->xdg_w == b->xdg_w &&
           a->xdg_h == b->xdg_h;
}

/* a done event: what was collected becomes the output's state */
static void wl_out_commit (WlOut *o)
{
    gboolean changed = !o->cur_valid || !wl_props_equal (&o->pend, &o->cur) ||
                       g_strcmp0 (o->pend_name, o->cur_name) ||
                       g_strcmp0 (o->pend_desc, o->cur_desc) ||
                       g_strcmp0 (o->pend_make, o->cur_make) ||
                       g_strcmp0 (o->pend_model, o->cur_model);
    if (!changed)
        return;
    o->cur = o->pend;
    wl_set_str (&o->cur_name, o->pend_name);
    wl_set_str (&o->cur_desc, o->pend_desc);
    wl_set_str (&o->cur_make, o->pend_make);
    wl_set_str (&o->cur_model, o->pend_model);
    o->cur.name = o->cur_name;
    o->cur_valid = TRUE;
    wl_out_changed (o);
}

static void wl_out_geometry (void *data, struct wl_output *wo, int32_t x, int32_t y, int32_t pw,
                             int32_t ph, int32_t subpixel, const char *make, const char *model,
                             int32_t transform)
{
    WlOut *o = data;
    o->pend.geo_x = x;
    o->pend.geo_y = y;
    o->pend.transform = wl_transform_sane (transform);
    wl_set_text (&o->pend_make, make, WL_TEXT_MAX);
    wl_set_text (&o->pend_model, model, WL_TEXT_MAX);
    if (o->version < 2)
        wl_out_commit (o);      /* version 1 has no done event */
}

static void wl_out_mode (void *data, struct wl_output *wo, uint32_t flags, int32_t w, int32_t h,
                         int32_t refresh)
{
    WlOut *o = data;
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    o->pend.mode_w = w;
    o->pend.mode_h = h;
    o->pend.refresh_mhz = refresh;
    if (o->version < 2)
        wl_out_commit (o);
}

static void wl_out_done (void *data, struct wl_output *wo)
{
    wl_out_commit (data);
}

static void wl_out_scale (void *data, struct wl_output *wo, int32_t factor)
{
    WlOut *o = data;
    o->pend.int_scale = factor;
}

static void wl_out_name (void *data, struct wl_output *wo, const char *name)
{
    WlOut *o = data;
    char *clean = wl_text_clean (name, WL_NAME_MAX);
    o->name_from_wl = clean != NULL;
    if (clean)
        wl_set_str (&o->pend_name, clean);
    g_free (clean);
}

static void wl_out_description (void *data, struct wl_output *wo, const char *text)
{
    WlOut *o = data;
    char *clean = wl_text_clean (text, WL_TEXT_MAX);
    o->desc_from_wl = clean != NULL;
    if (clean)
        wl_set_str (&o->pend_desc, clean);
    g_free (clean);
}

static const struct wl_output_listener wl_out_listener = {
    .geometry = wl_out_geometry,
    .mode = wl_out_mode,
    .done = wl_out_done,
    .scale = wl_out_scale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    .name = wl_out_name,
    .description = wl_out_description,
#endif
};

static void wl_xo_position (void *data, struct zxdg_output_v1 *xo, int32_t x, int32_t y)
{
    WlOut *o = data;
    o->pend.have_xdg_pos = TRUE;
    o->pend.xdg_x = x;
    o->pend.xdg_y = y;
}

static void wl_xo_size (void *data, struct zxdg_output_v1 *xo, int32_t w, int32_t h)
{
    WlOut *o = data;
    o->pend.have_xdg_size = TRUE;
    o->pend.xdg_w = w;
    o->pend.xdg_h = h;
}

static void wl_xo_done (void *data, struct zxdg_output_v1 *xo)
{
    /* sent up to version 2; from version 3 wl_output.done ends the group */
    wl_out_commit (data);
}

static void wl_xo_name (void *data, struct zxdg_output_v1 *xo, const char *name)
{
    WlOut *o = data;
    char *clean = o->name_from_wl ? NULL : wl_text_clean (name, WL_NAME_MAX);
    if (clean)
        wl_set_str (&o->pend_name, clean);
    g_free (clean);
}

static void wl_xo_description (void *data, struct zxdg_output_v1 *xo, const char *text)
{
    WlOut *o = data;
    char *clean = o->desc_from_wl ? NULL : wl_text_clean (text, WL_TEXT_MAX);
    if (clean)
        wl_set_str (&o->pend_desc, clean);
    g_free (clean);
}

static const struct zxdg_output_v1_listener wl_xo_listener = {
    .logical_position = wl_xo_position,
    .logical_size = wl_xo_size,
    .done = wl_xo_done,
    .name = wl_xo_name,
    .description = wl_xo_description,
};

static void wl_out_get_xdg (WlOut *o)
{
    if (o->xo || !W.xdg_output_mgr)
        return;
    o->xo = zxdg_output_manager_v1_get_xdg_output (W.xdg_output_mgr, o->wo);
    o->xo_version = W.xdg_output_version;
    zxdg_output_v1_add_listener (o->xo, &wl_xo_listener, o);
}

static void wl_out_add (guint32 name, guint32 version)
{
    WlOut *o = g_new0 (WlOut, 1);
    o->name = name;
    /* no higher than the libwayland in use knows events for: bound at
     * version 4 against a library whose wl_output ends at 3 (before
     * 1.20), the name event would be an unknown opcode and end the
     * connection */
    o->version = MIN (MIN (version, 4u), (guint32) wl_output_interface.version);
    o->pend.id = name;
    o->pend.int_scale = 1;
    o->wo = wl_registry_bind (W.registry, name, &wl_output_interface, o->version);
    wl_output_add_listener (o->wo, &wl_out_listener, o);
    W.outputs = g_list_append (W.outputs, o);
    wl_out_get_xdg (o);
}

static void wl_out_free (WlOut *o)
{
    if (o->xo)
        zxdg_output_v1_destroy (o->xo);
    if (o->wo) {
#ifdef WL_OUTPUT_RELEASE_SINCE_VERSION
        if (o->version >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release (o->wo);
        else
#endif
            wl_output_destroy (o->wo);
    }
    g_free (o->pend_name);
    g_free (o->pend_desc);
    g_free (o->pend_make);
    g_free (o->pend_model);
    g_free (o->cur_name);
    g_free (o->cur_desc);
    g_free (o->cur_make);
    g_free (o->cur_model);
    g_free (o);
}

static WlOut *wl_out_by_id (guint32 id)
{
    for (GList *l = W.outputs; l; l = l->next)
        if (((WlOut *) l->data)->name == id)
            return l->data;
    return NULL;
}

/* The layout now: the listed outputs in list order. *rects and *outs are
 * arrays of *n entries (g_free both); rects[i].src points into *props,
 * which the caller frees last. */
static void wl_layout_now (WlOutProps **props, WlOutRect **rects, WlOut ***outs, int *n)
{
    int cap = (int) g_list_length (W.outputs), k = 0;
    WlOutProps *p = g_new0 (WlOutProps, MAX (cap, 1));
    WlOut **src = g_new0 (WlOut *, MAX (cap, 1));
    for (GList *l = W.outputs; l; l = l->next) {
        WlOut *o = l->data;
        if (!wl_out_listed (o))
            continue;
        p[k] = o->cur;
        p[k].name = o->cur_name;
        src[k++] = o;
    }
    WlOutRect *r = g_new0 (WlOutRect, MAX (k, 1));
    WlOut **sorted = g_new0 (WlOut *, MAX (k, 1));
    wl_out_layout (p, k, W.viewporter != NULL, r);
    for (int i = 0; i < k; i++)
        sorted[i] = src[r[i].src - p];
    g_free (src);
    *props = p;
    *rects = r;
    *outs = sorted;
    *n = k;
}

static void wl_outputs (WalpeOutputList *out)
{
    out->n = 0;
    out->o = NULL;
    if (!W.dpy)
        return;
    /* before the event source runs nobody has read the connection since
     * open (): look now. Afterwards the source keeps the state current. */
    if (!W.started) {
        wl_roundtrip ();
        W.outputs_dirty = FALSE;        /* this read is the daemon's first knowledge */
    }
    WlOutProps *props;
    WlOutRect *rects;
    WlOut **outs;
    int n;
    wl_layout_now (&props, &rects, &outs, &n);
    out->o = g_new0 (WalpeOutput, MAX (n, 1));
    out->n = n;
    for (int i = 0; i < n; i++) {
        WalpeOutput *o = &out->o[i];
        WlOut *src = outs[i];
        char monn[24];
        g_snprintf (monn, sizeof monn, "mon%d", i);
        o->id = src->name;
        walpe_output_set_names (o, src->cur_name ? src->cur_name : monn, src->cur_make,
                                src->cur_model, NULL, src->cur_desc);
        o->x = rects[i].x;
        o->y = rects[i].y;
        o->width = rects[i].width;
        o->height = rects[i].height;
        o->logical_x = rects[i].lx;
        o->logical_y = rects[i].ly;
        o->logical_width = rects[i].lw;
        o->logical_height = rects[i].lh;
        o->refresh_hz = rects[i].refresh_hz;
        o->scale = (double) rects[i].scale120 / WL_SCALE_DEN;
        o->transform = src->cur.transform;
    }
    g_free (rects);
    g_free (outs);
    g_free (props);
}

/* ---------------------------------------------------------------- pointer */

static WlSurf *wl_surf_by_wl_surface (struct wl_surface *ws)
{
    if (!ws)
        return NULL;
    for (GList *l = W.surfaces; l; l = l->next)
        if (((WlSurf *) l->data)->surface == ws)
            return l->data;
    return NULL;
}

/* wl_surface.set_buffer_scale exists from version 3 of wl_surface on; a
 * surface has the version wl_compositor was bound at, which is the
 * compositor's when that is lower than 4 */
#ifndef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
#define WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION 3
#endif
static gboolean wl_surface_has_scale (struct wl_surface *ws)
{
    return wl_proxy_get_version ((struct wl_proxy *) ws) >=
           WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
}

/* a position on a surface as a pixel of the output list's space */
static void wl_ptr_place (WlSurf *s, wl_fixed_t fx, wl_fixed_t fy)
{
    g_mutex_lock (&s->lock);
    int lw = s->size.dst_w, lh = s->size.dst_h;
    g_mutex_unlock (&s->lock);
    wl_pointer_to_output (wl_fixed_to_double (fx), wl_fixed_to_double (fy), lw, lh, s->out_x,
                          s->out_y, s->out_w, s->out_h, &W.ptr.x, &W.ptr.y);
    W.ptr.seen = TRUE;
}

/* The pointer came onto a wallpaper: it needs an image, or it keeps the
 * one of the window it came from. The theme's default arrow, at the
 * output's integer scale. */
static void wl_cursor_show (WlSeat *seat, guint32 serial, WlSurf *s)
{
#ifdef HAVE_WL_CURSOR
    if (!W.shm || !W.compositor || !seat->pointer)
        return;
    int scale = 1;
    g_mutex_lock (&s->lock);
    scale = CLAMP (s->size_in.int_scale, 1, 8);
    g_mutex_unlock (&s->lock);
    if (!W.cursor_theme || W.cursor_theme_scale != scale) {
        if (W.cursor_theme)
            wl_cursor_theme_destroy (W.cursor_theme);
        W.cursor_theme = wl_cursor_theme_load (W.cursor_theme_name, W.cursor_size * scale, W.shm);
        W.cursor_theme_scale = scale;
    }
    if (!W.cursor_theme)
        return;
    struct wl_cursor *c = wl_cursor_theme_get_cursor (W.cursor_theme, "default");
    if (!c)
        c = wl_cursor_theme_get_cursor (W.cursor_theme, "left_ptr");
    if (!c || c->image_count < 1)
        return;
    struct wl_cursor_image *img = c->images[0];
    struct wl_buffer *buf = wl_cursor_image_get_buffer (img);
    if (!buf)
        return;
    if (!W.cursor_surface)
        W.cursor_surface = wl_compositor_create_surface (W.compositor);
    if (!W.cursor_surface)
        return;
    /* A buffer scale needs a buffer whose width and height are multiples
     * of it (wl_surface error invalid_size otherwise), and a theme hands
     * out the nearest size it has, not the one asked for (a 64 pixel image
     * at scale 3). Such an image is shown unscaled. */
    gboolean can_scale = wl_surface_has_scale (W.cursor_surface);
    if (!can_scale || img->width % (uint32_t) scale || img->height % (uint32_t) scale)
        scale = 1;
    wl_pointer_set_cursor (seat->pointer, serial, W.cursor_surface, (int32_t) img->hotspot_x / scale,
                           (int32_t) img->hotspot_y / scale);
    if (can_scale)
        wl_surface_set_buffer_scale (W.cursor_surface, scale);
    wl_surface_attach (W.cursor_surface, buf, 0, 0);
    wl_surface_damage (W.cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit (W.cursor_surface);
#else
    (void) seat;
    (void) serial;
    (void) s;
#endif
}

/* the daemon's pointer is this seat's from now on (seat->over is set) */
static void wl_ptr_show_seat (WlSeat *seat)
{
    W.ptr.seat = seat;
    W.ptr.inside = TRUE;
    W.ptr.surf = seat->over;
    wl_ptr_place (seat->over, seat->px, seat->py);
    W.motion_dirty = TRUE;
}

/* The seat's pointer is on none of our surfaces any more: it left, the
 * seat lost its pointer, or the seat is gone. Only the seat whose pointer
 * the daemon is shown changes anything for the daemon - another seat's
 * leave does not take the pointer off the wallpaper - and when a further
 * seat still has its pointer on one, that one is shown instead. */
static void wl_ptr_seat_out (WlSeat *seat)
{
    seat->over = NULL;
    if (W.ptr.seat != seat)
        return;
    W.ptr.seat = NULL;
    for (GList *l = W.seats; l; l = l->next) {
        WlSeat *other = l->data;
        if (other != seat && other->over) {
            wl_ptr_show_seat (other);
            return;
        }
    }
    /* the last position stays; the daemon is told that the pointer is out
     * of sight (WalpePointer.inside) */
    W.ptr.inside = FALSE;
    W.ptr.surf = NULL;
    W.motion_dirty = TRUE;
}

static void wl_ptr_enter (void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *ws,
                          wl_fixed_t x, wl_fixed_t y)
{
    WlSeat *seat = data;
    WlSurf *s = wl_surf_by_wl_surface (ws);
    if (!s)
        return;
    seat->over = s;
    seat->px = x;
    seat->py = y;
    wl_ptr_show_seat (seat);
    wl_cursor_show (seat, serial, s);
}

static void wl_ptr_leave (void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *ws)
{
    wl_ptr_seat_out (data);
}

static void wl_ptr_motion (void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
                           wl_fixed_t y)
{
    WlSeat *seat = data;
    if (!seat->over)
        return;
    seat->px = x;
    seat->py = y;
    wl_ptr_show_seat (seat);
}

static void wl_ptr_queue_button (int button, gboolean down, guint32 time_ms)
{
    if (!button || !W.sel_buttons || !W.ptr.seen)
        return;
    WlEv e = { .kind = WL_EVK_BUTTON, .button = button, .down = down, .x = W.ptr.x, .y = W.ptr.y,
               .time_ms = time_ms };
    wl_push_event (&e);
}

static void wl_ptr_button (void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    WlSeat *seat = data;
    if (!seat->over)
        return;
    if (W.ptr.seat != seat)
        wl_ptr_show_seat (seat);        /* the click is where this seat's pointer is */
    wl_ptr_queue_button (wl_button_number (button), state == WL_POINTER_BUTTON_STATE_PRESSED, time);
}

/* the axis events collected so far become wheel clicks: one "down" per
 * click, as the daemon gets them for X11's wheel buttons */
static void wl_ptr_axis_flush (WlSeat *seat)
{
    if (!seat->ax_any)
        return;
    for (int a = 0; a < 2; a++) {
        int clicks = wl_axis_clicks (&seat->acc, a, seat->ax_value[a], seat->ax_discrete[a],
                                     seat->ax_v120[a], seat->ax_stop[a]);
        int b = wl_axis_button (a, clicks);
        int n = b && seat->over ? MIN (ABS (clicks), WL_WHEEL_BURST_MAX) : 0;
        if (n && W.ptr.seat != seat)
            wl_ptr_show_seat (seat);
        for (int i = 0; i < n; i++)
            wl_ptr_queue_button (b, TRUE, seat->ax_time);
        seat->ax_value[a] = 0;
        seat->ax_discrete[a] = seat->ax_v120[a] = 0;
        seat->ax_stop[a] = FALSE;
    }
    seat->ax_any = FALSE;
}

static void wl_ptr_axis (void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
                         wl_fixed_t value)
{
    WlSeat *seat = data;
    if (axis > 1)
        return;
    seat->ax_value[axis] += wl_fixed_to_double (value);
    seat->ax_time = time;
    seat->ax_any = TRUE;
    if (seat->version < 5)
        wl_ptr_axis_flush (seat);       /* no frame event before version 5 */
}

static void wl_ptr_frame (void *data, struct wl_pointer *p)
{
    wl_ptr_axis_flush (data);
}

static void wl_ptr_axis_source (void *data, struct wl_pointer *p, uint32_t source)
{
}

static void wl_ptr_axis_stop (void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
    WlSeat *seat = data;
    if (axis > 1)
        return;
    seat->ax_stop[axis] = TRUE;
    seat->ax_any = TRUE;
}

static void wl_ptr_axis_discrete (void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
    WlSeat *seat = data;
    if (axis > 1)
        return;
    seat->ax_discrete[axis] = (int) CLAMP ((gint64) seat->ax_discrete[axis] + discrete,
                                           -WL_AXIS_STEPS_MAX, WL_AXIS_STEPS_MAX);
    seat->ax_any = TRUE;
}

#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
static void wl_ptr_axis_value120 (void *data, struct wl_pointer *p, uint32_t axis, int32_t value120)
{
    WlSeat *seat = data;
    if (axis > 1)
        return;
    seat->ax_v120[axis] = (int) CLAMP ((gint64) seat->ax_v120[axis] + value120,
                                       -120 * WL_AXIS_STEPS_MAX, 120 * WL_AXIS_STEPS_MAX);
    seat->ax_any = TRUE;
}
#endif

#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void wl_ptr_axis_relative_direction (void *data, struct wl_pointer *p, uint32_t axis,
                                            uint32_t direction)
{
}
#endif

static const struct wl_pointer_listener wl_ptr_listener = {
    .enter = wl_ptr_enter,
    .leave = wl_ptr_leave,
    .motion = wl_ptr_motion,
    .button = wl_ptr_button,
    .axis = wl_ptr_axis,
    .frame = wl_ptr_frame,
    .axis_source = wl_ptr_axis_source,
    .axis_stop = wl_ptr_axis_stop,
    .axis_discrete = wl_ptr_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    .axis_value120 = wl_ptr_axis_value120,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    .axis_relative_direction = wl_ptr_axis_relative_direction,
#endif
};

static void wl_seat_drop_pointer (WlSeat *seat)
{
    if (!seat->pointer)
        return;
#ifdef WL_POINTER_RELEASE_SINCE_VERSION
    if (seat->version >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release (seat->pointer);
    else
#endif
        wl_pointer_destroy (seat->pointer);
    seat->pointer = NULL;
}

static void wl_seat_capabilities (void *data, struct wl_seat *ws, uint32_t caps)
{
    WlSeat *seat = data;
    gboolean has = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has && !seat->pointer) {
        seat->pointer = wl_seat_get_pointer (seat->seat);
        wl_pointer_add_listener (seat->pointer, &wl_ptr_listener, seat);
    } else if (!has && seat->pointer) {
        wl_seat_drop_pointer (seat);
        wl_ptr_seat_out (seat);
    }
}

static void wl_seat_name (void *data, struct wl_seat *ws, const char *name)
{
}

static const struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

static void wl_seat_add (guint32 name, guint32 version)
{
    /* no higher than the listener above knows events for */
    guint32 known = 7;
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    known = 8;
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    known = 9;
#endif
    WlSeat *seat = g_new0 (WlSeat, 1);
    seat->name = name;
    seat->version = MIN (MIN (version, known), (guint32) wl_seat_interface.version);
    seat->seat = wl_registry_bind (W.registry, name, &wl_seat_interface, seat->version);
    wl_seat_add_listener (seat->seat, &wl_seat_listener, seat);
    W.seats = g_list_append (W.seats, seat);
}

static void wl_seat_free (WlSeat *seat)
{
    wl_seat_drop_pointer (seat);
#ifdef WL_SEAT_RELEASE_SINCE_VERSION
    if (seat->version >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release (seat->seat);
    else
#endif
        wl_seat_destroy (seat->seat);
    g_free (seat);
}

/* --------------------------------------------------------------- registry */

/* WALPE_WL_IGNORE=name[,name...] (testing): the globals of these
 * interfaces are treated as if the compositor did not have them, e.g.
 * "wp_viewporter" to exercise the integer-scale path, or
 * "zwlr_layer_shell_v1" to see what a compositor without layer shell
 * gives. list: the variable's value, NULL = none. */
static gboolean wl_ignored (const char *list, const char *interface)
{
    gsize n = strlen (interface);
    for (const char *p = list; p && *p;) {
        const char *end = strchr (p, ',');
        gsize len = end ? (gsize) (end - p) : strlen (p);
        if (len == n && !strncmp (p, interface, n))
            return TRUE;
        p = end ? end + 1 : p + len;
    }
    return FALSE;
}

/* The compositor names the clock its presentation time stamps are on, and
 * that number is what clock_gettime () is asked with to bring a stamp onto
 * the monotonic scale. Only the system's own time-of-day and monotonic
 * clocks are taken; any other number (a CPU-time clock, a clock device
 * named through a file descriptor number of this process) is not handed
 * on, and the stamps are then read as they come. */
static gboolean wl_clock_known (uint32_t clk)
{
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_REALTIME_COARSE
    case CLOCK_REALTIME_COARSE:
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    case CLOCK_MONOTONIC_COARSE:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
#ifdef CLOCK_TAI
    case CLOCK_TAI:
#endif
        return TRUE;
    default:
        return FALSE;
    }
}

static void wl_presentation_clock (void *data, struct wp_presentation *p, uint32_t clk)
{
    if (wl_clock_known (clk)) {
        W.pres_clock = (clockid_t) clk;
    } else {
        g_message ("wayland: the compositor presents on clock %u, which is none this program "
                   "reads: presentation time stamps are taken as monotonic", (unsigned) clk);
        W.pres_clock = CLOCK_MONOTONIC;
    }
}

static const struct wp_presentation_listener wl_presentation_listener = { wl_presentation_clock };

static void wl_wm_ping (void *data, struct xdg_wm_base *base, uint32_t serial)
{
    xdg_wm_base_pong (base, serial);
}

static const struct xdg_wm_base_listener wl_wm_listener = { wl_wm_ping };

static void wl_reg_global (void *data, struct wl_registry *reg, uint32_t name, const char *itf,
                           uint32_t version)
{
    WlGlobal *g = g_new0 (WlGlobal, 1);
    g->name = name;
    g->version = version;
    g->interface = g_strdup (itf);
    g_ptr_array_add (W.globals, g);
    if (wl_ignored (W.ignore, itf))
        return;

    if (!strcmp (itf, wl_compositor_interface.name)) {
        if (!W.compositor)
            W.compositor = wl_registry_bind (reg, name, &wl_compositor_interface, MIN (version, 4u));
    } else if (!strcmp (itf, wl_shm_interface.name)) {
        if (!W.shm)
            W.shm = wl_registry_bind (reg, name, &wl_shm_interface, 1);
    } else if (!strcmp (itf, wl_output_interface.name)) {
        wl_out_add (name, version);
    } else if (!strcmp (itf, wl_seat_interface.name)) {
        wl_seat_add (name, version);
    } else if (!strcmp (itf, zxdg_output_manager_v1_interface.name)) {
        if (!W.xdg_output_mgr) {
            W.xdg_output_version = MIN (version, 3u);
            W.xdg_output_name = name;
            W.xdg_output_mgr = wl_registry_bind (reg, name, &zxdg_output_manager_v1_interface,
                                                 W.xdg_output_version);
            for (GList *l = W.outputs; l; l = l->next)
                wl_out_get_xdg (l->data);
        }
    } else if (!strcmp (itf, zwlr_layer_shell_v1_interface.name)) {
        if (!W.layer_shell) {
            W.layer_shell_version = MIN (version, 4u);
            W.layer_shell_name = name;
            W.layer_shell = wl_registry_bind (reg, name, &zwlr_layer_shell_v1_interface,
                                              W.layer_shell_version);
            W.layer_shell_gone = FALSE;
        }
    } else if (!strcmp (itf, wp_viewporter_interface.name)) {
        if (!W.viewporter) {
            W.viewporter_name = name;
            W.viewporter = wl_registry_bind (reg, name, &wp_viewporter_interface, 1);
            W.outputs_dirty = TRUE;     /* the pixel size of an output depends on it */
        }
    } else if (!strcmp (itf, wp_fractional_scale_manager_v1_interface.name)) {
        if (!W.frac_mgr) {
            W.frac_name = name;
            W.frac_mgr = wl_registry_bind (reg, name, &wp_fractional_scale_manager_v1_interface, 1);
        }
    } else if (!strcmp (itf, wp_presentation_interface.name)) {
        if (!W.presentation) {
            W.presentation_name = name;
            W.presentation = wl_registry_bind (reg, name, &wp_presentation_interface, 1);
            wp_presentation_add_listener (W.presentation, &wl_presentation_listener, NULL);
        }
    } else if (!strcmp (itf, xdg_wm_base_interface.name)) {
        if (!W.wm_base) {
            W.wm_base_name = name;
            W.wm_base = wl_registry_bind (reg, name, &xdg_wm_base_interface, MIN (version, 3u));
            xdg_wm_base_add_listener (W.wm_base, &wl_wm_listener, NULL);
        }
    } else if (!strcmp (itf, zwp_linux_dmabuf_v1_interface.name)) {
        W.dmabuf_name = name;
        W.dmabuf_version = version;
    }
}

static void wl_reg_global_remove (void *data, struct wl_registry *reg, uint32_t name)
{
    WlGlobal *g = NULL;
    guint at = 0;
    for (guint i = 0; i < W.globals->len && !g; i++)
        if (((WlGlobal *) g_ptr_array_index (W.globals, i))->name == name) {
            g = g_ptr_array_index (W.globals, i);
            at = i;
        }
    if (!g)
        return;
    if (!strcmp (g->interface, wl_output_interface.name)) {
        WlOut *o = wl_out_by_id (name);
        if (o) {
            wl_surfaces_output_gone (o);
            W.outputs = g_list_remove (W.outputs, o);
            wl_out_free (o);
            W.outputs_dirty = TRUE;
        }
    } else if (!strcmp (g->interface, wl_seat_interface.name)) {
        for (GList *l = W.seats; l; l = l->next) {
            WlSeat *seat = l->data;
            if (seat->name != name)
                continue;
            /* no leave event follows for a seat that is gone: without this
             * its pointer would count as resting on the wallpaper for good
             * (the same as when a seat loses its pointer capability) */
            wl_ptr_seat_out (seat);
            W.seats = g_list_delete_link (W.seats, l);
            wl_seat_free (seat);
            break;
        }
    } else if (name == W.layer_shell_name) {
        /* the proxy is kept (surfaces hold wrappers of it) but nothing is
         * created through it any more */
        if (W.layer_shell && !W.layer_shell_gone) {
            W.layer_shell_gone = TRUE;
            wl_surfaces_shell_gone ();
        }
    } else if (name == W.xdg_output_name && W.xdg_output_mgr) {
        /* outputs keep the zxdg_output_v1 objects they have */
        zxdg_output_manager_v1_destroy (W.xdg_output_mgr);
        W.xdg_output_mgr = NULL;
        W.xdg_output_name = 0;
    } else if (name == W.viewporter_name && W.viewporter) {
        /* the surfaces keep their wp_viewport objects; an output's pixel
         * size is defined without it from now on, which the daemon sees
         * as a change of the layout */
        wp_viewporter_destroy (W.viewporter);
        W.viewporter = NULL;
        W.viewporter_name = 0;
        W.outputs_dirty = TRUE;
    } else if (name == W.frac_name && W.frac_mgr) {
        wp_fractional_scale_manager_v1_destroy (W.frac_mgr);
        W.frac_mgr = NULL;
        W.frac_name = 0;
    } else if (name == W.presentation_name) {
        /* kept, like the layer shell (wrappers); no feedback is asked for any more */
        g_atomic_int_set (&W.no_feedback, 1);
    } else if (name == W.wm_base_name) {
        W.wm_base_gone = TRUE;
    }
    g_ptr_array_remove_index (W.globals, at);
}

static const struct wl_registry_listener wl_reg_listener = { wl_reg_global, wl_reg_global_remove };

/* ---------------------------------------------- the compositor's DRM device
 * zwp_linux_dmabuf_v1 (version 4) tells the device the compositor
 * composes with as a device number; the node is found by comparing it
 * with the character devices in /dev/dri. Asked on a queue of its own so
 * that nothing else is dispatched while waiting. */
typedef struct {
    gboolean done;
    dev_t dev;
    gboolean have_dev;
} WlDmabufAsk;

static void wl_fb_done (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb)
{
    ((WlDmabufAsk *) data)->done = TRUE;
}

static void wl_fb_format_table (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb, int32_t fd,
                                uint32_t size)
{
    close (fd);
}

static void wl_fb_main_device (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
                               struct wl_array *dev)
{
    WlDmabufAsk *a = data;
    if (dev && dev->size == sizeof (dev_t)) {
        memcpy (&a->dev, dev->data, sizeof (dev_t));
        a->have_dev = TRUE;
    }
}

static void wl_fb_tranche_done (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb)
{
}

static void wl_fb_tranche_device (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
                                  struct wl_array *dev)
{
}

static void wl_fb_tranche_formats (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
                                   struct wl_array *idx)
{
}

static void wl_fb_tranche_flags (void *data, struct zwp_linux_dmabuf_feedback_v1 *fb, uint32_t flags)
{
}

static const struct zwp_linux_dmabuf_feedback_v1_listener wl_fb_listener = {
    .done = wl_fb_done,
    .format_table = wl_fb_format_table,
    .main_device = wl_fb_main_device,
    .tranche_done = wl_fb_tranche_done,
    .tranche_target_device = wl_fb_tranche_device,
    .tranche_formats = wl_fb_tranche_formats,
    .tranche_flags = wl_fb_tranche_flags,
};

static char *wl_drm_node_of (dev_t dev)
{
    char *found = NULL;
    GDir *d = g_dir_open ("/dev/dri", 0, NULL);
    const char *n;
    while (d && !found && (n = g_dir_read_name (d))) {
        char *path = g_build_filename ("/dev/dri", n, NULL);
        struct stat st;
        if (stat (path, &st) == 0 && S_ISCHR (st.st_mode) && st.st_rdev == dev)
            found = path;
        else
            g_free (path);
    }
    if (d)
        g_dir_close (d);
    return found;
}

/* dpy / reg: any connection whose registry announced the global `name` at
 * `version`. NULL = not told (older than version 4, or no answer by
 * deadline_us). */
static char *wl_dmabuf_device (struct wl_display *dpy, struct wl_registry *reg, guint32 name,
                               guint32 version, gint64 deadline_us)
{
    if (!name || version < 4)
        return NULL;
    struct wl_event_queue *q = wl_display_create_queue (dpy);
    struct wl_registry *wrap = q ? wl_proxy_create_wrapper (reg) : NULL;
    if (!wrap) {
        if (q)
            wl_event_queue_destroy (q);
        return NULL;
    }
    wl_proxy_set_queue ((struct wl_proxy *) wrap, q);
    struct zwp_linux_dmabuf_v1 *dm = wl_registry_bind (wrap, name, &zwp_linux_dmabuf_v1_interface, 4);
    wl_proxy_wrapper_destroy (wrap);
    WlDmabufAsk ask = { 0 };
    struct zwp_linux_dmabuf_feedback_v1 *fb = zwp_linux_dmabuf_v1_get_default_feedback (dm);
    zwp_linux_dmabuf_feedback_v1_add_listener (fb, &wl_fb_listener, &ask);
    for (int i = 0; i < 3 && !ask.done; i++)
        if (wl_sync_until (dpy, q, deadline_us) <= 0)
            break;
    zwp_linux_dmabuf_feedback_v1_destroy (fb);
    zwp_linux_dmabuf_v1_destroy (dm);
    wl_event_queue_destroy (q);
    return ask.have_dev ? wl_drm_node_of (ask.dev) : NULL;
}

/* --------------------------------------------------------------- surfaces */

/* frames may be presented on it (lock held) */
static gboolean wl_surf_ready_locked (const WlSurf *s)
{
    return s->role != WL_ROLE_NONE && wl_ls_can_present (&s->ls);
}

/* The role object is gone (closed by the compositor, its output removed,
 * or the surface is being freed). The wl_surface itself stays: the EGL
 * window is bound to it, and a commit on a surface without a role is
 * harmless. The outstanding frame callback is dropped with it: a surface
 * that is not mapped is not sent one. (lock held) */
static void wl_surface_role_gone_locked (WlSurf *s)
{
    if (s->layer) {
        zwlr_layer_surface_v1_destroy (s->layer);
        s->layer = NULL;
    }
    if (s->toplevel) {
        xdg_toplevel_destroy (s->toplevel);
        s->toplevel = NULL;
    }
    if (s->xdg) {
        xdg_surface_destroy (s->xdg);
        s->xdg = NULL;
    }
    s->role_on_rq = FALSE;
    if (s->frame_cb) {
        wl_callback_destroy (s->frame_cb);
        s->frame_cb = NULL;
    }
    wl_frame_reset (&s->fs);
}

/* the logical size changed (a configure). The buffers keep their size
 * once the EGL window exists - the daemon renders at the size it was
 * given - and only the size they are shown at follows. (lock held) */
static void wl_surface_resized_locked (WlSurf *s)
{
    WlSizeOut n;
    wl_surface_sizes (&s->size_in, &n);
    if (!s->egl_window) {
        s->size = n;
        return;
    }
    if (n.dst_w != s->size.dst_w || n.dst_h != s->size.dst_h) {
        s->size.dst_w = n.dst_w;
        s->size.dst_h = n.dst_h;
        s->size_dirty = TRUE;
    }
}

/* what the state machine answered (lock held) */
static void wl_ls_apply_locked (WlSurf *s, guint act)
{
    if (act & WL_LS_ACT_DESTROY_ROLE)
        wl_surface_role_gone_locked (s);
    s->todo |= act & (WL_LS_ACT_REPORT_CLOSED | WL_LS_ACT_ASK_FRAME | WL_LS_ACT_RECREATED);
}

/* ---- role listeners: either thread (see the top of the file) */

static void wl_layer_configure (void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                                uint32_t w, uint32_t h)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (s->layer == ls) {
        guint act = wl_ls_event (&s->ls, WL_LS_EV_CONFIGURE, serial, (int) w, (int) h,
                                 g_get_monotonic_time ());
        /* what the state machine took (it drops an out-of-date configure) */
        s->size_in.conf_w = s->ls.conf_w;
        s->size_in.conf_h = s->ls.conf_h;
        wl_surface_resized_locked (s);
        wl_ls_apply_locked (s, act);
    }
    g_mutex_unlock (&s->lock);
    wl_attention ();
}

static void wl_layer_closed (void *data, struct zwlr_layer_surface_v1 *ls)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (s->layer == ls)
        wl_ls_apply_locked (s, wl_ls_event (&s->ls, WL_LS_EV_CLOSED, 0, 0, 0,
                                            g_get_monotonic_time ()));
    g_mutex_unlock (&s->lock);
    wl_attention ();
}

static const struct zwlr_layer_surface_v1_listener wl_layer_listener = {
    .configure = wl_layer_configure,
    .closed = wl_layer_closed,
};

static void wl_xdg_configure (void *data, struct xdg_surface *xs, uint32_t serial)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (s->xdg == xs) {
        guint act = wl_ls_event (&s->ls, WL_LS_EV_CONFIGURE, serial, s->top_w, s->top_h,
                                 g_get_monotonic_time ());
        s->size_in.conf_w = s->ls.conf_w;
        s->size_in.conf_h = s->ls.conf_h;
        wl_surface_resized_locked (s);
        wl_ls_apply_locked (s, act);
    }
    g_mutex_unlock (&s->lock);
    wl_attention ();
}

static const struct xdg_surface_listener wl_xdg_listener = { wl_xdg_configure };

static void wl_top_configure (void *data, struct xdg_toplevel *tl, int32_t w, int32_t h,
                              struct wl_array *states)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (s->toplevel == tl) {
        s->top_w = w;
        s->top_h = h;
    }
    g_mutex_unlock (&s->lock);
}

static void wl_top_close (void *data, struct xdg_toplevel *tl)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (s->toplevel == tl)
        wl_ls_apply_locked (s, wl_ls_event (&s->ls, WL_LS_EV_CLOSED, 0, 0, 0,
                                            g_get_monotonic_time ()));
    g_mutex_unlock (&s->lock);
    wl_attention ();
}

/* bound at version 3 at most: the later events are never sent */
static const struct xdg_toplevel_listener wl_top_listener = {
    .configure = wl_top_configure,
    .close = wl_top_close,
};

static void wl_frac_scale (void *data, struct wp_fractional_scale_v1 *f, uint32_t scale)
{
    /* recorded for `status`; sizes come from the output's properties */
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    s->size_in.scale120 = (int) scale;
    g_mutex_unlock (&s->lock);
}

static const struct wp_fractional_scale_v1_listener wl_frac_listener = { wl_frac_scale };

/* ---- frame callback and presentation feedback: delivered to rq */

static void wl_frame_cb_done (void *data, struct wl_callback *cb, uint32_t time)
{
    WlSurf *s = data;
    gboolean shown = FALSE;
    g_mutex_lock (&s->lock);
    if (s->frame_cb == cb) {
        s->frame_cb = NULL;
        shown = wl_frame_done (&s->fs);
        wl_callback_destroy (cb);
    }
    g_mutex_unlock (&s->lock);
    if (shown)
        wl_attention ();
}

static const struct wl_callback_listener wl_frame_cb_listener = { wl_frame_cb_done };

/* the feedback's time stamp on the scale of g_get_monotonic_time () */
static gint64 wl_presentation_to_mono (gint64 t_us)
{
    if (W.pres_clock == CLOCK_MONOTONIC)
        return t_us;
    struct timespec a, b;
    if (clock_gettime (W.pres_clock, &a) != 0 || clock_gettime (CLOCK_MONOTONIC, &b) != 0)
        return t_us;
    gint64 au = (gint64) a.tv_sec * G_USEC_PER_SEC + a.tv_nsec / 1000;
    gint64 bu = (gint64) b.tv_sec * G_USEC_PER_SEC + b.tv_nsec / 1000;
    return t_us + (bu - au);
}

/* TRUE = fb was outstanding and is no longer (lock held) */
static gboolean wl_feedback_take_locked (WlSurf *s, struct wp_presentation_feedback *fb)
{
    GSList *l = g_slist_find (s->feedbacks, fb);
    if (!l)
        return FALSE;
    s->feedbacks = g_slist_delete_link (s->feedbacks, l);
    s->n_feedback--;
    return TRUE;
}

static void wl_feedback_sync_output (void *data, struct wp_presentation_feedback *fb,
                                     struct wl_output *out)
{
}

static void wl_feedback_presented (void *data, struct wp_presentation_feedback *fb, uint32_t sec_hi,
                                   uint32_t sec_lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi,
                                   uint32_t seq_lo, uint32_t flags)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (wl_feedback_take_locked (s, fb)) {
        s->stats.presented++;
        s->pres_flags = (int) flags;
        s->stats.last_presented_us = wl_presentation_to_mono (wl_presented_us (sec_hi, sec_lo, nsec));
        if (refresh)
            s->stats.refresh_hz = wl_refresh_hz_from_ns (refresh);
        wp_presentation_feedback_destroy (fb);
    }
    g_mutex_unlock (&s->lock);
}

static void wl_feedback_discarded (void *data, struct wp_presentation_feedback *fb)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    if (wl_feedback_take_locked (s, fb)) {
        s->stats.discarded++;
        wp_presentation_feedback_destroy (fb);
    }
    g_mutex_unlock (&s->lock);
}

static const struct wp_presentation_feedback_listener wl_feedback_listener = {
    .sync_output = wl_feedback_sync_output,
    .presented = wl_feedback_presented,
    .discarded = wl_feedback_discarded,
};

/* ---- giving a surface its role */

/* Create the layer role and commit it without a buffer; the compositor
 * answers with a configure, which arrives on rq. (lock held) */
static gboolean wl_layer_create_locked (WlSurf *s, gint64 now_us)
{
    if (!s->shell_rq || !s->wl_out)
        return FALSE;
    s->layer = zwlr_layer_shell_v1_get_layer_surface (
        s->shell_rq, s->surface, s->wl_out,
        W.layer == WL_LAYER_BOTTOM ? ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM
                                   : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "walpe");
    if (!s->layer)
        return FALSE;
    s->role_on_rq = TRUE;
    zwlr_layer_surface_v1_add_listener (s->layer, &wl_layer_listener, s);
    zwlr_layer_surface_v1_set_size (s->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor (s->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone (s->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity (
        s->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit (s->surface);
    wl_ls_event (&s->ls, WL_LS_EV_CREATE, 0, 0, 0, now_us);
    return TRUE;
}

/* the same for the plain window of --windowed (lock held) */
static gboolean wl_toplevel_create_locked (WlSurf *s, gint64 now_us)
{
    if (!s->wm_base_rq)
        return FALSE;
    s->xdg = xdg_wm_base_get_xdg_surface (s->wm_base_rq, s->surface);
    if (!s->xdg)
        return FALSE;
    xdg_surface_add_listener (s->xdg, &wl_xdg_listener, s);
    s->toplevel = xdg_surface_get_toplevel (s->xdg);
    xdg_toplevel_add_listener (s->toplevel, &wl_top_listener, s);
    xdg_toplevel_set_title (s->toplevel, s->title ? s->title : "WalPE");
    xdg_toplevel_set_app_id (s->toplevel, "walpe");
    s->role_on_rq = TRUE;
    wl_surface_commit (s->surface);
    wl_ls_event (&s->ls, WL_LS_EV_CREATE, 0, 0, 0, now_us);
    return TRUE;
}

static void wl_role_ack_locked (WlSurf *s)
{
    if (!s->ls.ack_due)
        return;
    if (s->layer)
        zwlr_layer_surface_v1_ack_configure (s->layer, s->ls.serial);
    else if (s->xdg)
        xdg_surface_ack_configure (s->xdg, s->ls.serial);
    s->ls.ack_due = FALSE;
}

static gboolean wl_pred_role_settled (gpointer data)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    gboolean r = s->ls.state != WL_LS_WAIT;
    g_mutex_unlock (&s->lock);
    return r;
}

/* The wait for the first configure is over. Configured: acknowledge, and
 * hand the role object's events to the main thread from now on (events
 * that were already queued on rq are still delivered there; the listeners
 * do not care which thread runs them). Not configured: the role object is
 * destroyed again, so that a swap cannot attach a buffer to a surface that
 * was never configured, which the protocol forbids. (lock held) */
static gboolean wl_role_handshake_end_locked (WlSurf *s, gint64 now_us)
{
    if (s->ls.state == WL_LS_WAIT)
        wl_ls_apply_locked (s, wl_ls_event (&s->ls, WL_LS_EV_TIMEOUT, 0, 0, 0, now_us));
    if (!wl_ls_can_present (&s->ls))
        return FALSE;
    if (s->role_on_rq) {
        if (s->layer)
            wl_proxy_set_queue ((struct wl_proxy *) s->layer, NULL);
        if (s->xdg)
            wl_proxy_set_queue ((struct wl_proxy *) s->xdg, NULL);
        if (s->toplevel)
            wl_proxy_set_queue ((struct wl_proxy *) s->toplevel, NULL);
        s->role_on_rq = FALSE;
    }
    wl_role_ack_locked (s);
    return TRUE;
}

/* send the surface state that is waiting; it takes effect with the next
 * commit (lock held) */
static void wl_surface_apply_locked (WlSurf *s)
{
    if (s->size_dirty) {
        if (s->viewport)
            wp_viewport_set_destination (s->viewport, s->size.dst_w, s->size.dst_h);
        else if (wl_surface_has_scale (s->surface))
            wl_surface_set_buffer_scale (s->surface, s->size.buffer_scale);
        s->size_dirty = FALSE;
    }
    if (s->input_dirty) {
        /* a wallpaper takes pointer input only while its content uses the
         * pointer: with an empty input region every click and scroll goes
         * to whatever the compositor does on its bare desktop */
        if (s->kind == WALPE_SURFACE_WALLPAPER)
            wl_surface_set_input_region (s->surface, s->want_pointer ? NULL : s->empty_region);
        s->input_dirty = FALSE;
    }
}

/* Render thread, from pre_swap: the compositor closed the layer surface
 * while its output is still there. Between two swaps nothing is on its
 * way to the surface, so this is the one moment at which the role can be
 * made again: take the buffer off (a role is only given to a surface
 * without one), create, and wait a bounded time for the configure on rq.
 * The swap that follows maps the surface. */
static void wl_layer_recreate (WlSurf *s)
{
    g_mutex_lock (&s->rq_lock);
    g_mutex_lock (&s->lock);
    gint64 now = g_get_monotonic_time ();
    gboolean ok = wl_ls_wants_create (&s->ls, now) && s->wl_out != NULL;
    if (ok) {
        wl_surface_attach (s->surface, NULL, 0, 0);
        wl_surface_commit (s->surface);
        ok = wl_layer_create_locked (s, now);
    }
    g_mutex_unlock (&s->lock);
    if (ok) {
        if (!wl_wait_queue (s->rq, wl_pred_role_settled, s, now + WL_RECREATE_WAIT_US))
            wl_mark_dead ("layer surface re-creation");
        g_mutex_lock (&s->lock);
        if (wl_role_handshake_end_locked (s, g_get_monotonic_time ())) {
            /* surface state does not depend on the role, but say it again */
            s->size_dirty = TRUE;
            s->input_dirty = TRUE;
        }
        g_mutex_unlock (&s->lock);
    }
    g_mutex_unlock (&s->rq_lock);
    wl_attention ();
}

static EGLSurface wl_egl_make_surface (EGLDisplay dpy, EGLConfig conf, struct wl_egl_window *win)
{
    /* A config with alpha bits would let pixels with alpha < 1 blend with
     * whatever the compositor has behind the wallpaper. Where the EGL
     * offers it, the surface is presented as opaque regardless. */
    EGLint alpha = 0;
    eglGetConfigAttrib (dpy, conf, EGL_ALPHA_SIZE, &alpha);
    gboolean opaque = alpha > 0 && epoxy_has_egl_extension (dpy, "EGL_EXT_present_opaque");
    EGLint min_iv = 0;
    eglGetConfigAttrib (dpy, conf, EGL_MIN_SWAP_INTERVAL, &min_iv);
    if (min_iv > 0 && !W.interval_warned) {
        W.interval_warned = TRUE;
        g_warning ("wayland: this EGL config cannot swap without waiting (EGL_MIN_SWAP_INTERVAL "
                   "%d; a driver setting such as vblank_mode=3 does that): the EGL library will "
                   "wait for frame callbacks itself, and a wallpaper that is not shown can then "
                   "block its render thread", (int) min_iv);
    }
    if (alpha > 0 && !opaque && !W.alpha_warned) {
        W.alpha_warned = TRUE;
        g_message ("wayland: the EGL config has %d alpha bits and EGL_EXT_present_opaque is not "
                   "offered: pixels a wallpaper leaves translucent show what the compositor has "
                   "behind it", (int) alpha);
    }
    for (int pass = opaque ? 0 : 1; pass < 2; pass++) {
        EGLSurface sf;
        if (W.egl_how == WL_HOW_PLATFORM && W.egl_make_window) {
            const EGLAttrib at[] = { EGL_PRESENT_OPAQUE_EXT, EGL_TRUE, EGL_NONE };
            sf = W.egl_make_window (dpy, conf, win, pass == 0 ? at : NULL);
        } else {
            const EGLint at[] = { EGL_PRESENT_OPAQUE_EXT, EGL_TRUE, EGL_NONE };
            if (W.egl_how == WL_HOW_PLATFORM_EXT)
                sf = eglCreatePlatformWindowSurfaceEXT (dpy, conf, win, pass == 0 ? at : NULL);
            else
                sf = eglCreateWindowSurface (dpy, conf, (EGLNativeWindowType) win,
                                             pass == 0 ? at : NULL);
        }
        if (sf != EGL_NO_SURFACE)
            return sf;
        /* no EGL call after the last failure: the caller reads eglGetError */
    }
    return EGL_NO_SURFACE;
}

/* main thread; the surface's render thread has ended or never existed */
static void wl_surf_destroy (WlSurf *s)
{
    if (s->lost_timer)
        g_source_remove (s->lost_timer);
    W.surfaces = g_list_remove (W.surfaces, s);
    for (GList *l = W.seats; l; l = l->next)
        if (((WlSeat *) l->data)->over == s)
            ((WlSeat *) l->data)->over = NULL;
    if (W.ptr.surf == s) {
        W.ptr.surf = NULL;
        W.ptr.seat = NULL;
        W.ptr.inside = FALSE;
    }
    /* (its COVERED value is not taken back here: this runs inside a call
     * of the daemon, which must not be answered with a signal; the
     * registry drops a provider's values when it stops) */
    g_mutex_lock (&s->rq_lock);
    g_mutex_lock (&s->lock);
    if (s->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface (s->egl_dpy, s->egl_surface);
    if (s->egl_window)
        wl_egl_window_destroy (s->egl_window);
    wl_surface_role_gone_locked (s);
    for (GSList *l = s->feedbacks; l; l = l->next)
        wp_presentation_feedback_destroy (l->data);
    g_slist_free (s->feedbacks);
    s->feedbacks = NULL;
    if (s->frac)
        wp_fractional_scale_v1_destroy (s->frac);
    if (s->viewport)
        wp_viewport_destroy (s->viewport);
    if (s->empty_region)
        wl_region_destroy (s->empty_region);
    if (s->surface_rq)
        wl_proxy_wrapper_destroy (s->surface_rq);
    if (s->presentation_rq)
        wl_proxy_wrapper_destroy (s->presentation_rq);
    if (s->shell_rq)
        wl_proxy_wrapper_destroy (s->shell_rq);
    if (s->wm_base_rq)
        wl_proxy_wrapper_destroy (s->wm_base_rq);
    if (s->surface)
        wl_surface_destroy (s->surface);
    /* every object that delivered to rq is destroyed; events still queued
     * for them go with the queue */
    if (s->rq)
        wl_event_queue_destroy (s->rq);
    g_mutex_unlock (&s->lock);
    g_mutex_unlock (&s->rq_lock);
    g_mutex_clear (&s->lock);
    g_mutex_clear (&s->rq_lock);
    g_free (s->out_name);
    g_free (s->title);
    g_free (s);
}

static void *wl_wrap_on (void *proxy, struct wl_event_queue *q)
{
    void *w = proxy ? wl_proxy_create_wrapper (proxy) : NULL;
    if (w)
        wl_proxy_set_queue ((struct wl_proxy *) w, q);
    return w;
}

static WalpeSurface *wl_surface_new (const WalpeSurfaceSpec *spec, EGLDisplay dpy, EGLConfig conf,
                                     EGLSurface *egl_surface, int *w, int *h)
{
    if (egl_surface)
        *egl_surface = EGL_NO_SURFACE;
    if (!W.dpy || wl_is_dead () || !W.compositor || !spec)
        return NULL;
    WlOut *o = NULL;
    if (spec->kind == WALPE_SURFACE_WALLPAPER) {
        o = spec->output ? wl_out_by_id (spec->output->id) : NULL;
        if (!o) {
            g_warning ("wayland: output %s is not there any more: no surface",
                       spec->output && spec->output->name ? spec->output->name : "?");
            return NULL;
        }
        if (!W.layer_shell || W.layer_shell_gone) {
            g_warning ("wayland: the compositor has no layer shell (zwlr_layer_shell_v1): no "
                       "wallpaper surface");
            return NULL;
        }
    } else if (spec->kind == WALPE_SURFACE_WINDOW && (!W.wm_base || W.wm_base_gone)) {
        g_warning ("wayland: the compositor has no xdg_wm_base: no window");
        return NULL;
    }

    WlSurf *s = g_new0 (WlSurf, 1);
    g_mutex_init (&s->lock);
    g_mutex_init (&s->rq_lock);
    s->kind = spec->kind;
    s->user = spec->user;
    s->egl_dpy = dpy;
    s->egl_surface = EGL_NO_SURFACE;
    s->surface = wl_compositor_create_surface (W.compositor);
    s->rq = wl_display_create_queue (W.dpy);
    if (!s->surface || !s->rq) {
        wl_surf_destroy (s);
        return NULL;
    }
    s->surface_rq = wl_wrap_on (s->surface, s->rq);
    s->empty_region = wl_compositor_create_region (W.compositor);
    /* nothing behind a wallpaper needs drawing */
    struct wl_region *all = wl_compositor_create_region (W.compositor);
    wl_region_add (all, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region (s->surface, all);
    wl_region_destroy (all);
    if (spec->kind != WALPE_SURFACE_PROBE) {
        s->presentation_rq = wl_wrap_on (W.presentation, s->rq);
        if (W.viewporter)
            s->viewport = wp_viewporter_get_viewport (W.viewporter, s->surface);
        if (W.frac_mgr) {
            s->frac = wp_fractional_scale_manager_v1_get_fractional_scale (W.frac_mgr, s->surface);
            wp_fractional_scale_v1_add_listener (s->frac, &wl_frac_listener, s);
        }
    }
    s->size_in.viewporter = s->viewport != NULL;
    s->size_in.int_scale = 1;
    s->interval = 1;

    if (spec->kind == WALPE_SURFACE_WALLPAPER) {
        s->role = WL_ROLE_LAYER;
        s->shell_rq = wl_wrap_on (W.layer_shell, s->rq);
        s->wl_out = o->wo;
        s->out_id = o->name;
        s->out_name = g_strdup (spec->output->name);
        s->out_x = spec->output->x;
        s->out_y = spec->output->y;
        s->out_w = spec->output->width;
        s->out_h = spec->output->height;
        s->refresh_hz = spec->output->refresh_hz;
        s->size_in.out_w = s->out_w;
        s->size_in.out_h = s->out_h;
        /* (a wl_surface too old for set_buffer_scale shows a buffer one
         * to one: its buffers are then made for scale 1) */
        s->size_in.int_scale = wl_surface_has_scale (s->surface) ? wl_out_int_scale (&o->cur) : 1;
        wl_ls_init (&s->ls, W.recreate_max);
        wl_surface_set_input_region (s->surface, s->empty_region);
    } else {
        s->size_in.asked_w = spec->width > 0 ? spec->width : 960;
        s->size_in.asked_h = spec->height > 0 ? spec->height : 540;
        s->out_w = s->size_in.asked_w;
        s->out_h = s->size_in.asked_h;
        if (spec->kind == WALPE_SURFACE_WINDOW) {
            s->role = WL_ROLE_TOPLEVEL;
            s->wm_base_rq = wl_wrap_on (W.wm_base, s->rq);
            s->title = g_strdup (spec->title);
            wl_ls_init (&s->ls, 0);
        }
    }

    if (s->role != WL_ROLE_NONE) {
        gint64 now = g_get_monotonic_time ();
        g_mutex_lock (&s->rq_lock);
        g_mutex_lock (&s->lock);
        gboolean ok = s->role == WL_ROLE_LAYER ? wl_layer_create_locked (s, now)
                                               : wl_toplevel_create_locked (s, now);
        g_mutex_unlock (&s->lock);
        if (ok) {
            if (!wl_wait_queue (s->rq, wl_pred_role_settled, s, now + WL_FIRST_CONFIGURE_US))
                wl_mark_dead ("first configure");
            g_mutex_lock (&s->lock);
            ok = wl_role_handshake_end_locked (s, g_get_monotonic_time ());
            g_mutex_unlock (&s->lock);
        }
        g_mutex_unlock (&s->rq_lock);
        if (!ok) {
            g_warning ("wayland: the compositor did not configure the %s%s within %d s: no surface",
                       s->role == WL_ROLE_LAYER ? "wallpaper surface on " : "window",
                       s->out_name ? s->out_name : "",
                       (int) (WL_FIRST_CONFIGURE_US / G_USEC_PER_SEC));
            wl_surf_destroy (s);
            return NULL;
        }
    }

    g_mutex_lock (&s->lock);
    wl_surface_sizes (&s->size_in, &s->size);
    s->size_dirty = TRUE;
    wl_surface_apply_locked (s);
    g_mutex_unlock (&s->lock);
    s->egl_window = wl_egl_window_create (s->surface, s->size.buf_w, s->size.buf_h);
    if (!s->egl_window) {
        g_warning ("wayland: wl_egl_window_create (%d x %d) failed", s->size.buf_w, s->size.buf_h);
        wl_surf_destroy (s);
        return NULL;
    }
    s->egl_surface = wl_egl_make_surface (dpy, conf, s->egl_window);
    if (s->egl_surface == EGL_NO_SURFACE) {
        wl_surf_destroy (s);    /* makes no EGL call: eglGetError () keeps the reason */
        return NULL;
    }
    if (s->role == WL_ROLE_LAYER && s->out_w > 0 &&
        (s->size.buf_w != s->out_w || s->size.buf_h != s->out_h))
        g_message ("wayland: %s: the surface is %d x %d pixels, the output %d x %d (configured "
                   "%d x %d at scale %d, no viewporter)", s->out_name, s->size.buf_w, s->size.buf_h,
                   s->out_w, s->out_h, s->size.dst_w, s->size.dst_h, s->size.buffer_scale);
    W.surfaces = g_list_append (W.surfaces, s);
    if (egl_surface)
        *egl_surface = s->egl_surface;
    if (w)
        *w = s->size.buf_w;
    if (h)
        *h = s->size.buf_h;
    return (WalpeSurface *) s;
}

static void wl_surface_free (WalpeSurface *ws)
{
    if (ws)
        wl_surf_destroy ((WlSurf *) ws);
}

static void wl_surface_size (WalpeSurface *ws, int *w, int *h)
{
    WlSurf *s = (WlSurf *) ws;
    g_mutex_lock (&s->lock);
    if (w)
        *w = s->size.buf_w;
    if (h)
        *h = s->size.buf_h;
    g_mutex_unlock (&s->lock);
}

/* Render thread, between two frames. With a viewport the buffers may have
 * any size: the destination (the logical size) stays, the compositor
 * scales. The EGL library allocates the new size for the next frame. */
static gboolean wl_surface_set_buffer_size (WalpeSurface *ws, int w, int h)
{
    WlSurf *s = (WlSurf *) ws;
    if (!s->viewport || !s->egl_window || w > WL_BUFFER_MAX || h > WL_BUFFER_MAX)
        return FALSE;
    g_mutex_lock (&s->lock);
    s->size_in.user_w = w < 1 || h < 1 ? 0 : w;
    s->size_in.user_h = w < 1 || h < 1 ? 0 : h;
    WlSizeOut n;
    wl_surface_sizes (&s->size_in, &n);
    if (n.buf_w != s->size.buf_w || n.buf_h != s->size.buf_h) {
        s->size.buf_w = n.buf_w;
        s->size.buf_h = n.buf_h;
        wl_egl_window_resize (s->egl_window, n.buf_w, n.buf_h, 0, 0);
        s->size_dirty = TRUE;
    }
    g_mutex_unlock (&s->lock);
    return TRUE;
}

static guint32 wl_surface_output (WalpeSurface *ws)
{
    return ((WlSurf *) ws)->out_id;
}

static unsigned long wl_surface_native_id (WalpeSurface *ws)
{
    WlSurf *s = (WlSurf *) ws;
    return s->surface ? wl_proxy_get_id ((struct wl_proxy *) s->surface) : 0;
}

/* Main thread: the wallpaper on this surface uses the pointer, or not
 * (the daemon decides per wallpaper, and never asks for it when its
 * wayland_pointer setting is off). Recorded, and sent with the next
 * frame's commit; when no swap is in progress it is committed at once, so
 * that it also arrives while the wallpaper is paused. */
static void wl_surface_want_pointer (WalpeSurface *ws, gboolean want)
{
    WlSurf *s = (WlSurf *) ws;
    if (s->kind != WALPE_SURFACE_WALLPAPER)
        return;
    g_mutex_lock (&s->lock);
    if (s->want_pointer != !!want) {
        s->want_pointer = !!want;
        s->input_dirty = TRUE;
        if (!s->in_swap && wl_surf_ready_locked (s) && !wl_is_dead ()) {
            wl_surface_apply_locked (s);
            wl_surface_commit (s->surface);
        }
    }
    g_mutex_unlock (&s->lock);
}

/* ---- presentation: render thread */

static int wl_surface_set_interval (WalpeSurface *ws, int iv)
{
    WlSurf *s = (WlSurf *) ws;
    g_mutex_lock (&s->lock);
    s->interval = iv > 0 ? 1 : 0;
    g_mutex_unlock (&s->lock);
    /* the EGL swap never waits: a surface that is not shown gets no frame
     * callback, and a thread blocked inside the EGL library for one could
     * neither pause nor end. The wait is done in swapped (), bounded. */
    return 0;
}

static void wl_surface_pre_swap (WalpeSurface *ws)
{
    WlSurf *s = (WlSurf *) ws;
    if (s->role == WL_ROLE_NONE || wl_is_dead ())
        return;
    gint64 now = g_get_monotonic_time ();
    g_mutex_lock (&s->lock);
    /* still set: the swap announced last time was never reported back
     * (the daemon calls swapped () after a successful swap only) */
    if (s->in_swap)
        wl_frame_swap_missed (&s->fs, now);
    if (s->role == WL_ROLE_LAYER && wl_ls_wants_create (&s->ls, now)) {
        g_mutex_unlock (&s->lock);
        wl_layer_recreate (s);
        now = g_get_monotonic_time ();
        g_mutex_lock (&s->lock);
    }
    if (wl_surf_ready_locked (s)) {
        wl_role_ack_locked (s);
        wl_surface_apply_locked (s);
        if (wl_frame_pre_swap (&s->fs, now)) {
            s->frame_cb = wl_surface_frame (s->surface_rq);
            wl_callback_add_listener (s->frame_cb, &wl_frame_cb_listener, s);
        }
        if (s->presentation_rq && s->n_feedback < WL_FEEDBACK_MAX &&
            !g_atomic_int_get (&W.no_feedback)) {
            struct wp_presentation_feedback *fb =
                wp_presentation_feedback (s->presentation_rq, s->surface);
            if (fb) {
                wp_presentation_feedback_add_listener (fb, &wl_feedback_listener, s);
                s->feedbacks = g_slist_prepend (s->feedbacks, fb);
                s->n_feedback++;
            }
        }
    }
    s->in_swap = TRUE;
    g_mutex_unlock (&s->lock);
}

static gboolean wl_pred_frame_arrived (gpointer data)
{
    WlSurf *s = data;
    g_mutex_lock (&s->lock);
    gboolean r = !s->fs.cb_pending;
    g_mutex_unlock (&s->lock);
    return r;
}

static void wl_surface_swapped (WalpeSurface *ws)
{
    WlSurf *s = (WlSurf *) ws;
    if (s->role == WL_ROLE_NONE || wl_is_dead ())
        return;
    g_mutex_lock (&s->lock);
    s->in_swap = FALSE;
    /* what the main thread wanted while the swap was in progress, and no
     * further frame may come for a long time */
    if (s->input_dirty && wl_surf_ready_locked (s)) {
        wl_surface_apply_locked (s);
        wl_surface_commit (s->surface);
        /* sent now: the wait below returns without flushing when no frame
         * callback is outstanding any more, and this may be the last frame
         * of a wallpaper that is about to pause (a flush never blocks) */
        (void) wl_display_flush (W.dpy);
    }
    gint64 wait = wl_frame_wait_us (&s->fs, s->interval, s->refresh_hz);
    double hz = s->refresh_hz;
    g_mutex_unlock (&s->lock);

    /* interval 1: until the compositor has shown the frame, but never for
     * long. Interval 0: only what has arrived already. */
    gint64 now = g_get_monotonic_time ();
    g_mutex_lock (&s->rq_lock);
    gboolean ok = wl_wait_queue (s->rq, wl_pred_frame_arrived, s, wait > 0 ? now + wait : 0);
    g_mutex_unlock (&s->rq_lock);
    if (!ok) {
        wl_mark_dead ("frame callback");
        return;
    }
    g_mutex_lock (&s->lock);
    gboolean hid = wl_frame_after_swap (&s->fs, wait > 0, g_get_monotonic_time (), hz);
    g_mutex_unlock (&s->lock);
    if (hid)
        wl_attention ();        /* the main thread reports it (WALPE_SIG_COVERED) */
}

static gboolean wl_surface_present_stats (WalpeSurface *ws, WalpePresentStats *out)
{
    WlSurf *s = (WlSurf *) ws;
    g_mutex_lock (&s->lock);
    *out = s->stats;
    out->committed = s->fs.commits;
    out->wait_timeouts = s->fs.timeouts;
    out->hidden = s->fs.hidden || (s->role != WL_ROLE_NONE && !wl_ls_can_present (&s->ls));
    g_mutex_unlock (&s->lock);
    return TRUE;
}

/* ------------------------------------------- output and shell going away */

static void wl_out_changed (WlOut *o)
{
    W.outputs_dirty = TRUE;
    for (GList *l = W.surfaces; l; l = l->next) {
        WlSurf *s = l->data;
        double hz = wl_refresh_hz_from_mhz (o->cur.refresh_mhz);
        if (s->out_id != o->name || hz <= 0.0)
            continue;
        g_mutex_lock (&s->lock);
        s->refresh_hz = hz;
        g_mutex_unlock (&s->lock);
    }
}

static void wl_surfaces_output_gone (WlOut *o)
{
    for (GList *l = W.surfaces; l; l = l->next) {
        WlSurf *s = l->data;
        g_mutex_lock (&s->lock);
        if (s->wl_out == o->wo) {
            s->wl_out = NULL;
            wl_ls_apply_locked (s, wl_ls_event (&s->ls, WL_LS_EV_OUTPUT_GONE, 0, 0, 0,
                                                g_get_monotonic_time ()));
        }
        g_mutex_unlock (&s->lock);
    }
    g_atomic_int_set (&W.attention, 1);
}

static void wl_surfaces_shell_gone (void)
{
    for (GList *l = W.surfaces; l; l = l->next) {
        WlSurf *s = l->data;
        if (s->role != WL_ROLE_LAYER)
            continue;
        g_mutex_lock (&s->lock);
        wl_ls_apply_locked (s, wl_ls_event (&s->ls, WL_LS_EV_SHELL_GONE, 0, 0, 0,
                                            g_get_monotonic_time ()));
        g_mutex_unlock (&s->lock);
    }
    g_atomic_int_set (&W.attention, 1);
}

/* ------------------------------------------------- main thread follow-ups */

/* A layer surface is LOST: ask the daemon for a frame on it now and then
 * (it draws one even while that monitor is paused), because the render
 * thread makes the role again right before a swap. Nobody drawing for a
 * few seconds ends the attempt: the surface is reported closed. */
#define WL_LOST_TICK_MS 300
#define WL_LOST_TICKS   15

static gboolean wl_lost_tick (gpointer data)
{
    WlSurf *s = data;
    gpointer user = s->user;
    g_mutex_lock (&s->lock);
    gboolean lost = s->ls.state == WL_LS_LOST, gave_up = FALSE;
    if (lost && ++s->lost_ticks > WL_LOST_TICKS) {
        wl_ls_apply_locked (s, wl_ls_event (&s->ls, WL_LS_EV_GIVE_UP, 0, 0, 0,
                                            g_get_monotonic_time ()));
        gave_up = TRUE;
    }
    g_mutex_unlock (&s->lock);
    if (!lost || gave_up) {
        s->lost_timer = 0;
        wl_attention ();
        return G_SOURCE_REMOVE;
    }
    if (W.started && W.opts.events && W.opts.events->surface_exposed)
        W.opts.events->surface_exposed (user, W.opts.user);
    return G_SOURCE_CONTINUE;
}

static gboolean wl_surf_hidden (WlSurf *s, guint *todo, WlLsState *state)
{
    g_mutex_lock (&s->lock);
    gboolean hidden = s->fs.hidden || (s->role != WL_ROLE_NONE && !wl_ls_can_present (&s->ls));
    if (todo) {
        *todo = s->todo;
        s->todo = 0;
    }
    if (state)
        *state = s->ls.state;
    g_mutex_unlock (&s->lock);
    return hidden;
}

/* from the event source: what listeners and render threads left for the
 * main thread */
static void wl_service_surfaces (void)
{
    GList *copy = g_list_copy (W.surfaces);
    for (GList *l = copy; l; l = l->next) {
        WlSurf *s = l->data;
        if (!g_list_find (W.surfaces, s))
            continue;           /* freed by a callback below */
        /* its render thread is not waiting on the surface's queue: look at
         * it here, so that a paused wallpaper learns that it is shown again
         * and its feedbacks are taken */
        if (g_mutex_trylock (&s->rq_lock)) {
            if (wl_display_dispatch_queue_pending (W.dpy, s->rq) < 0)
                wl_mark_dead ("surface queue");
            g_mutex_unlock (&s->rq_lock);
        }
        guint todo = 0;
        WlLsState st = WL_LS_NONE;
        gboolean hidden = wl_surf_hidden (s, &todo, &st);
        if (todo & WL_LS_ACT_RECREATED)
            g_message ("wayland: %s: the compositor had closed the wallpaper surface; it has its "
                       "role again", s->out_name ? s->out_name : "window");
        if (todo & WL_LS_ACT_REPORT_CLOSED) {
            WlEv e = { .kind = WL_EVK_CLOSED, .surface_user = s->user };
            wl_push_event (&e);
            g_message ("wayland: %s: the surface is gone (%s)", s->out_name ? s->out_name : "window",
                       s->kind == WALPE_SURFACE_WINDOW ? "window closed"
                                                       : "closed by the compositor or output removed");
        }
        if (st == WL_LS_LOST && !s->lost_timer) {
            s->lost_ticks = 0;
            s->lost_timer = g_timeout_add (WL_LOST_TICK_MS, wl_lost_tick, s);
        }
        if (s->kind == WALPE_SURFACE_WALLPAPER && W.prov_started &&
            (!s->hidden_known || hidden != s->hidden_reported)) {
            s->hidden_known = TRUE;
            s->hidden_reported = hidden;
            walpe_signal_set (&wl_frame_provider, WALPE_SIG_COVERED, s->out_id, hidden);
        }
    }
    g_list_free (copy);
}

/* the backend's own pause signal: "the compositor is not showing the
 * surface" (no frame callback), reported per output as COVERED */
static gboolean wl_prov_start (const WalpeSignalProvider *self, char **why)
{
    W.prov_started = TRUE;
    for (GList *l = W.surfaces; l; l = l->next) {
        WlSurf *s = l->data;
        if (s->kind != WALPE_SURFACE_WALLPAPER)
            continue;
        s->hidden_reported = wl_surf_hidden (s, NULL, NULL);
        s->hidden_known = TRUE;
        walpe_signal_set (self, WALPE_SIG_COVERED, s->out_id, s->hidden_reported);
    }
    return TRUE;
}

static void wl_prov_stop (const WalpeSignalProvider *self)
{
    W.prov_started = FALSE;
}

static const WalpeSignalProvider wl_frame_provider = {
    .name = "wl-frame",
    .signals = WALPE_SIG_BIT (WALPE_SIG_COVERED),
    .backends = 1u << WALPE_BACKEND_WAYLAND,
    .start = wl_prov_start,
    .stop = wl_prov_stop,
};

/* ----------------------------------------------------------- event source */

/* tell the daemon what was collected; only ever called from the event
 * source's dispatch, never from inside a call the daemon makes */
static void wl_emit_pending (void)
{
    /* what ran before this in the same dispatch (the daemon's signal sink,
     * called from wl_service_surfaces) may have closed the backend: W is
     * cleared then, and there is nothing left to tell */
    if (!W.is_open)
        return;
    const WalpePlatformEvents *ev = W.opts.events;
    gpointer user = W.opts.user;
    if (wl_is_dead () && !W.lost_reported) {
        W.lost_reported = TRUE;
        if (W.pfd_added) {
            g_source_remove_poll (W.src, &W.pfd);
            W.pfd_added = FALSE;
            /* nobody writes revents any more: what the last poll said
             * (G_IO_OUT with a socket that was full) must not make every
             * later check () answer "ready" */
            W.pfd.revents = 0;
        }
        g_array_set_size (W.evq, 0);
        if (ev && ev->connection_lost)
            ev->connection_lost (user);
        return;
    }
    if (W.outputs_dirty) {
        W.outputs_dirty = FALSE;
        if (ev && W.layout_events && ev->outputs_changed)
            ev->outputs_changed (user);
        if (!W.is_open)
            return;
    }
    if (W.evq->len) {
        GArray *q = W.evq;
        W.evq = g_array_new (FALSE, FALSE, sizeof (WlEv));
        for (guint i = 0; ev && i < q->len && W.is_open; i++) {
            const WlEv *e = &g_array_index (q, WlEv, i);
            switch (e->kind) {
            case WL_EVK_BUTTON:
                if (ev->button && (!ev->button_wanted || ev->button_wanted (e->button, user)))
                    ev->button (e->button, e->down, e->x, e->y, TRUE, e->time_ms, user);
                break;
            case WL_EVK_CLOSED:
                if (ev->surface_closed)
                    ev->surface_closed (e->surface_user, user);
                break;
            }
        }
        g_array_unref (q);
        if (!W.is_open)
            return;
    }
    if (W.motion_dirty) {
        W.motion_dirty = FALSE;
        if (ev && W.sel_motion && ev->pointer_motion)
            ev->pointer_motion (user);
    }
}

static gboolean wl_src_pending (void)
{
    return (wl_is_dead () && !W.lost_reported) || W.outputs_dirty || W.motion_dirty ||
           (W.evq && W.evq->len) || g_atomic_int_get (&W.attention);
}

static gboolean wl_src_prepare (GSource *src, gint *timeout)
{
    *timeout = -1;
    if (W.src_reading) {
        /* a prepare without the check that belongs to it */
        wl_display_cancel_read (W.dpy);
        W.src_reading = FALSE;
    }
    if (wl_is_dead ())
        return !W.lost_reported;
    if (wl_src_pending ())
        return TRUE;
    if (wl_display_prepare_read (W.dpy) != 0)
        return TRUE;            /* events are queued: dispatch them */
    W.src_reading = TRUE;
    W.pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    if (wl_display_flush (W.dpy) < 0) {
        if (errno == EAGAIN) {
            W.pfd.events |= G_IO_OUT;   /* the socket is full: come back when it drains */
        } else {
            wl_display_cancel_read (W.dpy);
            W.src_reading = FALSE;
            wl_mark_dead ("flush");
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean wl_src_check (GSource *src)
{
    gboolean ready = FALSE;
    if (W.src_reading) {
        W.src_reading = FALSE;
        if (W.pfd.revents & G_IO_IN) {
            if (wl_display_read_events (W.dpy) < 0)
                wl_mark_dead ("read");
            ready = TRUE;
        } else {
            wl_display_cancel_read (W.dpy);
            if (W.pfd.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
                wl_mark_dead ("socket");
        }
    }
    if (W.pfd.revents & G_IO_OUT)
        ready = TRUE;
    return ready || wl_src_pending ();
}

static gboolean wl_src_dispatch (GSource *src, GSourceFunc cb, gpointer data)
{
    if (!wl_is_dead () && wl_display_dispatch_pending (W.dpy) < 0)
        wl_mark_dead ("dispatch");
    g_atomic_int_set (&W.attention, 0);
    if (!wl_is_dead ())
        wl_service_surfaces ();
    wl_emit_pending ();
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs wl_src_funcs = {
    .prepare = wl_src_prepare,
    .check = wl_src_check,
    .dispatch = wl_src_dispatch,
};

/* -------------------------------------------------------------------- EGL */

/* The version of the EGL client library (major * 10 + minor) before any
 * display exists; 0 = it does not say. EGL 1.5 answers the question, 1.4
 * answers it with NULL and the error EGL_BAD_DISPLAY - which is taken off
 * again here, so that it is not found later by somebody who asks why
 * something else failed. Asked directly: what the dispatch library's
 * epoxy_egl_version () does with that NULL depends on its release. */
static int wl_egl_client_version (void)
{
    const char *v = eglQueryString (EGL_NO_DISPLAY, EGL_VERSION);
    if (!v) {
        (void) eglGetError ();
        return 0;
    }
    char *end = NULL;
    gint64 major = g_ascii_strtoll (v, &end, 10);
    if (end == v || *end != '.')
        return 0;
    const char *m = end + 1;
    gint64 minor = g_ascii_strtoll (m, &end, 10);
    if (end == m || major < 1 || major > 9 || minor < 0 || minor > 9)
        return 0;
    return (int) (major * 10 + minor);
}

static EGLDisplay wl_egl_display (void)
{
    if (!W.dpy)
        return EGL_NO_DISPLAY;
    if (W.egl_dpy != EGL_NO_DISPLAY)
        return W.egl_dpy;
    gboolean wl_platform = epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_KHR_platform_wayland") ||
                           epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_wayland");
    EGLDisplay d = EGL_NO_DISPLAY;
    W.egl_how = WL_HOW_LEGACY;
    W.egl_make_window = NULL;
    if (wl_platform && wl_egl_client_version () >= 15) {
        /* by name (see WlEglGetDisplayFn): no display is current here */
        WlEglGetDisplayFn get = (WlEglGetDisplayFn) eglGetProcAddress ("eglGetPlatformDisplay");
        WlEglMakeWindowFn make =
            (WlEglMakeWindowFn) eglGetProcAddress ("eglCreatePlatformWindowSurface");
        if (get && make)
            d = get (EGL_PLATFORM_WAYLAND_KHR, W.dpy, NULL);
        if (d != EGL_NO_DISPLAY) {
            W.egl_how = WL_HOW_PLATFORM;
            W.egl_make_window = make;
        }
    }
    if (d == EGL_NO_DISPLAY && wl_platform &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_base")) {
        d = eglGetPlatformDisplayEXT (EGL_PLATFORM_WAYLAND_KHR, W.dpy, NULL);
        if (d != EGL_NO_DISPLAY)
            W.egl_how = WL_HOW_PLATFORM_EXT;
    }
    if (d == EGL_NO_DISPLAY)
        d = eglGetDisplay ((EGLNativeDisplayType) W.dpy);
    W.egl_dpy = d;
    return d;
}

static const char *wl_egl_note (void)
{
    switch (W.egl_how) {
    case WL_HOW_PLATFORM:     return ", Wayland (eglGetPlatformDisplay)";
    case WL_HOW_PLATFORM_EXT: return ", Wayland (eglGetPlatformDisplayEXT)";
    default:                  return ", Wayland (eglGetDisplay)";
    }
}

static unsigned long wl_egl_visual (void)
{
    return WL_VISUAL_XRGB8888;
}

/* a surface nobody will see, with an EGL window of 1 x 1: can the config
 * make a window surface, and can its swap run without waiting? */
static gboolean wl_egl_config_usable (EGLDisplay dpy, EGLConfig conf)
{
    if (!W.dpy || !W.compositor || wl_is_dead ())
        return FALSE;
    EGLint type = 0, min_iv = 0;
    eglGetConfigAttrib (dpy, conf, EGL_SURFACE_TYPE, &type);
    eglGetConfigAttrib (dpy, conf, EGL_MIN_SWAP_INTERVAL, &min_iv);
    if (!(type & EGL_WINDOW_BIT) || min_iv > 0)
        return FALSE;
    struct wl_surface *ws = wl_compositor_create_surface (W.compositor);
    struct wl_egl_window *win = ws ? wl_egl_window_create (ws, 1, 1) : NULL;
    gboolean ok = FALSE;
    if (win) {
        gboolean a_warned = W.alpha_warned, i_warned = W.interval_warned;
        W.alpha_warned = W.interval_warned = TRUE;      /* a trial: say nothing */
        EGLSurface sf = wl_egl_make_surface (dpy, conf, win);
        W.alpha_warned = a_warned;
        W.interval_warned = i_warned;
        ok = sf != EGL_NO_SURFACE;
        if (ok)
            eglDestroySurface (dpy, sf);
        else
            (void) eglGetError ();
        wl_egl_window_destroy (win);
    }
    if (ws)
        wl_surface_destroy (ws);
    return ok;
}

static const char *wl_display_node (void)
{
    return W.display_node;
}

/* ---------------------------------------------------------------- pointer */

static gboolean wl_pointer (WalpePointer *out)
{
    if (!W.ptr.seen)
        return FALSE;
    out->x = W.ptr.x;
    out->y = W.ptr.y;
    out->inside = W.ptr.inside;
    out->under = W.ptr.inside ? (guintptr) W.ptr.surf : 0;
    return TRUE;
}

/* a client only ever sees the pointer on its own surfaces, and a wallpaper
 * surface has nothing of the daemon's above it */
static gboolean wl_on_desktop (guintptr under)
{
    return under != 0;
}

static void wl_input_select (gboolean buttons, gboolean motion)
{
    W.sel_buttons = buttons;
    W.sel_motion = motion;
}

static gboolean wl_has_input_events (void)
{
    return TRUE;
}

/* ------------------------------------------------------------- life cycle */

static void wl_close (void);

static const char *wl_no_layer_shell_text (void)
{
    const char *desk = g_getenv ("XDG_CURRENT_DESKTOP");
    gboolean gnome = wl_global_find ("gtk_shell1") != NULL ||
                     (desk && (strstr (desk, "GNOME") || strstr (desk, "gnome")));
    /* (not "use the X11 backend": through Xwayland its window lies above
     * every other window - common/backend.h) */
    return gnome
        ? "this Wayland compositor (GNOME's Mutter) has no layer shell (zwlr_layer_shell_v1), the "
          "protocol a wallpaper surface needs. On GNOME use an X11 session where there is one (\"GNOME on Xorg\")"
        : "this Wayland compositor has no layer shell (zwlr_layer_shell_v1), the protocol a "
          "wallpaper surface needs. Use a compositor that offers it (sway, Hyprland, river, "
          "labwc, Wayfire, niri, KDE Plasma, COSMIC) or an X11 session";
}

/* The add-on for KDE Plasma (wayland_logic.h), as the files say. Main
 * thread (open, status). The tray asks for status again and again:
 * the settings of Plasma's desktops are read only when the file was
 * written since the last time. */
static WlPlasmaAddon wl_plasma_addon (void)
{
    static WlPlasmaAddon state;
    static gint64 seen = -2;
    const char *const *sys = g_get_system_data_dirs ();
    gboolean installed = FALSE;
    for (int i = -1; !installed && (i < 0 || sys[i]); i++) {
        char *f = g_build_filename (i < 0 ? g_get_user_data_dir () : sys[i], "plasma", "wallpapers",
                                    WL_PLASMA_TYPE_ID, "metadata.json", NULL);
        installed = g_file_test (f, G_FILE_TEST_EXISTS);
        g_free (f);
    }
    if (!installed) {
        seen = -2;
        return state = WL_PLASMA_ADDON_NONE;
    }
    char *file = g_build_filename (g_get_user_config_dir (), "plasma-org.kde.plasma.desktop-appletsrc",
                                   NULL);
    struct stat st;
    gint64 now = stat (file, &st) == 0
                     ? (gint64) st.st_mtim.tv_sec * 1000000 + st.st_mtim.tv_nsec / 1000 : -1;
    if (now != seen) {
        char *text = NULL;
        g_file_get_contents (file, &text, NULL, NULL);
        state = wl_plasma_addon_state (installed, text);
        seen = now;
        g_free (text);
    }
    g_free (file);
    return state;
}

/* Which layer the wallpaper surfaces go on (wayland_logic.h says why KWin
 * gets another one), said once in the log: on KDE Plasma it decides what
 * the user sees of Plasma's own desktop. */
static void wl_layer_decide (gboolean say)
{
    const char *asked = g_getenv ("WALPE_WL_LAYER");
    gboolean kwin = FALSE;
    for (guint i = 0; W.globals && i < W.globals->len && !kwin; i++)
        kwin = wl_global_is_kwin (((WlGlobal *) g_ptr_array_index (W.globals, i))->interface);
    WlLayer named = WL_LAYER_BACKGROUND;
    W.layer = wl_layer_choose (asked, kwin, &W.layer_why);
    W.kwin = kwin;
    if (!say)
        return;
    if (wl_layer_parse (asked, &named) < 0)
        g_warning ("wayland: WALPE_WL_LAYER=%s: not a layer this program uses (background, "
                   "bottom, auto); taken as auto", asked);
    if (W.layer_why == WL_LAYER_WHY_ASKED)
        g_message ("wayland: WALPE_WL_LAYER: the wallpaper surfaces go on the layer \"%s\"",
                   wl_layer_name (W.layer));
    if (!kwin)
        return;
    WlPlasmaAddon addon = wl_plasma_addon ();
    const char *hint = wl_plasma_hint (kwin, addon);
    if (W.layer == WL_LAYER_BOTTOM)
        g_message ("wayland: KDE Plasma (KWin): the wallpaper surfaces go on the layer \"bottom\"%s: "
                   "in front of Plasma's own desktop window, which is opaque and which KWin would "
                   "put in front of a surface on \"background\" at the first click. WalPE add-on "
                   "for Plasma: %s%s",
                   W.layer_why == WL_LAYER_WHY_KWIN ? ", not \"background\"" : "",
                   wl_plasma_addon_name (addon),
                   hint ? "" : ". Where the wallpaper type is \"WalPE\", Plasma's desktop is "
                               "see-through and its icons and widgets are shown over the wallpaper");
    else
        g_message ("wayland: KDE Plasma (KWin): on the layer \"background\" the wallpaper shares a "
                   "stacking layer with Plasma's own desktop window. Where the desktop's wallpaper "
                   "type is not \"WalPE\" (the add-on for Plasma: %s) it is shown in front of "
                   "Plasma's desktop or hidden behind it, depending on which window was active "
                   "when the daemon started, and a click on the desktop hides it "
                   "(docs/WAYLAND.md, \"KDE Plasma\")", wl_plasma_addon_name (addon));
    if (hint)
        g_message ("wayland: KDE Plasma: %s", hint);
}

/* the connection the probe had to keep (WAYLAND_SOCKET can be used once) */
static struct wl_display *wl_kept_dpy;

static gboolean wl_open (const WalpePlatformOpts *opts, char **err)
{
    if (W.is_open) {
        if (err)
            *err = g_strdup ("the Wayland backend is open already");
        return FALSE;
    }
    memset (&W, 0, sizeof W);
    W.opts = *opts;
    W.pres_clock = CLOCK_MONOTONIC;
    W.egl_dpy = EGL_NO_DISPLAY;

    int wait_s = MAX (opts->reconnect_wait_s, opts->wait_for_display ? 2 : 0);
    gint64 deadline = g_get_monotonic_time () + (gint64) wait_s * G_USEC_PER_SEC;
    int connect_errno = 0;
    if (wl_kept_dpy) {
        W.dpy = wl_kept_dpy;
        wl_kept_dpy = NULL;
    }
    while (!W.dpy) {
        W.dpy = wl_display_connect (NULL);
        if (W.dpy)
            break;
        connect_errno = errno;
        if (g_get_monotonic_time () >= deadline)
            break;
        g_usleep (200000);      /* the compositor is (re)starting: its socket is not there yet */
    }
    if (!W.dpy) {
        if (err)
            *err = g_strdup_printf ("cannot connect to the Wayland compositor%s: %s",
                                    g_getenv ("WAYLAND_DISPLAY") || g_getenv ("WAYLAND_SOCKET")
                                        ? "" : " (WAYLAND_DISPLAY is not set)",
                                    connect_errno ? g_strerror (connect_errno) : "no socket");
        return FALSE;
    }
    W.is_open = TRUE;
    W.fd = wl_display_get_fd (W.dpy);
    W.ignore = g_strdup (g_getenv ("WALPE_WL_IGNORE"));
    if (W.ignore && *W.ignore)
        g_message ("wayland: WALPE_WL_IGNORE: treating as absent: %s", W.ignore);
    W.globals = g_ptr_array_new_with_free_func (wl_global_free);
    W.evq = g_array_new (FALSE, FALSE, sizeof (WlEv));
    W.registry = wl_display_get_registry (W.dpy);
    wl_registry_add_listener (W.registry, &wl_reg_listener, NULL);
    /* three rounds: the globals; what the objects bound in the first say
     * (outputs, seats, the presentation clock); what objects made in the
     * second say (xdg-output of an output announced before its manager) */
    const char *fail = NULL;
    gint64 answer_by = g_get_monotonic_time () + (gint64) WL_START_ANSWER_S * G_USEC_PER_SEC;
    for (int i = 0; i < 3 && !fail; i++) {
        int r = wl_sync_until (W.dpy, NULL, answer_by);
        if (r < 0)
            fail = "the Wayland compositor closed the connection during start-up";
        else if (r == 0)
            fail = "the Wayland compositor accepted the connection but did not answer within "
                   G_STRINGIFY (WL_START_ANSWER_S) " seconds";
    }
    if (!fail && !W.compositor)
        fail = "the Wayland compositor offers no wl_compositor";
    if (!fail && opts->windowed && !W.wm_base)
        fail = "the Wayland compositor has no xdg_wm_base (xdg-shell), which --windowed needs";
    if (!fail && !opts->windowed && !opts->probe && !W.layer_shell)
        fail = wl_no_layer_shell_text ();
    if (fail) {
        if (err)
            *err = g_strdup (fail);
        wl_close ();
        return FALSE;
    }
    const char *e = g_getenv ("XCURSOR_THEME");
    W.cursor_theme_name = e && *e ? g_strdup (e) : NULL;
    e = g_getenv ("XCURSOR_SIZE");
    W.cursor_size = e && *e ? (int) CLAMP (g_ascii_strtoll (e, NULL, 10), 8, 256) : 24;
    e = g_getenv ("WALPE_WL_RECREATE");
    W.recreate_max = e && !strcmp (e, "0") ? 0 : 3;
    wl_layer_decide (!opts->probe && !opts->windowed);
    if (!opts->probe)
        W.display_node = wl_dmabuf_device (W.dpy, W.registry, W.dmabuf_name, W.dmabuf_version,
                                           answer_by);
    W.outputs_dirty = FALSE;    /* the daemon reads the list next */
    W.motion_dirty = FALSE;
    return TRUE;
}

static void wl_close (void)
{
    if (!W.is_open)
        return;
    W.is_open = FALSE;
    while (W.surfaces)
        wl_surf_destroy (W.surfaces->data);
    if (W.src) {
        g_source_destroy (W.src);
        g_source_unref (W.src);
    }
#ifdef HAVE_WL_CURSOR
    if (W.cursor_theme)
        wl_cursor_theme_destroy (W.cursor_theme);
    if (W.cursor_surface)
        wl_surface_destroy (W.cursor_surface);
#endif
    while (W.seats) {
        WlSeat *seat = W.seats->data;
        W.seats = g_list_delete_link (W.seats, W.seats);
        wl_seat_free (seat);
    }
    while (W.outputs) {
        WlOut *o = W.outputs->data;
        W.outputs = g_list_delete_link (W.outputs, W.outputs);
        wl_out_free (o);
    }
    if (W.xdg_output_mgr)
        zxdg_output_manager_v1_destroy (W.xdg_output_mgr);
    if (W.layer_shell) {
        if (W.layer_shell_version >= 3)
            zwlr_layer_shell_v1_destroy (W.layer_shell);
        else
            wl_proxy_destroy ((struct wl_proxy *) W.layer_shell);
    }
    if (W.viewporter)
        wp_viewporter_destroy (W.viewporter);
    if (W.frac_mgr)
        wp_fractional_scale_manager_v1_destroy (W.frac_mgr);
    if (W.presentation)
        wp_presentation_destroy (W.presentation);
    if (W.wm_base)
        xdg_wm_base_destroy (W.wm_base);
    if (W.shm)
        wl_shm_destroy (W.shm);
    if (W.compositor)
        wl_compositor_destroy (W.compositor);
    if (W.registry)
        wl_registry_destroy (W.registry);
    if (W.dpy) {
        if (!wl_is_dead ())
            wl_display_flush (W.dpy);
        wl_display_disconnect (W.dpy);
    }
    if (W.globals)
        g_ptr_array_unref (W.globals);
    if (W.evq)
        g_array_unref (W.evq);
    g_free (W.display_node);
    g_free (W.cursor_theme_name);
    g_free (W.ignore);
    memset (&W, 0, sizeof W);
}

static void wl_describe_globals (GString *s)
{
    if (W.layer_shell)
        g_string_append_printf (s, "layer-shell v%u", W.layer_shell_version);
    else
        g_string_append (s, "no layer-shell");
    if (W.xdg_output_mgr)
        g_string_append_printf (s, ", xdg-output v%u", W.xdg_output_version);
    if (W.viewporter)
        g_string_append (s, ", viewporter");
    if (W.frac_mgr)
        g_string_append (s, ", fractional-scale");
    if (W.presentation)
        g_string_append (s, ", presentation-time");
    if (W.wm_base)
        g_string_append (s, ", xdg-shell");
    if (W.dmabuf_version)
        g_string_append_printf (s, ", linux-dmabuf v%u", W.dmabuf_version);
}

/* is there a seat with a pointer at all? (Whether wallpapers take pointer
 * input is the daemon's setting: it then never asks for it.) */
static const char *wl_pointer_state (void)
{
    for (GList *l = W.seats; l; l = l->next)
        if (((WlSeat *) l->data)->pointer)
            return "yes";
    return "none";
}

static char *wl_describe (void)
{
    GString *s = g_string_new ("compositor: ");
    wl_describe_globals (s);
    {
        /* and what it lacks of the protocols the backend would use: a bug
         * report from another compositor is read against this list first */
        const struct { gboolean have; const char *name; } opt[] = {
            { W.xdg_output_mgr != NULL, "xdg-output" }, { W.viewporter != NULL, "viewporter" },
            { W.frac_mgr != NULL, "fractional-scale" }, { W.presentation != NULL, "presentation-time" },
            { W.wm_base != NULL, "xdg-shell" }, { W.dmabuf_version != 0, "linux-dmabuf" },
        };
        const char *sep = "; lacks: ";
        for (guint i = 0; i < G_N_ELEMENTS (opt); i++)
            if (!opt[i].have) {
                g_string_append_printf (s, "%s%s", sep, opt[i].name);
                sep = ", ";
            }
    }
    g_string_append_printf (s, "; %u outputs; pointer: %s", g_list_length (W.outputs),
                            wl_pointer_state ());
    return g_string_free (s, FALSE);
}

static void wl_start (gboolean layout_events, const WalpeSignalSink *sink, gpointer sink_user)
{
    if (!W.is_open)
        return;
    W.layout_events = layout_events;
    if (!W.src) {
        W.src = g_source_new (&wl_src_funcs, sizeof (GSource));
        g_source_set_name (W.src, "walpe-wayland");
        /* Above every other source of the loop, and not for speed: between
         * prepare and check this thread is a registered reader of the
         * socket, and no other thread's read completes until it has read
         * or cancelled. GLib skips the check of a source when one of
         * higher priority became ready in the same iteration - the main
         * thread would then run that source's callback (which may join a
         * render thread) still registered, and the render thread could
         * wait for it inside wl_display_read_events. With nothing above
         * it, this source's check always runs. */
        g_source_set_priority (W.src, G_PRIORITY_HIGH - 50);
        W.pfd.fd = W.fd;
        W.pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        W.pfd.revents = 0;
        g_source_add_poll (W.src, &W.pfd);
        W.pfd_added = TRUE;
        g_source_attach (W.src, NULL);
    }
    W.started = TRUE;
    if (!W.opts.probe) {
        walpe_signals_register (&wl_frame_provider);
        walpe_signals_start (WALPE_BACKEND_WAYLAND, sink, sink_user);
    }
}

static void wl_sync (void)
{
    wl_roundtrip ();
}

static void wl_present_log (void)
{
    g_message ("present: Wayland: the EGL swap never waits; interval 1 waits for the surface's "
               "frame callback (at most %d..%d ms, then the surface counts as not shown); "
               "presentation feedback: %s; viewporter: %s; fractional scale: %s",
               (int) (wl_frame_limit_us (1000.0) / 1000), (int) (wl_frame_limit_us (1.0) / 1000),
               W.presentation ? (W.pres_clock == CLOCK_MONOTONIC ? "yes (monotonic clock)"
                                                                  : "yes (converted clock)")
                              : "no",
               W.viewporter ? "yes" : "no (buffers are logical size x integer scale)",
               W.frac_mgr ? "yes" : "no");
}

static void wl_status (JsonBuilder *b)
{
    GString *g = g_string_new (NULL);
    wl_describe_globals (g);
    json_builder_set_member_name (b, "wl_compositor_globals");
    json_builder_add_string_value (b, g->str);
    g_string_free (g, TRUE);
    json_builder_set_member_name (b, "wl_layer");
    json_builder_add_string_value (b, wl_layer_name (W.layer));
    json_builder_set_member_name (b, "wl_layer_why");
    json_builder_add_string_value (b, W.layer_why == WL_LAYER_WHY_ASKED ? "WALPE_WL_LAYER"
                                      : W.layer_why == WL_LAYER_WHY_KWIN
                                            ? "the compositor is KWin (KDE Plasma)"
                                            : "default");
    if (W.kwin) {
        /* what the user sees of Plasma's desktop: wayland_logic.h, the add-on for KDE Plasma */
        WlPlasmaAddon addon = wl_plasma_addon ();
        const char *hint = wl_plasma_hint (W.kwin, addon);
        json_builder_set_member_name (b, "wl_plasma_addon");
        json_builder_add_string_value (b, wl_plasma_addon_name (addon));
        json_builder_set_member_name (b, "wl_plasma_icons");
        json_builder_add_string_value (b, hint ? "hidden behind the wallpaper"
                                               : "shown over the wallpaper where the wallpaper "
                                                 "type is WalPE");
        if (hint) {
            json_builder_set_member_name (b, "wl_plasma_hint");
            json_builder_add_string_value (b, hint);
        }
    }
    json_builder_set_member_name (b, "wl_threading");
    json_builder_add_string_value (b, "queue-per-surface");
    json_builder_set_member_name (b, "wl_seat_pointer");
    json_builder_add_string_value (b, wl_pointer_state ());
    json_builder_set_member_name (b, "wl_connection");
    json_builder_add_string_value (b, wl_is_dead () ? "lost" : "ok");
    json_builder_set_member_name (b, "wl_surfaces");
    json_builder_begin_array (b);
    for (GList *l = W.surfaces; l; l = l->next) {
        WlSurf *s = l->data;
        g_mutex_lock (&s->lock);
        WlSizeOut sz = s->size;
        int scale120 = s->size_in.scale120, recreated = (int) s->ls.recreated;
        int pres_flags = s->stats.presented ? s->pres_flags : -1;
        const char *role = s->role == WL_ROLE_NONE ? "none" : wl_ls_state_name (s->ls.state);
        gboolean ptr = s->want_pointer, hidden = s->fs.hidden;
        double fhz = s->stats.refresh_hz;
        g_mutex_unlock (&s->lock);
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "output");
        json_builder_add_string_value (b, s->out_name ? s->out_name : "");
        json_builder_set_member_name (b, "id");
        json_builder_add_int_value (b, s->out_id);
        json_builder_set_member_name (b, "role");
        json_builder_add_string_value (b, role);
        json_builder_set_member_name (b, "buffer");
        json_builder_begin_array (b);
        json_builder_add_int_value (b, sz.buf_w);
        json_builder_add_int_value (b, sz.buf_h);
        json_builder_end_array (b);
        json_builder_set_member_name (b, "logical");
        json_builder_begin_array (b);
        json_builder_add_int_value (b, sz.dst_w);
        json_builder_add_int_value (b, sz.dst_h);
        json_builder_end_array (b);
        json_builder_set_member_name (b, "viewport");
        json_builder_add_boolean_value (b, sz.viewport);
        json_builder_set_member_name (b, "scale120");
        json_builder_add_int_value (b, scale120);
        json_builder_set_member_name (b, "pointer");
        json_builder_add_boolean_value (b, ptr);
        json_builder_set_member_name (b, "hidden");
        json_builder_add_boolean_value (b, hidden);
        json_builder_set_member_name (b, "recreated");
        json_builder_add_int_value (b, recreated);
        json_builder_set_member_name (b, "presented_flags");
        json_builder_add_int_value (b, pres_flags);
        json_builder_set_member_name (b, "feedback_refresh_mhz");
        json_builder_add_int_value (b, (gint64) (fhz * 1000.0 + 0.5));
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
}

const WalpePlatformOps walpe_wayland_ops = {
    .name = "wayland",
    .backend = WALPE_BACKEND_WAYLAND,
    .open = wl_open,
    .close = wl_close,
    .describe = wl_describe,
    .start = wl_start,
    .sync = wl_sync,
    .outputs = wl_outputs,
    .egl_display = wl_egl_display,
    .egl_note = wl_egl_note,
    .egl_visual = wl_egl_visual,
    .egl_config_usable = wl_egl_config_usable,
    .display_node = wl_display_node,
    .paces = TRUE,
    .surface_new = wl_surface_new,
    .surface_free = wl_surface_free,
    .surface_size = wl_surface_size,
    .surface_set_buffer_size = wl_surface_set_buffer_size,
    .surface_output = wl_surface_output,
    .surface_native_id = wl_surface_native_id,
    .surface_want_pointer = wl_surface_want_pointer,
    .surface_set_interval = wl_surface_set_interval,
    .surface_pre_swap = wl_surface_pre_swap,
    .surface_swapped = wl_surface_swapped,
    .surface_present_stats = wl_surface_present_stats,
    .pointer = wl_pointer,
    .on_desktop = wl_on_desktop,
    .input_select = wl_input_select,
    .has_input_events = wl_has_input_events,
    .present_log = wl_present_log,
    .status = wl_status,
};

/* ------------------------------------------------------------------ probe
 * A first look at the compositor without opening the backend: is there a
 * layer shell (wallpapers), an xdg_wm_base (--windowed), and which DRM
 * device does it compose with. A connection of its own. */
typedef struct {
    gboolean layer, xdg;
    guint32 dm_name, dm_version;
} WlProbe;

static void wl_probe_global (void *data, struct wl_registry *reg, uint32_t name, const char *itf,
                             uint32_t version)
{
    WlProbe *p = data;
    if (wl_ignored (g_getenv ("WALPE_WL_IGNORE"), itf))
        return;
    if (!strcmp (itf, zwlr_layer_shell_v1_interface.name))
        p->layer = TRUE;
    else if (!strcmp (itf, xdg_wm_base_interface.name))
        p->xdg = TRUE;
    else if (!strcmp (itf, zwp_linux_dmabuf_v1_interface.name)) {
        p->dm_name = name;
        p->dm_version = version;
    }
}

static void wl_probe_global_remove (void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener wl_probe_listener = { wl_probe_global,
                                                               wl_probe_global_remove };

void walpe_wayland_probe (WalpePlatformProbe *out)
{
    /* a socket handed over in WAYLAND_SOCKET can be connected to once:
     * that connection is kept for open () instead of being closed */
    gboolean once = g_getenv ("WAYLAND_SOCKET") != NULL;
    struct wl_display *dpy = wl_kept_dpy ? wl_kept_dpy : wl_display_connect (NULL);
    if (!dpy)
        return;
    WlProbe p = { 0 };
    struct wl_registry *reg = wl_display_get_registry (dpy);
    wl_registry_add_listener (reg, &wl_probe_listener, &p);
    /* no answer in time counts as no compositor: the caller goes on to
     * X11 or says that the compositor does not answer */
    gint64 answer_by = g_get_monotonic_time () + (gint64) WL_START_ANSWER_S * G_USEC_PER_SEC;
    if (wl_sync_until (dpy, NULL, answer_by) > 0) {
        out->connected = TRUE;
        out->layer_shell = p.layer;
        out->xdg_shell = p.xdg;
        out->display_node = wl_dmabuf_device (dpy, reg, p.dm_name, p.dm_version, answer_by);
    }
    wl_registry_destroy (reg);
    if (once || wl_kept_dpy) {
        wl_kept_dpy = out->connected ? dpy : NULL;
        if (!out->connected)
            wl_display_disconnect (dpy);
    } else {
        wl_display_disconnect (dpy);
    }
}

void walpe_wayland_probe_release (void)
{
    if (wl_kept_dpy) {
        wl_display_disconnect (wl_kept_dpy);
        wl_kept_dpy = NULL;
    }
}
