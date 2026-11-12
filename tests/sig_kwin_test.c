/* sig_kwin_test.c - the provider "kwin" (src/signals/kwin.c) against a
 * stand-in KWin on a private bus.
 *
 * The test starts a bus of its own (GTestDBus; skipped with 77 where there
 * is no dbus-daemon) and a thread that owns org.kde.KWin on it with the
 * part of org.kde.kwin.Scripting the provider uses: loadScript,
 * unloadScript and start (and an object at the id loadScript answers that
 * is not the new script's, as in KWin after another script was unloaded). The stand-in keeps the
 * text of the script it was given and "runs" it by calling the method the
 * text names, on the connection the text names. The provider must:
 *   keep no connection and load nothing while no rule is on;
 *   load the script when one is switched on, with its own unique name,
 *   its process id and the see-through list in the text, have KWin run it
 *   by "start", and remove the file once the script has reported;
 *   turn a report into covered / focus_app / fullscreen_on per output and
 *   the global fullscreen, and take values back that a later report lacks;
 *   believe KWin's connection only;
 *   follow the daemon's outputs when their ids change;
 *   load the script again when the see-through list changes and when KWin
 *   comes back on the bus, and report nothing while it is away;
 *   say why when KWin is too old for the script;
 *   unload the script when the last rule goes off and when it is stopped;
 *   not start with WALPE_KWIN_SCRIPT=0.
 * The registry is tests/sig_stub.h. */
#include "walpe-build-config.h"
#include "sig_stub.h"
#include "signals/providers.h"
#include "signals/rules.h"
#include "signals/kwinscript.h"
#include <gio/gio.h>
#include <unistd.h>

#define KWIN "org.kde.KWin"

static const char fake_xml[] =
    "<node>"
    " <interface name='org.kde.kwin.Scripting'>"
    "  <method name='loadScript'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='i' direction='out'/></method>"
    "  <method name='unloadScript'><arg type='s' direction='in'/><arg type='b' direction='out'/></method>"
    "  <method name='start'/>"
    " </interface>"
    " <interface name='org.kde.kwin.Script'>"
    "  <method name='run'/>"
    " </interface>"
    "</node>";

static struct {
    GThread *thread;
    GMainContext *ctx;
    GMainLoop *loop;
    GDBusConnection *bus;
    GDBusNodeInfo *info;
    char *address;
    gint ready;
    gint loads, unloads, runs, wrong_runs;
    gboolean old;           /* answers like a KWin before Plasma 6 */
    gboolean loaded;        /* a script of our name is in "KWin" */
    GMutex lock;
    char *file, *name;      /* of the last loadScript */
    char *text;             /* what the file held when it was run */
    char *dest;             /* the connection the script reports to */
    gint report_ok, report_denied;
} F;

static gboolean fake_unload_later (gpointer u)
{
    (void) u;
    g_mutex_lock (&F.lock);
    F.loaded = FALSE;
    g_mutex_unlock (&F.lock);
    return G_SOURCE_REMOVE;
}

