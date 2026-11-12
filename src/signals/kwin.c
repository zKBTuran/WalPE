/* kwin.c - provider "kwin": fullscreen, covered and focused windows per
 * output on KDE Plasma, from a script the daemon loads into KWin.
 *
 * KWin has no window list for clients (zwlr_foreign_toplevel_manager_v1)
 * and goes on sending frame callbacks to a wallpaper surface that windows
 * cover, so on Plasma nothing told the daemon to pause behind a
 * fullscreen or maximized window. KWin's script interface can: see
 * kwinscript.h for the script and for what it counts.
 *
 * What this file keeps, and when:
 *   - Nothing unless the compositor is KWin (told by the globals of the
 *     providers' Wayland session; WALPE_KWIN_SCRIPT=0 switches the
 *     provider off, =1 takes KWin for granted).
 *   - Nothing but the answer "it would work here" while none of the three
 *     rules is on (pause_fullscreen, pause_covered, focus_action): no
 *     connection to the session bus, no script in KWin.
 *   - With a rule on: a connection of its own to the session bus (no name
 *     is owned: the script calls the connection's unique name), one
 *     object on it with one method, a watch on the name org.kde.KWin,
 *     and the script in KWin. KWin calls only when an answer changes.
 *     KWin leaving the bus drops the answers; KWin coming back (it was
 *     restarted) gets the script again. The script is unloaded when the
 *     last rule is switched off and when the daemon stops.
 * Everything runs on the main thread. */
#include "walpe-build-config.h"
#include "signals/providers.h"
#include "signals/wl_session.h"
#include "signals/kwinscript.h"
#include <glib/gstdio.h>
#include <unistd.h>

#define KWIN_BUS_NAME "org.kde.KWin"

static struct {
    gboolean started;           /* the compositor is KWin: the provider answers for its signals */
    guint rules;                /* WALPE_SIG_BIT () of the rules that are switched on */
    GDBusConnection *bus;       /* only while a rule is on */
    GCancellable *cancel;       /* of everything asked on `bus` */
    gboolean connecting;
    guint watch, object;
    char *owner;                /* KWin's unique name; NULL = not on the bus */
    gboolean loaded;            /* the script has reported */
    char *see;                  /* the see-through list the loaded script was given */
    char *state;                /* its last report */
    GHashTable *ids;            /* output ids that have a value from it */
    const char *trouble;        /* why it reports nothing right now; NULL = it does, or no rule is on */
    char *trouble_own;          /* storage of a trouble text made here */
    gboolean loading, reload;   /* a load is under way; another one is wanted after it */
    guint retry;                /* a second try to load, pending */
    gboolean retried;
    char *address;              /* tests: the bus to use */
} K;

static void kwin_arm (void);

static void kwin_trouble (const char *fixed, char *made)
{
    g_free (K.trouble_own);
    K.trouble_own = made;
    K.trouble = made ? made : fixed;
    if (K.trouble)
        g_message ("kwin: %s", K.trouble);
}

/* ---- values */

static void kwin_set (guint32 id, guint flags)
{
    walpe_signal_set (&walpe_sig_kwin, WALPE_SIG_COVERED, id, (flags & WALPE_KWIN_COVERED) != 0);
    walpe_signal_set (&walpe_sig_kwin, WALPE_SIG_FOCUS_APP, id, (flags & WALPE_KWIN_FOCUS) != 0);
    walpe_signal_set (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN_ON, id, (flags & WALPE_KWIN_FULLSCREEN) != 0);
}

/* K.state -> the registry. Outputs are matched by name, as the window
 * list's are (wl_toplevel.c); an output the daemon shows nothing on has
 * no id and still counts for the global "a fullscreen window has the
 * focus". Outputs that had a value and are not in the report lose it. */
static void kwin_apply (void)
{
    GHashTable *seen = g_hash_table_new (NULL, NULL);
    gboolean fullscreen = FALSE;
    walpe_signals_begin ();
    for (const char *p = K.state; p && *p;) {
        const char *end = strchr (p, '\n'), *name;
        gsize len = end ? (gsize) (end - p) : strlen (p), nlen;
        guint flags;
        if (walpe_kwin_report_line (p, len, &name, &nlen, &flags)) {
            char *n = g_strndup (name, nlen);
            guint32 id = walpe_sigprov_output_id (n);
            g_free (n);
            fullscreen = fullscreen || (flags & WALPE_KWIN_FULLSCREEN);
            if (id) {
                kwin_set (id, flags);
                g_hash_table_add (seen, GUINT_TO_POINTER (id));
            }
        }
        p = end ? end + 1 : NULL;
    }
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init (&it, K.ids);
    while (g_hash_table_iter_next (&it, &key, NULL))
        if (!g_hash_table_contains (seen, key))
            kwin_set (GPOINTER_TO_UINT (key), 0);
    walpe_signal_set (&walpe_sig_kwin, WALPE_SIG_FULLSCREEN, 0, fullscreen);
    walpe_signals_end ();
    g_hash_table_unref (K.ids);
    K.ids = seen;
}

