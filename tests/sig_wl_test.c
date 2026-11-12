/* sig_wl_test.c - the Wayland pause providers (src/signals/wl_*.c) against
 * a stand-in compositor.
 *
 * The compositor is a libwayland-server display on a thread of this
 * process, listening on a socket in a private runtime directory. It
 * offers what the scenario says (wl_seat, wl_output with or without
 * names, ext_idle_notifier_v1, zwlr_foreign_toplevel_manager_v1 in a
 * given version, ext_foreign_toplevel_list_v1) and plays windows, idle
 * state and outputs as the test tells it. The providers are the real
 * ones; the registry is tests/sig_stub.h.
 *
 *   A  everything offered: focus / covered per output, fullscreen and the
 *      output the fullscreen window is on, the see-through list,
 *      minimized and closed windows, the idle timeout (set, changed, off),
 *      outputs that go away or change their id, the compositor dropping
 *      the connection
 *   B  window list version 1, outputs without names: no fullscreen
 *      provider (with the reason), covered still works on a single output
 *   C  only the stateless ext list, no idle notifier: every provider
 *      declines with its sentence
 *   D  WALPE_WL_COVERED=all: an unfocused fullscreen window covers
 *   E  a compositor that never answers: the providers give up
 *   F  a TMPDIR too long for a socket path: this test, run again with
 *      one, still brings its compositor up and ends
 *   G  fullscreen where the window's output cannot be told (outputs
 *      without names and several monitors; a window the compositor names
 *      no output for): every output, and status says so
 *
 * No real compositor, no display, no GPU. */
#include "walpe-build-config.h"
#include "sig_stub.h"
#include "signals/providers.h"
#include "signals/wlplan.h"
#include <glib/gstdio.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <wayland-server.h>
#include "ext-idle-notify-v1-server-protocol.h"
#include "ext-foreign-toplevel-list-v1-server-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-server-protocol.h"

/* ================================================================ compositor */

#define MAX_OUT 4
#define MAX_WIN 8

typedef struct {
    gboolean used;
    char name[32];
    guint32 version;            /* offered */
    struct wl_global *global;
    struct wl_list resources;   /* wl_resource link: what clients bound */
} SrvOut;

typedef struct {
    gboolean used;
    char app_id[64];
    guint state;                /* WALPE_TL_* */
    guint outs;                 /* bit i = on output i */
    struct wl_resource *handle; /* for the one manager client; NULL = none / destroyed */
} SrvWin;

typedef struct {
    struct wl_list link;
    struct wl_resource *res;
    guint32 timeout_ms;
} SrvIdle;

typedef struct {
    guint32 seat;               /* versions offered, 0 = not offered */
    guint32 idle;
    guint32 toplevel;
    guint32 ext_list;
    guint32 out_version;
    const char *out_names[MAX_OUT];
} SrvConf;

static struct {
    GThread *thread;
    struct wl_display *dpy;
    struct wl_event_loop *loop;
    int wake_fd;
    GAsyncQueue *calls;
    gint quit, ready;
    char socket[64];
    SrvConf conf;
    SrvOut out[MAX_OUT];
    SrvWin win[MAX_WIN];
    struct wl_resource *mgr;        /* the bound toplevel manager */
    struct wl_list idles;           /* SrvIdle */
    /* read by the test thread */
    gint n_idle;
    gint last_idle_ms;
    gint n_clients;
    gint handles_destroyed;
} S;

/* ---- wl_seat */

static void seat_get_none (struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void) c; (void) r; (void) id;  /* the providers ask for no device */
}

static void res_destroy (struct wl_client *c, struct wl_resource *r)
{
    (void) c;
    wl_resource_destroy (r);
}

static const struct wl_seat_interface seat_impl = { seat_get_none, seat_get_none, seat_get_none, res_destroy };

static void seat_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &wl_seat_interface, (int) version, id);
    wl_resource_set_implementation (r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities (r, 0);
}

/* ---- wl_output */

static const struct wl_output_interface out_impl = { res_destroy };

static void out_res_gone (struct wl_resource *r)
{
    wl_list_remove (wl_resource_get_link (r));
}

static void out_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    SrvOut *o = data;
    struct wl_resource *r = wl_resource_create (c, &wl_output_interface, (int) version, id);
    wl_resource_set_implementation (r, &out_impl, o, out_res_gone);
    wl_list_insert (&o->resources, wl_resource_get_link (r));
    wl_output_send_geometry (r, 0, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN, "walpe", "test",
                             WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode (r, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
    if (version >= 2)
        wl_output_send_scale (r, 1);
    if (version >= 4) {
        wl_output_send_name (r, o->name);
        wl_output_send_description (r, "stand-in output");
    }
    if (version >= 2)
        wl_output_send_done (r);
}

static void srv_out_add (int i, const char *name)
{
    SrvOut *o = &S.out[i];
    o->used = TRUE;
    g_strlcpy (o->name, name, sizeof o->name);
    o->version = S.conf.out_version;
    wl_list_init (&o->resources);
    o->global = wl_global_create (S.dpy, &wl_output_interface, (int) o->version, o, out_bind);
}

/* ---- ext_idle_notifier_v1 */

static void idle_res_gone (struct wl_resource *r)
{
    SrvIdle *n = wl_resource_get_user_data (r);
    wl_list_remove (&n->link);
    g_free (n);
    g_atomic_int_add (&S.n_idle, -1);
}

static const struct ext_idle_notification_v1_interface idle_note_impl = { res_destroy };

static void idle_get (struct wl_client *c, struct wl_resource *r, uint32_t id, uint32_t timeout,
                      struct wl_resource *seat)
{
    (void) seat;
    SrvIdle *n = g_new0 (SrvIdle, 1);
    n->timeout_ms = timeout;
    n->res = wl_resource_create (c, &ext_idle_notification_v1_interface, wl_resource_get_version (r), id);
    wl_resource_set_implementation (n->res, &idle_note_impl, n, idle_res_gone);
    wl_list_insert (&S.idles, &n->link);
    g_atomic_int_set (&S.last_idle_ms, (gint) timeout);
    g_atomic_int_inc (&S.n_idle);
}

/* (by name: the interface gained a third request in its version 2) */
static const struct ext_idle_notifier_v1_interface idle_impl = {
    .destroy = res_destroy,
    .get_idle_notification = idle_get,
};

static void idle_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &ext_idle_notifier_v1_interface, (int) version, id);
    wl_resource_set_implementation (r, &idle_impl, NULL, NULL);
}

/* ---- zwlr_foreign_toplevel_manager_v1 */

static void h_nop (struct wl_client *c, struct wl_resource *r)
{
    (void) c; (void) r;
}

