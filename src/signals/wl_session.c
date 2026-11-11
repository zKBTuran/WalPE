/* wl_session.c - a Wayland connection of their own for the pause
 * providers (wl_idle.c, wl_toplevel.c).
 *
 * Why not the connection the wallpaper is drawn on: what happens here is
 * optional. A compositor that sends something unexpected on one of the
 * window-list protocols, or a mistake in how they are used, ends in a
 * protocol error, and a protocol error ends the connection it happened
 * on. On a connection of its own that costs the pause rules; on the
 * drawing connection it would cost the wallpaper. (The X11 side does the
 * same for "skip hidden areas".) It also means these files need nothing
 * from the drawing code but the daemon's id for an output name.
 *
 * The session binds wl_seat and every wl_output (version 4 when offered:
 * that is where the output's name comes from), remembers which globals
 * exist (wlplan.h decides what they allow) and feeds the connection to
 * the main loop through a GSource. It is shared and reference counted;
 * the last unref disconnects. */
#include "walpe-build-config.h"
#include "signals/wl_session.h"
#include "signals/providers.h"
#include <errno.h>
#include <poll.h>
#include <string.h>

typedef struct {
    guint32 name;
    char *interface;
    guint32 version;
} WsGlobal;

typedef struct {
    GSource src;
    WlSession *s;
    gpointer tag;
    gboolean reading;       /* between wl_display_prepare_read and read / cancel */
    gboolean error;
} WsSource;

struct WlSession {
    int refs;
    struct wl_display *dpy;
    struct wl_registry *reg;
    WsSource *src;
    gboolean alive;
    WalpeWlCaps caps;
    GArray *globals;        /* WsGlobal */
    struct wl_seat *seat;
    GPtrArray *outputs;     /* WlSessOutput * */
    GPtrArray *users;       /* const WlSessUser * */
    gboolean notify_outputs;    /* collect: tell the users once per dispatch */
    guint notify_id;
};

static WlSession *the_session;

/* ---- telling the users */

static void ws_notify_now (WlSession *s)
{
    if (s->notify_id) {
        g_source_remove (s->notify_id);
        s->notify_id = 0;
    }
    s->notify_outputs = FALSE;
    /* a user may remove itself from inside the call */
    GPtrArray *copy = g_ptr_array_copy (s->users, NULL, NULL);
    for (guint i = 0; i < copy->len; i++) {
        const WlSessUser *u = copy->pdata[i];
        if (g_ptr_array_find (s->users, u, NULL) && u->outputs_changed)
            u->outputs_changed (u->user);
    }
    g_ptr_array_unref (copy);
}

static gboolean ws_notify_cb (gpointer user)
{
    WlSession *s = user;
    s->notify_id = 0;
    if (s->notify_outputs)
        ws_notify_now (s);
    return G_SOURCE_REMOVE;
}

/* several events of one output arrive in a burst: one call after it */
static void ws_outputs_dirty (WlSession *s)
{
    s->notify_outputs = TRUE;
    if (!s->notify_id)
        s->notify_id = g_idle_add (ws_notify_cb, s);
}

static void ws_output_lookup (WlSessOutput *o)
{
    o->id = o->done ? walpe_sigprov_output_id (o->name) : 0;
}

/* ---- wl_output */

static void out_geometry (void *data, struct wl_output *out, int32_t x, int32_t y, int32_t pw,
                          int32_t ph, int32_t subpixel, const char *make, const char *model,
                          int32_t transform)
{
    (void) data; (void) out; (void) x; (void) y; (void) pw; (void) ph; (void) subpixel;
    (void) make; (void) model; (void) transform;
}

static void out_mode (void *data, struct wl_output *out, uint32_t flags, int32_t w, int32_t h,
                      int32_t refresh)
{
    (void) data; (void) out; (void) flags; (void) w; (void) h; (void) refresh;
}

static void out_scale (void *data, struct wl_output *out, int32_t factor)
{
    (void) data; (void) out; (void) factor;
}

static void out_description (void *data, struct wl_output *out, const char *text)
{
    (void) data; (void) out; (void) text;
}

static WlSessOutput *ws_output_by_proxy (WlSession *s, struct wl_output *proxy)
{
    for (guint i = 0; s && i < s->outputs->len; i++) {
        WlSessOutput *o = s->outputs->pdata[i];
        if (o->proxy == proxy)
            return o;
    }
    return NULL;
}

static void out_name (void *data, struct wl_output *out, const char *name)
{
    WlSessOutput *o = ws_output_by_proxy (data, out);
    if (!o || !g_strcmp0 (o->name, name))
        return;
    g_free (o->name);
    o->name = g_strdup (name);
    /* version 4 sends done after it; a later rename does too */
}

