/* logind.c - provider "logind": the session is locked, the machine is
 * about to suspend, or the user is on another virtual terminal.
 *
 * systemd-logind (and elogind, which offers the same interface on systems
 * without systemd) publishes this on the system bus, whatever the window
 * system:
 *   org.freedesktop.login1.Manager   signal PrepareForSleep (b): true
 *                                    before suspend / hibernate, false
 *                                    after the wake-up
 *   org.freedesktop.login1.Session   properties LockedHint (the screen
 *                                    locker says the session is locked)
 *                                    and Active (the session is the one
 *                                    shown on its seat)
 * All three mean that nobody sees the wallpaper, so they are reported as
 * WALPE_SIG_ASLEEP (the signal behind "Pause while the display sleeps").
 * Which of them are used: logindopt.h (WALPE_LOGIND).
 *
 * The session is found the way loginctl does it: $XDG_SESSION_ID, else
 * the session this process belongs to, else the user's display session (a
 * daemon started by the systemd user manager is in no session of its
 * own). Without a session only suspend is followed.
 *
 * Start-up asks the bus synchronously (a handful of local round trips,
 * each with a one-second limit) so that the provider can answer "not
 * available" at once; everything after that is asynchronous. No
 * inhibitor lock is taken: pausing needs no time before the suspend. */
#include "signals/providers.h"
#include "signals/logindopt.h"
#include <unistd.h>

#define L1_NAME "org.freedesktop.login1"
#define L1_PATH "/org/freedesktop/login1"
#define L1_MANAGER "org.freedesktop.login1.Manager"
#define L1_SESSION "org.freedesktop.login1.Session"
#define L1_USER "org.freedesktop.login1.User"
#define L1_PROPS "org.freedesktop.DBus.Properties"
#define L1_CALL_MS 1000

static struct {
    GDBusConnection *test_bus;      /* walpe_sig_logind_use_bus */
    GDBusConnection *bus;
    gboolean started;
    guint use;                      /* WALPE_LOGIND_* in use */
    char *session;                  /* object path, NULL = none found */
    guint sleep_sub, props_sub, name_watch;
    GCancellable *cancel;           /* of the property reads under way */
    gboolean sleeping, locked, inactive;
    gboolean waking;                /* resumed, lock state not read back yet */
    gboolean reported;              /* the value last given to the registry */
} L;

void walpe_sig_logind_use_bus (GDBusConnection *bus)
{
    g_clear_object (&L.test_bus);
    if (bus)
        L.test_bus = g_object_ref (bus);
}

void walpe_sig_logind_state (WalpeLogindState *out)
{
    out->running = L.started;
    out->have_session = L.session != NULL;
    out->sleeping = L.sleeping;
    out->locked = L.locked;
    out->inactive = L.inactive;
    out->session_path = g_strdup (L.session);
}

static void l1_report (void)
{
    gboolean asleep = walpe_logind_asleep (L.use, L.sleeping, L.locked, L.inactive);
    if (asleep != L.reported) {
        L.reported = asleep;
        if (asleep)
            g_message ("logind: %s: wallpapers count as unseen",
                       (L.use & WALPE_LOGIND_SLEEP) && L.sleeping ? "the machine is going to sleep"
                       : (L.use & WALPE_LOGIND_LOCK) && L.locked ? "the session is locked"
                                                                 : "the session is not the active one");
        else
            g_message ("logind: session awake, unlocked and active again");
    }
    walpe_signal_set (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0, asleep);
}

/* ---- the session object */

/* a{sv} of session properties (GetAll reply or PropertiesChanged) */
static void l1_take_props (GVariant *dict)
{
    gboolean b;
    if (g_variant_lookup (dict, "LockedHint", "b", &b))
        L.locked = b;
    if (g_variant_lookup (dict, "Active", "b", &b))
        L.inactive = !b;
}

static void l1_props_read_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (!r) {
        /* cancelled (stop / a newer read) leaves the state alone; a
         * session that is gone cannot be locked or inactive any more */
        if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED) && L.started) {
            L.locked = L.inactive = FALSE;
            if (L.waking)
                L.sleeping = L.waking = FALSE;
            l1_report ();
        }
        g_error_free (e);
        return;
    }
    if (L.started) {
        GVariant *dict = g_variant_get_child_value (r, 0);
        l1_take_props (dict);
        g_variant_unref (dict);
        if (L.waking)
            L.sleeping = L.waking = FALSE;
        l1_report ();
    }
    g_variant_unref (r);
}

static void l1_props_read (void)
{
    if (!L.bus || !L.session)
        return;
    if (L.cancel) {
        g_cancellable_cancel (L.cancel);
        g_object_unref (L.cancel);
    }
    L.cancel = g_cancellable_new ();
    g_dbus_connection_call (L.bus, L1_NAME, L.session, L1_PROPS, "GetAll",
                            g_variant_new ("(s)", L1_SESSION), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, L1_CALL_MS, L.cancel, l1_props_read_cb,
                            NULL);
}