static void kwin_forget (void)
{
    g_clear_pointer (&K.state, g_free);
    K.loaded = FALSE;
    if (K.ids)
        kwin_apply ();
}

void walpe_sig_kwin_outputs_changed (void)
{
    if (K.started && K.ids)
        kwin_apply ();      /* ids are looked up again; a gone output loses its values */
}

/* the file KWin is given the script in */
static char *kwin_script_file (void)
{
    char *base = g_strdup_printf ("walpe-kwin-%u.js", (guint) getpid ());
    char *f = g_build_filename (g_get_user_runtime_dir (), base, NULL);
    g_free (base);
    return f;
}

/* ---- the object the script calls */

static const char kwin_xml[] =
    "<node><interface name='" WALPE_KWIN_IFACE "'>"
    "<method name='Report'><arg type='s' name='state' direction='in'/></method>"
    "</interface></node>";

static void kwin_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer user)
{
    (void) c; (void) path; (void) iface; (void) user;
    /* only KWin's own connection is believed (its scripts call from it) */
    if (g_strcmp0 (method, "Report") || !K.owner || g_strcmp0 (sender, K.owner)) {
        g_dbus_method_invocation_return_error_literal (inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                       "only KWin reports here");
        return;
    }
    const char *state = NULL;
    g_variant_get (args, "(&s)", &state);
    if (!K.loaded) {
        /* the script runs: KWin has read its file */
        char *f = kwin_script_file ();
        g_unlink (f);
        g_free (f);
    }
    if (!strcmp (state, "unsupported")) {
        kwin_forget ();
        kwin_trouble ("this KWin's script interface is older than Plasma 6: its windows cannot be "
                      "asked about", NULL);
    } else {
        if (!K.loaded) {
            kwin_trouble (NULL, NULL);
            g_message ("kwin: the pause script is loaded in KWin: it reports, per output, windows "
                       "that cover it and the focused window (fullscreen or not) when that changes");
        }
        K.loaded = TRUE;
        g_free (K.state);
        K.state = g_strdup (state);
        kwin_apply ();
    }
    g_dbus_method_invocation_return_value (inv, NULL);
}

static const GDBusInterfaceVTable kwin_vt = { kwin_method, NULL, NULL, { 0 } };

/* ---- the script in KWin */

static void kwin_load (void);

/* A load has come to its end, whichever. One that was asked for meanwhile
 * (the see-through list changed again) starts now: never two at a time,
 * or the second finds the first one's script in KWin. */
static void kwin_load_done (void)
{
    K.loading = FALSE;
    if (K.reload) {
        K.reload = FALSE;
        kwin_load ();
    }
}

/* KWin has answered "start". It reads the script's file in a thread of its
 * own after that: the file stays until the script has reported
 * (kwin_method) or the rules go off. */
static void kwin_ran (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (r) {
        g_variant_unref (r);
    } else if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error (&e);
        return;         /* the connection was let go, which has reset everything */
    } else {
        kwin_trouble (NULL, g_strdup_printf ("KWin did not run the pause script "
                                             "(org.kde.kwin.Scripting.start): %s", e->message));
    }
    g_clear_error (&e);
    kwin_load_done ();
}

static gboolean kwin_load_again (gpointer user);

static void kwin_loaded (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (!r) {
        if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            kwin_trouble (NULL, g_strdup_printf ("KWin did not take the pause script "
                                                 "(org.kde.kwin.Scripting.loadScript): %s", e->message));
            kwin_load_done ();
        }
        g_clear_error (&e);
        return;
    }
    gint32 id = -1;
    g_variant_get (r, "(i)", &id);
    g_variant_unref (r);
    if (id < 0 || !K.bus) {
        /* -1: a script of the name is loaded. KWin takes one out a moment
         * after it has answered unloadScript: once more, then give up */
        if (K.bus && !K.retried) {
            K.retried = TRUE;
            K.retry = g_timeout_add (300, kwin_load_again, NULL);
        } else if (K.bus) {
            kwin_trouble ("KWin did not take the pause script (org.kde.kwin.Scripting.loadScript "
                          "answered -1)", NULL);
            kwin_load_done ();
        }
        return;
    }
    K.retried = FALSE;
    /* Not "run" on the script's own object, /Scripting/Script<id>: the id
     * KWin answers is the number of scripts it has loaded, so after another
     * script was unloaded it is the id, and the object, of one that is still
     * there (KWin 6.8), and "run" would reach that one. "start" runs every
     * loaded script that is not running yet. */
    g_dbus_connection_call (K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "start", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, K.cancel, kwin_ran, NULL);
}

