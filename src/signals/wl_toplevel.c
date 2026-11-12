/* wl_toplevel.c - providers "wl-fullscreen" and "wl-windows": what the
 * compositor's window list says about fullscreen, covering and focused
 * windows (wlr-foreign-toplevel-management-unstable-v1).
 *
 * The compositor announces every toplevel as an object and keeps sending
 * its state (maximized, minimized, activated, fullscreen), its app id and
 * the outputs it is on; a batch of changes ends with `done`. From that:
 *
 *   fullscreen (global)   an activated toplevel is fullscreen
 *   fullscreen_on (output) ... and is on the output; on every output
 *                         (WALPE_OUTPUT_ALL) where the compositor does not
 *                         say which one it is on (wlplan.h)
 *   focus_app (output)    an activated toplevel is on the output
 *   covered (output)      a fullscreen or maximized toplevel is on it and
 *                         is not minimized, not in the "see-through
 *                         windows" list, and - unless WALPE_WL_COVERED=all
 *                         - is the activated one (wlplan.h says why)
 *
 * This is coarser than the X11 side, which knows every window's rectangle
 * and counts 85 % coverage: Wayland tells a client no geometry. Tiling
 * compositors report tiled windows as neither maximized nor fullscreen,
 * so there "covered" means a fullscreen window; that the compositor stops
 * asking a hidden surface for frames is seen by the drawing side.
 *
 * Two providers, one tracker: "wl-fullscreen" needs version 2 of the
 * protocol (version 1 has no fullscreen state), "wl-windows" any version;
 * each can be absent without the other. ext-foreign-toplevel-list-v1
 * carries no state at all and cannot stand in (wlplan.h words the reason
 * for status). */
#include "walpe-build-config.h"
#include "signals/providers.h"
#include "signals/wl_session.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

typedef struct {
    struct zwlr_foreign_toplevel_handle_v1 *h;
    guint state;            /* WALPE_TL_*, as of the last done */
    guint pending;          /* as of the last state event */
    GPtrArray *outs;        /* struct wl_output * of the session (borrowed) */
    gboolean had_output;    /* the compositor has named an output for it at some time */
    gboolean changed;       /* an event since its last done said something the rules
                             * read: its outputs or its app id (its state: pending) */
    char *app_id;
} Tl;

static struct {
    WlSession *sess;
    struct zwlr_foreign_toplevel_manager_v1 *mgr;
    guint32 version;
    GPtrArray *tls;         /* Tl * */
    gboolean fs_on, win_on; /* which of the two providers is started */
    gboolean cover_all;     /* WALPE_WL_COVERED=all */
    guint recompute_id;
    GArray *reported;       /* guint32: output ids a per-output value was given for */
    GArray *fs_reported;    /* guint32: output ids fullscreen_on was given for */
    WalpeTlWhere fs_where;  /* why fullscreen_on is reported for every output now
                             * (WALPE_TL_WHERE_KNOWN = it is not) */
    gboolean names_outputs; /* this compositor has named an output for a window
                             * (output_enter) since the tracker came up */
    guint passes;           /* tl_recompute runs (walpe_sig_wl_passes) */
} W;

static void tl_free (gpointer p)
{
    Tl *t = p;
    if (t->h)
        zwlr_foreign_toplevel_handle_v1_destroy (t->h);
    g_ptr_array_free (t->outs, TRUE);
    g_free (t->app_id);
    g_free (t);
}

/* ---- values */

static gboolean tl_on_output (Tl *t, guint32 id)
{
    for (guint i = 0; i < t->outs->len; i++) {
        WlSessOutput *o = walpe_wlsess_output_of (W.sess, t->outs->pdata[i]);
        if (o && o->id == id)
            return TRUE;
    }
    return FALSE;
}

static gboolean id_listed (GArray *a, guint32 id)
{
    for (guint i = 0; i < a->len; i++)
        if (g_array_index (a, guint32, i) == id)
            return TRUE;
    return FALSE;
}

/* the daemon's outputs a window is on -> ids (no id twice); the answer
 * says whether that is the whole truth (wlplan.h) */