static void fake_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) user;
    if (!strcmp (method, "loadScript")) {
        const char *file, *name;
        g_variant_get (args, "(&s&s)", &file, &name);
        g_mutex_lock (&F.lock);
        g_free (F.file);
        g_free (F.name);
        F.file = g_strdup (file);
        F.name = g_strdup (name);
        gboolean was = F.loaded;
        F.loaded = TRUE;
        g_mutex_unlock (&F.lock);
        g_atomic_int_inc (&F.loads);
        /* KWin answers -1 for a name that is loaded already */
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(i)", was ? -1 : 7));
    } else if (!strcmp (method, "unloadScript")) {
        /* as KWin does it: answered at once, the script gone when its event
         * loop comes round again (a loadScript that arrived together with
         * this call still finds the name taken) */
        g_mutex_lock (&F.lock);
        gboolean was = F.loaded;
        g_mutex_unlock (&F.lock);
        GSource *later = g_idle_source_new ();
        g_source_set_callback (later, fake_unload_later, NULL, NULL);
        g_source_attach (later, F.ctx);
        g_source_unref (later);
        g_atomic_int_inc (&F.unloads);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(b)", was));
    } else if (!strcmp (method, "run")) {
        /* the object at the id loadScript answered is another script's (see
         * above): nothing of ours runs */
        g_atomic_int_inc (&F.wrong_runs);
        g_dbus_method_invocation_return_value (inv, NULL);
    } else if (!strcmp (method, "start")) {
        char *text = NULL;
        g_mutex_lock (&F.lock);
        g_file_get_contents (F.file, &text, NULL, NULL);
        g_free (F.text);
        F.text = text;
        g_free (F.dest);
        F.dest = NULL;
        const char *d = text ? strstr (text, "const DEST = \"") : NULL;
        if (d) {
            d += strlen ("const DEST = \"");
            F.dest = g_strndup (d, strcspn (d, "\""));
        }
        g_mutex_unlock (&F.lock);
        g_atomic_int_inc (&F.runs);
        g_dbus_method_invocation_return_value (inv, NULL);
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
        own ? g_variant_new ("(su)", KWIN, 0u) : g_variant_new ("(s)", KWIN), NULL,
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
        fprintf (stderr, "fake kwin: %s\n", e->message);
        g_atomic_int_set (&F.ready, -1);
        return NULL;
    }
    F.info = g_dbus_node_info_new_for_xml (fake_xml, NULL);
    g_dbus_connection_register_object (F.bus, "/Scripting", F.info->interfaces[0], &fake_vt, NULL, NULL, NULL);
    /* the id KWin answers is the number of scripts it has loaded: after
     * another script was unloaded, the object at that id is a script that
     * was there before, not the new one. The stand-in is always in that
     * state: only "start" runs the script. */
    g_dbus_connection_register_object (F.bus, "/Scripting/Script7", F.info->interfaces[1], &fake_vt, NULL,
                                       NULL, NULL);
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
    while (!g_atomic_int_get (&c.done)) {
        g_main_context_iteration (NULL, FALSE);     /* the provider answers on this thread */
        g_usleep (500);
    }
}

/* what the script would do: call Report on the daemon's connection */
static void report_from (GDBusConnection *from, const char *state)
{
    g_mutex_lock (&F.lock);
    char *dest = g_strdup (F.dest);
    g_mutex_unlock (&F.lock);
    if (!dest)
        return;
    g_dbus_connection_call (from, dest, WALPE_KWIN_OBJECT, WALPE_KWIN_IFACE, "Report",
                            g_variant_new ("(s)", state), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL,
                            NULL);
    g_free (dest);
}

static void f_report (gpointer state) { report_from (F.bus, state); }
static void f_own (gpointer a)        { fake_own (GPOINTER_TO_INT (a)); }

/* ---- the daemon's outputs: "A" and "B", ids that the test changes */
static guint32 id_a = 1, id_b = 2;

static guint32 lookup (const char *name, gpointer user)
{
    (void) user;
    return !g_strcmp0 (name, "A") ? id_a : !g_strcmp0 (name, "B") ? id_b : 0;
}

/* ---- the checks */

typedef struct {
    WalpeSignal sig;
    guint32 output;
    int want;
} Want;

static gboolean value_is (gpointer p)
{
    Want *w = p;
    return stub_value (&walpe_sig_kwin, w->sig, w->output) == w->want;
}

#define WAIT_VALUE(s, o, v, what)                                                           \
    do {                                                                                    \
        Want w_ = { s, o, v };                                                              \
        CHECK (stub_wait (value_is, &w_, 3000), "%s (%s on %u is %d)", what,                \
               walpe_signal_name (s), (guint) (o), stub_value (&walpe_sig_kwin, s, o));     \
    } while (0)