static void h_activate (struct wl_client *c, struct wl_resource *r, struct wl_resource *seat)
{
    (void) c; (void) r; (void) seat;
}

static void h_set_rectangle (struct wl_client *c, struct wl_resource *r, struct wl_resource *surface,
                             int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void) c; (void) r; (void) surface; (void) x; (void) y; (void) w; (void) h;
}

static void h_set_fullscreen (struct wl_client *c, struct wl_resource *r, struct wl_resource *out)
{
    (void) c; (void) r; (void) out;
}

static void h_res_gone (struct wl_resource *r)
{
    SrvWin *w = wl_resource_get_user_data (r);
    if (w && w->handle == r)
        w->handle = NULL;
    g_atomic_int_inc (&S.handles_destroyed);
}

static const struct zwlr_foreign_toplevel_handle_v1_interface handle_impl = {
    .set_maximized = h_nop,
    .unset_maximized = h_nop,
    .set_minimized = h_nop,
    .unset_minimized = h_nop,
    .activate = h_activate,
    .close = h_nop,
    .set_rectangle = h_set_rectangle,
    .destroy = res_destroy,
    .set_fullscreen = h_set_fullscreen,
    .unset_fullscreen = h_nop,
};

static void win_send_state (SrvWin *w)
{
    struct wl_array a;
    wl_array_init (&a);
    static const struct { guint bit; uint32_t v; } map[] = {
        { WALPE_TL_MAXIMIZED, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED },
        { WALPE_TL_MINIMIZED, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED },
        { WALPE_TL_ACTIVATED, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED },
        { WALPE_TL_FULLSCREEN, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN },
    };
    for (guint i = 0; i < G_N_ELEMENTS (map); i++) {
        if (!(w->state & map[i].bit))
            continue;
        /* a version 1 client must never be sent the fullscreen state */
        if (map[i].bit == WALPE_TL_FULLSCREEN && wl_resource_get_version (w->handle) < 2)
            continue;
        uint32_t *p = wl_array_add (&a, sizeof *p);
        *p = map[i].v;
    }
    zwlr_foreign_toplevel_handle_v1_send_state (w->handle, &a);
    wl_array_release (&a);
}

static void win_send_outputs (SrvWin *w, guint was, guint now)
{
    struct wl_client *c = wl_resource_get_client (w->handle);
    for (int i = 0; i < MAX_OUT; i++) {
        gboolean a = (was >> i) & 1, b = (now >> i) & 1;
        if (a == b || !S.out[i].used)
            continue;
        struct wl_resource *r;
        wl_resource_for_each (r, &S.out[i].resources) {
            if (wl_resource_get_client (r) != c)
                continue;
            if (b)
                zwlr_foreign_toplevel_handle_v1_send_output_enter (w->handle, r);
            else
                zwlr_foreign_toplevel_handle_v1_send_output_leave (w->handle, r);
        }
    }
}

static void win_announce (SrvWin *w)
{
    if (!S.mgr)
        return;
    struct wl_client *c = wl_resource_get_client (S.mgr);
    w->handle = wl_resource_create (c, &zwlr_foreign_toplevel_handle_v1_interface,
                                    wl_resource_get_version (S.mgr), 0);
    wl_resource_set_implementation (w->handle, &handle_impl, w, h_res_gone);
    zwlr_foreign_toplevel_manager_v1_send_toplevel (S.mgr, w->handle);
    zwlr_foreign_toplevel_handle_v1_send_title (w->handle, "a window");
    zwlr_foreign_toplevel_handle_v1_send_app_id (w->handle, w->app_id);
    win_send_outputs (w, 0, w->outs);
    win_send_state (w);
    zwlr_foreign_toplevel_handle_v1_send_done (w->handle);
}

static void mgr_res_gone (struct wl_resource *r)
{
    if (S.mgr == r)
        S.mgr = NULL;
}

static void mgr_stop (struct wl_client *c, struct wl_resource *r)
{
    (void) c;
    zwlr_foreign_toplevel_manager_v1_send_finished (r);
    wl_resource_destroy (r);
}

static const struct zwlr_foreign_toplevel_manager_v1_interface mgr_impl = { mgr_stop };

static void mgr_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    struct wl_resource *r = wl_resource_create (c, &zwlr_foreign_toplevel_manager_v1_interface,
                                                (int) version, id);
    wl_resource_set_implementation (r, &mgr_impl, NULL, mgr_res_gone);
    S.mgr = r;
    for (int i = 0; i < MAX_WIN; i++)
        if (S.win[i].used)
            win_announce (&S.win[i]);
}

/* ---- ext_foreign_toplevel_list_v1: offered, never bound by the providers */

static void ext_bind (struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void) data;
    wl_resource_create (c, &ext_foreign_toplevel_list_v1_interface, (int) version, id);
}

/* ---- thread */

typedef struct {
    void (*fn) (gpointer);
    gpointer arg;
    gint done;
} SrvCall;

static int srv_wake (int fd, uint32_t mask, void *data)
{
    (void) mask; (void) data;
    guint64 v;
    if (read (fd, &v, sizeof v) < 0)
        return 0;
    SrvCall *c;
    while ((c = g_async_queue_try_pop (S.calls))) {
        c->fn (c->arg);
        wl_display_flush_clients (S.dpy);
        g_atomic_int_set (&c->done, 1);
    }
    return 0;
}

static void client_created (struct wl_listener *l, void *data)
{
    (void) l; (void) data;
    g_atomic_int_inc (&S.n_clients);
}

static struct wl_listener client_listener = { .notify = client_created };

static gpointer srv_thread (gpointer u)
{
    (void) u;
    S.dpy = wl_display_create ();
    S.loop = wl_display_get_event_loop (S.dpy);
    wl_list_init (&S.idles);
    if (wl_display_add_socket (S.dpy, S.socket) != 0) {
        wl_display_destroy (S.dpy);
        g_atomic_int_set (&S.ready, -1);
        return NULL;
    }
    wl_display_add_client_created_listener (S.dpy, &client_listener);
    if (S.conf.seat)
        wl_global_create (S.dpy, &wl_seat_interface, (int) S.conf.seat, NULL, seat_bind);
    for (int i = 0; i < MAX_OUT; i++)
        if (S.conf.out_names[i])
            srv_out_add (i, S.conf.out_names[i]);
    if (S.conf.idle)    /* (not above what the installed protocol file defines) */
        wl_global_create (S.dpy, &ext_idle_notifier_v1_interface,
                          MIN ((int) S.conf.idle, ext_idle_notifier_v1_interface.version), NULL,
                          idle_bind);
    if (S.conf.toplevel)
        wl_global_create (S.dpy, &zwlr_foreign_toplevel_manager_v1_interface, (int) S.conf.toplevel,
                          NULL, mgr_bind);
    if (S.conf.ext_list)
        wl_global_create (S.dpy, &ext_foreign_toplevel_list_v1_interface, (int) S.conf.ext_list, NULL,
                          ext_bind);
    struct wl_event_source *ws = wl_event_loop_add_fd (S.loop, S.wake_fd, WL_EVENT_READABLE, srv_wake, NULL);
    g_atomic_int_set (&S.ready, 1);
    while (!g_atomic_int_get (&S.quit)) {
        wl_display_flush_clients (S.dpy);
        wl_event_loop_dispatch (S.loop, 20);
    }
    wl_event_source_remove (ws);
    wl_display_destroy_clients (S.dpy);
    wl_display_destroy (S.dpy);
    return NULL;
}