static WalpeTlWhere tl_where (Tl *t, GArray *ids)
{
    guint n = t->outs->len;
    guint32 *id = g_new0 (guint32, n + 1);
    gboolean *named = g_new0 (gboolean, n + 1);
    for (guint i = 0; i < n; i++) {
        WlSessOutput *o = walpe_wlsess_output_of (W.sess, t->outs->pdata[i]);
        id[i] = o ? o->id : 0;
        named[i] = o && o->name != NULL;
    }
    WalpeTlWhere w = walpe_tl_where (n, id, named);
    for (guint i = 0; w == WALPE_TL_WHERE_KNOWN && i < n; i++)
        if (id[i] && !id_listed (ids, id[i]))
            g_array_append_val (ids, id[i]);
    g_free (id);
    g_free (named);
    return w;
}

/* fullscreen: is there a focused fullscreen window, and on which of the
 * daemon's outputs (several windows can be activated at once: one per
 * seat). One whose output is not known counts for every output. */
static void tl_recompute_fullscreen (void)
{
    gboolean fs = FALSE;
    WalpeTlWhere where = WALPE_TL_WHERE_KNOWN;
    GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
    for (guint i = 0; i < W.tls->len; i++) {
        Tl *t = W.tls->pdata[i];
        if (!walpe_tl_is_fullscreen_focus (t->state))
            continue;
        fs = TRUE;
        /* A new window whose output has not been named yet. A compositor
         * tells a window's state when the window is mapped and its output
         * when the window has its place, which is later (sway: after the
         * client's first frame, tens of milliseconds). Counted for every
         * output meanwhile, a window that starts in fullscreen paused the
         * wallpapers of all other monitors for that moment. A compositor
         * that has named outputs before will name this one: until then
         * the window is on none. (One that never names any, and a window
         * that has lost the outputs it had, count for every output as
         * before.) */
        if (!t->outs->len && !t->had_output && W.names_outputs)
            continue;
        WalpeTlWhere w = tl_where (t, ids);
        if (w != WALPE_TL_WHERE_KNOWN && where == WALPE_TL_WHERE_KNOWN)
            where = w;
    }
    /* (often only for a moment: a compositor may announce a new window's
     * state before its output; status has the sentence while it lasts) */
    if (where != W.fs_where && where != WALPE_TL_WHERE_KNOWN)
        g_debug ("wl-toplevel: fullscreen counts for every monitor: %s",
                 walpe_tl_where_why (where));
    W.fs_where = where;
    walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN, 0, fs);
    walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN_ON, WALPE_OUTPUT_ALL,
                      where != WALPE_TL_WHERE_KNOWN);
    for (guint k = 0; k < ids->len; k++)
        walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN_ON,
                          g_array_index (ids, guint32, k), TRUE);
    /* an output the window left, or one that is gone */
    for (guint k = 0; k < W.fs_reported->len; k++) {
        guint32 id = g_array_index (W.fs_reported, guint32, k);
        if (!id_listed (ids, id))
            walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN_ON, id, FALSE);
    }
    g_array_free (W.fs_reported, TRUE);
    W.fs_reported = ids;
}

static void tl_recompute (void)
{
    if (W.recompute_id) {
        g_source_remove (W.recompute_id);
        W.recompute_id = 0;
    }
    if (!W.tls)
        return;
    W.passes++;
    const char *see = walpe_sigprov_see_through ();
    walpe_signals_begin ();
    if (W.fs_on)
        tl_recompute_fullscreen ();
    if (W.win_on) {
        /* the outputs the daemon shows on, as this connection knows them */
        GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
        GPtrArray *outs = W.sess ? walpe_wlsess_outputs (W.sess) : NULL;
        for (guint i = 0; outs && i < outs->len; i++) {
            WlSessOutput *o = outs->pdata[i];
            if (o->id && !id_listed (ids, o->id))
                g_array_append_val (ids, o->id);
        }
        for (guint k = 0; k < ids->len; k++) {
            guint32 id = g_array_index (ids, guint32, k);
            gboolean covered = FALSE, focus = FALSE;
            for (guint i = 0; i < W.tls->len; i++) {
                Tl *t = W.tls->pdata[i];
                if (!tl_on_output (t, id))
                    continue;
                focus = focus || walpe_tl_is_focus (t->state);
                covered = covered ||
                          walpe_tl_covers (t->state, W.cover_all, walpe_tl_listed (see, t->app_id));
            }
            walpe_signal_set (&walpe_sig_wl_windows, WALPE_SIG_COVERED, id, covered);
            walpe_signal_set (&walpe_sig_wl_windows, WALPE_SIG_FOCUS_APP, id, focus);
        }
        /* an output that is gone, or no longer one of the daemon's */
        for (guint k = 0; k < W.reported->len; k++) {
            guint32 id = g_array_index (W.reported, guint32, k);
            if (!id_listed (ids, id)) {
                walpe_signal_set (&walpe_sig_wl_windows, WALPE_SIG_COVERED, id, FALSE);
                walpe_signal_set (&walpe_sig_wl_windows, WALPE_SIG_FOCUS_APP, id, FALSE);
            }
        }
        g_array_free (W.reported, TRUE);
        W.reported = ids;
    }
    walpe_signals_end ();
}

