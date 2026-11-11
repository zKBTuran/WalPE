/* sig_logind_test.c - the logind provider (src/signals/logind.c) against
 * a stand-in login manager on a private bus.
 *
 * The test starts a bus of its own (GTestDBus: a dbus-daemon that only
 * this process talks to; skipped with 77 where there is no dbus-daemon),
 * and a thread that owns org.freedesktop.login1 on it with a Manager, one
 * session and one user object. The provider is pointed at that bus
 * (walpe_sig_logind_use_bus) and must:
 *   find its session by $XDG_SESSION_ID, by process id, through the
 *   user's display session, or carry on with suspend only;
 *   report asleep for the lock hint, PrepareForSleep and an inactive
 *   session, and awake again afterwards, including the state it finds at
 *   start and a change that arrives without its value;
 *   honour WALPE_LOGIND (facts in use; off by default on X11);
 *   not stay asleep when the login manager leaves the bus;
 *   decline with a sentence when there is none.
 * The registry is tests/sig_stub.h. */
#include "walpe-build-config.h"
#include "sig_stub.h"
#include "signals/providers.h"
#include <gio/gio.h>
#include <unistd.h>

#define L1_NAME "org.freedesktop.login1"
#define L1_PATH "/org/freedesktop/login1"
#define SESSION_PATH "/org/freedesktop/login1/session/_37"
#define USER_PATH "/org/freedesktop/login1/user/_walpe"

static const char fake_xml[] =
    "<node>"
    " <interface name='org.freedesktop.login1.Manager'>"
    "  <method name='GetSession'><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
    "  <method name='GetSessionByPID'><arg type='u' direction='in'/><arg type='o' direction='out'/></method>"
    "  <method name='GetUser'><arg type='u' direction='in'/><arg type='o' direction='out'/></method>"
    "  <signal name='PrepareForSleep'><arg type='b'/></signal>"
    " </interface>"
    " <interface name='org.freedesktop.login1.Session'>"
    "  <property name='LockedHint' type='b' access='read'/>"
    "  <property name='Active' type='b' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.login1.User'>"
    "  <property name='Display' type='(so)' access='read'/>"
    " </interface>"
    "</node>";

/* the stand-in's state: written by the test thread only through
 * fake_run () (which executes on the service thread), read by handlers */
static struct {
    GThread *thread;
    GMainContext *ctx;
    GMainLoop *loop;
    GDBusConnection *bus;
    GDBusNodeInfo *info;
    char *address;
    gboolean by_id, by_pid, has_display;    /* which lookups answer */
    gboolean locked, active;
    gint ready;
    gint calls_by_id, calls_by_pid, calls_get_user;
} F = { .by_id = TRUE, .by_pid = TRUE, .has_display = TRUE, .active = TRUE };

static void fake_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) u;
    if (!strcmp (method, "GetSession")) {
        const char *id = NULL;
        g_variant_get (params, "(&s)", &id);
        g_atomic_int_inc (&F.calls_by_id);
        if (F.by_id && !g_strcmp0 (id, "37"))
            g_dbus_method_invocation_return_value (inv, g_variant_new ("(o)", SESSION_PATH));
        else
            g_dbus_method_invocation_return_dbus_error (inv, "org.freedesktop.login1.NoSuchSession",
                                                        "No session known");
    } else if (!strcmp (method, "GetSessionByPID")) {
        guint32 pid = 0;
        g_variant_get (params, "(u)", &pid);
        g_atomic_int_inc (&F.calls_by_pid);
        if (F.by_pid && pid == (guint32) getpid ())
            g_dbus_method_invocation_return_value (inv, g_variant_new ("(o)", SESSION_PATH));
        else
            g_dbus_method_invocation_return_dbus_error (inv, "org.freedesktop.login1.NoSessionForPID",
                                                        "PID does not belong to any known session");
    } else if (!strcmp (method, "GetUser")) {
        g_atomic_int_inc (&F.calls_get_user);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(o)", USER_PATH));
    } else {
        g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
    }
}

static GVariant *fake_prop (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                            const char *prop, GError **err, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) err; (void) u;
    if (!strcmp (prop, "LockedHint"))
        return g_variant_new_boolean (F.locked);
    if (!strcmp (prop, "Active"))
        return g_variant_new_boolean (F.active);
    if (!strcmp (prop, "Id"))
        return g_variant_new_string ("37");
    if (!strcmp (prop, "Display"))
        return F.has_display ? g_variant_new ("(so)", "37", SESSION_PATH)
                             : g_variant_new ("(so)", "", "/");
    return NULL;
}

static const GDBusInterfaceVTable fake_vt = { fake_method, fake_prop, NULL, { 0 } };