static void srv_run (void (*fn) (gpointer), gpointer arg)
{
    SrvCall c = { fn, arg, 0 };
    g_async_queue_push (S.calls, &c);
    guint64 one = 1;
    if (write (S.wake_fd, &one, sizeof one) < 0)
        g_error ("eventfd");
    while (!g_atomic_int_get (&c.done))
        g_usleep (300);
}

static int srv_serial;

static gboolean srv_up (const SrvConf *conf)
{
    memset (&S.out, 0, sizeof S.out);
    memset (&S.win, 0, sizeof S.win);
    S.mgr = NULL;
    S.conf = *conf;
    S.quit = S.ready = 0;
    S.n_idle = S.n_clients = S.handles_destroyed = 0;
    S.last_idle_ms = -1;
    g_snprintf (S.socket, sizeof S.socket, "walpe-sigtest-%d", srv_serial++);
    /* the providers read this when they connect (no thread of theirs
     * exists while the test changes it) */
    g_setenv ("WALPE_SIG_WAYLAND_DISPLAY", S.socket, TRUE);
    S.thread = g_thread_new ("fake-compositor", srv_thread, NULL);
    while (!g_atomic_int_get (&S.ready))
        g_usleep (500);
    return g_atomic_int_get (&S.ready) > 0;
}

static void srv_down (void)
{
    g_atomic_int_set (&S.quit, 1);
    guint64 one = 1;
    if (write (S.wake_fd, &one, sizeof one) < 0)
        g_error ("eventfd");
    g_thread_join (S.thread);
    S.thread = NULL;
}

/* A scenario starts here. Without its compositor it is over: the thread
 * has ended, and srv_run () would wait for ever for a call nobody takes. */
static gboolean srv_start (const SrvConf *conf, const char *name)
{
    const gboolean up = srv_up (conf);
    CHECK (up, "compositor %s up", name);
    if (!up)
        srv_down ();
    return up;
}

/* ---- what the test asks of it (each runs on the compositor thread) */

typedef struct {
    int idx;
    const char *app_id;
    guint state, outs;
} WinArg;

static void s_win_add (gpointer p)
{
    WinArg *a = p;
    SrvWin *w = &S.win[a->idx];
    memset (w, 0, sizeof *w);
    w->used = TRUE;
    g_strlcpy (w->app_id, a->app_id, sizeof w->app_id);
    w->state = a->state;
    w->outs = a->outs;
    win_announce (w);
}

static void s_win_set (gpointer p)
{
    WinArg *a = p;
    SrvWin *w = &S.win[a->idx];
    guint was = w->outs;
    w->state = a->state;
    w->outs = a->outs;
    if (!w->handle)
        return;
    win_send_outputs (w, was, w->outs);
    win_send_state (w);
    zwlr_foreign_toplevel_handle_v1_send_done (w->handle);
}

/* what a client's new title makes a compositor send: the title and done */
static void s_win_title (gpointer p)
{
    static int n;
    SrvWin *w = &S.win[GPOINTER_TO_INT (p)];
    if (!w->handle)
        return;
    char t[32];
    g_snprintf (t, sizeof t, "title %d", ++n);
    zwlr_foreign_toplevel_handle_v1_send_title (w->handle, t);
    zwlr_foreign_toplevel_handle_v1_send_done (w->handle);
}

static void s_win_close (gpointer p)
{
    SrvWin *w = &S.win[GPOINTER_TO_INT (p)];
    if (w->handle)
        zwlr_foreign_toplevel_handle_v1_send_closed (w->handle);
    w->used = FALSE;
}

static void s_idle (gpointer p)
{
    SrvIdle *n;
    wl_list_for_each (n, &S.idles, link) {
        if (GPOINTER_TO_INT (p))
            ext_idle_notification_v1_send_idled (n->res);
        else
            ext_idle_notification_v1_send_resumed (n->res);
    }
}

static void s_out_remove (gpointer p)
{
    SrvOut *o = &S.out[GPOINTER_TO_INT (p)];
    if (o->global)
        wl_global_destroy (o->global);
    o->global = NULL;
    /* (the bound resources stay until the client lets go, as in a real
     * compositor) */
}

static void s_kill_clients (gpointer p)
{
    (void) p;
    wl_display_destroy_clients (S.dpy);
}

static void win_add (int idx, const char *app_id, guint state, guint outs)
{
    WinArg a = { idx, app_id, state, outs };
    srv_run (s_win_add, &a);
}

static void win_set (int idx, guint state, guint outs)
{
    WinArg a = { idx, NULL, state, outs };
    srv_run (s_win_set, &a);
}

/* ================================================================ the test side */

/* the daemon's output ids by name */
static struct {
    const char *name;
    guint32 id;
} id_map[4];
static guint32 single_id;   /* answer for name == NULL */

static guint32 lookup (const char *name, gpointer user)
{
    (void) user;
    if (!name)
        return single_id;
    for (guint i = 0; i < G_N_ELEMENTS (id_map); i++)
        if (id_map[i].name && !strcmp (id_map[i].name, name))
            return id_map[i].id;
    return 0;
}

typedef struct {
    const WalpeSignalProvider *p;
    WalpeSignal sig;
    guint32 out;
    int want;
} Want;

static gboolean want_is (gpointer p)
{
    Want *w = p;
    return stub_value (w->p, w->sig, w->out) == w->want;
}

static gboolean expect (const WalpeSignalProvider *p, WalpeSignal sig, guint32 out, int want)
{
    Want w = { p, sig, out, want };
    return stub_wait (want_is, &w, 3000);
}

#define EXPECT(p, sig, out, want, what)                                                      \
    CHECK (expect (p, sig, out, want), "%s: %s on output %u is %d, want %d", what,           \
           walpe_signal_name (sig), (guint) (out), stub_value (p, sig, out), want)