static gboolean tl_recompute_cb (gpointer user)
{
    (void) user;
    W.recompute_id = 0;
    tl_recompute ();
    return G_SOURCE_REMOVE;
}

/* a workspace switch changes many windows in one burst: one pass after it */
static void tl_dirty (void)
{
    if (!W.recompute_id)
        W.recompute_id = g_idle_add_full (G_PRIORITY_DEFAULT, tl_recompute_cb, NULL, NULL);
}

/* ---- zwlr_foreign_toplevel_handle_v1 */

static void h_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    (void) data; (void) h; (void) title;    /* not needed, and not ours to keep */
}

static void h_app_id (void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id)
{
    (void) h;
    Tl *t = data;
    if (!g_strcmp0 (t->app_id, app_id))
        return;
    g_free (t->app_id);
    t->app_id = g_strdup (app_id);
    t->changed = TRUE;
}

static void h_output_enter (void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *out)
{
    (void) h;
    Tl *t = data;
    t->had_output = TRUE;
    t->changed = TRUE;
    W.names_outputs = TRUE;
    /* NULL: an output this connection has already let go of */
    if (out && !g_ptr_array_find (t->outs, out, NULL))
        g_ptr_array_add (t->outs, out);
}

static void h_output_leave (void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *out)
{
    (void) h;
    Tl *t = data;
    t->changed = TRUE;
    if (out)
        g_ptr_array_remove (t->outs, out);
}

static void h_state (void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st)
{
    (void) h;
    Tl *t = data;
    guint bits = 0;
    const uint32_t *v;
    wl_array_for_each (v, st) {
        switch (*v) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            bits |= WALPE_TL_MAXIMIZED;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            bits |= WALPE_TL_MINIMIZED;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            bits |= WALPE_TL_ACTIVATED;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            bits |= WALPE_TL_FULLSCREEN;
            break;
        default:    /* a state of a later version: not ours to interpret */
            break;
        }
    }
    t->pending = bits;
}

static void h_done (void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void) h;
    Tl *t = data;
    /* The list is worked through again when this window said something
     * the rules read: its state, its outputs, its app id. A compositor
     * sends `done` after a change of the title too - a terminal's prompt,
     * a browser tab, a player's clock, several times a second - and each
     * one used to cost a pass over every window and output (with its
     * arrays made and freed) that could only find what the last one had.
     * Everything else the rules read announces itself: the session's
     * outputs (tl_outputs_changed), the settings (tl_recheck), a window
     * that closes. */
    const gboolean changed = t->changed || t->pending != t->state;
    t->state = t->pending;
    t->changed = FALSE;
    if (changed)
        tl_dirty ();
}

static void h_closed (void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void) h;
    if (W.tls)
        g_ptr_array_remove (W.tls, data);   /* tl_free: destroys the handle */
    tl_dirty ();
}

static void h_parent (void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                      struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void) data; (void) h; (void) parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener h_listener = {
    .title = h_title,
    .app_id = h_app_id,
    .output_enter = h_output_enter,
    .output_leave = h_output_leave,
    .state = h_state,
    .done = h_done,
    .closed = h_closed,
    .parent = h_parent,
};

/* ---- zwlr_foreign_toplevel_manager_v1 */

static void m_toplevel (void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                        struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void) data; (void) m;
    Tl *t = g_new0 (Tl, 1);
    t->h = h;
    t->changed = TRUE;      /* its first done: a window more */
    t->outs = g_ptr_array_new ();
    if (W.tls) {
        g_ptr_array_add (W.tls, t);
        zwlr_foreign_toplevel_handle_v1_add_listener (h, &h_listener, t);
    } else {
        tl_free (t);    /* being torn down */
    }
}

static void tl_all_off (void)
{
    if (W.tls)
        g_ptr_array_set_size (W.tls, 0);
    tl_recompute ();
}