static gboolean count_is (gpointer p)
{
    gint **c = p;
    return g_atomic_int_get (c[0]) >= GPOINTER_TO_INT (c[1]);
}

#define WAIT_COUNT(counter, n, what)                                                   \
    do {                                                                               \
        gpointer c_[2] = { &(counter), GINT_TO_POINTER (n) };                          \
        CHECK (stub_wait (count_is, c_, 3000), "%s (%d of %d)", what,                  \
               g_atomic_int_get (&(counter)), n);                                      \
    } while (0)

static gboolean state_has (gpointer needle)
{
    const char *s = walpe_sig_kwin_state ();
    return s && strstr (s, needle);
}

static gboolean file_gone (gpointer u)
{
    (void) u;
    g_mutex_lock (&F.lock);
    gboolean gone = F.file && !g_file_test (F.file, G_FILE_TEST_EXISTS);
    g_mutex_unlock (&F.lock);
    return gone;
}

static void settle (void)
{
    gint64 end = g_get_monotonic_time () + 200000;
    while (g_get_monotonic_time () < end)
        if (!g_main_context_iteration (NULL, FALSE))
            g_usleep (2000);
}

static gboolean start_provider (void)
{
    stub_reset ();
    walpe_sigprov_register (WALPE_BACKEND_WAYLAND);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, NULL, NULL);
    StubProv *sp = stub_prov (&walpe_sig_kwin);
    return sp && sp->started;
}

static char *text_now (void)
{
    g_mutex_lock (&F.lock);
    char *t = g_strdup (F.text ? F.text : "");
    g_mutex_unlock (&F.lock);
    return t;
}