static void settle (void)
{
    gint64 end = g_get_monotonic_time () + 150000;
    while (g_get_monotonic_time () < end)
        if (!g_main_context_iteration (NULL, FALSE))
            g_usleep (2000);
}

static gboolean idle_count_is (gpointer n)
{
    return g_atomic_int_get (&S.n_idle) == GPOINTER_TO_INT (n);
}

static const char *why_of (const WalpeSignalProvider *p)
{
    StubProv *sp = stub_prov (p);
    return sp && sp->why ? sp->why : "";
}

static gboolean started (const WalpeSignalProvider *p)
{
    StubProv *sp = stub_prov (p);
    return sp && sp->started;
}

static void providers_start (void)
{
    stub_reset ();
    walpe_sigprov_register (WALPE_BACKEND_WAYLAND);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, NULL, NULL);
}

#define FS  (&walpe_sig_wl_fullscreen)
#define ON  WALPE_SIG_FULLSCREEN_ON     /* per output: the focused fullscreen window is on it */
#define ALL WALPE_OUTPUT_ALL            /* ... or on one that cannot be told: every output */
#define WIN (&walpe_sig_wl_windows)
#define IDL (&walpe_sig_wl_idle)
#define A_ WALPE_TL_ACTIVATED
#define M_ WALPE_TL_MAXIMIZED
#define F_ WALPE_TL_FULLSCREEN
#define N_ WALPE_TL_MINIMIZED

