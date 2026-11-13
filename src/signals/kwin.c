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
 *     rules is on (pause_fullscreen, pause_covered, focus_action) and no
 *     wallpaper wants the pointer from the Plasma add-on: no connection
 *     to the session bus, no script in KWin.
 *   - With a rule on: a connection of its own to the session bus (no name
 *     is owned: the script calls the connection's unique name), one
 *     object on it with one method, a watch on the name org.kde.KWin,
 *     and the pause script in KWin. KWin calls only when an answer
 *     changes. KWin leaving the bus drops the answers; KWin coming back
 *     (it was restarted) gets the script again. The script is unloaded
 *     when the last rule is switched off and when the daemon stops. A
 *     daemon that is killed leaves it in KWin, reporting to a name nobody
 *     has; the next one unloads it by its name before it loads its own
 *     (ks_load).
 *   - While a wallpaper wants the pointer from the Plasma add-on ("the
 *     pointer" below): the same connection, a second script in KWin that
 *     tells where the pointer is over Plasma's desktop, and the one name
 *     the daemon ever owns on the bus, for the buttons and the wheel.
 * The two scripts come and go independently of each other.
 * Everything runs on the main thread. */
#include "walpe-build-config.h"
#include "signals/providers.h"
#include "signals/wl_session.h"
#include "signals/kwinscript.h"
#include <glib/gstdio.h>
#include <unistd.h>

#define KWIN_BUS_NAME "org.kde.KWin"

/* The two scripts the daemon can have in KWin. Each is loaded, replaced
 * and taken out by itself (ks_* below): the pause script while a pause
 * rule is on, the pointer script while a wallpaper wants the pointer from
 * the Plasma add-on - which comes and goes with every pause of such a
 * wallpaper, and must not touch what the pause script has said. */
typedef enum { KS_PAUSE = 0, KS_POINTER, KS_N } KKind;

typedef struct {
    const char *name;           /* in KWin */
    const char *file;           /* of the file KWin is given it in: a printf pattern for the pid */
    const char *method;         /* what it calls here */
    const char *what;           /* in messages */
    gboolean want;              /* it is to be in KWin */
    char *given;                /* what the text in KWin was made of (ks_inputs); NULL = none loaded */
    gboolean loading, reload;   /* a load is under way; another one is wanted after it */
    guint chain;                /* counts the loads: an answer to an earlier one is nobody's */
    guint retry;                /* a second try to load, pending */
    gboolean retried;
    gboolean heard;             /* the script in KWin has called */
    guint object;
    char *path;                 /* the one `object` is at: the script that is (being) loaded */
    const char *trouble;        /* why it reports nothing right now; NULL = it does, or is not wanted */
    char *trouble_own;          /* storage of a trouble text made here */
} KScript;

static struct {
    gboolean started;           /* the compositor is KWin: the provider answers for its signals */
    guint rules;                /* WALPE_SIG_BIT () of the rules that are switched on */
    GDBusConnection *bus;       /* only while a rule is on or the pointer is wanted */
    GCancellable *cancel;       /* of everything asked on `bus` */
    gboolean connecting;
    guint watch;
    guint script;               /* counts the scripts loaded: each has an object path of its own */
    char *owner;                /* KWin's unique name; NULL = not on the bus */
    KScript s[KS_N];
    char *state;                /* the pause script's last report */
    GHashTable *ids;            /* output ids that have a value from it */
    char *address;              /* tests: the bus to use */
    /* the pointer from the Plasma add-on (see "the pointer" below) */
    gboolean ptr_want;          /* the name and the pointer script are kept */
    gboolean ptr_now;           /* a wallpaper wants the pointer this moment: events are handed on */
    guint ptr_linger;           /* no longer wanted: what is kept goes when this fires */
    char *ptr_outputs;          /* the outputs whose wallpaper wants it, one per line */
    guint ptr_ms;               /* the least time between two positions */
    char *ptr_on;               /* the output the pointer script last named; NULL = it is on none */
    WalpeSigPointerFn ptr_fn;
    gpointer ptr_user;
    guint ptr_own, ptr_object, ptr_watch;
    gboolean ptr_named;         /* the bus gave us the name */
    char *ptr_shell;            /* the unique name of Plasma's shell; NULL = not on the bus */
} K = {
    .s = { [KS_PAUSE] = { .name = WALPE_KWIN_SCRIPT_NAME, .file = "walpe-kwin-%u.js", .method = "Report",
                          .what = "pause" },
           [KS_POINTER] = { .name = WALPE_KWIN_POINTER_SCRIPT_NAME, .file = "walpe-kwin-pointer-%u.js",
                            .method = "Pointer", .what = "pointer" } },
};

static void kwin_arm (void);

static void ks_trouble (KScript *s, const char *fixed, char *made)
{
    g_free (s->trouble_own);
    s->trouble_own = made;
    s->trouble = made ? made : fixed;
    if (s->trouble)
        g_message ("kwin: %s", s->trouble);
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

/* What a script said is no longer true: it is being replaced or taken
 * out, or KWin is gone. The pause script's answers go; the wallpaper the
 * pointer script named last is told that the pointer left. */
static void ks_forget (KScript *s)
{
    s->heard = FALSE;
    if (s == &K.s[KS_PAUSE]) {
        g_clear_pointer (&K.state, g_free);
        if (K.ids)
            kwin_apply ();
    } else if (K.ptr_on) {
        char *on = g_steal_pointer (&K.ptr_on);
        if (K.ptr_fn)
            K.ptr_fn (on, WALPE_KWIN_PTR_LEAVE, 0, 0, 0, 0, K.ptr_user);
        g_free (on);
    }
}

void walpe_sig_kwin_outputs_changed (void)
{
    if (K.started && K.ids)
        kwin_apply ();      /* ids are looked up again; a gone output loses its values */
}

/* the file KWin is given a script in */
static char *ks_file (const KScript *s)
{
    char *base = g_strdup_printf (s->file, (guint) getpid ());
    char *f = g_build_filename (g_get_user_runtime_dir (), base, NULL);
    g_free (base);
    return f;
}

static void ks_unlink (const KScript *s)
{
    char *f = ks_file (s);
    g_unlink (f);
    g_free (f);
}

/* what a script's text depends on besides the daemon itself: a change of
 * it loads the script again */
static char *ks_inputs (const KScript *s)
{
    if (s == &K.s[KS_PAUSE])
        return g_strdup (walpe_sigprov_see_through () ? walpe_sigprov_see_through () : "");
    return g_strdup_printf ("%u\n%s", K.ptr_ms, K.ptr_outputs ? K.ptr_outputs : "");
}

static char *ks_text (const KScript *s)
{
    const char *dest = g_dbus_connection_get_unique_name (K.bus);
    if (s == &K.s[KS_PAUSE])
        return walpe_kwin_script_text (dest, s->path, (guint) getpid (), walpe_sigprov_see_through ());
    return walpe_kwin_pointer_script_text (dest, s->path, K.ptr_outputs, K.ptr_ms);
}

/* ---- the object a script calls */

static const char kwin_xml[] =
    "<node><interface name='" WALPE_KWIN_IFACE "'>"
    "<method name='Report'><arg type='s' name='state' direction='in'/></method>"
    "<method name='Pointer'><arg type='s' name='where' direction='in'/></method>"
    "</interface></node>";

/* the pause script: per output, what the windows do */
static void kwin_report (KScript *s, const char *state)
{
    if (!strcmp (state, "unsupported")) {
        ks_forget (s);
        ks_trouble (s, "this KWin's script interface is older than Plasma 6: its windows cannot be "
                       "asked about", NULL);
        return;
    }
    if (!s->heard) {
        ks_trouble (s, NULL, NULL);
        g_message ("kwin: the pause script is loaded in KWin: it reports, per output, windows "
                   "that cover it and the focused window (fullscreen or not) when that changes");
    }
    s->heard = TRUE;
    g_free (K.state);
    K.state = g_strdup (state);
    kwin_apply ();
}

/* the pointer script: where the pointer is over Plasma's desktop
 * (kwinscript.h has the three things it says) */
static void kwin_pointer_report (KScript *s, const char *where)
{
    const char *name;
    gsize nlen;
    double x = 0, y = 0;
    if (!strcmp (where, "unsupported")) {
        ks_forget (s);
        ks_trouble (s, "this KWin's script interface does not tell where the pointer is: with the "
                       "Plasma add-on the wallpaper gets no pointer position", NULL);
        return;
    }
    if (!s->heard) {
        ks_trouble (s, NULL, NULL);
        char *outs = g_strdup (K.ptr_outputs && *K.ptr_outputs ? K.ptr_outputs : "no output");
        g_message ("kwin: the pointer script is loaded in KWin: it reports where the pointer is over "
                   "Plasma's desktop on %s (the outputs whose wallpaper uses it), at most every %u ms",
                   g_strdelimit (outs, "\n", ' '), K.ptr_ms);
        g_free (outs);
    }
    s->heard = TRUE;
    switch (walpe_kwin_pointer_line (where, &name, &nlen, &x, &y)) {
    case WALPE_KWIN_PTR_MOTION:
        if (!K.ptr_on || strlen (K.ptr_on) != nlen || strncmp (K.ptr_on, name, nlen)) {
            g_free (K.ptr_on);
            K.ptr_on = g_strndup (name, nlen);
        }
        if (K.ptr_fn && K.ptr_now)
            K.ptr_fn (K.ptr_on, WALPE_KWIN_PTR_MOTION, x, y, 0, 0, K.ptr_user);
        break;
    case WALPE_KWIN_PTR_LEAVE: {
        char *on = g_strndup (name, nlen);
        g_clear_pointer (&K.ptr_on, g_free);
        if (K.ptr_fn && K.ptr_now)
            K.ptr_fn (on, WALPE_KWIN_PTR_LEAVE, 0, 0, 0, 0, K.ptr_user);
        g_free (on);
        break;
    }
    default:
        break;      /* "ready", or nothing this program knows */
    }
}

static void kwin_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer user)
{
    (void) c; (void) iface;
    KScript *s = user;
    /* only KWin's own connection is believed (its scripts call from it),
     * and of its scripts only the one loaded last (ks_object) */
    if (g_strcmp0 (method, s->method) || !K.owner || g_strcmp0 (sender, K.owner) || g_strcmp0 (path, s->path)) {
        g_dbus_method_invocation_return_error_literal (inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                       "only KWin reports here");
        return;
    }
    const char *text = NULL;
    g_variant_get (args, "(&s)", &text);
    if (!s->heard)
        ks_unlink (s);      /* the script runs: KWin has read its file */
    if (s == &K.s[KS_PAUSE])
        kwin_report (s, text);
    else
        kwin_pointer_report (s, text);
    g_dbus_method_invocation_return_value (inv, NULL);
}

static const GDBusInterfaceVTable kwin_vt = { kwin_method, NULL, NULL, { 0 } };

static void ks_object_off (KScript *s)
{
    if (K.bus && s->object)
        g_dbus_connection_unregister_object (K.bus, s->object);
    s->object = 0;
    g_clear_pointer (&s->path, g_free);
}

/* The object the next script reports to, at a path no script before it was
 * given. The script that is replaced (the see-through list changed) runs
 * until KWin has taken it out, and what it reports in that time used to
 * arrive at the one object there was: it counted as the new script's first
 * report, so the new script's file was removed before KWin had read it -
 * KWin then drops the script without a word - and the old script's last
 * answer stood for good, with "loaded" in status (seen on KWin 6.8.2: a
 * window closed in the moment the list changed, two times out of six).
 * Now such a report finds no object. */
static void ks_object (KScript *s)
{
    ks_object_off (s);
    s->path = g_strdup_printf (WALPE_KWIN_OBJECT "/s%u", ++K.script);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml (kwin_xml, NULL);
    s->object = g_dbus_connection_register_object (K.bus, s->path, info->interfaces[0], &kwin_vt, s, NULL,
                                                   NULL);
    g_dbus_node_info_unref (info);
}

/* ---- a script in KWin */

/* one load on its way through KWin's three answers */
typedef struct {
    KScript *s;
    guint chain;
    char *file;
} KLoad;

static void kload_free (KLoad *l)
{
    g_free (l->file);
    g_free (l);
}

/* the answer belongs to the load that is under way (not to one from before
 * the script was taken out or loaded anew, nor to a connection let go) */
static gboolean kload_current (const KLoad *l, const GError *e)
{
    return K.bus && l->s->loading && l->chain == l->s->chain &&
           !(e && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED));
}

static void ks_load (KScript *s);

/* A load has come to its end, whichever. One that was asked for meanwhile
 * (the see-through list changed again) starts now: never two at a time,
 * or the second finds the first one's script in KWin. */
static void ks_load_done (KScript *s)
{
    s->loading = FALSE;
    if (s->reload) {
        s->reload = FALSE;
        ks_load (s);
    }
}

/* KWin has answered "start". It reads the script's file in a thread of its
 * own after that: the file stays until the script has called (kwin_method)
 * or is no longer wanted. */
static void ks_ran (GObject *src, GAsyncResult *res, gpointer user)
{
    KLoad *l = user;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (r)
        g_variant_unref (r);
    if (kload_current (l, e)) {
        if (!r)
            ks_trouble (l->s, NULL, g_strdup_printf ("KWin did not run the %s script "
                                                     "(org.kde.kwin.Scripting.start): %s", l->s->what,
                                                     e->message));
        ks_load_done (l->s);
    }
    g_clear_error (&e);
    kload_free (l);
}

static gboolean ks_load_again (gpointer user);

static void ks_loaded (GObject *src, GAsyncResult *res, gpointer user)
{
    KLoad *l = user;
    KScript *s = l->s;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    gint32 id = -1;
    if (r) {
        g_variant_get (r, "(i)", &id);
        g_variant_unref (r);
    }
    if (!kload_current (l, e)) {
        /* (cancelled: the connection was let go, which has reset everything) */
    } else if (!r) {
        ks_trouble (s, NULL, g_strdup_printf ("KWin did not take the %s script "
                                              "(org.kde.kwin.Scripting.loadScript): %s", s->what,
                                              e->message));
        ks_load_done (s);
    } else if (id < 0 && !s->retried) {
        /* -1: a script of the name is loaded. KWin takes one out a moment
         * after it has answered unloadScript: once more, then give up */
        s->retried = TRUE;
        s->retry = g_timeout_add (300, ks_load_again, s);
    } else if (id < 0) {
        ks_trouble (s, NULL, g_strdup_printf ("KWin did not take the %s script "
                                              "(org.kde.kwin.Scripting.loadScript answered -1)", s->what));
        ks_load_done (s);
    } else {
        s->retried = FALSE;
        /* Not "run" on the script's own object, /Scripting/Script<id>: the id
         * KWin answers is the number of scripts it has loaded, so after another
         * script was unloaded it is the id, and the object, of one that is still
         * there (KWin 6.8), and "run" would reach that one. "start" runs every
         * loaded script that is not running yet. */
        g_dbus_connection_call (K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "start", NULL,
                                NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, K.cancel, ks_ran, l);
        l = NULL;
    }
    g_clear_error (&e);
    if (l)
        kload_free (l);
}

/* KWin has answered unloadScript: now the new one */
static void ks_unloaded (GObject *src, GAsyncResult *res, gpointer user)
{
    KLoad *l = user;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (r)
        g_variant_unref (r);
    if (kload_current (l, e)) {
        g_dbus_connection_call (K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                                g_variant_new ("(ss)", l->file, l->s->name), G_VARIANT_TYPE ("(i)"),
                                G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, K.cancel, ks_loaded, l);
        l = NULL;
    }
    g_clear_error (&e);
    if (l)
        kload_free (l);
}

static gboolean ks_load_again (gpointer user)
{
    KScript *s = user;
    s->retry = 0;
    s->loading = FALSE;
    ks_load (s);
    return G_SOURCE_REMOVE;
}

/* (Re)load: a script of an earlier daemon, or the one made of the old
 * inputs, goes first, and the new one when KWin has answered that. */
static void ks_load (KScript *s)
{
    if (!K.bus || !K.owner || !s->want)
        return;
    if (s->loading) {
        s->reload = TRUE;
        return;
    }
    GError *e = NULL;
    KLoad *l = g_new0 (KLoad, 1);
    l->s = s;
    l->chain = ++s->chain;
    l->file = ks_file (s);
    ks_object (s);
    char *text = ks_text (s);
    gboolean ok = g_file_set_contents (l->file, text, -1, &e);
    g_free (text);
    g_free (s->given);
    s->given = ks_inputs (s);
    ks_forget (s);
    if (!ok) {
        ks_trouble (s, NULL, g_strdup_printf ("the %s script could not be written: %s", s->what, e->message));
        g_clear_error (&e);
        kload_free (l);
        return;
    }
    s->loading = TRUE;
    g_dbus_connection_call (K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
                            g_variant_new ("(s)", s->name), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000,
                            K.cancel, ks_unloaded, l);
}

/* A script goes: out of KWin (unload; not when the connection is dead
 * already or KWin is not there), and with it what it said. An answer to a
 * load that was under way finds another number in `chain` and does nothing
 * more; KWin works through what it was sent in order, so the unloadScript
 * here comes after whatever of that load had gone out. */
static void ks_off (KScript *s, gboolean unload)
{
    if (s->retry)
        g_source_remove (s->retry);
    s->retry = 0;
    s->retried = s->loading = s->reload = FALSE;
    s->chain++;
    if (K.bus && unload && K.owner && s->given && !g_dbus_connection_is_closed (K.bus)) {
        GVariant *r = g_dbus_connection_call_sync (
            K.bus, KWIN_BUS_NAME, "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
            g_variant_new ("(s)", s->name), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
        if (r)
            g_variant_unref (r);
    }
    g_clear_pointer (&s->given, g_free);
    ks_object_off (s);
    ks_forget (s);
    ks_unlink (s);
}

/* what is wanted of a script against what is in KWin */
static void ks_sync (KScript *s)
{
    if (!s->want) {
        if (s->given || s->loading || s->retry)
            ks_off (s, TRUE);
        ks_trouble (s, NULL, NULL);
        return;
    }
    if (!K.owner)
        return;         /* kwin_appeared loads it */
    char *now = ks_inputs (s);
    if (g_strcmp0 (now, s->given))
        ks_load (s);
    g_free (now);
}

static void kwin_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer user)
{
    (void) c; (void) name; (void) user;
    g_free (K.owner);
    K.owner = g_strdup (owner);
    for (int i = 0; i < KS_N; i++) {
        KScript *s = &K.s[i];
        s->retried = FALSE;
        if (s->want) {
            ks_trouble (s, NULL, NULL);
            g_clear_pointer (&s->given, g_free);    /* whatever was in the KWin before this one */
            ks_load (s);
        }
    }
}

static void kwin_vanished (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    g_clear_pointer (&K.owner, g_free);
    for (int i = 0; i < KS_N; i++) {
        KScript *s = &K.s[i];
        if (s->retry)
            g_source_remove (s->retry);
        s->retry = 0;
        s->loading = s->reload = FALSE;
        s->chain++;
        g_clear_pointer (&s->given, g_free);
        ks_forget (s);
        if (K.bus && s->want)
            ks_trouble (s, i == KS_PAUSE
                               ? "KWin is not on the session bus (org.kde.KWin): windows cannot be asked "
                                 "about until it is"
                               : "KWin is not on the session bus (org.kde.KWin): the pointer's position "
                                 "over Plasma's desktop is not known until it is", NULL);
    }
}

/* ---- the connection */

static void kwin_pointer_off (void);

/* everything goes: the connection is closed, or was */
static void kwin_disconnect (gboolean unload)
{
    if (K.cancel) {
        g_cancellable_cancel (K.cancel);
        g_clear_object (&K.cancel);
    }
    K.connecting = FALSE;
    for (int i = 0; i < KS_N; i++)
        ks_off (&K.s[i], unload);
    if (K.watch)
        g_bus_unwatch_name (K.watch);
    K.watch = 0;
    g_clear_pointer (&K.owner, g_free);
    kwin_pointer_off ();
    if (K.bus) {
        g_signal_handlers_disconnect_by_data (K.bus, &K);
        if (!g_dbus_connection_is_closed (K.bus))
            g_dbus_connection_close_sync (K.bus, NULL, NULL);
        g_clear_object (&K.bus);
    }
}

static void kwin_closed (GDBusConnection *c, gboolean remote, GError *err, gpointer user)
{
    (void) c; (void) remote; (void) err; (void) user;
    kwin_disconnect (FALSE);
    ks_trouble (&K.s[KS_PAUSE], "the connection to the session bus ended: windows cannot be asked about",
                NULL);
}

static void kwin_connected (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src; (void) user;
    GError *e = NULL;
    GDBusConnection *bus = g_dbus_connection_new_for_address_finish (res, &e);
    if (!bus) {
        if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            K.connecting = FALSE;
            ks_trouble (&K.s[KS_PAUSE], NULL,
                        g_strdup_printf ("no connection to the session bus: %s", e->message));
        }
        g_clear_error (&e);
        return;
    }
    K.connecting = FALSE;
    K.bus = bus;
    g_dbus_connection_set_exit_on_close (bus, FALSE);
    g_signal_connect (bus, "closed", G_CALLBACK (kwin_closed), &K);
    kwin_arm ();        /* what the connection is wanted for */
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

/* ---- the pointer
 * With the add-on for Plasma (contrib/plasma) Plasma's desktop window lies
 * over the wallpaper and the compositor gives the daemon no pointer there.
 * It comes from two sides, both only while the platform says a helper can
 * exist and a wallpaper uses the pointer (walpe_platform_pointer_helper):
 *
 *   - Where the pointer is: from a script of the daemon's own in KWin (the
 *     pointer script, kwinscript.h), as the pause rules have theirs. It
 *     calls Pointer on its object here while the pointer is over Plasma's
 *     desktop on an output whose wallpaper wants it, at most once in
 *     ptr_ms, and once more when the pointer leaves. Not the add-on's
 *     wallpaper type, which did this first: to see the pointer everywhere
 *     in the desktop window it needs an item above everything in it, and
 *     Qt then gives Plasma's own icons and widgets no hover (seen on
 *     Plasma 6.8.2).
 *   - Buttons and the wheel: from the add-on's wallpaper type, which sees
 *     them in the desktop window without taking them and calls Event here.
 *     How it finds the daemon: by a name on the bus,
 *     io.github.walpe.Pointer, which the daemon owns only while the
 *     pointer is wanted - the type watches for the name and hands nothing
 *     on while nobody has it. Any process of the session can take a name,
 *     so neither side takes one for more than it is:
 *       - the type sends to the connection that has the name, by that
 *         connection's own name, and only if the bus says it is the process
 *         whose surface KWin sees under the desktop (Script.qml): somebody
 *         else with the name gets nothing, and the daemon, not getting the
 *         name, says so in status;
 *       - the daemon believes the connection that owns org.kde.plasmashell
 *         at the time (as the scripts' reports are believed from the one
 *         that owns org.kde.KWin). While Plasma's shell is not on the bus,
 *         the first process to take its name is believed in its place: it
 *         can move this pointer and press buttons 1 to 3 in a wallpaper
 *         that uses the pointer. The session bus has nothing to tell the
 *         two apart by; such a process can do more than that to the
 *         session in other ways. */
#define PTR_NAME "io.github.walpe.Pointer"
#define PTR_PATH "/io/github/walpe/Pointer"

static const char ptr_xml[] =
    "<node><interface name='" PTR_NAME "'>"
    "<method name='Event'><arg type='s' name='output' direction='in'/>"
    "<arg type='u' name='what' direction='in'/><arg type='d' name='x' direction='in'/>"
    "<arg type='d' name='y' direction='in'/><arg type='d' name='a' direction='in'/>"
    "<arg type='d' name='b' direction='in'/></method>"
    "</interface></node>";

static void ptr_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer user)
{
    (void) c; (void) path; (void) iface; (void) user;
    if (g_strcmp0 (method, "Event") || !K.ptr_shell || g_strcmp0 (sender, K.ptr_shell)) {
        g_dbus_method_invocation_return_error_literal (inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                       "only Plasma's shell reports here");
        return;
    }
    const char *output = NULL;
    guint32 what = 0;
    double x = 0, y = 0, a = 0, b = 0;
    g_variant_get (args, "(&sudddd)", &output, &what, &x, &y, &a, &b);
    if (K.ptr_fn && K.ptr_now)
        K.ptr_fn (output, what, x, y, a, b, K.ptr_user);
    g_dbus_method_invocation_return_value (inv, NULL);
}

