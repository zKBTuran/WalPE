/* sig_gnome_test.c - the provider "gnome" (src/signals/gnome.c) against a
 * stand-in for the WalPE extension for GNOME Shell on a private bus.
 *
 * The test starts a bus of its own (GTestDBus; skipped with 77 where there
 * is no dbus-daemon) and a thread that owns io.github.walpe.Gnome on it
 * with the method Windows and the signal WindowsChanged, as
 * contrib/gnome/walpe@walpe.github.io/extension.js does. The provider must:
 *   not start, and say why, while nobody owns the name;
 *   not start with WALPE_GNOME_EXT=0;
 *   ask for the state when it starts, with the see-through list;
 *   turn a report into covered / focus_app / fullscreen_on per output and
 *   the global fullscreen, and take values back that a later report lacks;
 *   believe the owner of the name only;
 *   follow the daemon's outputs when their ids change;
 *   ask again when the see-through list changes;
 *   report nothing while the extension is away, say so, and ask again when
 *   it is back;
 *   let go of everything when it is stopped.
 * The lines of a report are read by winreport.h. The registry is
 * tests/sig_stub.h. */
#include "walpe-build-config.h"
#include "sig_stub.h"
#include "signals/providers.h"
#include "signals/winreport.h"
#include "platform/wl_gnome.h"
#include <gio/gio.h>
#include <unistd.h>

static const char fake_xml[] =
    "<node><interface name='" WL_GNOME_IFACE "'>"
    " <method name='Windows'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    " <signal name='WindowsChanged'><arg type='s'/></signal>"
    "</interface></node>";

static struct {
    GThread *thread;
    GMainContext *ctx;
    GMainLoop *loop;
    GDBusConnection *bus, *other;   /* other: a second connection, not the owner */
    GDBusNodeInfo *info;
    char *address;
    gint ready;
    gint asked;
    GMutex lock;
    char *state;            /* what Windows answers */
    char *see;              /* the list of the last question */
} F;

static void fake_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) user;
    if (!strcmp (method, "Windows")) {
        const char *see;
        g_variant_get (args, "(&s)", &see);
        g_mutex_lock (&F.lock);
        g_free (F.see);
        F.see = g_strdup (see);
        char *state = g_strdup (F.state ? F.state : "");
        g_mutex_unlock (&F.lock);
        g_atomic_int_inc (&F.asked);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(s)", state));
        g_free (state);
    } else {
        g_dbus_method_invocation_return_error_literal (inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, method);
    }
}

static const GDBusInterfaceVTable fake_vt = { fake_method, NULL, NULL, { 0 } };