static void scenario_a (void)
{
    SrvConf conf = { .seat = 7, .idle = 1, .toplevel = 3, .ext_list = 1, .out_version = 4,
                     .out_names = { "DP-1", "HDMI-A-1", "DP-9" } };
    if (!srv_start (&conf, "A"))
        return;
    id_map[0].name = "DP-1";      id_map[0].id = 11;
    id_map[1].name = "HDMI-A-1";  id_map[1].id = 22;
    id_map[2].name = NULL;        /* DP-9 is not one of the daemon's */
    single_id = 0;
    walpe_sigprov_set_output_lookup (lookup, NULL);
    walpe_sigprov_set_see_through (NULL);
    walpe_sigprov_set_idle_timeout (60);
    g_unsetenv ("WALPE_WL_COVERED");

    /* a window that exists before the providers start */
    win_add (0, "foot", A_, 1u << 0);
    providers_start ();
    CHECK (started (FS) && started (WIN) && started (IDL), "A: all three start (%s|%s|%s)", why_of (FS),
           why_of (WIN), why_of (IDL));
    CHECK (g_atomic_int_get (&S.n_clients) == 1, "A: the three share one connection (%d)",
           g_atomic_int_get (&S.n_clients));
    /* reported by start itself, before the main loop ran */
    CHECK (stub_value (WIN, WALPE_SIG_FOCUS_APP, 11) == 1, "A: the existing window's focus is known at start");
    CHECK (stub_value (WIN, WALPE_SIG_COVERED, 11) == 0, "A: a plain window covers nothing");
    CHECK (stub_value (WIN, WALPE_SIG_FOCUS_APP, 22) == 0 && stub_value (WIN, WALPE_SIG_COVERED, 22) == 0,
           "A: the other output has values too (off)");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 0, "A: no fullscreen");
    CHECK (stub_value (FS, ON, 11) != 1 && stub_value (FS, ON, 22) != 1 && stub_value (FS, ON, ALL) == 0,
           "A: no fullscreen window on any output");
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL, "A: named outputs: the rule can tell the monitors apart");
    CHECK (stub_value (WIN, WALPE_SIG_COVERED, 0) == -1, "A: nothing is reported for an output that is not the daemon's");

    win_set (0, A_ | M_, 1u << 0);
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 1, "A: focused + maximized");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 0, "A: maximized is not fullscreen");
    win_set (0, A_ | F_, 1u << 0);
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 1, "A: focused + fullscreen");
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 1, "A: fullscreen covers");
    EXPECT (FS, ON, 11, 1, "A: the fullscreen window is on the first output");
    CHECK (stub_value (FS, ON, 22) != 1 && stub_value (FS, ON, ALL) == 0,
           "A: ... and on no other: the second output's wallpaper runs on");
    CHECK (walpe_signal_get (ON, 11) && !walpe_signal_get (ON, 22), "A: as the daemon reads it");

    /* a second, unfocused fullscreen window on the other output */
    win_add (1, "mpv", F_, 1u << 1);
    settle ();
    CHECK (stub_value (WIN, WALPE_SIG_COVERED, 22) == 0, "A: an unfocused fullscreen window does not cover by default");
    CHECK (stub_value (WIN, WALPE_SIG_FOCUS_APP, 22) == 0, "A: and has no focus");
    CHECK (stub_value (FS, ON, 22) != 1 && stub_value (FS, ON, 11) == 1,
           "A: a fullscreen window without the focus does not count for its output");

    /* the focus moves to it */
    win_set (0, F_, 1u << 0);
    win_set (1, A_ | F_, 1u << 1);
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 22, 1, "A: focus moved");
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 1, "A: focused fullscreen covers its output");
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 11, 0, "A: the first output lost the focus");
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 0, "A: and is no longer counted as covered");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 1, "A: still a focused fullscreen window");
    EXPECT (FS, ON, 22, 1, "A: the focused fullscreen window is on the second output now");
    EXPECT (FS, ON, 11, 0, "A: the first output has a fullscreen window without the focus: not its rule");

    /* a window on two outputs counts on both */
    win_set (1, A_ | M_, (1u << 0) | (1u << 1));
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 1, "A: a window spanning both outputs covers both");
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 0, "A: no longer fullscreen");
    EXPECT (FS, ON, 22, 0, "A: maximized is not fullscreen, on either output");
    CHECK (stub_value (FS, ON, 11) == 0, "A: maximized is not fullscreen, on either output");
    win_set (1, A_ | F_, (1u << 0) | (1u << 1));
    EXPECT (FS, ON, 11, 1, "A: a fullscreen window spanning both outputs is on the first");
    EXPECT (FS, ON, 22, 1, "A: ... and on the second");
    CHECK (stub_value (FS, ON, ALL) == 0, "A: two outputs by name, not \"every output\"");
    win_set (1, A_ | F_, 1u << 1);
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 0, "A: back on one output");
    EXPECT (FS, ON, 11, 0, "A: back on one output");
    CHECK (stub_value (FS, ON, 22) == 1, "A: back on one output");

    /* the see-through list */
    walpe_sigprov_set_see_through ("st, MPV");
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 0, "A: a listed app id never covers");
    CHECK (stub_value (WIN, WALPE_SIG_FOCUS_APP, 22) == 1, "A: it still has the focus");
    walpe_sigprov_set_see_through (NULL);
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 1, "A: list emptied");

    /* a window on an output the daemon does not show on */
    win_set (1, A_ | F_, 1u << 2);
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 0, "A: moved to a foreign output");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 1, "A: the global fullscreen value does not look at outputs");
    EXPECT (FS, ON, 22, 0, "A: the fullscreen window left the daemon's outputs");
    CHECK (stub_value (FS, ON, 11) == 0 && stub_value (FS, ON, ALL) == 0,
           "A: on a named output that is not the daemon's: known, and no monitor of the daemon has it");
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL, "A: nothing to explain in status");
    win_set (1, A_ | F_, 1u << 1);
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 1, "A: and back");
    EXPECT (FS, ON, 22, 1, "A: and back");

    /* A title is none of the rules' business: 20 new titles of a window,
     * and 5 times its state and outputs said again as they are, do not
     * make the providers work through the window list (it was one pass
     * per `done`). What does concern them still does. */
    settle ();
    {
        guint before = walpe_sig_wl_passes ();
        for (int i = 0; i < 25; i++) {
            if (i < 20)
                srv_run (s_win_title, GINT_TO_POINTER (1));
            else
                win_set (1, A_ | F_, 1u << 1);
            /* each one reaches the providers by itself, as a title typed
             * or counted up does */
            gint64 end = g_get_monotonic_time () + 20000;
            while (g_get_monotonic_time () < end)
                if (!g_main_context_iteration (NULL, FALSE))
                    g_usleep (1000);
        }
        settle ();
        CHECK (walpe_sig_wl_passes () == before,
               "A: 20 title changes and 5 repeated states: %u passes over the window list, want 0",
               walpe_sig_wl_passes () - before);
        CHECK (stub_value (FS, ON, 22) == 1 && stub_value (WIN, WALPE_SIG_COVERED, 22) == 1,
               "A: ... and the values stand");
        win_set (1, A_ | M_, 1u << 1);
        EXPECT (FS, ON, 22, 0, "A: after them, a state that changes is followed");
        CHECK (walpe_sig_wl_passes () > before, "A: ... by a pass");
        win_set (1, A_ | F_, 1u << 1);
        EXPECT (FS, ON, 22, 1, "A: and back to fullscreen");
    }

    /* minimized, then closed */
    win_set (1, A_ | F_ | N_, 1u << 1);
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 0, "A: minimized");
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 0, "A: minimized covers nothing");
    EXPECT (FS, ON, 22, 0, "A: minimized: no fullscreen window on its output");
    win_set (1, A_ | F_, 1u << 1);
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 1, "A: restored");
    EXPECT (FS, ON, 22, 1, "A: restored");
    int destroyed = g_atomic_int_get (&S.handles_destroyed);
    srv_run (s_win_close, GINT_TO_POINTER (1));
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 0, "A: closed");
    EXPECT (FS, ON, 22, 0, "A: closed");
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 0, "A: closed");
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 22, 0, "A: closed");
    settle ();
    CHECK (g_atomic_int_get (&S.handles_destroyed) == destroyed + 1, "A: the closed window's handle was given back");
    CHECK (Stub.batches > 0 && Stub.depth == 0, "A: values of one pass arrive as one batch");

    /* a new window that is fullscreen and has the focus from the start,
     * its output not named yet: a compositor tells the state when the
     * window is mapped and the output when the window has its place, a
     * moment later. No monitor has it meanwhile (counted for every output,
     * it paused the other monitors for that moment); then the one it is on */
    win_add (3, "game", A_ | F_, 0);
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 1, "A: a new fullscreen window has the focus");
    settle ();
    CHECK (stub_value (FS, ON, ALL) != 1 && stub_value (FS, ON, 11) != 1 && stub_value (FS, ON, 22) != 1,
           "A: its output is not named yet: no monitor has it meanwhile (every %d, first %d, second %d)",
           stub_value (FS, ON, ALL), stub_value (FS, ON, 11), stub_value (FS, ON, 22));
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL, "A: and status has nothing to explain");
    win_set (3, A_ | F_, 1u << 1);
    EXPECT (FS, ON, 22, 1, "A: the compositor names its output: that monitor");
    CHECK (stub_value (FS, ON, ALL) != 1 && stub_value (FS, ON, 11) != 1, "A: and no other");
    /* it leaves that output without entering another: now nobody can tell */
    win_set (3, A_ | F_, 0);
    EXPECT (FS, ON, ALL, 1, "A: a window that was on an output and is on none: every output");
    EXPECT (FS, ON, 22, 0, "A: not under the id of the output it left");
    srv_run (s_win_close, GINT_TO_POINTER (3));
    EXPECT (FS, ON, ALL, 0, "A: that window closed");
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 0, "A: that window closed");

    /* ---- idle */
    CHECK (stub_wait (idle_count_is, GINT_TO_POINTER (1), 3000) && g_atomic_int_get (&S.last_idle_ms) == 60000,
           "A: one notification for 60 s (%d objects, %d ms)", g_atomic_int_get (&S.n_idle),
           g_atomic_int_get (&S.last_idle_ms));
    CHECK (stub_value (IDL, WALPE_SIG_IDLE, 0) == 0, "A: not idle at start");
    srv_run (s_idle, GINT_TO_POINTER (1));
    EXPECT (IDL, WALPE_SIG_IDLE, 0, 1, "A: idled");
    srv_run (s_idle, GINT_TO_POINTER (0));
    EXPECT (IDL, WALPE_SIG_IDLE, 0, 0, "A: resumed");
    /* the setting changes while idle: a new object, counting from now */
    srv_run (s_idle, GINT_TO_POINTER (1));
    EXPECT (IDL, WALPE_SIG_IDLE, 0, 1, "A: idled again");
    walpe_sigprov_set_idle_timeout (5);
    walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);
    CHECK (stub_value (IDL, WALPE_SIG_IDLE, 0) == 0, "A: a new timeout starts as not idle");
    settle ();
    CHECK (g_atomic_int_get (&S.n_idle) == 1 && g_atomic_int_get (&S.last_idle_ms) == 5000,
           "A: the old object is gone, the new one asks for 5 s (%d, %d)", g_atomic_int_get (&S.n_idle),
           g_atomic_int_get (&S.last_idle_ms));
    walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);
    settle ();
    CHECK (g_atomic_int_get (&S.n_idle) == 1, "A: a recheck without a change makes no new object");
    walpe_sigprov_set_idle_timeout (0);
    walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);
    CHECK (stub_wait (idle_count_is, GINT_TO_POINTER (0), 3000), "A: rule off: no object kept");
    CHECK (stub_value (IDL, WALPE_SIG_IDLE, 0) == 0, "A: rule off: not idle");
    CHECK (started (IDL), "A: the provider stays up with the rule off (status: it would work)");

    /* ---- outputs */
    win_set (0, A_ | M_, 1u << 0);
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 1, "A: covered again");
    win_set (0, A_ | F_, 1u << 0);
    EXPECT (FS, ON, 11, 1, "A: fullscreen on the first output again");
    /* the daemon renumbers its outputs */
    id_map[0].id = 33;
    walpe_sigprov_outputs_changed ();
    EXPECT (WIN, WALPE_SIG_COVERED, 33, 1, "A: the value follows the new id");
    EXPECT (WIN, WALPE_SIG_COVERED, 11, 0, "A: and is taken off the old one");
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 11, 0, "A: focus too");
    EXPECT (FS, ON, 33, 1, "A: the fullscreen window's output follows the new id");
    EXPECT (FS, ON, 11, 0, "A: and is taken off the old one");
    win_set (0, A_ | M_, 1u << 0);
    EXPECT (FS, ON, 33, 0, "A: maximized again");
    /* an output is unplugged */
    win_add (2, "imv", A_ | F_, 1u << 1);
    win_set (0, M_, 1u << 0);
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 1, "A: a covering window on the second output");
    srv_run (s_out_remove, GINT_TO_POINTER (1));
    EXPECT (WIN, WALPE_SIG_COVERED, 22, 0, "A: its output went away: nothing is covered there");
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 22, 0, "A: nor focused");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 1, "A: the window itself is still fullscreen and focused");
    /* ... on an output that is gone, and the compositor has named no new
     * one for it: where it is cannot be told, so it counts for every
     * output (as the rule did before it looked at outputs) */
    EXPECT (FS, ON, ALL, 1, "A: a fullscreen window on no output counts for every output");
    EXPECT (FS, ON, 22, 0, "A: not under the id of the output that is gone");
    CHECK (walpe_signal_get (ON, 33) && walpe_signal_get (ON, 22), "A: as the daemon reads it: every output");
    CHECK (walpe_sig_wl_fullscreen_everywhere () &&
           strstr (walpe_sig_wl_fullscreen_everywhere (), "which output"), "A: and status can say why");

    /* ---- the compositor drops the connection */
    srv_run (s_kill_clients, NULL);
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 0, "A: connection lost");
    EXPECT (FS, ON, ALL, 0, "A: connection lost");
    EXPECT (WIN, WALPE_SIG_COVERED, 33, 0, "A: connection lost");
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 33, 0, "A: connection lost");
    CHECK (!walpe_signal_get (WALPE_SIG_IDLE, 0), "A: connection lost: not idle");
    stub_reset ();      /* stop on a dead connection must be harmless */
    srv_down ();
}

