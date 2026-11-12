/* gnome.c - provider "gnome": fullscreen, covered and focused windows per
 * output on GNOME Shell, from the WalPE extension (contrib/gnome).
 *
 * Mutter has no window list for clients. The extension that keeps the
 * daemon's wallpaper windows on the desktop (platform/wl_gnome.h) sees
 * every window; it works the three answers out inside the shell, per
 * monitor, and sends them only when one changes (signal WindowsChanged of
 * io.github.walpe.Gnome; method Windows for the state right now, which
 * also carries the "see-through windows" setting). The lines are those of
 * winreport.h.
 *
 * What this file keeps, and when:
 *   - Nothing unless the compositor is Mutter (told by a global of the
 *     providers' Wayland session; WALPE_GNOME_EXT=0 switches the provider
 *     off, =1 takes Mutter for granted) and the extension owns its name on
 *     the session bus when the provider starts.
 *   - Then: a connection of its own to the session bus, a watch on the
 *     name and one signal subscription. The extension leaving the bus
 *     (switched off) drops the answers; coming back, it is asked again.
 * Everything runs on the main thread. */
#include "walpe-build-config.h"
#include "signals/providers.h"
#include "signals/wl_session.h"
#include "signals/winreport.h"
#include "platform/wl_gnome.h"

static struct {
    gboolean started;
    GDBusConnection *bus;
    GCancellable *cancel;       /* of everything asked on `bus` */
    guint watch, sub;
    gboolean there;             /* the extension owns its name */
    gboolean reporting;         /* a state has come from it */
    char *state;                /* its last report */
    char *see;                  /* the see-through list it was given */
    GHashTable *ids;            /* output ids that have a value from it */
    const char *trouble;        /* why it reports nothing right now; NULL = it does */
    char *address;              /* tests: the bus to use */
} G;

static void gnome_set (guint32 id, guint flags)
{
    walpe_signal_set (&walpe_sig_gnome, WALPE_SIG_COVERED, id, (flags & WALPE_WINREPORT_COVERED) != 0);
    walpe_signal_set (&walpe_sig_gnome, WALPE_SIG_FOCUS_APP, id, (flags & WALPE_WINREPORT_FOCUS) != 0);
    walpe_signal_set (&walpe_sig_gnome, WALPE_SIG_FULLSCREEN_ON, id,
                      (flags & WALPE_WINREPORT_FULLSCREEN) != 0);
}

/* G.state -> the registry. Outputs are matched by name, as the window
 * list's are (wl_toplevel.c); an output the daemon shows nothing on has
 * no id and still counts for the global "a fullscreen window has the
 * focus". Outputs that had a value and are not in the report lose it. */
static void gnome_apply (void)
{
    GHashTable *seen = g_hash_table_new (NULL, NULL);
    gboolean fullscreen = FALSE;
    walpe_signals_begin ();
    for (const char *p = G.state; p && *p;) {
        const char *end = strchr (p, '\n'), *name;
        gsize len = end ? (gsize) (end - p) : strlen (p), nlen;
        guint flags;
        if (walpe_winreport_line (p, len, &name, &nlen, &flags)) {
            char *n = g_strndup (name, nlen);
            guint32 id = walpe_sigprov_output_id (n);
            g_free (n);
            fullscreen = fullscreen || (flags & WALPE_WINREPORT_FULLSCREEN);
            if (id) {
                gnome_set (id, flags);
                g_hash_table_add (seen, GUINT_TO_POINTER (id));
            }
        }
        p = end ? end + 1 : NULL;
    }
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init (&it, G.ids);
    while (g_hash_table_iter_next (&it, &key, NULL))
        if (!g_hash_table_contains (seen, key))
            gnome_set (GPOINTER_TO_UINT (key), 0);
    walpe_signal_set (&walpe_sig_gnome, WALPE_SIG_FULLSCREEN, 0, fullscreen);
    walpe_signals_end ();
    g_hash_table_unref (G.ids);
    G.ids = seen;
}