static const GDBusInterfaceVTable ptr_vt = { ptr_method, NULL, NULL, { 0 } };

static void ptr_shell_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer user)
{
    (void) c; (void) name; (void) user;
    g_free (K.ptr_shell);
    K.ptr_shell = g_strdup (owner);
}

static void ptr_shell_vanished (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    g_clear_pointer (&K.ptr_shell, g_free);
}

static void ptr_name_acquired (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    K.ptr_named = TRUE;
}

static void ptr_name_lost (GDBusConnection *c, const char *name, gpointer user)
{
    (void) c; (void) name; (void) user;
    K.ptr_named = FALSE;        /* another walpe-daemon has it, or the connection ended */
}

static void kwin_pointer_on (void)
{
    if (!K.bus || K.ptr_own)
        return;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml (ptr_xml, NULL);
    K.ptr_object = g_dbus_connection_register_object (K.bus, PTR_PATH, info->interfaces[0], &ptr_vt, NULL,
                                                      NULL, NULL);
    g_dbus_node_info_unref (info);
    K.ptr_watch = g_bus_watch_name_on_connection (K.bus, "org.kde.plasmashell", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  ptr_shell_appeared, ptr_shell_vanished, NULL, NULL);
    K.ptr_own = g_bus_own_name_on_connection (K.bus, PTR_NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                              ptr_name_acquired, ptr_name_lost, NULL, NULL);
}