static void scenario_b (void)
{
    SrvConf conf = { .seat = 5, .idle = 0, .toplevel = 1, .out_version = 3, .out_names = { "ignored" } };
    if (!srv_start (&conf, "B"))
        return;
    memset (id_map, 0, sizeof id_map);
    single_id = 7;
    walpe_sigprov_set_output_lookup (lookup, NULL);
    win_add (0, "firefox", A_ | M_, 1u << 0);
    providers_start ();
    CHECK (!started (FS) && strstr (why_of (FS), "version 1"), "B: no fullscreen provider: %s", why_of (FS));
    CHECK (started (WIN), "B: the window provider runs on version 1: %s", why_of (WIN));
    CHECK (!started (IDL) && strstr (why_of (IDL), "ext_idle_notifier_v1"), "B: no idle: %s", why_of (IDL));
    CHECK (*walpe_signal_sources (WALPE_SIG_FULLSCREEN) == 0 &&
           strstr (walpe_signal_missing (WALPE_SIG_FULLSCREEN), "version 1"), "B: status has the reason");
    CHECK (!g_strcmp0 (walpe_signal_sources (WALPE_SIG_COVERED), "wl-windows"), "B: covered has its source");
    /* outputs without names: everything is on the daemon's only output */
    EXPECT (WIN, WALPE_SIG_COVERED, 7, 1, "B: nameless output mapped to the only one");
    EXPECT (WIN, WALPE_SIG_FOCUS_APP, 7, 1, "B: focus");
    /* a fullscreen state must not reach a version 1 client (the stand-in
     * holds it back as a compositor does): the window is then plain */
    win_set (0, A_ | F_, 1u << 0);
    EXPECT (WIN, WALPE_SIG_COVERED, 7, 0, "B: version 1 cannot see fullscreen");
    /* two daemon outputs and no names: no way to tell, nothing is said */
    single_id = 0;
    win_set (0, A_ | M_, 1u << 0);
    walpe_sigprov_outputs_changed ();
    EXPECT (WIN, WALPE_SIG_COVERED, 7, 0, "B: without names and with several outputs nothing is claimed");
    stub_reset ();
    srv_down ();
}

static void scenario_c (void)
{
    SrvConf conf = { .seat = 7, .idle = 0, .toplevel = 0, .ext_list = 1, .out_version = 4,
                     .out_names = { "DP-1" } };
    if (!srv_start (&conf, "C"))
        return;
    providers_start ();
    CHECK (!started (FS) && !started (WIN) && !started (IDL), "C: nothing starts");
    CHECK (strstr (why_of (WIN), "ext_foreign_toplevel_list_v1") && strstr (why_of (WIN), "state"),
           "C: windows: %s", why_of (WIN));
    CHECK (strstr (why_of (FS), "ext_foreign_toplevel_list_v1") != NULL, "C: fullscreen: %s", why_of (FS));
    CHECK (strstr (why_of (IDL), "ext_idle_notifier_v1") != NULL, "C: idle: %s", why_of (IDL));
    CHECK (Stub.n_val == 0, "C: nothing was reported");
    stub_reset ();
    srv_down ();
}

static void scenario_d (void)
{
    SrvConf conf = { .seat = 7, .idle = 2, .toplevel = 2, .out_version = 4, .out_names = { "eDP-1" } };
    if (!srv_start (&conf, "D"))
        return;
    memset (id_map, 0, sizeof id_map);
    id_map[0].name = "eDP-1";
    id_map[0].id = 5;
    single_id = 5;
    walpe_sigprov_set_output_lookup (lookup, NULL);
    walpe_sigprov_set_idle_timeout (0);
    g_setenv ("WALPE_WL_COVERED", "all", TRUE);
    win_add (0, "mpv", F_, 1u << 0);
    win_add (1, "foot", A_, 0);     /* focused, on no known output */
    providers_start ();
    CHECK (started (FS) && started (WIN) && started (IDL), "D: all start");
    CHECK (stub_value (WIN, WALPE_SIG_COVERED, 5) == 1, "D: WALPE_WL_COVERED=all counts the unfocused fullscreen window");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 0, "D: the fullscreen rule still wants the focus");
    CHECK (stub_value (WIN, WALPE_SIG_FOCUS_APP, 5) == 0, "D: a focused window on no output is nowhere");
    settle ();
    CHECK (g_atomic_int_get (&S.n_idle) == 0, "D: idle rule off from the start: no object");
    /* stopping gives the handles back and leaves no value on */
    int before = g_atomic_int_get (&S.handles_destroyed);
    stub_reset ();
    g_usleep (100000);
    CHECK (g_atomic_int_get (&S.handles_destroyed) >= before + 2, "D: stop destroys the window handles");
    g_unsetenv ("WALPE_WL_COVERED");
    srv_down ();
}