static void out_done (void *data, struct wl_output *out)
{
    WlSession *s = data;
    WlSessOutput *o = ws_output_by_proxy (s, out);
    if (!o)
        return;
    o->done = TRUE;
    ws_output_lookup (o);
    ws_outputs_dirty (s);
}

static const struct wl_output_listener out_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_description,
};

static void ws_output_free (gpointer p)
{
    WlSessOutput *o = p;
    if (o->proxy)
        wl_output_destroy (o->proxy);   /* (release exists from version 3; destroy is always right) */
    g_free (o->name);
    g_free (o);
}

/* ---- registry */

static void reg_global (void *data, struct wl_registry *reg, uint32_t name, const char *interface,
                        uint32_t version)
{
    WlSession *s = data;
    WsGlobal g = { name, g_strdup (interface), version };
    g_array_append_val (s->globals, g);
    walpe_wlcaps_add (&s->caps, interface, version);
    if (!strcmp (interface, "wl_seat") && !s->seat) {
        /* version 1 is all the idle notification needs (a seat to name) */
        s->seat = wl_registry_bind (reg, name, &wl_seat_interface, 1);
    } else if (!strcmp (interface, "wl_output")) {
        WlSessOutput *o = g_new0 (WlSessOutput, 1);
        o->global = name;
        o->version = MIN (version, 4u);
        o->proxy = wl_registry_bind (reg, name, &wl_output_interface, o->version);
        g_ptr_array_add (s->outputs, o);
        wl_output_add_listener (o->proxy, &out_listener, s);
        if (o->version < 2) {
            /* version 1 has no done event: complete as it is, nameless */
            o->done = TRUE;
            ws_output_lookup (o);
            ws_outputs_dirty (s);
        }
    }
}