static gboolean fake_own (gboolean own)
{
    GVariant *r = g_dbus_connection_call_sync (
        F.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        own ? "RequestName" : "ReleaseName",
        own ? g_variant_new ("(su)", L1_NAME, 0u) : g_variant_new ("(s)", L1_NAME), NULL,
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (r)
        g_variant_unref (r);
    return r != NULL;
}

static gpointer fake_thread (gpointer u)
{
    (void) u;
    g_main_context_push_thread_default (F.ctx);
    GError *e = NULL;
    F.bus = g_dbus_connection_new_for_address_sync (
        F.address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &e);
    if (!F.bus) {
        fprintf (stderr, "fake logind: %s\n", e->message);
        g_atomic_int_set (&F.ready, -1);
        return NULL;
    }
    F.info = g_dbus_node_info_new_for_xml (fake_xml, NULL);
    g_dbus_connection_register_object (F.bus, L1_PATH, F.info->interfaces[0], &fake_vt, NULL, NULL, NULL);
    g_dbus_connection_register_object (F.bus, SESSION_PATH, F.info->interfaces[1], &fake_vt, NULL, NULL, NULL);
    g_dbus_connection_register_object (F.bus, USER_PATH, F.info->interfaces[2], &fake_vt, NULL, NULL, NULL);
    fake_own (TRUE);
    g_atomic_int_set (&F.ready, 1);
    g_main_loop_run (F.loop);
    g_dbus_connection_close_sync (F.bus, NULL, NULL);
    g_object_unref (F.bus);
    g_main_context_pop_thread_default (F.ctx);
    return NULL;
}

/* run fn on the service thread and wait for it */
typedef struct {
    void (*fn) (gpointer);
    gpointer arg;
    gint done;
} FakeCall;

static gboolean fake_call_cb (gpointer p)
{
    FakeCall *c = p;
    c->fn (c->arg);
    g_dbus_connection_flush_sync (F.bus, NULL, NULL);
    g_atomic_int_set (&c->done, 1);
    return G_SOURCE_REMOVE;
}

static void fake_run (void (*fn) (gpointer), gpointer arg)
{
    FakeCall c = { fn, arg, 0 };
    g_main_context_invoke (F.ctx, fake_call_cb, &c);
    while (!g_atomic_int_get (&c.done))
        g_usleep (500);
}

static void emit_props (gboolean with_values)
{
    GVariantBuilder ch, inv;
    g_variant_builder_init (&ch, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_init (&inv, G_VARIANT_TYPE ("as"));
    if (with_values) {
        g_variant_builder_add (&ch, "{sv}", "LockedHint", g_variant_new_boolean (F.locked));
        g_variant_builder_add (&ch, "{sv}", "Active", g_variant_new_boolean (F.active));
    } else {
        g_variant_builder_add (&inv, "s", "LockedHint");
    }
    g_dbus_connection_emit_signal (F.bus, NULL, SESSION_PATH, "org.freedesktop.DBus.Properties",
                                   "PropertiesChanged",
                                   g_variant_new ("(sa{sv}as)", "org.freedesktop.login1.Session", &ch, &inv),
                                   NULL);
}

static void f_lock (gpointer a)          { F.locked = GPOINTER_TO_INT (a); emit_props (TRUE); }
static void f_lock_quiet (gpointer a)    { F.locked = GPOINTER_TO_INT (a); }
static void f_lock_novalue (gpointer a)  { F.locked = GPOINTER_TO_INT (a); emit_props (FALSE); }
static void f_active (gpointer a)        { F.active = GPOINTER_TO_INT (a); emit_props (TRUE); }
static void f_lookup (gpointer a)
{
    int m = GPOINTER_TO_INT (a);
    F.by_id = m & 1;
    F.by_pid = (m >> 1) & 1;
    F.has_display = (m >> 2) & 1;
}
static void f_sleep (gpointer a)
{
    g_dbus_connection_emit_signal (F.bus, NULL, L1_PATH, "org.freedesktop.login1.Manager",
                                   "PrepareForSleep", g_variant_new ("(b)", GPOINTER_TO_INT (a)), NULL);
}
static void f_own (gpointer a)           { fake_own (GPOINTER_TO_INT (a)); }

/* ---- the checks */

static gboolean asleep_is (gpointer want)
{
    return stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0) == GPOINTER_TO_INT (want);
}

#define WAIT_ASLEEP(v, what)                                                                 \
    CHECK (stub_wait (asleep_is, GINT_TO_POINTER (v), 3000), "%s (asleep is %d)", what,      \
           stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0))