/* the note of the fullscreen rule in status ("pause_rules"), NULL = none */
static char *fs_note (gboolean all_wanted)
{
    WalpeConfig cfg = { 0 };
    cfg.pause_fullscreen = TRUE;
    cfg.pause_fullscreen_all = all_wanted;
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    walpe_sigprov_status_json (b, &cfg);
    json_builder_end_object (b);
    JsonNode *root = json_builder_get_root (b);
    g_object_unref (b);
    JsonObject *r = json_object_get_object_member (json_node_get_object (root), "pause_rules");
    JsonObject *o = r ? json_object_get_object_member (r, "pause_fullscreen") : NULL;
    char *note = o && json_object_has_member (o, "note")
                     ? g_strdup (json_object_get_string_member (o, "note")) : NULL;
    if (o && !json_object_get_boolean_member (o, "works")) {
        g_free (note);
        note = g_strdup ("(the rule does not work)");
    }
    json_node_unref (root);
    return note;
}

/* ---- G: fullscreen windows whose output cannot be told. A compositor
 * whose wl_output is older than version 4 names no output: with one
 * monitor every window is on that one; with several nothing says which,
 * and the fullscreen rule counts the window for every output - what it
 * did before it looked at outputs - and says so in status. The same for
 * a window the compositor announces no output for. */
static void scenario_g (void)
{
    SrvConf conf = { .seat = 7, .idle = 0, .toplevel = 3, .out_version = 3,
                     .out_names = { "unnamed-0", "unnamed-1" } };
    if (!srv_start (&conf, "G"))
        return;
    memset (id_map, 0, sizeof id_map);
    single_id = 7;          /* the daemon shows on a single output */
    walpe_sigprov_set_output_lookup (lookup, NULL);
    walpe_sigprov_set_see_through (NULL);
    walpe_sigprov_set_idle_timeout (0);
    /* (no window is given an output before the providers have looked: a
     * compositor that has named no output for any window may be one that
     * never does, and its fullscreen window counts for every output) */
    win_add (0, "mpv", A_ | F_, 0);
    providers_start ();
    CHECK (started (FS) && started (WIN), "G: both window providers start (%s|%s)", why_of (FS), why_of (WIN));
    EXPECT (FS, ON, ALL, 1, "G: no output named for any window so far: every output");
    win_set (0, A_ | F_, 1u << 0);
    EXPECT (FS, ON, ALL, 0, "G: the compositor names the window's output");
    CHECK (!g_strcmp0 (walpe_signal_sources (ON), "wl-fullscreen"), "G: status names who tells the output");
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 1, "G: a focused fullscreen window");
    EXPECT (FS, ON, 7, 1, "G: no names, one monitor: the window is on that one");
    CHECK (stub_value (FS, ON, ALL) == 0, "G: ... by its id, not as \"every output\"");
    char *note = fs_note (FALSE);
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL && note == NULL,
           "G: one monitor: nothing to explain (%s)", note ? note : "no note");
    g_free (note);

    /* the daemon shows on several outputs: an output without a name can be
     * any of them */
    single_id = 0;
    walpe_sigprov_outputs_changed ();
    EXPECT (FS, ON, ALL, 1, "G: no names, several monitors: every output");
    EXPECT (FS, ON, 7, 0, "G: and no id of its own");
    CHECK (stub_value (FS, WALPE_SIG_FULLSCREEN, 0) == 1, "G: the window is still there");
    CHECK (walpe_signal_get (ON, 1) && walpe_signal_get (ON, 2), "G: as the daemon reads it: each of its monitors");
    const char *why = walpe_sig_wl_fullscreen_everywhere ();
    CHECK (why && strstr (why, "wl_output") && strstr (why, "version 4"), "G: the reason: %s", why ? why : "(none)");
    note = fs_note (FALSE);
    CHECK (note && strstr (note, "pauses every monitor") && strstr (note, "wl_output"),
           "G: status says that every monitor pauses and why: %s", note ? note : "(no note)");
    g_free (note);
    note = fs_note (TRUE);
    CHECK (note == NULL, "G: with pause_fullscreen_all set that is what was asked for: no note (%s)",
           note ? note : "none");
    g_free (note);

    /* the window loses the focus: no monitor has a focused fullscreen
     * window, but status still says what would happen (the outputs are
     * as unnamed as before) */
    win_set (0, F_, 1u << 0);
    EXPECT (FS, WALPE_SIG_FULLSCREEN, 0, 0, "G: focus lost");
    EXPECT (FS, ON, ALL, 0, "G: focus lost: no output has it");
    why = walpe_sig_wl_fullscreen_everywhere ();
    CHECK (why && strstr (why, "wl_output"), "G: status keeps the reason while nothing is fullscreen");

    /* one monitor again; a window the compositor names no output for */
    single_id = 7;
    walpe_sigprov_outputs_changed ();
    settle ();
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL, "G: one monitor again: no reason left");
    win_set (0, A_ | F_, 0);
    EXPECT (FS, ON, ALL, 1, "G: a fullscreen window on no announced output: every output");
    why = walpe_sig_wl_fullscreen_everywhere ();
    CHECK (why && strstr (why, "which output"), "G: the reason: %s", why ? why : "(none)");
    win_set (0, A_ | F_, 1u << 1);
    EXPECT (FS, ON, ALL, 0, "G: the compositor names its output: known again");
    EXPECT (FS, ON, 7, 1, "G: ... the only monitor");
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL, "G: nothing to explain any more");

    /* stopping leaves no value on */
    win_set (0, A_ | F_, 0);
    EXPECT (FS, ON, ALL, 1, "G: every output once more");
    walpe_signals_stop ();
    CHECK (!walpe_signal_get (ON, 7) && !walpe_signal_get (ON, ALL) &&
           !walpe_signal_get (WALPE_SIG_FULLSCREEN, 0), "G: stopped: nothing stays on");
    CHECK (walpe_sig_wl_fullscreen_everywhere () == NULL, "G: stopped: no reason");
    stub_reset ();
    srv_down ();
}