/* KWin has answered unloadScript: now the new one */
static void kwin_unloaded (GObject *src, GAsyncResult *res, gpointer user)
{
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    char *file = user;
    if (r)
        g_variant_unref (r);
    if (!K.bus || (!r && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED))) {
        /* (cancelled: the connection was let go, which has reset everything) */
    } else
        g_dbus_connection_call (K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                                g_variant_new ("(ss)", file, WALPE_KWIN_SCRIPT_NAME), G_VARIANT_TYPE ("(i)"),
                                G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, K.cancel, kwin_loaded, NULL);
    g_clear_error (&e);
    g_free (file);
}

static gboolean kwin_load_again (gpointer user)
{
    (void) user;
    K.retry = 0;
    K.loading = FALSE;
    kwin_load ();
    return G_SOURCE_REMOVE;
}

/* (Re)load: a script of an earlier daemon, or the one with the old
 * see-through list, goes first, and the new one when KWin has answered
 * that. */
static void kwin_load (void)
{
    if (!K.bus || !K.owner)
        return;
    if (K.loading) {
        K.reload = TRUE;
        return;
    }
    GError *e = NULL;
    char *file = kwin_script_file ();
    char *text = walpe_kwin_script_text (g_dbus_connection_get_unique_name (K.bus), (guint) getpid (),
                                         walpe_sigprov_see_through ());
    gboolean ok = g_file_set_contents (file, text, -1, &e);
    g_free (text);
    if (!ok) {
        kwin_trouble (NULL, g_strdup_printf ("the pause script could not be written: %s", e->message));
        g_clear_error (&e);
        g_free (file);
        return;
    }
    K.loading = TRUE;
    g_free (K.see);
    K.see = g_strdup (walpe_sigprov_see_through ());
    kwin_forget ();
    g_dbus_connection_call (K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
                            g_variant_new ("(s)", WALPE_KWIN_SCRIPT_NAME), NULL,
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, K.cancel, kwin_unloaded, file);
}

static void kwin_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer user)
{
    (void) c; (void) name; (void) user;
    K.retried = FALSE;
    g_free (K.owner);
    K.owner = g_strdup (owner);
    kwin_trouble (NULL, NULL);
    kwin_load ();
}

static void kwin_vanished (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    g_clear_pointer (&K.owner, g_free);
    K.loading = K.reload = FALSE;
    kwin_forget ();
    if (K.bus)
        kwin_trouble ("KWin is not on the session bus (org.kde.KWin): windows cannot be asked about "
                      "until it is", NULL);
}

/* ---- the connection */

/* unload: the script is taken out of KWin first (a rule switched off,
 * the daemon stopping); FALSE when the connection is already dead */
static void kwin_disconnect (gboolean unload)
{
    if (K.cancel) {
        g_cancellable_cancel (K.cancel);
        g_clear_object (&K.cancel);
    }
    K.connecting = FALSE;
    if (K.retry)
        g_source_remove (K.retry);
    K.retry = 0;
    K.retried = K.loading = K.reload = FALSE;
    if (K.bus) {
        if (unload && K.owner && !g_dbus_connection_is_closed (K.bus)) {
            GVariant *r = g_dbus_connection_call_sync (
                K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
                g_variant_new ("(s)", WALPE_KWIN_SCRIPT_NAME), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000,
                NULL, NULL);
            if (r)
                g_variant_unref (r);
        }
        if (K.watch)
            g_bus_unwatch_name (K.watch);
        if (K.object)
            g_dbus_connection_unregister_object (K.bus, K.object);
        g_signal_handlers_disconnect_by_data (K.bus, &K);
        if (!g_dbus_connection_is_closed (K.bus))
            g_dbus_connection_close_sync (K.bus, NULL, NULL);
        g_clear_object (&K.bus);
    }
    K.watch = K.object = 0;
    g_clear_pointer (&K.owner, g_free);
    g_clear_pointer (&K.see, g_free);
    kwin_forget ();
    char *f = kwin_script_file ();
    g_unlink (f);
    g_free (f);
}

static void kwin_closed (GDBusConnection *c, gboolean remote, GError *err, gpointer user)
{
    (void) c; (void) remote; (void) err; (void) user;
    kwin_disconnect (FALSE);
    kwin_trouble ("the connection to the session bus ended: windows cannot be asked about", NULL);
}