static void kwin_pointer_off (void)
{
    if (K.ptr_own)
        g_bus_unown_name (K.ptr_own);
    if (K.ptr_watch)
        g_bus_unwatch_name (K.ptr_watch);
    if (K.bus && K.ptr_object)
        g_dbus_connection_unregister_object (K.bus, K.ptr_object);
    K.ptr_own = K.ptr_watch = K.ptr_object = 0;
    K.ptr_named = FALSE;
    g_clear_pointer (&K.ptr_shell, g_free);
}

/* The connection exists while a pause rule or the pointer wants it, and
 * carries only what is wanted. */
static void kwin_arm (void)
{
    gboolean rules = K.started && K.rules != 0, pointer = K.started && K.ptr_want;
    K.s[KS_PAUSE].want = rules;
    K.s[KS_POINTER].want = pointer;
    if (!rules && !pointer) {
        if (K.bus || K.connecting)
            kwin_disconnect (TRUE);
        for (int i = 0; i < KS_N; i++)
            ks_trouble (&K.s[i], NULL, NULL);
        return;
    }
    if (!K.bus) {
        if (K.connecting)
            return;
        char *addr = kwin_bus_address ();
        if (!addr) {
            ks_trouble (&K.s[KS_PAUSE], "no session bus (DBUS_SESSION_BUS_ADDRESS is not set and the "
                        "runtime directory has no bus socket): KWin cannot be asked about its windows", NULL);
            return;
        }
        K.connecting = TRUE;
        K.cancel = g_cancellable_new ();
        g_dbus_connection_new_for_address (
            addr,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, K.cancel, kwin_connected, NULL);
        g_free (addr);
        return;         /* kwin_connected comes back here */
    }
    /* a script that is not wanted goes before the watch can bring it back */
    for (int i = 0; i < KS_N; i++)
        ks_sync (&K.s[i]);
    if (!K.watch)
        K.watch = g_bus_watch_name_on_connection (K.bus, KWIN_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  kwin_appeared, kwin_vanished, NULL, NULL);
    if (pointer)
        kwin_pointer_on ();
    else
        kwin_pointer_off ();
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

/* how long the name and the pointer script outlive the last wallpaper that
 * wanted the pointer */
#define PTR_LINGER_MS 500

static gboolean kwin_pointer_lingered (gpointer user)
{
    (void) user;
    K.ptr_linger = 0;
    K.ptr_want = FALSE;
    g_clear_pointer (&K.ptr_outputs, g_free);
    if (K.started)
        kwin_arm ();
    return G_SOURCE_REMOVE;
}

/* "Wanted" comes and goes with every pause of a wallpaper that uses the
 * pointer, and a pause can last a few hundredths of a second (a compositor
 * that is late with a frame under load: seen with KWin on a software
 * renderer, three times in forty seconds). Taking the script out of KWin
 * and putting it back for each of those would be work for KWin for
 * nothing: what is kept stays for PTR_LINGER_MS after the last want, and a
 * want that returns in that time with the same outputs finds everything as
 * it was. Events stop being handed on at once. */
void walpe_sig_kwin_set_pointer (gboolean want, const char *outputs, guint ms, WalpeSigPointerFn fn,
                                 gpointer user)
{
    K.ptr_fn = fn;
    K.ptr_user = user;
    K.ptr_now = !!want;
    if (!want) {
        if (K.ptr_want && !K.ptr_linger)
            K.ptr_linger = g_timeout_add (PTR_LINGER_MS, kwin_pointer_lingered, NULL);
        return;
    }
    if (K.ptr_linger)
        g_source_remove (K.ptr_linger);
    K.ptr_linger = 0;
    ms = CLAMP (ms, 4, 1000);
    if (K.ptr_want && ms == K.ptr_ms && !g_strcmp0 (outputs, K.ptr_outputs))
        return;
    K.ptr_want = TRUE;
    K.ptr_ms = ms;
    g_free (K.ptr_outputs);
    K.ptr_outputs = g_strdup (outputs);
    if (K.started)
        kwin_arm ();
}

const char *walpe_sig_kwin_pointer_state (void)
{
    static char *text;
    if (!K.started || !K.ptr_want || !K.ptr_now)
        return NULL;
    if (!K.bus)
        return "wanted from the Plasma add-on, but there is no connection to the session bus";
    const KScript *s = &K.s[KS_POINTER];
    g_free (text);
    text = g_strdup_printf (
        "%s; its position: %s",
        K.ptr_named ? "taken from the Plasma add-on where a desktop has the wallpaper type WalPE "
                      "(" PTR_NAME " on the session bus)"
                    : "wanted from the Plasma add-on; the name " PTR_NAME " on the session bus is "
                      "not ours (yet)",
        s->trouble ? s->trouble : s->heard ? "from the pointer script in KWin" : "the pointer script is being "
                                                                                 "loaded into KWin");
    return text;
}

void walpe_sig_kwin_use_address (const char *address)
{
    g_free (K.address);
    K.address = g_strdup (address);
}

const char *walpe_sig_kwin_state (void)
{
    const KScript *s = &K.s[KS_PAUSE];
    if (!K.started)
        return NULL;
    if (!K.rules)
        return "off: none of the pause rules it serves is switched on";
    if (s->trouble)
        return s->trouble;
    return s->heard ? "the pause script is loaded in KWin" : "loading the pause script into KWin";
}

const char *walpe_sig_kwin_trouble (void)
{
    return K.started && K.rules ? K.s[KS_PAUSE].trouble : NULL;
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
    if (K.ptr_linger)
        g_source_remove (K.ptr_linger);
    K.ptr_linger = 0;
    K.ptr_want = K.ptr_now = FALSE;
    g_clear_pointer (&K.ptr_outputs, g_free);
    g_clear_pointer (&K.ids, g_hash_table_unref);
    for (int i = 0; i < KS_N; i++) {
        K.s[i].want = FALSE;
        g_clear_pointer (&K.s[i].trouble_own, g_free);
        K.s[i].trouble = NULL;
    }
}

/* the see-through list changed: the script carries it */
static void kwin_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) now;
    if (sig == WALPE_SIG_COVERED && K.bus && K.started)
        ks_sync (&K.s[KS_PAUSE]);
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