/* ---- E: a compositor that accepts the connection and never says a word
 * (a listening socket nobody serves): the providers give up within a few
 * seconds and say why, instead of holding the caller for ever */
static void scenario_e (const char *dir)
{
    char *path = g_build_filename (dir, "walpe-sigtest-silent", NULL);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen (path) >= sizeof sa.sun_path) {
        printf ("sig-wl: E skipped: the temporary directory's path is too long for a socket\n");
        g_free (path);
        return;
    }
    g_strlcpy (sa.sun_path, path, sizeof sa.sun_path);
    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK (fd >= 0 && bind (fd, (struct sockaddr *) &sa, sizeof sa) == 0 && listen (fd, 8) == 0,
           "E: the silent socket is there");
    g_setenv ("WALPE_SIG_WAYLAND_DISPLAY", path, TRUE);
    gint64 t0 = g_get_monotonic_time ();
    providers_start ();
    double took = (double) (g_get_monotonic_time () - t0) / G_USEC_PER_SEC;
    CHECK (took < 9.0, "E: the start came back after %.1f s", took);
    CHECK (!started (FS) && !started (WIN) && !started (IDL), "E: no provider starts");
    CHECK (strstr (why_of (WIN), "did not answer") != NULL, "E: windows: %s", why_of (WIN));
    CHECK (strstr (why_of (IDL), "did not answer") != NULL, "E: idle: %s", why_of (IDL));
    stub_reset ();
    if (fd >= 0)
        close (fd);
    g_unlink (path);
    g_free (path);
}

/* ---- F: a temporary directory whose path leaves no room for a socket
 * name (TMPDIR of a build deep in the file system). The compositor's
 * thread ended with "socket path too long" and the test waited for ever
 * for it. This test is run again with such a TMPDIR, start only: it takes
 * a directory of its own (main), its compositor comes up, and it ends. */
#define LONG_NAME 120

static void scenario_start_only (void)
{
    SrvConf conf = { .seat = 7, .out_version = 4, .out_names = { "DP-1" } };
    if (!srv_start (&conf, "of the start-only run"))
        return;
    win_add (0, "foot", A_, 1u << 0);
    srv_down ();
}

static void scenario_f (const char *self, const char *dir)
{
    char name[LONG_NAME + 1];
    memset (name, 'x', LONG_NAME);
    name[LONG_NAME] = 0;
    char *deep = g_build_filename (dir, name, NULL);
    CHECK (g_mkdir (deep, 0700) == 0, "F: a directory with a name of %d bytes", LONG_NAME);
    char **env = g_environ_setenv (g_get_environ (), "TMPDIR", deep, TRUE);
    char *argv[] = { (char *) self, (char *) "--start-only", NULL };
    GPid pid = 0;
    gboolean ended = FALSE;
    int st = 0;
    if (g_spawn_async (NULL, argv, env, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL)) {
        const gint64 until = g_get_monotonic_time () + 20 * G_USEC_PER_SEC;
        while (!(ended = waitpid (pid, &st, WNOHANG) == pid) && g_get_monotonic_time () < until)
            g_usleep (20000);
        if (!ended) {
            kill (pid, SIGKILL);
            waitpid (pid, &st, 0);
        }
        CHECK (ended, "F: the run with a TMPDIR of %d bytes ends by itself", (int) strlen (deep));
        CHECK (ended && WIFEXITED (st) && WEXITSTATUS (st) == 0, "F: ... and passes (wait status 0x%x)",
               (guint) st);
    } else {
        CHECK (FALSE, "F: %s --start-only is started", self);
    }
    g_strfreev (env);
    /* (what a run that had to be stopped left behind) */
    GDir *d = g_dir_open (deep, 0, NULL);
    const char *sub;
    while (d && (sub = g_dir_read_name (d))) {
        char *subdir = g_build_filename (deep, sub, NULL);
        GDir *d2 = g_dir_open (subdir, 0, NULL);
        const char *leaf;
        while (d2 && (leaf = g_dir_read_name (d2))) {
            char *lp = g_build_filename (subdir, leaf, NULL);
            g_unlink (lp);
            g_free (lp);
        }
        if (d2)
            g_dir_close (d2);
        g_rmdir (subdir);
        g_free (subdir);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (deep);
    g_free (deep);
}

/* the sockets of this test under dir: the compositor's ("walpe-sigtest-N")
 * and scenario E's, the longest name. An address holds sizeof sun_path
 * bytes with its NUL (108 on Linux). */
static gboolean dir_fits (const char *dir)
{
    struct sockaddr_un sa;
    return strlen (dir) + 1 + strlen ("walpe-sigtest-silent") < sizeof sa.sun_path;
}

int main (int argc, char **argv)
{
    const gboolean start_only = argc > 1 && !strcmp (argv[1], "--start-only");
    /* a runtime directory of its own: the socket never meets a real
     * session. Where TMPDIR points to is not ours to choose, and may be
     * too long for a socket path: then a directory in /tmp. */
    char *dir = g_dir_make_tmp ("walpe-sigwl-XXXXXX", NULL);
    if (!dir) {
        fprintf (stderr, "sig-wl: no temporary directory\n");
        return 1;
    }
    if (!dir_fits (dir)) {
        g_rmdir (dir);
        g_free (dir);
        dir = g_strdup ("/tmp/walpe-sigwl-XXXXXX");
        if (!g_mkdtemp (dir)) {
            printf ("sig-wl: skipped: the temporary directory's path is too long for a socket and no "
                    "directory can be made in /tmp\n");
            g_free (dir);
            return 77;
        }
    }
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
    g_setenv ("WALPE_LOGIND", "0", TRUE);
    S.wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    S.calls = g_async_queue_new ();

    if (start_only) {
        scenario_start_only ();
    } else {
        scenario_a ();
        scenario_b ();
        scenario_c ();
        scenario_d ();
        scenario_g ();
        scenario_e (dir);
        char *self = g_file_read_link ("/proc/self/exe", NULL);
        scenario_f (self ? self : argv[0], dir);
        g_free (self);
    }

    /* the sockets and their lock files */
    for (int i = 0; i < srv_serial; i++) {
        char *n = g_strdup_printf ("%s/walpe-sigtest-%d", dir, i);
        char *l = g_strconcat (n, ".lock", NULL);
        g_unlink (n);
        g_unlink (l);
        g_free (n);
        g_free (l);
    }
    g_rmdir (dir);
    g_free (dir);

    if (stub_fails) {
        fprintf (stderr, "sig-wl: %d check(s) failed\n", stub_fails);
        return 1;
    }
    printf (start_only ? "sig-wl: start ok\n" : "sig-wl: ok\n");
    return 0;
}