static gboolean fake_own (gboolean own)
{
    GVariant *r = g_dbus_connection_call_sync (
        F.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        own ? "RequestName" : "ReleaseName",
        own ? g_variant_new ("(su)", WL_GNOME_BUS_NAME, 0u) : g_variant_new ("(s)", WL_GNOME_BUS_NAME),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (r)
        g_variant_unref (r);
    return r != NULL;
}

static GDBusConnection *fake_connect (void)
{
    return g_dbus_connection_new_for_address_sync (
        F.address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
}

static gpointer fake_thread (gpointer u)
{
    (void) u;
    g_main_context_push_thread_default (F.ctx);
    F.bus = fake_connect ();
    F.other = fake_connect ();
    if (!F.bus || !F.other) {
        fprintf (stderr, "fake extension: no connection to the test bus\n");
        g_atomic_int_set (&F.ready, -1);
        return NULL;
    }
    F.info = g_dbus_node_info_new_for_xml (fake_xml, NULL);
    g_dbus_connection_register_object (F.bus, WL_GNOME_OBJECT, F.info->interfaces[0], &fake_vt, NULL, NULL,
                                       NULL);
    g_atomic_int_set (&F.ready, 1);
    g_main_loop_run (F.loop);
    g_dbus_connection_close_sync (F.bus, NULL, NULL);
    g_dbus_connection_close_sync (F.other, NULL, NULL);
    g_object_unref (F.bus);
    g_object_unref (F.other);
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
    g_dbus_connection_flush_sync (F.other, NULL, NULL);
    g_atomic_int_set (&c->done, 1);
    return G_SOURCE_REMOVE;
}

static void fake_run (void (*fn) (gpointer), gpointer arg)
{
    FakeCall c = { fn, arg, 0 };
    g_main_context_invoke (F.ctx, fake_call_cb, &c);
    while (!g_atomic_int_get (&c.done)) {
        g_main_context_iteration (NULL, FALSE);     /* the provider answers on this thread */
        g_usleep (500);
    }
}

static void changed_from (GDBusConnection *from, const char *state)
{
    g_dbus_connection_emit_signal (from, NULL, WL_GNOME_OBJECT, WL_GNOME_IFACE, "WindowsChanged",
                                   g_variant_new ("(s)", state), NULL);
}

static void f_changed (gpointer state)  { changed_from (F.bus, state); }
static void f_forged (gpointer state)   { changed_from (F.other, state); }
static void f_own (gpointer a)          { fake_own (GPOINTER_TO_INT (a)); }

static void set_state (const char *state)
{
    g_mutex_lock (&F.lock);
    g_free (F.state);
    F.state = g_strdup (state);
    g_mutex_unlock (&F.lock);
}

/* ---- the daemon's outputs: "A" and "B", ids that the test changes */
static guint32 id_a = 1, id_b = 2;

static guint32 lookup (const char *name, gpointer user)
{
    (void) user;
    return !g_strcmp0 (name, "A") ? id_a : !g_strcmp0 (name, "B") ? id_b : 0;
}

typedef struct {
    WalpeSignal sig;
    guint32 output;
    int want;
} Want;

static gboolean value_is (gpointer p)
{
    Want *w = p;
    return stub_value (&walpe_sig_gnome, w->sig, w->output) == w->want;
}

#define WAIT_VALUE(s, o, v, what)                                                           \
    do {                                                                                    \
        Want w_ = { s, o, v };                                                              \
        CHECK (stub_wait (value_is, &w_, 3000), "%s (%s on %u is %d)", what,                \
               walpe_signal_name (s), (guint) (o), stub_value (&walpe_sig_gnome, s, o));    \
    } while (0)

static gboolean asked_is (gpointer n)
{
    return g_atomic_int_get (&F.asked) >= GPOINTER_TO_INT (n);
}

static gboolean state_has (gpointer needle)
{
    const char *s = walpe_sig_gnome_state ();
    return s && strstr (s, needle);
}

static void settle (void)
{
    gint64 end = g_get_monotonic_time () + 200000;
    while (g_get_monotonic_time () < end)
        if (!g_main_context_iteration (NULL, FALSE))
            g_usleep (2000);
}

static StubProv *start_provider (void)
{
    stub_reset ();
    walpe_sigprov_register (WALPE_BACKEND_WAYLAND);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, NULL, NULL);
    return stub_prov (&walpe_sig_gnome);
}

static char *see_now (void)
{
    g_mutex_lock (&F.lock);
    char *t = g_strdup (F.see ? F.see : "(none)");
    g_mutex_unlock (&F.lock);
    return t;
}

int main (void)
{
    /* ---- without a bus: the reading of a report */
    const char *name;
    gsize nlen;
    guint flags;
    CHECK (walpe_winreport_line ("DP-1\tcaf", 8, &name, &nlen, &flags) && nlen == 4 &&
           flags == (WALPE_WINREPORT_COVERED | WALPE_WINREPORT_FOCUS | WALPE_WINREPORT_FULLSCREEN),
           "a line with every flag");
    CHECK (walpe_winreport_line ("DP-1\t", 5, &name, &nlen, &flags) && flags == 0, "a line with none");
    CHECK (!walpe_winreport_line ("DP-1", 4, &name, &nlen, &flags) &&
           !walpe_winreport_line ("\tc", 2, &name, &nlen, &flags) &&
           !walpe_winreport_line ("DP-1\tx", 6, &name, &nlen, &flags), "what is no line");

    char *dbus_daemon = g_find_program_in_path ("dbus-daemon");
    if (!dbus_daemon) {
        printf ("sig-gnome: skipped (no dbus-daemon)\n");
        return stub_fails ? 1 : 77;
    }
    g_free (dbus_daemon);
    g_setenv ("WALPE_SIG_WAYLAND_DISPLAY", "/nonexistent/walpe-test/wayland-none", TRUE);
    g_setenv ("WALPE_LOGIND", "0", TRUE);
    g_setenv ("WALPE_GNOME_EXT", "1", TRUE);    /* no compositor here: Mutter is taken for granted */

    GTestDBus *tb = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (tb);
    F.address = g_strdup (g_test_dbus_get_bus_address (tb));
    g_mutex_init (&F.lock);
    F.ctx = g_main_context_new ();
    F.loop = g_main_loop_new (F.ctx, FALSE);
    F.thread = g_thread_new ("fake-extension", fake_thread, NULL);
    while (!g_atomic_int_get (&F.ready))
        g_usleep (1000);
    if (g_atomic_int_get (&F.ready) < 0)
        return 1;
    walpe_sig_gnome_use_address (F.address);
    walpe_sigprov_set_output_lookup (lookup, NULL);
    walpe_sigprov_set_see_through ("st");

    /* ---- nobody owns the name: it does not answer for the signals */
    StubProv *sp = start_provider ();
    CHECK (sp && sp->failed && !sp->started && sp->why && strstr (sp->why, WL_GNOME_EXT_UUID),
           "no extension on the bus: declines and names it: %s", sp && sp->why ? sp->why : "(nothing)");
    CHECK (walpe_sig_gnome_state () == NULL, "nothing for the status then");
    CHECK (*walpe_signal_sources (WALPE_SIG_FULLSCREEN) == 0, "no source for fullscreen");

    /* ---- switched off */
    fake_run (f_own, GINT_TO_POINTER (TRUE));
    g_setenv ("WALPE_GNOME_EXT", "0", TRUE);
    sp = start_provider ();
    CHECK (sp && sp->failed && sp->why && strstr (sp->why, "WALPE_GNOME_EXT"), "WALPE_GNOME_EXT=0: not asked");
    CHECK (g_atomic_int_get (&F.asked) == 0, "and nothing was asked");
    g_setenv ("WALPE_GNOME_EXT", "1", TRUE);

    /* ---- the extension is there: asked at start, with the list */
    set_state ("A\tc\nB\taf\nelsewhere\t");
    sp = start_provider ();
    CHECK (sp && sp->started, "the extension owns its name: the provider starts");
    CHECK (stub_wait (asked_is, GINT_TO_POINTER (1), 3000), "it asks for the state");
    char *see = see_now ();
    CHECK (!strcmp (see, "st"), "with the see-through list: %s", see);
    g_free (see);
    WAIT_VALUE (WALPE_SIG_COVERED, 1, 1, "A is covered");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN_ON, 2, 1, "the fullscreen window is on B");
    WAIT_VALUE (WALPE_SIG_FOCUS_APP, 2, 1, "an application has the focus on B");
    CHECK (stub_value (&walpe_sig_gnome, WALPE_SIG_FOCUS_APP, 1) == 0 &&
           stub_value (&walpe_sig_gnome, WALPE_SIG_COVERED, 2) == 0, "and nothing else");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN, 0, 1, "a fullscreen window has the focus");
    CHECK (stub_wait (state_has, "reports", 3000), "status: %s", walpe_sig_gnome_state ());
    CHECK (strstr (walpe_signal_sources (WALPE_SIG_COVERED), "gnome") != NULL, "it is a source of covered");

    /* ---- a change */
    fake_run (f_changed, "A\t\nB\ta");
    WAIT_VALUE (WALPE_SIG_COVERED, 1, 0, "A is uncovered");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN_ON, 2, 0, "nothing fullscreen on B");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN, 0, 0, "nor anywhere");
    CHECK (stub_value (&walpe_sig_gnome, WALPE_SIG_FOCUS_APP, 2) == 1, "the focus stays on B");
    /* a fullscreen window on an output the daemon shows nothing on */
    fake_run (f_changed, "A\t\nB\t\nelsewhere\taf");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN, 0, 1, "fullscreen elsewhere counts for the global answer");
    CHECK (stub_value (&walpe_sig_gnome, WALPE_SIG_FULLSCREEN_ON, 1) == 0 &&
           stub_value (&walpe_sig_gnome, WALPE_SIG_FULLSCREEN_ON, 2) == 0, "and for no output of the daemon");
    /* an output that is in no line any more loses what it had */
    fake_run (f_changed, "A\tc\nB\tc");
    WAIT_VALUE (WALPE_SIG_COVERED, 2, 1, "B covered");
    fake_run (f_changed, "A\tc");
    WAIT_VALUE (WALPE_SIG_COVERED, 2, 0, "B is in no line: its value is taken back");
    CHECK (stub_value (&walpe_sig_gnome, WALPE_SIG_COVERED, 1) == 1, "A keeps its own");

    /* ---- only the owner of the name is believed */
    fake_run (f_forged, "A\t\nB\tcaf");
    settle ();
    CHECK (stub_value (&walpe_sig_gnome, WALPE_SIG_COVERED, 1) == 1 &&
           stub_value (&walpe_sig_gnome, WALPE_SIG_FULLSCREEN_ON, 2) == 0,
           "a signal from another connection changes nothing");

    /* ---- the daemon's outputs get other ids */
    id_a = 7;
    walpe_sigprov_outputs_changed ();
    WAIT_VALUE (WALPE_SIG_COVERED, 7, 1, "A's value follows its new id");
    WAIT_VALUE (WALPE_SIG_COVERED, 1, 0, "and leaves the old one");

    /* ---- the see-through list changes: asked again */
    int before = g_atomic_int_get (&F.asked);
    walpe_sigprov_set_see_through ("st,foot");
    CHECK (stub_wait (asked_is, GINT_TO_POINTER (before + 1), 3000), "a changed list is a new question");
    see = see_now ();
    CHECK (!strcmp (see, "st,foot"), "which carries it: %s", see);
    g_free (see);
    walpe_sigprov_set_see_through ("st,foot");
    settle ();
    CHECK (g_atomic_int_get (&F.asked) == before + 1, "the same list is none");

    /* ---- the extension goes away (switched off) and comes back */
    fake_run (f_own, GINT_TO_POINTER (FALSE));
    WAIT_VALUE (WALPE_SIG_COVERED, 7, 0, "the extension left the bus: nothing is covered");
    CHECK (stub_wait (state_has, "not on the session bus", 3000), "and the status says so: %s",
           walpe_sig_gnome_state ());
    fake_run (f_changed, "A\tc");
    settle ();
    CHECK (stub_value (&walpe_sig_gnome, WALPE_SIG_COVERED, 7) == 0, "a signal of a former owner is not taken");
    before = g_atomic_int_get (&F.asked);
    set_state ("A\tca");
    fake_run (f_own, GINT_TO_POINTER (TRUE));
    CHECK (stub_wait (asked_is, GINT_TO_POINTER (before + 1), 3000), "back on the bus: asked again");
    WAIT_VALUE (WALPE_SIG_COVERED, 7, 1, "and its answer counts");
    CHECK (stub_wait (state_has, "reports", 3000), "status: %s", walpe_sig_gnome_state ());

    /* ---- stop */
    stub_reset ();
    CHECK (walpe_sig_gnome_state () == NULL, "stopped: nothing for the status");
    before = g_atomic_int_get (&F.asked);
    fake_run (f_changed, "A\tcaf");
    settle ();
    CHECK (g_atomic_int_get (&F.asked) == before, "and nothing is asked any more");

    walpe_sig_gnome_use_address (NULL);
    g_main_loop_quit (F.loop);
    g_thread_join (F.thread);
    g_test_dbus_down (tb);
    g_object_unref (tb);
    if (stub_fails) {
        fprintf (stderr, "sig-gnome: %d check(s) failed\n", stub_fails);
        return 1;
    }
    printf ("sig-gnome: ok\n");
    return 0;
}