static void m_finished (void *data, struct zwlr_foreign_toplevel_manager_v1 *m)
{
    (void) data;
    /* the compositor ends the list (it does after stop, or on its own
     * when it shuts the protocol down): nothing more will be said */
    if (m == W.mgr) {
        zwlr_foreign_toplevel_manager_v1_destroy (W.mgr);
        W.mgr = NULL;
        tl_all_off ();
    }
}

static const struct zwlr_foreign_toplevel_manager_v1_listener m_listener = { m_toplevel, m_finished };

/* ---- session */

static void tl_outputs_changed (gpointer user)
{
    (void) user;
    /* drop outputs the session no longer has; ids may have changed */
    for (guint i = 0; W.tls && i < W.tls->len; i++) {
        Tl *t = W.tls->pdata[i];
        for (guint k = t->outs->len; k-- > 0;)
            if (!walpe_wlsess_output_of (W.sess, t->outs->pdata[k]))
                g_ptr_array_remove_index (t->outs, k);
    }
    tl_recompute ();
}

static void tl_lost (gpointer user)
{
    (void) user;
    tl_all_off ();
    if (W.mgr) {
        zwlr_foreign_toplevel_manager_v1_destroy (W.mgr);
        W.mgr = NULL;
    }
}

static const WlSessUser tl_user = { tl_outputs_changed, tl_lost, NULL };

static gboolean tracker_up (char **why)
{
    if (W.sess)
        return TRUE;
    WlSession *s = walpe_wlsess_ref (why);
    if (!s)
        return FALSE;
    WalpeWlPlan plan;
    walpe_wlplan (walpe_wlsess_caps (s), &plan);
    if (!plan.windows) {
        if (why)
            *why = g_strdup (plan.windows_why);
        walpe_wlsess_unref (s);
        return FALSE;
    }
    W.tls = g_ptr_array_new_with_free_func (tl_free);
    W.reported = g_array_new (FALSE, FALSE, sizeof (guint32));
    W.fs_reported = g_array_new (FALSE, FALSE, sizeof (guint32));
    W.fs_where = WALPE_TL_WHERE_KNOWN;
    W.names_outputs = FALSE;
    W.mgr = walpe_wlsess_bind (s, &zwlr_foreign_toplevel_manager_v1_interface, 3, &W.version);
    if (!W.mgr) {
        if (why)
            *why = g_strdup (WALPE_WLPLAN_NO_LIST);
        g_clear_pointer (&W.tls, g_ptr_array_unref);
        g_array_free (W.reported, TRUE);
        W.reported = NULL;
        g_array_free (W.fs_reported, TRUE);
        W.fs_reported = NULL;
        walpe_wlsess_unref (s);
        return FALSE;
    }
    W.sess = s;
    const char *e = g_getenv ("WALPE_WL_COVERED");
    W.cover_all = e && !g_ascii_strcasecmp (e, "all");
    zwlr_foreign_toplevel_manager_v1_add_listener (W.mgr, &m_listener, NULL);
    walpe_wlsess_add_user (s, &tl_user);
    /* the windows that exist now, with their state (the handles are
     * announced in the first trip, their events arrive in the second) */
    walpe_wlsess_roundtrip (s);
    walpe_wlsess_roundtrip (s);
    g_message ("wl-toplevel: following %u window%s (zwlr_foreign_toplevel_manager_v1 version %u, "
               "covered = %s)", W.tls->len, W.tls->len == 1 ? "" : "s", W.version,
               W.cover_all ? "any fullscreen / maximized window" : "the focused window when it is "
                                                                   "fullscreen / maximized");
    return TRUE;
}

static void tracker_down (void)
{
    if (!W.sess || W.fs_on || W.win_on)
        return;
    if (W.recompute_id) {
        g_source_remove (W.recompute_id);
        W.recompute_id = 0;
    }
    walpe_wlsess_remove_user (W.sess, &tl_user);
    g_clear_pointer (&W.tls, g_ptr_array_unref);    /* destroys the handles */
    if (W.mgr) {
        /* stop is answered with finished, after which the object is gone
         * on the compositor's side too */
        zwlr_foreign_toplevel_manager_v1_stop (W.mgr);
        struct zwlr_foreign_toplevel_manager_v1 *m = W.mgr;
        if (walpe_wlsess_alive (W.sess))
            walpe_wlsess_roundtrip (W.sess);
        if (W.mgr == m) {   /* no finished came */
            zwlr_foreign_toplevel_manager_v1_destroy (W.mgr);
            W.mgr = NULL;
        }
    }
    if (W.reported)
        g_array_free (W.reported, TRUE);
    W.reported = NULL;
    if (W.fs_reported)
        g_array_free (W.fs_reported, TRUE);
    W.fs_reported = NULL;
    walpe_wlsess_unref (W.sess);
    W.sess = NULL;
}