static void reg_global_remove (void *data, struct wl_registry *reg, uint32_t name)
{
    (void) reg;
    WlSession *s = data;
    for (guint i = 0; i < s->globals->len; i++) {
        WsGlobal *g = &g_array_index (s->globals, WsGlobal, i);
        if (g->name != name)
            continue;
        if (!strcmp (g->interface, "wl_output")) {
            for (guint k = 0; k < s->outputs->len; k++) {
                WlSessOutput *o = s->outputs->pdata[k];
                if (o->global == name) {
                    /* the users drop their pointers to it in the call
                     * that follows; until then it stays in memory */
                    o->id = 0;
                    o->done = FALSE;
                    g_ptr_array_steal_index (s->outputs, k);
                    /* (freed after the users were told: they compare
                     * proxies, they do not keep the struct) */
                    ws_notify_now (s);
                    ws_output_free (o);
                    break;
                }
            }
        }
        g_free (g->interface);
        g_array_remove_index (s->globals, i);
        break;
    }
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

/* ---- main loop */

static void ws_lost (WlSession *s)
{
    if (!s->alive)
        return;
    s->alive = FALSE;
    int err = wl_display_get_error (s->dpy);
    g_message ("pause providers: the Wayland connection ended (%s); the rules that depend on the "
               "compositor report nothing from here on",
               err ? g_strerror (err) : "closed by the compositor");
    GPtrArray *copy = g_ptr_array_copy (s->users, NULL, NULL);
    for (guint i = 0; i < copy->len; i++) {
        const WlSessUser *u = copy->pdata[i];
        if (g_ptr_array_find (s->users, u, NULL) && u->lost)
            u->lost (u->user);
    }
    g_ptr_array_unref (copy);
}

static gboolean ws_prepare (GSource *src, gint *timeout)
{
    WsSource *w = (WsSource *) src;
    WlSession *s = w->s;
    *timeout = -1;
    if (!s->alive || w->reading)
        return FALSE;
    /* events read earlier and not handed out yet: dispatch first */
    if (wl_display_prepare_read (s->dpy) != 0)
        return TRUE;
    w->reading = TRUE;
    if (wl_display_flush (s->dpy) < 0 && errno != EAGAIN) {
        wl_display_cancel_read (s->dpy);
        w->reading = FALSE;
        w->error = TRUE;
        return TRUE;
    }
    return FALSE;
}

static gboolean ws_check (GSource *src)
{
    WsSource *w = (WsSource *) src;
    WlSession *s = w->s;
    if (!w->reading)
        return w->error;
    w->reading = FALSE;
    GIOCondition c = g_source_query_unix_fd (src, w->tag);
    if (c & G_IO_IN) {
        if (wl_display_read_events (s->dpy) < 0)
            w->error = TRUE;
    } else {
        wl_display_cancel_read (s->dpy);
    }
    if (c & (G_IO_ERR | G_IO_HUP))
        w->error = TRUE;
    return (c & G_IO_IN) || w->error;
}

static gboolean ws_dispatch (GSource *src, GSourceFunc cb, gpointer user)
{
    (void) cb; (void) user;
    WsSource *w = (WsSource *) src;
    WlSession *s = w->s;
    if (!s->alive)
        return G_SOURCE_REMOVE;
    if (wl_display_dispatch_pending (s->dpy) < 0)
        w->error = TRUE;
    if (w->error) {
        ws_lost (s);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs ws_funcs = { ws_prepare, ws_check, ws_dispatch, NULL, NULL, NULL };

/* ---- life */

static void ws_free (WlSession *s)
{
    if (s->notify_id)
        g_source_remove (s->notify_id);
    if (s->src) {
        if (s->src->reading)
            wl_display_cancel_read (s->dpy);
        g_source_destroy ((GSource *) s->src);
        g_source_unref ((GSource *) s->src);
    }
    g_ptr_array_free (s->outputs, TRUE);
    if (s->seat)
        wl_seat_destroy (s->seat);
    if (s->reg)
        wl_registry_destroy (s->reg);
    for (guint i = 0; i < s->globals->len; i++)
        g_free (g_array_index (s->globals, WsGlobal, i).interface);
    g_array_free (s->globals, TRUE);
    g_ptr_array_free (s->users, TRUE);
    if (s->dpy)
        wl_display_disconnect (s->dpy);
    g_free (s);
}

static void ws_sync_done (void *data, struct wl_callback *cb, uint32_t serial)
{
    (void) cb; (void) serial;
    *(gboolean *) data = TRUE;
}

static const struct wl_callback_listener ws_sync_listener = { ws_sync_done };

/* One round trip on the default queue, given up at `end`
 * (g_get_monotonic_time ()): 1 = answered, 0 = no answer in time, -1 = the
 * connection failed. Nobody else may be reading the connection. */
static int ws_roundtrip_until (struct wl_display *dpy, gint64 end)
{
    gboolean done = FALSE;
    int rc = 1;
    struct wl_callback *cb = wl_display_sync (dpy);
    if (!cb)
        return -1;
    wl_callback_add_listener (cb, &ws_sync_listener, &done);
    while (!done) {
        if (wl_display_prepare_read (dpy) != 0) {
            /* events read earlier wait to be handed out */
            if (wl_display_dispatch_pending (dpy) < 0) {
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
        gint64 left = (end - g_get_monotonic_time ()) / 1000;
        struct pollfd pfd = { wl_display_get_fd (dpy), POLLIN, 0 };
        int n = left > 0 ? poll (&pfd, 1, (int) MIN (left, 3600 * 1000)) : 0;
        if (n < 0 && errno == EINTR) {
            wl_display_cancel_read (dpy);
            continue;
        }
        if (n <= 0) {
            /* time is up */
            wl_display_cancel_read (dpy);
            rc = n == 0 ? 0 : -1;
            break;
        }
        if (wl_display_read_events (dpy) < 0 || wl_display_dispatch_pending (dpy) < 0) {
            rc = -1;
            break;
        }
    }
    /* (a `done` that comes after this is dropped with the object) */
    wl_callback_destroy (cb);
    return rc;
}

/* The first two round trips of a new connection get this long together:
 * a compositor that accepted the connection and then says nothing must
 * not hold the daemon's start. Each provider asks for the connection on
 * its own: after one such silence the next ones are refused at once for a
 * while instead of waiting the same time again. */
#define WS_START_MS 5000
#define WS_SILENT_HOLD_MS 10000
static gint64 ws_silent_at;     /* g_get_monotonic_time () of the last silence; 0 = none */

WlSession *walpe_wlsess_ref (char **why)
{
    if (the_session) {
        if (!the_session->alive) {
            if (why)
                *why = g_strdup ("the Wayland connection of the pause providers has ended");
            return NULL;
        }
        the_session->refs++;
        return the_session;
    }
    if (ws_silent_at &&
        g_get_monotonic_time () - ws_silent_at < (gint64) WS_SILENT_HOLD_MS * 1000) {
        if (why)
            *why = g_strdup_printf ("%s (the compositor did not answer)", WALPE_WLPLAN_NO_CONN);
        return NULL;
    }
    /* WALPE_SIG_WAYLAND_DISPLAY (tests): a compositor other than the
     * session's, by socket name */
    const char *name = g_getenv ("WALPE_SIG_WAYLAND_DISPLAY");
    struct wl_display *dpy = wl_display_connect (name && *name ? name : NULL);
    if (!dpy) {
        if (why)
            *why = g_strdup_printf ("%s (%s)", WALPE_WLPLAN_NO_CONN, g_strerror (errno));
        return NULL;
    }
    WlSession *s = g_new0 (WlSession, 1);
    s->refs = 1;
    s->dpy = dpy;
    s->alive = TRUE;
    s->globals = g_array_new (FALSE, TRUE, sizeof (WsGlobal));
    s->outputs = g_ptr_array_new_with_free_func (ws_output_free);
    s->users = g_ptr_array_new ();
    s->reg = wl_display_get_registry (dpy);
    wl_registry_add_listener (s->reg, &reg_listener, s);
    /* the globals, then what the bound ones say about themselves */
    gint64 end = g_get_monotonic_time () + (gint64) WS_START_MS * 1000;
    int rt = ws_roundtrip_until (dpy, end);
    if (rt > 0)
        rt = ws_roundtrip_until (dpy, end);
    ws_silent_at = rt == 0 ? g_get_monotonic_time () : 0;
    if (rt <= 0) {
        if (why)
            *why = g_strdup_printf ("%s (%s)", WALPE_WLPLAN_NO_CONN,
                                    rt == 0 ? "the compositor did not answer"
                                            : g_strerror (wl_display_get_error (dpy)));
        ws_free (s);
        return NULL;
    }
    WsSource *w = (WsSource *) g_source_new (&ws_funcs, sizeof (WsSource));
    w->s = s;
    w->tag = g_source_add_unix_fd ((GSource *) w, wl_display_get_fd (dpy),
                                   G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_set_name ((GSource *) w, "walpe pause providers (wayland)");
    g_source_attach ((GSource *) w, NULL);
    s->src = w;
    the_session = s;
    return s;
}

void walpe_wlsess_unref (WlSession *s)
{
    if (!s || --s->refs > 0)
        return;
    if (the_session == s)
        the_session = NULL;
    ws_free (s);
}

gboolean walpe_wlsess_alive (WlSession *s)
{
    return s && s->alive;
}

const WalpeWlCaps *walpe_wlsess_caps (WlSession *s)
{
    return &s->caps;
}

struct wl_seat *walpe_wlsess_seat (WlSession *s)
{
    return s->seat;
}

void *walpe_wlsess_bind (WlSession *s, const struct wl_interface *iface, guint32 max_version,
                         guint32 *bound_version)
{
    if (!s->alive)
        return NULL;
    for (guint i = 0; i < s->globals->len; i++) {
        WsGlobal *g = &g_array_index (s->globals, WsGlobal, i);
        if (strcmp (g->interface, iface->name) != 0)
            continue;
        guint32 v = MIN (g->version, max_version);
        if (bound_version)
            *bound_version = v;
        return wl_registry_bind (s->reg, g->name, iface, v);
    }
    return NULL;
}

GPtrArray *walpe_wlsess_outputs (WlSession *s)
{
    return s->outputs;
}

WlSessOutput *walpe_wlsess_output_of (WlSession *s, struct wl_output *proxy)
{
    return ws_output_by_proxy (s, proxy);
}

void walpe_wlsess_add_user (WlSession *s, const WlSessUser *u)
{
    if (!g_ptr_array_find (s->users, u, NULL))
        g_ptr_array_add (s->users, (gpointer) u);
}

void walpe_wlsess_remove_user (WlSession *s, const WlSessUser *u)
{
    g_ptr_array_remove (s->users, (gpointer) u);
}

void walpe_wlsess_flush (WlSession *s)
{
    if (s->alive && wl_display_flush (s->dpy) < 0 && errno != EAGAIN && s->src)
        s->src->error = TRUE;   /* reported from the main loop */
}

/* A round trip with a time limit. wl_display_roundtrip () waits for the
 * compositor without end, and this one is also made while a provider
 * stops (the window list's stop / finished): a compositor that does not
 * answer - hung, or held in a debugger - would keep the daemon from
 * leaving. FALSE: the connection failed or no answer came in time; what
 * arrives later is handled from the main loop like everything else. */
#define WS_ROUNDTRIP_MS 2000

gboolean walpe_wlsess_roundtrip (WlSession *s)
{
    if (!s->alive)
        return FALSE;
    /* The main loop's source may still hold the read it announced in its
     * prepare step (a turn of the loop in which its check step was not
     * reached). A second reader on this thread would wait for that one
     * for ever: give it back first. */
    if (s->src && s->src->reading) {
        wl_display_cancel_read (s->dpy);
        s->src->reading = FALSE;
    }
    return ws_roundtrip_until (s->dpy,
                               g_get_monotonic_time () + (gint64) WS_ROUNDTRIP_MS * 1000) > 0;
}

void walpe_wlsess_relookup (WlSession *s)
{
    for (guint i = 0; i < s->outputs->len; i++)
        ws_output_lookup (s->outputs->pdata[i]);
    ws_outputs_dirty (s);
}

/* providers.h: the daemon's outputs changed */
void walpe_sig_wl_outputs_changed (void)
{
    if (the_session && the_session->alive)
        walpe_wlsess_relookup (the_session);
}