static void gnome_forget (void)
{
    g_clear_pointer (&G.state, g_free);
    G.reporting = FALSE;
    if (G.ids)
        gnome_apply ();
}

void walpe_sig_gnome_outputs_changed (void)
{
    if (G.started && G.ids)
        gnome_apply ();     /* ids are looked up again; a gone output loses its values */
}

static void gnome_take (const char *state)
{
    if (!G.reporting) {
        G.trouble = NULL;
        g_message ("gnome: the WalPE extension reports, per monitor, windows that cover it and "
                   "the focused window (fullscreen or not) when that changes");
    }
    G.reporting = TRUE;
    g_free (G.state);
    G.state = g_strdup (state);
    gnome_apply ();
}

static void gnome_changed (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                           const char *signal, GVariant *args, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) signal; (void) user;
    const char *state = NULL;
    if (!G.there || !g_variant_is_of_type (args, G_VARIANT_TYPE ("(s)")))
        return;
    g_variant_get (args, "(&s)", &state);
    gnome_take (state);
}

static void gnome_asked (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (r) {
        const char *state = NULL;
        g_variant_get (r, "(&s)", &state);
        gnome_take (state);
        g_variant_unref (r);
    } else if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED) && G.there) {
        G.trouble = "the WalPE extension did not answer the question about its windows";
        g_message ("gnome: %s (%s)", G.trouble, e->message);
    }
    g_clear_error (&e);
}

/* the state right now; the call carries the see-through list */
static void gnome_ask (void)
{
    if (!G.bus || !G.there)
        return;
    g_free (G.see);
    G.see = g_strdup (walpe_sigprov_see_through ());
    g_dbus_connection_call (G.bus, WL_GNOME_BUS_NAME, WL_GNOME_OBJECT, WL_GNOME_IFACE, "Windows",
                            g_variant_new ("(s)", G.see ? G.see : ""), G_VARIANT_TYPE ("(s)"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, G.cancel, gnome_asked, NULL);
}

static void gnome_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer user)
{
    (void) c; (void) name; (void) owner; (void) user;
    G.there = TRUE;
    G.trouble = NULL;
    gnome_ask ();
}

static void gnome_vanished (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    gboolean was = G.there;
    G.there = FALSE;
    gnome_forget ();
    G.trouble = "the WalPE extension for GNOME Shell is not on the session bus (switched off?): "
                "windows cannot be asked about until it is";
    if (was)
        g_message ("gnome: %s. The wallpaper windows stay where they are", G.trouble);
}

/* the session bus's address without GLib's fallback of starting a bus */
static char *gnome_bus_address (void)
{
    if (G.address)
        return g_strdup (G.address);
    const char *env = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env)
        return g_strdup (env);
    char *sock = g_build_filename (g_get_user_runtime_dir (), "bus", NULL);
    char *addr = g_file_test (sock, G_FILE_TEST_EXISTS) ? g_strdup_printf ("unix:path=%s", sock) : NULL;
    g_free (sock);
    return addr;
}

static void gnome_disconnect (void)
{
    if (G.cancel) {
        g_cancellable_cancel (G.cancel);
        g_clear_object (&G.cancel);
    }
    if (G.bus) {
        if (G.watch)
            g_bus_unwatch_name (G.watch);
        if (G.sub)
            g_dbus_connection_signal_unsubscribe (G.bus, G.sub);
        if (!g_dbus_connection_is_closed (G.bus))
            g_dbus_connection_close_sync (G.bus, NULL, NULL);
        g_clear_object (&G.bus);
    }
    G.watch = G.sub = 0;
    G.there = FALSE;
    g_clear_pointer (&G.see, g_free);
}

void walpe_sig_gnome_use_address (const char *address)
{
    g_free (G.address);
    G.address = g_strdup (address);
}