static void l1_on_props (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *sig, GVariant *params, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) sig; (void) user;
    if (!L.started || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(sa{sv}as)")))
        return;
    GVariant *dict = g_variant_get_child_value (params, 1);
    GVariant *inval = g_variant_get_child_value (params, 2);
    l1_take_props (dict);
    /* a property named without its value: ask for it */
    if (g_variant_n_children (inval) > 0)
        l1_props_read ();
    g_variant_unref (dict);
    g_variant_unref (inval);
    l1_report ();
}

static void l1_on_sleep (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *sig, GVariant *params, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) sig; (void) user;
    if (!L.started || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(b)")))
        return;
    gboolean start = FALSE;
    g_variant_get (params, "(b)", &start);
    if (start) {
        L.sleeping = TRUE;
        L.waking = FALSE;
    } else if (L.session) {
        /* the session may have been locked on the way into the suspend,
         * when no signal could be delivered any more: stay "asleep" for
         * the one local round trip that reads the state back, instead of
         * running the wallpaper for a moment behind the lock screen */
        L.waking = TRUE;
        l1_props_read ();
        return;
    } else {
        L.sleeping = FALSE;
    }
    l1_report ();
}

/* one synchronous call returning (o); NULL when it failed */
static char *l1_call_path (const char *path, const char *iface, const char *method, GVariant *args)
{
    GVariant *r = g_dbus_connection_call_sync (L.bus, L1_NAME, path, iface, method, args,
                                               G_VARIANT_TYPE ("(o)"),
                                               G_DBUS_CALL_FLAGS_NO_AUTO_START, L1_CALL_MS, NULL, NULL);
    if (!r)
        return NULL;
    char *p = NULL;
    g_variant_get (r, "(o)", &p);
    g_variant_unref (r);
    return p;
}

static char *l1_find_session (void)
{
    char *p = NULL;
    const char *id = g_getenv ("XDG_SESSION_ID");
    if (id && *id)
        p = l1_call_path (L1_PATH, L1_MANAGER, "GetSession", g_variant_new ("(s)", id));
    if (!p)
        p = l1_call_path (L1_PATH, L1_MANAGER, "GetSessionByPID",
                          g_variant_new ("(u)", (guint32) getpid ()));
    if (!p) {
        /* in no session (started by the user manager): the user's
         * display session, property Display = (session id, object path) */
        char *user = l1_call_path (L1_PATH, L1_MANAGER, "GetUser",
                                   g_variant_new ("(u)", (guint32) getuid ()));
        GVariant *r = user ? g_dbus_connection_call_sync (L.bus, L1_NAME, user, L1_PROPS, "Get",
                                                          g_variant_new ("(ss)", L1_USER, "Display"),
                                                          G_VARIANT_TYPE ("(v)"),
                                                          G_DBUS_CALL_FLAGS_NO_AUTO_START, L1_CALL_MS,
                                                          NULL, NULL)
                           : NULL;
        if (r) {
            GVariant *v = NULL;
            g_variant_get (r, "(v)", &v);
            if (v && g_variant_is_of_type (v, G_VARIANT_TYPE ("(so)"))) {
                const char *sid = NULL, *sp = NULL;
                g_variant_get (v, "(&s&o)", &sid, &sp);
                /* no display session: ("", "/") */
                if (sid && *sid && sp && strcmp (sp, "/") != 0)
                    p = g_strdup (sp);
            }
            if (v)
                g_variant_unref (v);
            g_variant_unref (r);
        }
        g_free (user);
    }
    return p;
}

static void l1_session_attach (void)
{
    if (L.props_sub) {
        g_dbus_connection_signal_unsubscribe (L.bus, L.props_sub);
        L.props_sub = 0;
    }
    g_clear_pointer (&L.session, g_free);
    if (L.use & (WALPE_LOGIND_LOCK | WALPE_LOGIND_VT))
        L.session = l1_find_session ();
    if (!L.session)
        return;
    L.props_sub = g_dbus_connection_signal_subscribe (L.bus, L1_NAME, L1_PROPS, "PropertiesChanged",
                                                      L.session, L1_SESSION,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, l1_on_props, NULL, NULL);
}

/* the login manager came back after a restart: its sessions keep their
 * ids, but a state change in between was not seen */
static void l1_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer user)
{
    (void) c; (void) name; (void) owner; (void) user;
    if (!L.started)
        return;
    if (!L.session)
        l1_session_attach ();
    l1_props_read ();
}

static void l1_vanished (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    if (!L.started)
        return;
    /* nobody is left to say "awake again": do not stay paused on a
     * login manager that went away */
    L.sleeping = L.locked = L.inactive = L.waking = FALSE;
    l1_report ();
}

static gboolean l1_has_owner (void)
{
    GVariant *r = g_dbus_connection_call_sync (L.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                               "org.freedesktop.DBus", "NameHasOwner",
                                               g_variant_new ("(s)", L1_NAME), G_VARIANT_TYPE ("(b)"),
                                               G_DBUS_CALL_FLAGS_NONE, L1_CALL_MS, NULL, NULL);
    gboolean has = FALSE;
    if (r) {
        g_variant_get (r, "(b)", &has);
        g_variant_unref (r);
    }
    return has;
}