int main (void)
{
    /* ---- without a bus: the text of the script and the reading of a report */
    char *see = walpe_kwin_see_through_js (" St , org.kde.Konsole,bad\"name, x);evil(,, ok_1-2.a ");
    CHECK (!strcmp (see, "[\"st\",\"org.kde.konsole\",\"ok_1-2.a\"]"), "see-through list as an array: %s", see);
    g_free (see);
    see = walpe_kwin_see_through_js (NULL);
    CHECK (!strcmp (see, "[]"), "no list: %s", see);
    g_free (see);
    char *js = walpe_kwin_script_text (":1.99", 4242, "st");
    CHECK (strstr (js, "const DEST = \":1.99\", PID = 4242, SEE = [\"st\"], GRID = 24, NEED = 490;") != NULL,
           "the script carries where to report, whose windows do not count, the list and the X11 "
           "backend's grid and threshold (85 %% of 576 cells)");
    CHECK (strstr (js, "\"" WALPE_KWIN_OBJECT "\", \"" WALPE_KWIN_IFACE "\", \"Report\"") != NULL,
           "and the method it calls");
    g_free (js);
    const char *name;
    gsize nlen;
    guint flags;
    CHECK (walpe_kwin_report_line ("DP-1\tcaf", 8, &name, &nlen, &flags) && nlen == 4 &&
           flags == (WALPE_KWIN_COVERED | WALPE_KWIN_FOCUS | WALPE_KWIN_FULLSCREEN), "a line with every flag");
    CHECK (walpe_kwin_report_line ("DP-1\t", 5, &name, &nlen, &flags) && flags == 0, "a line with none");
    CHECK (!walpe_kwin_report_line ("DP-1", 4, &name, &nlen, &flags) &&
           !walpe_kwin_report_line ("\tc", 2, &name, &nlen, &flags) &&
           !walpe_kwin_report_line ("DP-1\tx", 6, &name, &nlen, &flags), "what is no line");
    char *rest = walpe_pause_rule_sources_without ("wl-frame,kwin", "kwin");
    CHECK (!strcmp (rest, "wl-frame") && walpe_pause_rule_has_source ("wl-frame,kwin", "kwin") &&
           !walpe_pause_rule_has_source ("wl-frame", "kwin") && !walpe_pause_rule_has_source ("", "kwin"),
           "sources without a provider that cannot report: %s", rest);
    g_free (rest);

    char *dbus_daemon = g_find_program_in_path ("dbus-daemon");
    if (!dbus_daemon) {
        printf ("sig-kwin: skipped (no dbus-daemon)\n");
        return stub_fails ? 1 : 77;
    }
    g_free (dbus_daemon);
    g_setenv ("WALPE_SIG_WAYLAND_DISPLAY", "/nonexistent/walpe-test/wayland-none", TRUE);
    g_setenv ("WALPE_LOGIND", "off", TRUE);
    g_setenv ("WALPE_KWIN_SCRIPT", "1", TRUE);      /* no compositor to tell KWin by */

    GTestDBus *tb = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (tb);
    F.address = g_strdup (g_test_dbus_get_bus_address (tb));
    F.ctx = g_main_context_new ();
    F.loop = g_main_loop_new (F.ctx, FALSE);
    F.thread = g_thread_new ("fake-kwin", fake_thread, NULL);
    while (!g_atomic_int_get (&F.ready))
        g_usleep (1000);
    if (g_atomic_int_get (&F.ready) < 0)
        return 1;
    GError *e = NULL;
    GDBusConnection *other = g_dbus_connection_new_for_address_sync (
        F.address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &e);
    if (!other) {
        fprintf (stderr, "sig-kwin: %s\n", e->message);
        return 1;
    }
    walpe_sig_kwin_use_address (F.address);
    walpe_sigprov_set_output_lookup (lookup, NULL);
    const guint FS = WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN), COV = WALPE_SIG_BIT (WALPE_SIG_COVERED);

    /* 1. no rule on: started (the rules would work), nothing kept */
    walpe_sigprov_set_window_rules (0);
    CHECK (start_provider (), "starts where the compositor is KWin");
    settle ();
    CHECK (g_atomic_int_get (&F.loads) == 0 && g_atomic_int_get (&F.unloads) == 0,
           "no rule on: KWin is not talked to");
    CHECK (state_has ("off"), "and the state says so: %s", walpe_sig_kwin_state ());
    CHECK (!g_strcmp0 (walpe_signal_sources (WALPE_SIG_FULLSCREEN), "kwin") &&
           strstr (walpe_signal_sources (WALPE_SIG_COVERED), "kwin") != NULL,
           "kwin is a source of fullscreen and covered");

    /* 2. a rule on: the script goes into KWin */
    walpe_sigprov_set_window_rules (FS);
    WAIT_COUNT (F.runs, 1, "the script is loaded and run");
    CHECK (g_atomic_int_get (&F.loads) == 1 && g_atomic_int_get (&F.unloads) == 1,
           "a script of an earlier daemon is taken out first, then one load");
    char *text = text_now ();
    char *pid = g_strdup_printf ("PID = %u,", (guint) getpid ());
    CHECK (strstr (text, pid) != NULL && strstr (text, "SEE = []") != NULL, "its text has the daemon's pid "
           "and an empty see-through list");
    g_mutex_lock (&F.lock);
    CHECK (F.dest && F.dest[0] == ':' && !g_strcmp0 (F.name, WALPE_KWIN_SCRIPT_NAME),
           "and reports to a unique name (no name is owned): %s, as %s", F.dest ? F.dest : "(none)",
           F.name ? F.name : "(none)");
    g_mutex_unlock (&F.lock);
    g_free (pid);
    g_free (text);
    settle ();
    CHECK (!file_gone (NULL), "the file stays until the script has reported (KWin reads it after it has "
           "answered)");
    CHECK (g_atomic_int_get (&F.wrong_runs) == 0, "the object at the id that loadScript answered is not "
           "asked to run: it can be another script's");
    CHECK (state_has ("loading"), "not counted as loaded before the first report: %s", walpe_sig_kwin_state ());

    /* 3. a report */
    fake_run (f_report, "A\tc\nB\taf");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 1, "A is covered");
    WAIT_VALUE (WALPE_SIG_FOCUS_APP, id_b, 1, "an application has the focus on B");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN_ON, id_b, 1, "and is fullscreen there");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN, 0, 1, "a fullscreen window has the focus");
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, id_b) == 0 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN_ON, id_a) == 0 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_FOCUS_APP, id_a) == 0, "and nothing else");
    CHECK (state_has ("loaded"), "the state: %s", walpe_sig_kwin_state ());
    CHECK (walpe_sig_kwin_trouble () == NULL, "no trouble");
    CHECK (file_gone (NULL), "the file is removed once the script has reported");

    /* 4. the next report takes back what it does not name */
    fake_run (f_report, "A\t\nB\ta");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 0, "A is no longer covered");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN, 0, 0, "no fullscreen window has the focus");
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_FOCUS_APP, id_b) == 1, "the focus is still on B");

    /* 5. somebody else on the bus is not believed */
    report_from (other, "A\tcaf\nB\tcaf");
    g_dbus_connection_flush_sync (other, NULL, NULL);
    settle ();
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, id_a) == 0 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN, 0) == 0, "a report from another connection changes nothing");

    /* 6. an output the daemon shows nothing on: only the global value */
    fake_run (f_report, "A\t\nB\t\nC\tcaf");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN, 0, 1, "fullscreen on an output that is not the daemon's counts globally");
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN_ON, id_a) == 0 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN_ON, id_b) == 0 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, id_a) == 0, "and for no output of the daemon");

    /* 7. the daemon's ids change, an output goes */
    fake_run (f_report, "A\tc\nB\taf");
    WAIT_VALUE (WALPE_SIG_FULLSCREEN_ON, id_b, 1, "B fullscreen again");
    id_b = 5;
    walpe_sigprov_outputs_changed ();
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN_ON, 5) == 1 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN_ON, 2) == 0 &&
           stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, id_a) == 1, "the values follow the new id");
    id_a = 0;
    walpe_sigprov_outputs_changed ();
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, 1) == 0, "an output that is gone loses its value");
    id_a = 1;
    walpe_sigprov_outputs_changed ();
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, 1) == 1, "and has it again when it is back");

    /* 8. the see-through list is part of the script */
    walpe_sigprov_set_see_through ("st, Alacritty");
    WAIT_COUNT (F.runs, 2, "a new list loads the script again");
    text = text_now ();
    CHECK (strstr (text, "SEE = [\"st\",\"alacritty\"]") != NULL, "with the list in it");
    g_free (text);
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 0, "what the old script said is dropped until the new one reports");
    walpe_sigprov_set_see_through ("st, Alacritty");
    settle ();
    CHECK (g_atomic_int_get (&F.runs) == 2, "the same list again loads nothing");
    fake_run (f_report, "A\tc\nB\t");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 1, "the new script reports");
    /* twice in a row: the script that is in KWin has to be gone before the next one is taken */
    walpe_sigprov_set_see_through ("");
    walpe_sigprov_set_see_through ("konsole");
    WAIT_COUNT (F.runs, 3, "two changes in a row end with a loaded script");
    settle ();
    text = text_now ();
    CHECK (strstr (text, "SEE = [\"konsole\"]") != NULL && walpe_sig_kwin_trouble () == NULL,
           "the one with the last list, and no trouble: %s", walpe_sig_kwin_state ());
    g_free (text);
    fake_run (f_report, "A\tc\nB\t");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 1, "which reports");

    /* 9. KWin leaves the bus and comes back (it was restarted) */
    int base = g_atomic_int_get (&F.runs);
    g_mutex_lock (&F.lock);
    F.loaded = FALSE;
    g_mutex_unlock (&F.lock);
    fake_run (f_own, GINT_TO_POINTER (0));
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 0, "KWin gone: nothing is covered");
    CHECK (stub_wait (state_has, "not on the session bus", 3000), "and the state says why: %s",
           walpe_sig_kwin_state ());
    CHECK (walpe_sig_kwin_trouble () != NULL, "that is trouble for the rules");
    fake_run (f_own, GINT_TO_POINTER (1));
    WAIT_COUNT (F.runs, base + 1, "KWin back: the script is loaded again");
    fake_run (f_report, "A\tc\nB\t");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 1, "and reports");
    CHECK (walpe_sig_kwin_trouble () == NULL, "no trouble any more");

    /* 10. a KWin whose script interface is too old */
    fake_run (f_report, "unsupported");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 0, "unsupported: nothing is reported");
    CHECK (stub_wait (state_has, "older than Plasma 6", 3000), "and the state says why: %s",
           walpe_sig_kwin_state ());
    fake_run (f_report, "A\tc\nB\t");
    WAIT_VALUE (WALPE_SIG_COVERED, id_a, 1, "a report after that counts again");

    /* 11. the last rule off: the script leaves KWin, the values go */
    int unloads = g_atomic_int_get (&F.unloads);
    walpe_sigprov_set_window_rules (FS | COV);
    settle ();
    CHECK (g_atomic_int_get (&F.unloads) == unloads && g_atomic_int_get (&F.runs) == base + 1,
           "another rule on: nothing to do");
    walpe_sigprov_set_window_rules (0);
    CHECK (g_atomic_int_get (&F.unloads) == unloads + 1, "no rule on: the script is unloaded before the call returns");
    CHECK (stub_value (&walpe_sig_kwin, WALPE_SIG_COVERED, id_a) == 0, "and nothing is covered");
    CHECK (state_has ("off"), "the state: %s", walpe_sig_kwin_state ());
    settle ();
    g_mutex_lock (&F.lock);
    CHECK (!F.loaded, "KWin has no script of ours");
    g_mutex_unlock (&F.lock);

    /* 12. stopped with a rule on: unloaded too */
    walpe_sigprov_set_window_rules (COV);
    WAIT_COUNT (F.runs, base + 2, "a rule on again: loaded again");
    unloads = g_atomic_int_get (&F.unloads);
    stub_reset ();
    CHECK (g_atomic_int_get (&F.unloads) == unloads + 1, "stop unloads the script");
    CHECK (walpe_sig_kwin_state () == NULL, "stopped");

    /* 13. switched off by the environment; and no bus at all */
    g_setenv ("WALPE_KWIN_SCRIPT", "0", TRUE);
    CHECK (!start_provider (), "WALPE_KWIN_SCRIPT=0: does not start");
    StubProv *sp = stub_prov (&walpe_sig_kwin);
    CHECK (sp && sp->why && strstr (sp->why, "WALPE_KWIN_SCRIPT=0"), "and says so");
    g_setenv ("WALPE_KWIN_SCRIPT", "1", TRUE);
    walpe_sig_kwin_use_address ("unix:path=/nonexistent/walpe-test/bus");
    CHECK (start_provider (), "starts: the compositor is KWin");
    CHECK (stub_wait (state_has, "no connection to the session bus", 3000), "a bus that is not there: %s",
           walpe_sig_kwin_state ());
    CHECK (walpe_sig_kwin_trouble () != NULL, "is trouble, so status will not say the rules work");
    stub_reset ();

    walpe_sig_kwin_use_address (NULL);
    g_dbus_connection_close_sync (other, NULL, NULL);
    g_object_unref (other);
    g_main_loop_quit (F.loop);
    g_thread_join (F.thread);
    g_test_dbus_down (tb);
    g_object_unref (tb);

    if (stub_fails) {
        fprintf (stderr, "sig-kwin: %d check(s) failed\n", stub_fails);
        return 1;
    }
    printf ("sig-kwin: ok\n");
    return 0;
}