/* a moment in which a wrong change would have arrived */
static void settle (void)
{
    gint64 end = g_get_monotonic_time () + 150000;
    while (g_get_monotonic_time () < end)
        if (!g_main_context_iteration (NULL, FALSE))
            g_usleep (2000);
}

static gboolean start_provider (WalpeBackend backend)
{
    stub_reset ();
    walpe_sigprov_register (backend);
    walpe_signals_start (backend, NULL, NULL);
    StubProv *sp = stub_prov (&walpe_sig_logind);
    return sp && sp->started;
}

static const char *start_why (void)
{
    StubProv *sp = stub_prov (&walpe_sig_logind);
    return sp && sp->why ? sp->why : "";
}

int main (void)
{
    char *dbus_daemon = g_find_program_in_path ("dbus-daemon");
    if (!dbus_daemon) {
        printf ("sig-logind: skipped (no dbus-daemon)\n");
        return 77;
    }
    g_free (dbus_daemon);   /* (the path was only asked for; unfreed it is a leak report under asan_leaks) */
    /* no compositor for the Wayland providers registered alongside; a
     * private runtime dir so nothing of the real session is reached */
    g_setenv ("WALPE_SIG_WAYLAND_DISPLAY", "/nonexistent/walpe-test/wayland-none", TRUE);
    g_unsetenv ("WALPE_LOGIND");
    g_unsetenv ("WALPE_LOGIND_BUS");
    g_setenv ("XDG_SESSION_ID", "37", TRUE);

    GTestDBus *tb = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (tb);
    F.address = g_strdup (g_test_dbus_get_bus_address (tb));
    F.ctx = g_main_context_new ();
    F.loop = g_main_loop_new (F.ctx, FALSE);
    F.thread = g_thread_new ("fake-logind", fake_thread, NULL);
    while (!g_atomic_int_get (&F.ready))
        g_usleep (1000);
    if (g_atomic_int_get (&F.ready) < 0)
        return 1;

    GError *e = NULL;
    GDBusConnection *bus = g_dbus_connection_new_for_address_sync (
        F.address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &e);
    if (!bus) {
        fprintf (stderr, "sig-logind: %s\n", e->message);
        return 1;
    }
    walpe_sig_logind_use_bus (bus);
    WalpeLogindState st;

    /* 1. Wayland backend, session by XDG_SESSION_ID, everything quiet */
    CHECK (start_provider (WALPE_BACKEND_WAYLAND), "starts on Wayland: %s", start_why ());
    walpe_sig_logind_state (&st);
    CHECK (st.running && st.have_session && !g_strcmp0 (st.session_path, SESSION_PATH),
           "session found by id: %s", st.session_path ? st.session_path : "(none)");
    g_free (st.session_path);
    CHECK (g_atomic_int_get (&F.calls_by_id) == 1 && g_atomic_int_get (&F.calls_by_pid) == 0,
           "the id from the environment is asked first and suffices");
    CHECK (stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0) == 0, "awake is reported at start");
    CHECK (!g_strcmp0 (walpe_signal_sources (WALPE_SIG_ASLEEP), "logind"), "logind is the source of asleep");

    /* 2. lock / unlock */
    fake_run (f_lock, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "lock hint -> asleep");
    fake_run (f_lock, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "unlock -> awake");

    /* 3. suspend / resume */
    fake_run (f_sleep, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "PrepareForSleep (true) -> asleep");
    fake_run (f_sleep, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "PrepareForSleep (false) -> awake");

    /* 4. another virtual terminal */
    fake_run (f_active, GINT_TO_POINTER (0));
    WAIT_ASLEEP (1, "session inactive -> asleep");
    fake_run (f_active, GINT_TO_POINTER (1));
    WAIT_ASLEEP (0, "session active again -> awake");

    /* 5. a change announced without its value is asked for */
    fake_run (f_lock_novalue, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "invalidated LockedHint is read back");
    fake_run (f_lock_novalue, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "and back");

    /* 6. locked during suspend, no signal delivered: the resume re-reads */
    fake_run (f_sleep, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "suspend");
    fake_run (f_lock_quiet, GINT_TO_POINTER (1));
    fake_run (f_sleep, GINT_TO_POINTER (0));
    settle ();
    CHECK (stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0) == 1,
           "woke up locked: still asleep after the re-read");
    fake_run (f_lock, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "unlocked after resume");

    /* 7. the login manager leaves the bus while the session is locked */
    fake_run (f_lock, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "locked");
    fake_run (f_own, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "login manager gone: not left asleep");
    /* ... and comes back: its state counts again */
    fake_run (f_own, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "login manager back: the lock is seen again");
    fake_run (f_lock, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "unlocked");

    /* 8. the state found at start */
    fake_run (f_lock_quiet, GINT_TO_POINTER (1));
    CHECK (start_provider (WALPE_BACKEND_WAYLAND), "restart");
    CHECK (stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0) == 1,
           "a session locked before the start is reported by start itself");
    fake_run (f_lock, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "unlock");

    /* 9. WALPE_LOGIND=sleep: the lock hint is not looked at */
    g_setenv ("WALPE_LOGIND", "sleep", TRUE);
    CHECK (start_provider (WALPE_BACKEND_WAYLAND), "starts with sleep only");
    fake_run (f_lock, GINT_TO_POINTER (1));
    settle ();
    CHECK (stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0) == 0, "lock ignored with WALPE_LOGIND=sleep");
    fake_run (f_sleep, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "suspend still counts");
    fake_run (f_sleep, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "resume");
    fake_run (f_lock_quiet, GINT_TO_POINTER (0));
    g_unsetenv ("WALPE_LOGIND");

    /* 10. session by process id */
    g_unsetenv ("XDG_SESSION_ID");
    g_atomic_int_set (&F.calls_by_pid, 0);
    CHECK (start_provider (WALPE_BACKEND_WAYLAND), "starts without XDG_SESSION_ID");
    walpe_sig_logind_state (&st);
    CHECK (st.have_session && g_atomic_int_get (&F.calls_by_pid) == 1, "session found by pid");
    g_free (st.session_path);

    /* 11. in no session: the user's display session */
    fake_run (f_lookup, GINT_TO_POINTER (4));   /* neither id nor pid, display yes */
    CHECK (start_provider (WALPE_BACKEND_WAYLAND), "starts when the process is in no session");
    walpe_sig_logind_state (&st);
    CHECK (st.have_session && !g_strcmp0 (st.session_path, SESSION_PATH), "display session of the user");
    g_free (st.session_path);
    fake_run (f_lock, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "its lock hint is followed");
    fake_run (f_lock, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "unlock");

    /* 12. no session to be found: suspend only */
    fake_run (f_lookup, GINT_TO_POINTER (0));
    CHECK (start_provider (WALPE_BACKEND_WAYLAND), "starts without any session");
    walpe_sig_logind_state (&st);
    CHECK (st.running && !st.have_session, "no session");
    g_free (st.session_path);
    fake_run (f_lock, GINT_TO_POINTER (1));
    settle ();
    CHECK (stub_value (&walpe_sig_logind, WALPE_SIG_ASLEEP, 0) == 0, "a session it does not follow is not its business");
    fake_run (f_lock_quiet, GINT_TO_POINTER (0));
    fake_run (f_sleep, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "suspend without a session");
    fake_run (f_sleep, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "resume without a session");
    fake_run (f_lookup, GINT_TO_POINTER (7));

    /* 13. X11 backend: off unless asked */
    CHECK (!start_provider (WALPE_BACKEND_X11), "declines on X11 by default");
    CHECK (strstr (start_why (), "WALPE_LOGIND=1") != NULL, "and says how to turn it on: %s", start_why ());
    CHECK (*walpe_signal_sources (WALPE_SIG_ASLEEP) == 0, "no source then");
    g_setenv ("WALPE_LOGIND", "1", TRUE);
    CHECK (start_provider (WALPE_BACKEND_X11), "starts on X11 with WALPE_LOGIND=1: %s", start_why ());
    fake_run (f_lock, GINT_TO_POINTER (1));
    WAIT_ASLEEP (1, "X11 opt-in follows the lock hint");
    fake_run (f_lock, GINT_TO_POINTER (0));
    WAIT_ASLEEP (0, "unlock");
    g_unsetenv ("WALPE_LOGIND");

    /* 14. stop drops everything */
    stub_reset ();
    walpe_sig_logind_state (&st);
    CHECK (!st.running && !st.have_session, "stopped");
    g_free (st.session_path);

    /* 15. a bus without a login manager */
    fake_run (f_own, GINT_TO_POINTER (0));
    CHECK (!start_provider (WALPE_BACKEND_WAYLAND), "declines without a login manager");
    CHECK (strstr (start_why (), "login manager") != NULL, "and says so: %s", start_why ());
    stub_reset ();

    walpe_sig_logind_use_bus (NULL);
    g_dbus_connection_close_sync (bus, NULL, NULL);
    g_object_unref (bus);
    g_main_loop_quit (F.loop);
    g_thread_join (F.thread);
    g_test_dbus_down (tb);
    g_object_unref (tb);

    if (stub_fails) {
        fprintf (stderr, "sig-logind: %d check(s) failed\n", stub_fails);
        return 1;
    }
    printf ("sig-logind: ok\n");
    return 0;
}