/* ---- the two providers */

static gboolean fs_start (const WalpeSignalProvider *self, char **why)
{
    (void) self;
    if (W.fs_on)
        return TRUE;
    if (!tracker_up (why))
        return FALSE;
    if (W.version < 2) {
        if (why)
            *why = g_strdup (WALPE_WLPLAN_NO_FS_STATE);
        tracker_down ();
        return FALSE;
    }
    W.fs_on = TRUE;
    tl_recompute ();
    return TRUE;
}

static void fs_stop (const WalpeSignalProvider *self)
{
    (void) self;
    if (!W.fs_on)
        return;
    walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN, 0, FALSE);
    walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN_ON, WALPE_OUTPUT_ALL, FALSE);
    for (guint k = 0; W.fs_reported && k < W.fs_reported->len; k++)
        walpe_signal_set (&walpe_sig_wl_fullscreen, WALPE_SIG_FULLSCREEN_ON,
                          g_array_index (W.fs_reported, guint32, k), FALSE);
    if (W.fs_reported)
        g_array_set_size (W.fs_reported, 0);
    W.fs_where = WALPE_TL_WHERE_KNOWN;
    W.fs_on = FALSE;
    tracker_down ();
}

/* For status: why the fullscreen rule pauses every monitor in this session
 * instead of the one the window is on; NULL = it can tell them apart (or
 * the provider does not run). That is so while a focused fullscreen
 * window's output is not known, and - window or not - while this
 * connection has an output it could not match to one of the daemon's for
 * want of a name: a window on that one could be anywhere. */
guint walpe_sig_wl_passes (void)
{
    return W.passes;
}

const char *walpe_sig_wl_fullscreen_everywhere (void)
{
    if (!W.fs_on || !W.sess)
        return NULL;
    if (W.fs_where != WALPE_TL_WHERE_KNOWN)
        return walpe_tl_where_why (W.fs_where);
    GPtrArray *outs = walpe_wlsess_outputs (W.sess);
    for (guint i = 0; outs && i < outs->len; i++) {
        WlSessOutput *o = outs->pdata[i];
        if (o->done && !o->name && !o->id)
            return WALPE_WLPLAN_FS_NO_NAME;
    }
    return NULL;
}

static gboolean win_start (const WalpeSignalProvider *self, char **why)
{
    (void) self;
    if (W.win_on)
        return TRUE;
    if (!tracker_up (why))
        return FALSE;
    W.win_on = TRUE;
    tl_recompute ();
    return TRUE;
}

static void win_stop (const WalpeSignalProvider *self)
{
    (void) self;
    if (!W.win_on)
        return;
    for (guint k = 0; W.reported && k < W.reported->len; k++) {
        guint32 id = g_array_index (W.reported, guint32, k);
        walpe_signal_set (&walpe_sig_wl_windows, WALPE_SIG_COVERED, id, FALSE);
        walpe_signal_set (&walpe_sig_wl_windows, WALPE_SIG_FOCUS_APP, id, FALSE);
    }
    if (W.reported)
        g_array_set_size (W.reported, 0);
    W.win_on = FALSE;
    tracker_down ();
}

/* a setting changed (the see-through list), or the daemon wants the
 * state looked at again: the list is always current, so work it out */
static void tl_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) sig;
    if (!W.sess)
        return;
    if (now)
        tl_recompute ();
    else
        tl_dirty ();
}

const WalpeSignalProvider walpe_sig_wl_fullscreen = {
    .name = "wl-fullscreen",
    .signals = WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN) | WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN_ON),
    .backends = 1u << WALPE_BACKEND_WAYLAND,
    .start = fs_start,
    .stop = fs_stop,
    .recheck = tl_recheck,
};

const WalpeSignalProvider walpe_sig_wl_windows = {
    .name = "wl-windows",
    .signals = WALPE_SIG_BIT (WALPE_SIG_COVERED) | WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP),
    .backends = 1u << WALPE_BACKEND_WAYLAND,
    .start = win_start,
    .stop = win_stop,
    .recheck = tl_recheck,
};