static gboolean l1_start (const WalpeSignalProvider *self, char **why)
{
    (void) self;
    if (L.started)
        return TRUE;
    gboolean x11 = walpe_sigprov_backend () == WALPE_BACKEND_X11;
    const char *env = g_getenv ("WALPE_LOGIND");
    L.use = walpe_logind_facts (env, x11);
    if (!L.use) {
        if (why)
            *why = g_strdup (x11 && !(env && *env)
                                 ? "logind is not used on X11 unless WALPE_LOGIND=1 is set (the X "
                                   "server's DPMS and screensaver state are followed there)"
                                 : "logind is switched off (WALPE_LOGIND)");
        return FALSE;
    }
    GError *e = NULL;
    if (L.test_bus) {
        L.bus = g_object_ref (L.test_bus);
    } else {
        /* WALPE_LOGIND_BUS=session (tests of the whole daemon): a
         * stand-in login manager on the session bus */
        gboolean sess = !g_strcmp0 (g_getenv ("WALPE_LOGIND_BUS"), "session");
        L.bus = g_bus_get_sync (sess ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, NULL, &e);
    }
    if (!L.bus) {
        if (why)
            *why = g_strdup_printf ("no system bus (%s)", e ? e->message : "no connection");
        g_clear_error (&e);
        return FALSE;
    }
    /* a bus that goes away must not end the daemon */
    g_dbus_connection_set_exit_on_close (L.bus, FALSE);
    if (!l1_has_owner ()) {
        if (why)
            *why = g_strdup ("no login manager on the system bus (org.freedesktop.login1: "
                             "systemd-logind or elogind)");
        g_clear_object (&L.bus);
        return FALSE;
    }
    L.started = TRUE;
    L.sleeping = L.locked = L.inactive = L.waking = L.reported = FALSE;
    if (L.use & WALPE_LOGIND_SLEEP)
        L.sleep_sub = g_dbus_connection_signal_subscribe (L.bus, L1_NAME, L1_MANAGER,
                                                          "PrepareForSleep", L1_PATH, NULL,
                                                          G_DBUS_SIGNAL_FLAGS_NONE, l1_on_sleep, NULL,
                                                          NULL);
    l1_session_attach ();
    if (L.session) {
        /* the state as it is now, before the first value is reported */
        GVariant *r = g_dbus_connection_call_sync (L.bus, L1_NAME, L.session, L1_PROPS, "GetAll",
                                                   g_variant_new ("(s)", L1_SESSION),
                                                   G_VARIANT_TYPE ("(a{sv})"),
                                                   G_DBUS_CALL_FLAGS_NO_AUTO_START, L1_CALL_MS, NULL,
                                                   NULL);
        if (r) {
            GVariant *dict = g_variant_get_child_value (r, 0);
            l1_take_props (dict);
            g_variant_unref (dict);
            g_variant_unref (r);
        }
        g_message ("logind: following session %s (%s%s%s)", L.session,
                   L.use & WALPE_LOGIND_SLEEP ? "suspend " : "", L.use & WALPE_LOGIND_LOCK ? "lock " : "",
                   L.use & WALPE_LOGIND_VT ? "active" : "");
    } else if (L.use & (WALPE_LOGIND_LOCK | WALPE_LOGIND_VT)) {
        g_message ("logind: no session found for this process or user: only suspend is followed");
    }
    L.name_watch = g_bus_watch_name_on_connection (L.bus, L1_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                   l1_appeared, l1_vanished, NULL, NULL);
    l1_report ();
    return TRUE;
}

static void l1_stop (const WalpeSignalProvider *self)
{
    (void) self;
    if (!L.started)
        return;
    L.started = FALSE;
    if (L.cancel) {
        g_cancellable_cancel (L.cancel);
        g_clear_object (&L.cancel);
    }
    if (L.name_watch)
        g_bus_unwatch_name (L.name_watch);
    if (L.sleep_sub)
        g_dbus_connection_signal_unsubscribe (L.bus, L.sleep_sub);
    if (L.props_sub)
        g_dbus_connection_signal_unsubscribe (L.bus, L.props_sub);
    L.name_watch = L.sleep_sub = L.props_sub = 0;
    g_clear_pointer (&L.session, g_free);
    g_clear_object (&L.bus);
    L.sleeping = L.locked = L.inactive = L.waking = L.reported = FALSE;
}

static void l1_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) now;
    if (L.started && sig == WALPE_SIG_ASLEEP)
        l1_props_read ();   /* the answer arrives on the main loop */
}

const WalpeSignalProvider walpe_sig_logind = {
    .name = "logind",
    .signals = WALPE_SIG_BIT (WALPE_SIG_ASLEEP),
    .backends = 0,          /* any; on X11 start () declines unless asked (logindopt.h) */
    .start = l1_start,
    .stop = l1_stop,
    .recheck = l1_recheck,
};