static void kwin_connected (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src; (void) user;
    GError *e = NULL;
    GDBusConnection *bus = g_dbus_connection_new_for_address_finish (res, &e);
    if (!bus) {
        if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            K.connecting = FALSE;
            kwin_trouble (NULL, g_strdup_printf ("no connection to the session bus: %s", e->message));
        }
        g_clear_error (&e);
        return;
    }
    K.connecting = FALSE;
    K.bus = bus;
    g_dbus_connection_set_exit_on_close (bus, FALSE);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml (kwin_xml, NULL);
    K.object = g_dbus_connection_register_object (bus, WALPE_KWIN_OBJECT, info->interfaces[0], &kwin_vt,
                                                  NULL, NULL, NULL);
    g_dbus_node_info_unref (info);
    g_signal_connect (bus, "closed", G_CALLBACK (kwin_closed), &K);
    K.watch = g_bus_watch_name_on_connection (bus, KWIN_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                              kwin_appeared, kwin_vanished, NULL, NULL);
}

/* the session bus's address without GLib's fallback of starting a bus */
static char *kwin_bus_address (void)
{
    if (K.address)
        return g_strdup (K.address);
    const char *env = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env)
        return g_strdup (env);
    char *sock = g_build_filename (g_get_user_runtime_dir (), "bus", NULL);
    char *addr = g_file_test (sock, G_FILE_TEST_EXISTS) ? g_strdup_printf ("unix:path=%s", sock) : NULL;
    g_free (sock);
    return addr;
}

/* connect while a rule is on, let go when none is */
static void kwin_arm (void)
{
    gboolean want = K.started && K.rules != 0;
    if (!want) {
        if (K.bus || K.connecting)
            kwin_disconnect (TRUE);
        kwin_trouble (NULL, NULL);
        return;
    }
    if (K.bus || K.connecting)
        return;
    char *addr = kwin_bus_address ();
    if (!addr) {
        kwin_trouble ("no session bus (DBUS_SESSION_BUS_ADDRESS is not set and the runtime directory "
                      "has no bus socket): KWin cannot be asked about its windows", NULL);
        return;
    }
    K.connecting = TRUE;
    K.cancel = g_cancellable_new ();
    g_dbus_connection_new_for_address (
        addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, K.cancel, kwin_connected, NULL);
    g_free (addr);
}

/* ---- what the daemon and the tests set */

void walpe_sig_kwin_set_rules (guint mask)
{
    mask &= WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN) | WALPE_SIG_BIT (WALPE_SIG_COVERED) |
            WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP);
    if (mask == K.rules)
        return;
    K.rules = mask;
    if (K.started)
        kwin_arm ();
}

void walpe_sig_kwin_use_address (const char *address)
{
    g_free (K.address);
    K.address = g_strdup (address);
}

const char *walpe_sig_kwin_state (void)
{
    if (!K.started)
        return NULL;
    if (!K.rules)
        return "off: none of the pause rules it serves is switched on";
    if (K.trouble)
        return K.trouble;
    return K.loaded ? "the pause script is loaded in KWin" : "loading the pause script into KWin";
}

const char *walpe_sig_kwin_trouble (void)
{
    return K.started && K.rules ? K.trouble : NULL;
}

/* ---- the provider */

static gboolean kwin_start (const WalpeSignalProvider *self, char **why)
{
    (void) self;
    if (K.started)
        return TRUE;
    const char *env = g_getenv ("WALPE_KWIN_SCRIPT");
    gboolean kwin = FALSE;
    if (env && !strcmp (env, "0")) {
        if (why)
            *why = g_strdup ("WALPE_KWIN_SCRIPT=0: KWin is not asked about its windows");
        return FALSE;
    }
    if (env && !strcmp (env, "1")) {
        kwin = TRUE;
    } else {
        WlSession *s = walpe_wlsess_ref (NULL);
        if (s) {
            kwin = walpe_wlsess_caps (s)->kwin;
            walpe_wlsess_unref (s);
        }
    }
    if (!kwin)
        return FALSE;       /* another compositor: no reason to give, the window list's stands */
    K.started = TRUE;
    K.ids = g_hash_table_new (NULL, NULL);
    kwin_arm ();
    return TRUE;
}

static void kwin_stop (const WalpeSignalProvider *self)
{
    (void) self;
    if (!K.started)
        return;
    kwin_disconnect (TRUE);
    K.started = FALSE;
    g_clear_pointer (&K.ids, g_hash_table_unref);
    g_clear_pointer (&K.trouble_own, g_free);
    K.trouble = NULL;
}

/* the see-through list changed: the script carries it */
static void kwin_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) now;
    if (sig == WALPE_SIG_COVERED && K.bus && K.owner && g_strcmp0 (K.see, walpe_sigprov_see_through ()))
        kwin_load ();
}

const WalpeSignalProvider walpe_sig_kwin = {
    .name = "kwin",
    .signals = WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN) | WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN_ON) |
               WALPE_SIG_BIT (WALPE_SIG_COVERED) | WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP),
    .backends = 1u << WALPE_BACKEND_WAYLAND,
    .start = kwin_start,
    .stop = kwin_stop,
    .recheck = kwin_recheck,
};