const char *walpe_sig_gnome_state (void)
{
    if (!G.started)
        return NULL;
    if (G.trouble)
        return G.trouble;
    return G.reporting ? "the WalPE extension reports the windows" : "asking the WalPE extension";
}

static gboolean gnome_start (const WalpeSignalProvider *self, char **why)
{
    (void) self;
    if (G.started)
        return TRUE;
    const char *env = g_getenv ("WALPE_GNOME_EXT");
    gboolean mutter = FALSE;
    if (env && !strcmp (env, "0")) {
        if (why)
            *why = g_strdup ("WALPE_GNOME_EXT=0: the WalPE extension for GNOME Shell is not asked");
        return FALSE;
    }
    if (env && !strcmp (env, "1")) {
        mutter = TRUE;
    } else {
        WlSession *s = walpe_wlsess_ref (NULL);
        if (s) {
            mutter = walpe_wlsess_caps (s)->mutter;
            walpe_wlsess_unref (s);
        }
    }
    if (!mutter)
        return FALSE;       /* another compositor: no reason to give, the window list's stands */
    char *addr = gnome_bus_address ();
    GError *e = NULL;
    if (addr)
        G.bus = g_dbus_connection_new_for_address_sync (
            addr,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, &e);
    g_free (addr);
    gboolean owned = FALSE;
    if (G.bus) {
        g_dbus_connection_set_exit_on_close (G.bus, FALSE);
        GVariant *r = g_dbus_connection_call_sync (G.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus", "NameHasOwner",
                                                   g_variant_new ("(s)", WL_GNOME_BUS_NAME),
                                                   G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE, 1500,
                                                   NULL, NULL);
        if (r) {
            g_variant_get (r, "(b)", &owned);
            g_variant_unref (r);
        }
    }
    if (!owned) {
        if (why)
            *why = G.bus ? g_strdup ("GNOME Shell tells clients nothing about other windows, and the "
                                     "WalPE extension for it (" WL_GNOME_EXT_UUID ") is not on the "
                                     "session bus")
                         : g_strdup_printf ("GNOME Shell tells clients nothing about other windows, and "
                                            "there is no session bus to ask the WalPE extension on%s%s",
                                            e ? ": " : "", e ? e->message : "");
        g_clear_error (&e);
        gnome_disconnect ();
        return FALSE;
    }
    G.started = TRUE;
    G.ids = g_hash_table_new (NULL, NULL);
    G.cancel = g_cancellable_new ();
    G.sub = g_dbus_connection_signal_subscribe (G.bus, WL_GNOME_BUS_NAME, WL_GNOME_IFACE, "WindowsChanged",
                                                WL_GNOME_OBJECT, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                gnome_changed, NULL, NULL);
    G.watch = g_bus_watch_name_on_connection (G.bus, WL_GNOME_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                              gnome_appeared, gnome_vanished, NULL, NULL);
    return TRUE;
}

static void gnome_stop (const WalpeSignalProvider *self)
{
    (void) self;
    if (!G.started)
        return;
    gnome_disconnect ();
    gnome_forget ();
    G.started = FALSE;
    g_clear_pointer (&G.ids, g_hash_table_unref);
    G.trouble = NULL;
}

/* the see-through list changed: the extension is told with the next question */
static void gnome_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) now;
    if (sig == WALPE_SIG_COVERED && G.started && G.there &&
        g_strcmp0 (G.see ? G.see : "", walpe_sigprov_see_through () ? walpe_sigprov_see_through () : ""))
        gnome_ask ();
}

const WalpeSignalProvider walpe_sig_gnome = {
    .name = "gnome",
    .signals = WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN) | WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN_ON) |
               WALPE_SIG_BIT (WALPE_SIG_COVERED) | WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP),
    .backends = 1u << WALPE_BACKEND_WAYLAND,
    .start = gnome_start,
    .stop = gnome_stop,
    .recheck = gnome_recheck,
};
