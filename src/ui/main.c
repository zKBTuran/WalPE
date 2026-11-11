/* walpe - GTK4 wallpaper browser.
 *
 * Layout: header bar (import, workshop, pause, engine settings) | library
 * (search, type filter, recycled GtkGridView of exact-size async
 * thumbnails) | selection pane (capped preview, title, badges, "Set as
 * wallpaper", Wallpaper options) | status bar (engine state, restart).
 *
 * Main-thread rule: no socket I/O, no image decoding, no directory scans
 * and no file copies run on the GTK main thread. Daemon IPC goes through
 * uiipc (worker threads, hard deadlines), thumbnails through thumbs (pool +
 * disk cache), library scans and copies through GTask threads. */
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store.h"
#include "numfield.h"
#include "delete.h"
#include "props.h"
#include "layers.h"   /* "Adjust layers" section under the options */
#include "perf.h"
#include "resdialog.h"   /* render resolution per monitor: dialog, settings rows, announcement */
#include "thumbs.h"
#include "uiipc.h"
#include "uistats.h"
#include "appearance.h"  /* dark / light from the desktop portal (Wayland sessions) */
#include "monlist.h"     /* the outputs as GDK names them, while no engine runs */
#ifdef WALPE_HAVE_WORKSHOP_PAGE
#include "workshop_page.h"   /* GtkWidget *walpe_workshop_page_new (void); */
#endif
#include "common/config.h"
#include "common/ipcsub.h"
#include "common/util.h"
#include "common/uitext.h"
#include "common/workshop.h"
#include "tray/trayconf.h"

#define TILE_W 160
#define TILE_H 90
#define PREVIEW_W 720
#define PREVIEW_H 405

/* ================================================== capped-height bin
 * A one-child container whose height request never exceeds max_height:
 * the preview takes at most ~35 % of the window, whatever the image. */

#define WALPE_TYPE_CAP_BOX (walpe_cap_box_get_type ())
G_DECLARE_FINAL_TYPE (WalpeCapBox, walpe_cap_box, WALPE, CAP_BOX, GtkWidget)

struct _WalpeCapBox {
    GtkWidget parent;
    GtkWidget *child;
    int max_height;
};

G_DEFINE_TYPE (WalpeCapBox, walpe_cap_box, GTK_TYPE_WIDGET)

static void cap_measure (GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat,
                         int *min_b, int *nat_b)
{
    WalpeCapBox *c = WALPE_CAP_BOX (w);
    *min = *nat = 0;
    *min_b = *nat_b = -1;
    if (!c->child || !gtk_widget_get_visible (c->child))
        return;
    gtk_widget_measure (c->child, o, for_size, min, nat, NULL, NULL);
    if (o == GTK_ORIENTATION_VERTICAL && c->max_height > 0) {
        *min = MIN (*min, c->max_height);
        *nat = MIN (*nat, c->max_height);
    }
}

static void cap_allocate (GtkWidget *w, int width, int height, int baseline)
{
    WalpeCapBox *c = WALPE_CAP_BOX (w);
    if (c->child && gtk_widget_get_visible (c->child))
        gtk_widget_allocate (c->child, width, height, baseline, NULL);
}

static void cap_dispose (GObject *o)
{
    WalpeCapBox *c = WALPE_CAP_BOX (o);
    g_clear_pointer (&c->child, gtk_widget_unparent);
    G_OBJECT_CLASS (walpe_cap_box_parent_class)->dispose (o);
}

static void walpe_cap_box_class_init (WalpeCapBoxClass *k)
{
    G_OBJECT_CLASS (k)->dispose = cap_dispose;
    GTK_WIDGET_CLASS (k)->measure = cap_measure;
    GTK_WIDGET_CLASS (k)->size_allocate = cap_allocate;
}

static void walpe_cap_box_init (WalpeCapBox *c) { c->max_height = 300; }

static GtkWidget *cap_box_new (GtkWidget *child)
{
    WalpeCapBox *c = g_object_new (WALPE_TYPE_CAP_BOX, NULL);
    c->child = child;
    gtk_widget_set_parent (child, GTK_WIDGET (c));
    return GTK_WIDGET (c);
}

static void cap_box_set_max (GtkWidget *w, int h)
{
    WalpeCapBox *c = WALPE_CAP_BOX (w);
    if (c->max_height != h) {
        c->max_height = h;
        gtk_widget_queue_resize (w);
    }
}

/* ================================================================ state */

typedef enum { D_UNKNOWN, D_UP, D_STARTING, D_BUSY, D_DOWN, D_GAVE_UP } DState;

/* per-monitor state ("monitors":[{index,name,geometry,current,paused}]);
 * daemons before per-monitor support report a bare count instead */
typedef struct {
    int index;
    char *name;
    int x, y, w, h;
    char *current;
    gboolean paused;
} MonInfo;

static void mon_info_free (gpointer p)
{
    MonInfo *m = p;
    g_free (m->name);
    g_free (m->current);
    g_free (m);
}

static struct {
    GtkApplication *app;
    GtkWidget *win;
    GtkWidget *paned;
    /* library */
    GListStore *model;
    GtkFilterListModel *filtered;
    GtkCustomFilter *filter;
    GtkSingleSelection *sel;
    GtkWidget *grid;
    GtkWidget *search;
    GtkWidget *type_dd;
    GtkWidget *count;
    char *search_text;
    GHashTable *bound;          /* GtkListItem* currently bound (badges) */
    guint scan_gen;
    guint rescan_id;
    gboolean first_scan_done;
    /* selection */
    GtkWidget *capbox;
    GtkPicture *preview;
    GtkWidget *preview_ph;
    GtkLabel *title;
    GtkWidget *type_badge, *active_badge, *steam_badge, *nopt_badge;
    GtkLabel *idlabel;
    GtkLabel *desc;
    GtkWidget *desc_more;
    char *desc_full;            /* description with line breaks */
    char *desc_one;             /* same, one paragraph (3-line clamp) */
    gboolean desc_open;
    GtkWidget *apply;
    GtkWidget *delete_btn;      /* "Delete…" next to the title */
    GtkWidget *tile_menu;       /* context menu of the grid tiles */
    GtkLabel *mon_label;
    GtkWidget *mon_box;         /* selector + diagram (per-monitor daemons only) */
    GtkWidget *mon_dd;
    GtkWidget *mon_area;
    GtkLabel *mon_now;          /* "Showing 1 · HDMI-0: …   2 · DP-2: …" */
    GtkWidget *stack;           /* Library | Workshop (when built) */
    GPtrArray *mons;            /* MonInfo, empty = no per-monitor support */
    char *mon_sig;              /* last rebuilt layout+currents (avoid churn) */
    gboolean mon_building;
    GtkWidget *props_panel;
    GtkWidget *layers_panel;    /* "Adjust layers" (layers.h) */
    gboolean layer_adjust;      /* the daemon knows "layers" / "layeradj" */
    GtkWidget *perf_panel;   /* quality settings of the shown wallpaper (perf.c) */
    guint props_idle;
    WalpeEntry *shown;          /* entry the details show (ref) */
    guint apply_guard;
    char *pending_apply;        /* re-sent once the engine is back up */
    /* header */
    GtkWidget *pause_btn;
    gboolean pause_building;
    /* status bar */
    GtkWidget *dot;
    GtkLabel *engine_lbl;
    GtkLabel *status;
    GtkWidget *restart_btn;
    guint status_clear_id;
    char *load_err;             /* the engine's last word on a wallpaper that did not load */
    gboolean status_load_err;   /* ... and the status line shows it */
    /* daemon state (from status polls) */
    DState dstate;
    char *current;
    int monitors;
    gboolean paused;
    gboolean by_path;
    JsonObject *last_state;     /* last "state" object (owned) */
    /* settings window */
    GtkWidget *settings;
    WalpeConfig *cfg;
    GFileMonitor *dirmon;
    gboolean closing;           /* the main window was destroyed (on_win_destroy) */
    gboolean want_res_dialog;   /* started with --resolution: the dialog opens with the window */
} U;

/* supervisor of the engine process */
static struct {
    guint poll_id;
    guint poll_secs;            /* interval of poll_id, 0 = no timer */
    WalpeIpcSub *sub;           /* state pushes from the daemon */
    gboolean poll_inflight;
    int poll_fails;             /* consecutive timeouts of a live daemon */
    GPid child;                 /* our spawned daemon, 0 = none/foreign */
    guint child_watch;
    gint64 spawn_us;
    gint64 up_since_us;
    int quick_fails;
    guint retry_id;
    gboolean auto_restart;      /* respawn when it dies (off after 3 quick fails / clean quit) */
    char *why;                  /* last failure reason */
} Sup = { .auto_restart = TRUE };

static void set_status (const char *msg, gboolean error);
static gboolean parse_monitors (JsonObject *st);
static void update_details (WalpeEntry *e);
static void refresh_badges (void);
static void sup_poll_now (void);
static void sup_poll_update (void);
static void sup_spawn (void);
static void sup_set_state (DState st, const char *msg);

/* ================================================================ utils */

static gboolean same_path (const char *a, const char *b)
{
    if (!a || !b || !*a || !*b)
        return FALSE;
    if (!strcmp (a, b))
        return TRUE;
    char *ca = g_canonicalize_filename (a, NULL), *cb = g_canonicalize_filename (b, NULL);
    gboolean r = !g_strcmp0 (ca, cb);
    g_free (ca);
    g_free (cb);
    return r;
}

/* path shows on some monitor (any monitor for per-monitor daemons) */
static gboolean is_active (const char *path)
{
    if (!path)
        return FALSE;
    if (U.mons && U.mons->len) {
        for (guint i = 0; i < U.mons->len; i++)
            if (same_path (((MonInfo *) U.mons->pdata[i])->current, path))
                return TRUE;
        return FALSE;
    }
    return same_path (U.current, path);
}

/* path shows on the monitor(s) the picker targets (button label) */
static gboolean is_on_target (const char *path)
{
    if (!path)
        return FALSE;
    if (!U.mons || !U.mons->len || !U.mon_dd)
        return same_path (U.current, path);
    guint sel = gtk_widget_get_visible (U.mon_box)
                    ? gtk_drop_down_get_selected (GTK_DROP_DOWN (U.mon_dd)) : 0;
    for (guint i = 0; i < U.mons->len; i++) {
        MonInfo *m = U.mons->pdata[i];
        if ((sel == 0 || sel == i + 1) && !same_path (m->current, path))
            return FALSE;
    }
    return TRUE;
}

static const char *type_label (const char *t)
{
    if (!g_strcmp0 (t, "scene")) return "Scene";
    if (!g_strcmp0 (t, "video")) return "Video";
    if (!g_strcmp0 (t, "web")) return "Web";
    if (!g_strcmp0 (t, "gif")) return "GIF";
    if (!g_strcmp0 (t, "image")) return "Image";
    if (!g_strcmp0 (t, "application")) return "App";
    return "Unsupported";
}

static void set_badge (GtkWidget *b, const char *type)
{
    static const char *const cls[] = { "scene", "video", "web", "gif", "image", "unsupported", NULL };
    for (int i = 0; cls[i]; i++)
        gtk_widget_remove_css_class (b, cls[i]);
    const char *c = "unsupported";
    for (int i = 0; cls[i]; i++)
        if (!g_strcmp0 (type, cls[i]))
            c = cls[i];
    gtk_widget_add_css_class (b, c);
    gtk_label_set_text (GTK_LABEL (b), type_label (type));
}

static GtkWidget *badge_new (const char *cls, const char *text)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_widget_add_css_class (l, "walpe-badge");
    if (cls)
        gtk_widget_add_css_class (l, cls);
    gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
    return l;
}

static WalpeEntry *selected_entry (void)
{
    return (WalpeEntry *) gtk_single_selection_get_selected_item (U.sel);
}

static guint count_options (WalpeEntry *e)
{
    return e ? e->n_options : 0;    /* counted by the scan (store.c) */
}

/* ============================================================ status bar */

static gboolean status_clear (gpointer u)
{
    (void) u;
    U.status_clear_id = 0;
    gtk_label_set_text (U.status, "");
    gtk_widget_remove_css_class (GTK_WIDGET (U.status), "error");
    return G_SOURCE_REMOVE;
}

static void set_status (const char *msg, gboolean error)
{
    if (!U.status)
        return;
    U.status_load_err = FALSE;   /* (on_status_reply sets it for its own message) */
    /* one line of bounded length, whatever a wallpaper, steamcmd or the
     * engine put into the message (uitext.h) */
    char *line = walpe_ui_text (msg, 400, WALPE_UITEXT_LINE);
    gtk_label_set_text (U.status, line);
    gtk_widget_set_tooltip_text (GTK_WIDGET (U.status), *line ? line : NULL);
    g_free (line);
    if (error)
        gtk_widget_add_css_class (GTK_WIDGET (U.status), "error");
    else
        gtk_widget_remove_css_class (GTK_WIDGET (U.status), "error");
    if (U.status_clear_id)
        g_source_remove (U.status_clear_id);
    /* info fades after a while; errors stay until replaced */
    U.status_clear_id = error ? 0 : g_timeout_add_seconds (8, status_clear, NULL);
}

static void props_status (const char *msg, gboolean error) { set_status (msg, error); }

/* ======================================================== engine supervisor
 * Follows the daemon's state on its own IPC lane. The state is pushed (a
 * subscribed connection, common/ipcsub.h): while that is live the GUI
 * does not poll at all when its window is not the active one, and asks
 * once every 10 s when it is (a daemon that hangs is still noticed).
 * Without pushes (daemon down, starting, or an older build) it polls
 * every 2 s as before - an older daemon only while the window is active. A dead daemon is
 * respawned with backoff (1, 3, 8 s); three deaths within 30 s of a start
 * stop the automatic restarts and say why. A clean exit (walpe-ctl quit)
 * is respected. "Restart engine" always works. Nothing here blocks. */

static char *daemon_log_path (void)
{
    return g_build_filename (g_get_user_cache_dir (), "walpe", "daemon.log", NULL);
}

static void sup_set_state (DState st, const char *msg)
{
    if (U.closing)
        return;
    U.dstate = st;
    static const char *const cls[] = { "up", "busy", "down", NULL };
    for (int i = 0; cls[i]; i++)
        gtk_widget_remove_css_class (U.dot, cls[i]);
    const char *c = st == D_UP ? "up" : (st == D_STARTING || st == D_BUSY) ? "busy" : "down";
    gtk_widget_add_css_class (U.dot, c);
    char *txt;
    if (st == D_UP) {
        const char *ty = U.last_state ? wpj_str (U.last_state, "type", "none") : "none";
        txt = g_strdup_printf ("Engine running%s · %d monitor%s%s", U.paused ? " (paused)" : "",
                               U.monitors, U.monitors == 1 ? "" : "s",
                               strcmp (ty, "none") ? "" : " · no wallpaper");
    } else if (st == D_STARTING) {
        txt = g_strdup ("Starting engine…");
    } else if (st == D_BUSY) {
        txt = g_strdup ("Engine not responding");
    } else if (st == D_GAVE_UP) {
        txt = g_strdup ("Engine keeps crashing");
    } else if (st == D_DOWN) {
        txt = g_strdup ("Engine stopped");
    } else {
        txt = g_strdup ("Connecting…");
    }
    gtk_label_set_text (U.engine_lbl, txt);
    g_free (txt);
    {
        /* no engine, so no monitor list from it: say at least which
         * outputs the toolkit sees, by the names the engine will use
         * (connector names on X11 and Wayland alike) */
        char *tip = st == D_UP ? NULL : walpe_ui_monitors_line (gdk_display_get_default ());
        gtk_widget_set_tooltip_text (GTK_WIDGET (U.engine_lbl), tip);
        g_free (tip);
    }
    gtk_widget_set_visible (U.restart_btn, st == D_DOWN || st == D_GAVE_UP || st == D_BUSY);
    gtk_button_set_label (GTK_BUTTON (U.restart_btn), st == D_BUSY ? "Restart engine" : "Start engine");
    if (msg)
        set_status (msg, st != D_UP && st != D_STARTING);
    gtk_widget_set_sensitive (U.pause_btn, st == D_UP);
    sup_poll_update ();
}

static gboolean sup_retry (gpointer u)
{
    (void) u;
    Sup.retry_id = 0;
    sup_spawn ();
    return G_SOURCE_REMOVE;
}

static void sup_schedule_respawn (void)
{
    if (Sup.retry_id || !Sup.auto_restart || Sup.child)
        return;
    static const guint delays[] = { 1, 3, 8 };
    guint d = delays[MIN (Sup.quick_fails, 2)];
    Sup.retry_id = g_timeout_add_seconds (d, sup_retry, NULL);
}

static void sup_give_up (void)
{
    Sup.auto_restart = FALSE;
    char *log = daemon_log_path ();
    char *base = U.current && *U.current ? g_path_get_basename (U.current) : g_strdup ("none");
    char *m = g_strdup_printf ("The engine stopped %d times right after starting (%s; it was "
                               "loading %s). Automatic restart is off: pick another wallpaper "
                               "or press Start engine. Log: %s",
                               Sup.quick_fails, Sup.why ? Sup.why : "no reason given", base, log);
    sup_set_state (D_GAVE_UP, m);
    g_free (m);
    g_free (base);
    g_free (log);
}

static void sup_child_exit (GPid pid, gint wstatus, gpointer u)
{
    (void) u;
    g_spawn_close_pid (pid);
    if (pid != Sup.child)
        return;
    Sup.child = 0;
    Sup.child_watch = 0;
    gint64 lived = g_get_monotonic_time () - Sup.spawn_us;
    g_free (Sup.why);
    if (WIFSIGNALED (wstatus))
        Sup.why = g_strdup_printf ("killed by signal %d (%s)", WTERMSIG (wstatus),
                                   g_strsignal (WTERMSIG (wstatus)));
    else
        Sup.why = g_strdup_printf ("exit status %d", WIFEXITED (wstatus) ? WEXITSTATUS (wstatus) : -1);
    gboolean clean = WIFEXITED (wstatus) && WEXITSTATUS (wstatus) == 0;
    if (clean && lived > 3 * G_USEC_PER_SEC) {
        /* asked to quit (walpe-ctl quit): no automatic restart */
        Sup.auto_restart = FALSE;
        sup_set_state (D_DOWN, "The engine was stopped. Press Start engine to run it again.");
        return;
    }
    if (clean) {
        /* exits 0 at once: another daemon owns the socket, or no display */
        sup_poll_now ();
        return;
    }
    if (lived < 30 * G_USEC_PER_SEC)
        Sup.quick_fails++;
    if (Sup.quick_fails >= 3) {
        sup_give_up ();
        return;
    }
    char *m = g_strdup_printf ("The engine stopped (%s). Restarting it…", Sup.why);
    sup_set_state (D_DOWN, m);
    g_free (m);
    sup_schedule_respawn ();
}

static void sup_child_setup (gpointer u)
{
    (void) u;
    setsid ();   /* own session: closing the GUI's terminal does not kill it */
}

static void sup_spawn (void)
{
    if (Sup.child)
        return;
    if (Sup.retry_id) {
        g_source_remove (Sup.retry_id);
        Sup.retry_id = 0;
    }
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname (exe) : NULL;
    char *path = dir ? g_build_filename (dir, "walpe-daemon", NULL) : NULL;
    if (!path || !g_file_test (path, G_FILE_TEST_IS_EXECUTABLE)) {
        g_free (path);
        path = g_strdup ("walpe-daemon");   /* installed: from PATH */
    }
    char *log = daemon_log_path ();
    char *ldir = g_path_get_dirname (log);
    g_mkdir_with_parents (ldir, 0700);
    struct stat st;
    if (stat (log, &st) == 0 && st.st_size > (8 << 20)) {
        char *old = g_strconcat (log, ".old", NULL);
        rename (log, old);
        chmod (old, 0600);
        g_free (old);
    }
    /* the engine's log names wallpaper paths and page output: the user's
     * own business (it was world-readable) */
    int fd = open (log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0)
        fchmod (fd, 0600);
    char *argv[] = { path, NULL };
    /* the GUI's own renderer choice must not leak into the daemon (WebKit) */
    char **envp = g_get_environ ();
    if (g_getenv ("WALPE_UI_SET_GSK"))
        envp = g_environ_unsetenv (envp, "GSK_RENDERER");
    envp = g_environ_unsetenv (envp, "WALPE_UI_SET_GSK");
    if (g_getenv ("WALPE_UI_SET_GDK")) {
        envp = g_environ_unsetenv (envp, "GDK_DISABLE");
        envp = g_environ_unsetenv (envp, "GDK_DEBUG");
    }
    envp = g_environ_unsetenv (envp, "WALPE_UI_SET_GDK");
    envp = g_environ_unsetenv (envp, "WALPE_UI_STATS");
    GError *err = NULL;
    GPid pid = 0;
    gboolean ok = g_spawn_async_with_pipes_and_fds (
        NULL, (const char *const *) argv, (const char *const *) envp,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDIN_FROM_DEV_NULL,
        sup_child_setup, NULL, -1, fd, fd, NULL, NULL, 0, &pid, NULL, NULL, NULL, &err);
    if (fd >= 0)
        close (fd);
    g_strfreev (envp);
    if (!ok) {
        char *m = g_strdup_printf ("Cannot start walpe-daemon: %s", err ? err->message : "?");
        sup_set_state (D_DOWN, m);
        g_free (m);
        g_clear_error (&err);
    } else {
        Sup.child = pid;
        Sup.spawn_us = g_get_monotonic_time ();
        Sup.child_watch = g_child_watch_add (pid, sup_child_exit, NULL);
        sup_set_state (D_STARTING, NULL);
        set_status ("Starting the engine…", FALSE);
        sup_poll_now ();
    }
    g_free (log);
    g_free (ldir);
    g_free (path);
    g_free (dir);
    g_free (exe);
}

/* an action found no daemon: (re)start it unless the user turned that off */
static void sup_on_down (void)
{
    if (Sup.child || Sup.retry_id)
        return;
    if (U.dstate == D_GAVE_UP)
        return;
    Sup.auto_restart = TRUE;
    sup_spawn ();
}

static void on_restart (GtkButton *b, gpointer u)
{
    (void) b; (void) u;
    Sup.quick_fails = 0;
    Sup.auto_restart = TRUE;
    if (U.dstate == D_BUSY && Sup.child) {
        /* our child hangs: end it; the exit handler respawns it */
        kill (Sup.child, SIGTERM);
        set_status ("Restarting the engine…", FALSE);
        return;
    }
    if (U.dstate == D_BUSY) {
        ui_ipc_send ("{\"cmd\":\"quit\"}", 0, NULL, NULL);
        set_status ("Asked the engine to quit; it is restarted when it has exited.", FALSE);
        return;
    }
    sup_spawn ();
}

static void apply_send (const char *path);

static const char *title_for_path (const char *path)
{
    if (!path || !*path)
        return "nothing";
    GListModel *m = G_LIST_MODEL (U.model);
    guint n = g_list_model_get_n_items (m);
    for (guint i = 0; i < n; i++) {
        WalpeEntry *e = g_list_model_get_item (m, i);
        gboolean hit = same_path (e->path, path);
        const char *t = e->title;
        g_object_unref (e);   /* the store keeps it alive */
        if (hit)
            return t;
    }
    const char *b = strrchr (path, '/');
    return b ? b + 1 : path;
}

static void mon_area_draw (GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    (void) a; (void) u;
    if (!U.mons || !U.mons->len)
        return;
    int x0 = G_MAXINT, y0 = G_MAXINT, x1 = G_MININT, y1 = G_MININT;
    for (guint i = 0; i < U.mons->len; i++) {
        MonInfo *m = U.mons->pdata[i];
        x0 = MIN (x0, m->x); y0 = MIN (y0, m->y);
        x1 = MAX (x1, m->x + m->w); y1 = MAX (y1, m->y + m->h);
    }
    if (x1 <= x0 || y1 <= y0)
        return;
    double s = MIN ((w - 4.0) / (x1 - x0), (h - 4.0) / (y1 - y0));
    double ox = (w - (x1 - x0) * s) / 2, oy = (h - (y1 - y0) * s) / 2;
    guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (U.mon_dd));
    GdkRGBA fg;
    gtk_widget_get_color (U.mon_area, &fg);
    for (guint i = 0; i < U.mons->len; i++) {
        MonInfo *m = U.mons->pdata[i];
        double rx = ox + (m->x - x0) * s + 1, ry = oy + (m->y - y0) * s + 1;
        double rw = m->w * s - 2, rh = m->h * s - 2;
        gboolean on = sel == 0 || sel == i + 1;
        cairo_rectangle (cr, rx, ry, rw, rh);
        if (on)
            cairo_set_source_rgba (cr, 0.21, 0.52, 0.89, 0.85);
        else
            cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.12);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.6);
        cairo_set_line_width (cr, 1);
        cairo_stroke (cr);
        char num[8];
        g_snprintf (num, sizeof (num), "%d", m->index + 1);
        cairo_set_font_size (cr, MIN (rh * 0.6, 13));
        cairo_text_extents_t te;
        cairo_text_extents (cr, num, &te);
        cairo_move_to (cr, rx + (rw - te.width) / 2 - te.x_bearing, ry + (rh - te.height) / 2 - te.y_bearing);
        if (on)
            cairo_set_source_rgb (cr, 1, 1, 1);
        else
            cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.9);
        cairo_show_text (cr, num);
    }
}

static void update_apply_button (void);

static void on_mon_selected (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) o; (void) ps; (void) u;
    if (U.mon_area)
        gtk_widget_queue_draw (U.mon_area);
    if (!U.mon_building)
        update_apply_button ();
}

/* status "monitors": array (per-monitor daemon) or a count (older) */
static gboolean parse_monitors (JsonObject *st)
{
    JsonNode *mn = json_object_get_member (st, "monitors");
    GPtrArray *nm = g_ptr_array_new_with_free_func (mon_info_free);
    if (mn && JSON_NODE_HOLDS_ARRAY (mn)) {
        JsonArray *a = json_node_get_array (mn);
        for (guint i = 0; i < json_array_get_length (a); i++) {
            JsonObject *o = wpj_arr_obj (a, i);
            if (!o)
                continue;
            MonInfo *m = g_new0 (MonInfo, 1);
            m->index = (int) wpj_num (o, "index", i);
            m->name = g_strdup (wpj_str (o, "name", ""));
            JsonArray *g = wpj_arr (o, "geometry");
            if (wpj_arr_len (g) >= 4) {
                m->x = (int) json_array_get_double_element (g, 0);
                m->y = (int) json_array_get_double_element (g, 1);
                m->w = (int) json_array_get_double_element (g, 2);
                m->h = (int) json_array_get_double_element (g, 3);
            }
            m->current = g_strdup (wpj_str (o, "current", ""));
            m->paused = wpj_bool (o, "paused", FALSE);
            g_ptr_array_add (nm, m);
        }
        U.monitors = (int) nm->len;
    } else {
        U.monitors = (int) wpj_num (st, "monitors", 1);   /* daemons before 11.1 */
    }
    if (json_object_has_member (st, "monitor_count"))
        U.monitors = (int) wpj_num (st, "monitor_count", U.monitors);
    GString *sig = g_string_new (NULL);
    for (guint i = 0; i < nm->len; i++) {
        MonInfo *m = nm->pdata[i];
        g_string_append_printf (sig, "%d|%s|%d,%d,%d,%d|%s;", m->index, m->name, m->x, m->y,
                                m->w, m->h, m->current);
    }
    gboolean changed = g_strcmp0 (sig->str, U.mon_sig) != 0;
    g_ptr_array_unref (U.mons);
    U.mons = nm;
    if (changed) {
        g_free (U.mon_sig);
        U.mon_sig = g_string_free (sig, FALSE);
        guint keep = gtk_drop_down_get_selected (GTK_DROP_DOWN (U.mon_dd));
        GtkStringList *sl = gtk_string_list_new (NULL);
        gtk_string_list_append (sl, "All monitors");
        GString *tip = g_string_new ("Currently showing:");
        for (guint i = 0; i < nm->len; i++) {
            MonInfo *m = nm->pdata[i];
            char *l = g_strdup_printf ("%d · %s · %d×%d", m->index + 1, *m->name ? m->name : "Monitor",
                                       m->w, m->h);
            gtk_string_list_append (sl, l);
            g_string_append_printf (tip, "\n%s: %s%s", l, title_for_path (m->current),
                                    m->paused ? " (paused)" : "");
            g_free (l);
        }
        U.mon_building = TRUE;
        gtk_drop_down_set_model (GTK_DROP_DOWN (U.mon_dd), G_LIST_MODEL (sl));
        g_object_unref (sl);
        gtk_drop_down_set_selected (GTK_DROP_DOWN (U.mon_dd), keep <= nm->len ? keep : 0);
        U.mon_building = FALSE;
        gtk_widget_set_tooltip_text (U.mon_box, tip->str);
        g_string_free (tip, TRUE);
        gtk_widget_set_visible (U.mon_box, nm->len > 1);
        /* which wallpaper each monitor shows, in plain sight */
        GString *now = g_string_new (NULL);
        for (guint i = 0; i < nm->len && nm->len > 1; i++) {
            MonInfo *m = nm->pdata[i];
            g_string_append_printf (now, "%s%d · %s: %s%s", now->len ? "     " : "Showing  ",
                                    m->index + 1, *m->name ? m->name : "Monitor",
                                    title_for_path (m->current), m->paused ? " (paused)" : "");
        }
        gtk_label_set_text (U.mon_now, now->str);
        gtk_widget_set_tooltip_text (GTK_WIDGET (U.mon_now), now->len ? now->str : NULL);
        gtk_widget_set_visible (GTK_WIDGET (U.mon_now), now->len > 0);
        g_string_free (now, TRUE);
        gtk_widget_queue_draw (U.mon_area);
    } else {
        g_string_free (sig, TRUE);
    }
    return changed;
}

static void settings_software_info (void);
static void settings_follow (void);

static void on_status_reply (JsonObject *reply, const char *err, gpointer u)
{
    (void) u;
    Sup.poll_inflight = FALSE;
    if (U.closing)
        return;
    JsonObject *st = reply ? wpj_obj (reply, "state") : NULL;
    if (!st) {
        if (ui_ipc_err_is_down (err))
            walpe_res_ui_status (NULL);     /* no engine: its monitors are not known */
        if (ui_ipc_err_is_down (err) || !reply) {
            gboolean down = ui_ipc_err_is_down (err);
            if (!down && ++Sup.poll_fails < 3)
                return;   /* a busy daemon (loading) misses a poll or two */
            if (U.dstate == D_STARTING &&
                g_get_monotonic_time () - Sup.spawn_us < 12 * G_USEC_PER_SEC)
                return;   /* still coming up */
            if (!down) {
                sup_set_state (D_BUSY, "The engine is running but does not answer.");
                return;
            }
            if (U.dstate == D_STARTING && Sup.child == 0) {
                Sup.quick_fails++;
                g_free (Sup.why);
                Sup.why = g_strdup ("it never opened its control socket");
            }
            if (U.dstate == D_UP || U.dstate == D_UNKNOWN || U.dstate == D_STARTING) {
                if (Sup.quick_fails >= 3) {
                    sup_give_up ();
                    return;
                }
                if (Sup.auto_restart && !Sup.child) {
                    sup_set_state (D_DOWN, U.dstate == D_UP
                                   ? "The engine stopped. Restarting it…"
                                   : "The engine is not running. Starting it…");
                    sup_schedule_respawn ();
                } else if (!Sup.child) {
                    sup_set_state (D_DOWN, NULL);
                }
            }
        }
        return;
    }
    Sup.poll_fails = 0;
    gint64 now = g_get_monotonic_time ();
    if (U.dstate != D_UP)
        Sup.up_since_us = now;
    if (Sup.quick_fails && now - Sup.up_since_us > 30 * G_USEC_PER_SEC)
        Sup.quick_fails = 0;   /* it stayed up: forgive earlier crashes */
    Sup.auto_restart = TRUE;
    if (U.last_state)
        json_object_unref (U.last_state);
    U.last_state = json_object_ref (st);
    settings_software_info ();   /* the engine restarts itself for that switch */
    /* render resolution per monitor: the dialog, its rows in the settings
     * window and the announcement of a new monitor all follow this line */
    walpe_res_ui_status (st);
    const char *path = wpj_str (st, "path", "");
    gboolean cur_changed = g_strcmp0 (path, U.current) != 0;
    g_free (U.current);
    U.current = g_strdup (path);
    gboolean mons_changed = parse_monitors (st);
    if (mons_changed)
        cur_changed = TRUE;
    {
        /* A wallpaper the engine could not load. It answers "open" before
         * it loads, so "Set as wallpaper" on a broken wallpaper ended with
         * "Wallpaper set: ..." here while the engine put the previous one
         * back (or showed nothing): the reason was in its status and in
         * its log only. Shown when it appears or changes; taken away when
         * the wallpaper loads after all. */
        char *le = walpe_store_load_error_text (st);
        if (g_strcmp0 (le, U.load_err)) {
            if (le) {
                set_status (le, TRUE);
                U.status_load_err = TRUE;
            } else if (U.status_load_err) {
                status_clear (NULL);
                U.status_load_err = FALSE;
            }
            g_free (U.load_err);
            U.load_err = le;
        } else {
            g_free (le);
        }
    }
    U.paused = wpj_bool (st, "paused", FALSE);
    U.by_path = wpj_bool (st, "props_by_path", FALSE);
    U.layer_adjust = wpj_bool (st, "layer_adjust", FALSE);
    {
        /* settings saved by someone else (walpe-ctl, the tray): the one
         * the GUI itself acts on follows */
        static double serial = -1;
        double s = wpj_num (st, "config_serial", -1);
        if (s >= 0 && serial >= 0 && s != serial) {
            WalpeConfig *c = walpe_config_load ();
            walpe_config_free (U.cfg);
            U.cfg = c;
            thumbs_set_anim_mode (c->preview_anim);
            /* and the open settings window shows them (it was filled
             * only when it was opened: a switch changed by walpe-ctl or
             * the tray stayed as it was, and the next click on it sent
             * the wrong way round) */
            settings_follow ();
        }
        serial = s;
    }
    DState was = U.dstate;
    sup_set_state (D_UP, NULL);
    if (was == D_STARTING || was == D_DOWN || was == D_GAVE_UP || was == D_BUSY)
        set_status ("Engine running.", FALSE);
    /* pause toggle mirrors the daemon */
    U.pause_building = TRUE;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.pause_btn), U.paused);
    gtk_button_set_label (GTK_BUTTON (U.pause_btn), U.paused ? "Resume" : "Pause");
    U.pause_building = FALSE;
    char *ml = U.monitors > 1 && !U.mons->len
                   ? g_strdup_printf ("Applies to all %d monitors", U.monitors) : g_strdup ("");
    gtk_label_set_text (U.mon_label, ml);
    gtk_widget_set_visible (GTK_WIDGET (U.mon_label), *ml != 0);
    g_free (ml);
    if (cur_changed)
        refresh_badges ();
    WalpeEntry *e = U.shown;
    walpe_props_panel_set_state (U.props_panel, e && is_active (e->path), U.by_path);
    walpe_layers_panel_set_state (U.layers_panel, e && is_active (e->path), U.layer_adjust);
    if (e && cur_changed)
        update_details (e);
    if (U.pending_apply) {
        char *p = U.pending_apply;
        U.pending_apply = NULL;
        apply_send (p);
        g_free (p);
    }
}

static void sup_poll_now (void)
{
    if (Sup.poll_inflight)
        return;
    Sup.poll_inflight = TRUE;
    ui_ipc_send ("{\"cmd\":\"status\"}", UI_IPC_POLL, on_status_reply, NULL);
}

static gboolean sup_tick (gpointer u)
{
    (void) u;
    sup_poll_now ();
    return G_SOURCE_CONTINUE;
}

/* the poll timer follows what the window and the subscription are doing */
static void sup_poll_update (void)
{
    if (U.closing)
        return;
    gboolean live = Sup.sub && walpe_ipc_sub_live (Sup.sub);
    gboolean active = U.win && gtk_window_is_active (GTK_WINDOW (U.win));
    guint secs;
    if (live)
        secs = active ? 10 : 0;
    else if (Sup.sub && walpe_ipc_sub_unsupported (Sup.sub) && U.dstate == D_UP)
        secs = active ? 2 : 0;
    else
        secs = 2;
    if (secs == Sup.poll_secs)
        return;
    if (Sup.poll_id)
        g_source_remove (Sup.poll_id);
    Sup.poll_secs = secs;
    Sup.poll_id = secs ? g_timeout_add_seconds (secs, sup_tick, NULL) : 0;
}

/* a status line pushed by the daemon, or NULL: it went away (or cannot
 * push): the supervisor's own request finds out which */
static void on_sub_line (const char *line, gpointer u)
{
    (void) u;
    JsonNode *n = line ? wpj_parse_data (line, -1) : NULL;
    if (n && JSON_NODE_HOLDS_OBJECT (n))
        on_status_reply (json_node_get_object (n), NULL, NULL);
    else
        sup_poll_now ();
    if (n)
        json_node_unref (n);
    sup_poll_update ();
}

static void on_win_active (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) o; (void) ps; (void) u;
    if (U.win && gtk_window_is_active (GTK_WINDOW (U.win)) &&
        !(Sup.sub && walpe_ipc_sub_live (Sup.sub)))
        sup_poll_now ();    /* back in use: no stale state */
    sup_poll_update ();
}

/* ================================================================ library */

static gboolean filter_match (gpointer item, gpointer u)
{
    (void) u;
    WalpeEntry *e = item;
    guint t = U.type_dd ? gtk_drop_down_get_selected (GTK_DROP_DOWN (U.type_dd)) : 0;
    switch (t) {
    case 1: if (g_strcmp0 (e->type, "scene")) return FALSE; break;
    case 2: if (g_strcmp0 (e->type, "video")) return FALSE; break;
    case 3: if (g_strcmp0 (e->type, "web")) return FALSE; break;
    case 4: if (g_strcmp0 (e->type, "image") && g_strcmp0 (e->type, "gif")) return FALSE; break;
    case 5: if (!e->has_props) return FALSE; break;
    default: break;
    }
    return !U.search_text || (e->search && strstr (e->search, U.search_text));
}

static void update_count (void)
{
    guint n = g_list_model_get_n_items (G_LIST_MODEL (U.filtered));
    guint all = g_list_model_get_n_items (G_LIST_MODEL (U.model));
    char *s = n == all ? g_strdup_printf ("%u wallpapers", all)
                       : g_strdup_printf ("%u of %u", n, all);
    gtk_label_set_text (GTK_LABEL (U.count), s);
    g_free (s);
}

static void refilter (void)
{
    gtk_filter_changed (GTK_FILTER (U.filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_count ();
}

static void on_search_changed (GtkSearchEntry *e, gpointer u)
{
    (void) u;
    g_free (U.search_text);
    const char *t = gtk_editable_get_text (GTK_EDITABLE (e));
    char *n = g_utf8_normalize (t, -1, G_NORMALIZE_ALL);
    U.search_text = *t ? g_utf8_casefold (n ? n : t, -1) : NULL;
    g_free (n);
    refilter ();
}

static void on_type_changed (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) o; (void) ps; (void) u;
    refilter ();
}

/* select the entry with this path (filtered model), scroll to it */
static gboolean select_path (const char *path)
{
    GListModel *m = G_LIST_MODEL (U.filtered);
    guint n = g_list_model_get_n_items (m);
    for (guint i = 0; i < n && path; i++) {
        WalpeEntry *e = g_list_model_get_item (m, i);
        gboolean hit = same_path (e->path, path);
        g_object_unref (e);
        if (hit) {
            gtk_single_selection_set_selected (U.sel, i);
            gtk_grid_view_scroll_to (GTK_GRID_VIEW (U.grid), i, GTK_LIST_SCROLL_FOCUS, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

typedef struct { char *dir; guint gen; } ScanJob;

static void scan_thread (GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    ScanJob *j = data;
    g_task_return_pointer (t, walpe_store_scan_library (j->dir), NULL);
}

static void scan_job_free (gpointer p)
{
    ScanJob *j = p;
    g_free (j->dir);
    g_free (j);
}

static void scan_done (GObject *src, GAsyncResult *res, gpointer u)
{
    (void) src; (void) u;
    ScanJob *j = g_task_get_task_data (G_TASK (res));
    GList *entries = g_task_propagate_pointer (G_TASK (res), NULL);
    if (j->gen != U.scan_gen || !U.model || U.closing) {
        g_list_free_full (entries, g_object_unref);
        return;
    }
    guint n = g_list_length (entries);
    if (U.first_scan_done && n == g_list_model_get_n_items (G_LIST_MODEL (U.model))) {
        /* nothing changed (the dir monitor fires for unrelated writes):
         * keep the model, the scroll position and the recycled tiles */
        gboolean same = TRUE;
        guint i = 0;
        for (GList *l = entries; l && same; l = l->next, i++) {
            WalpeEntry *a = l->data, *b = g_list_model_get_item (G_LIST_MODEL (U.model), i);
            same = !g_strcmp0 (a->path, b->path) && !g_strcmp0 (a->title, b->title) &&
                   !g_strcmp0 (a->type, b->type) && !g_strcmp0 (a->preview, b->preview) &&
                   /* a preview replaced or completed on disk shows again */
                   a->preview_mtime == b->preview_mtime && a->preview_size == b->preview_size;
            g_object_unref (b);
        }
        if (same) {
            g_list_free_full (entries, g_object_unref);
            return;
        }
    }
    WalpeEntry *keep = selected_entry ();
    char *keep_path = keep ? g_strdup (keep->path) : NULL;
    gpointer *arr = g_new (gpointer, n ? n : 1);
    guint i = 0;
    for (GList *l = entries; l; l = l->next)
        arr[i++] = l->data;
    g_list_store_splice (U.model, 0, g_list_model_get_n_items (G_LIST_MODEL (U.model)), arr, n);
    g_free (arr);
    g_list_free_full (entries, g_object_unref);
    update_count ();
    if (!U.first_scan_done) {
        U.first_scan_done = TRUE;
        /* start on the active wallpaper (from config.json: no IPC wait) */
        if (!select_path (U.current && *U.current ? U.current : U.cfg->current) && n)
            gtk_single_selection_set_selected (U.sel, 0);
    } else if (keep_path) {
        select_path (keep_path);
    }
    g_free (keep_path);
    if (U.last_state) {
        /* monitor labels name wallpapers by title: redo them with the scan */
        g_clear_pointer (&U.mon_sig, g_free);
        parse_monitors (U.last_state);
    }
    if (!n)
        set_status ("The library is empty: Import… media files or Add folder… with Wallpaper "
                    "Engine projects.", FALSE);
}

static void refresh_model (void)
{
    ScanJob *j = g_new0 (ScanJob, 1);
    j->dir = g_strdup (U.cfg->wallpapers_dir);
    j->gen = ++U.scan_gen;
    GTask *t = g_task_new (NULL, NULL, scan_done, NULL);
    g_task_set_task_data (t, j, scan_job_free);
    g_task_run_in_thread (t, scan_thread);
    g_object_unref (t);
}

/* A folder that is copied into the library appears first and gets its
 * project.json (and a complete preview) later; only the library root is
 * watched, so nothing reports that. After a change the library is therefore
 * scanned again 3 s and 10 s later (a scan that finds nothing new leaves
 * the view alone). One scan 0.6 s after the folder appeared missed every
 * copy that took longer. */
static int rescan_more;

static gboolean rescan_tick (gpointer u)
{
    (void) u;
    U.rescan_id = 0;
    refresh_model ();
    if (rescan_more > 0) {
        U.rescan_id = g_timeout_add (rescan_more == 2 ? 3000 : 10000, rescan_tick, NULL);
        rescan_more--;
    }
    return G_SOURCE_REMOVE;
}

static void schedule_rescan (void)
{
    if (U.rescan_id)
        g_source_remove (U.rescan_id);
    rescan_more = 2;
    U.rescan_id = g_timeout_add (600, rescan_tick, NULL);
}

static void on_dir_changed (GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer u)
{
    (void) m; (void) f; (void) o; (void) u;
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_MOVED_IN ||
        ev == G_FILE_MONITOR_EVENT_MOVED_OUT || ev == G_FILE_MONITOR_EVENT_RENAMED)
        schedule_rescan ();
}

/* ------------------------------------------------------------ grid tiles */

typedef struct {
    GtkWidget *pic, *ph, *type, *active, *steam, *title;
} Tile;

static void setup_tile (GtkSignalListItemFactory *f, GtkListItem *item, gpointer u)
{
    (void) f; (void) u;
    Tile *t = g_new0 (Tile, 1);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request (box, TILE_W, -1);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    GtkWidget *ov = gtk_overlay_new ();
    gtk_widget_add_css_class (ov, "walpe-thumb");
    gtk_widget_set_size_request (ov, TILE_W, TILE_H);
    gtk_widget_set_halign (ov, GTK_ALIGN_CENTER);
    t->ph = gtk_label_new (NULL);
    gtk_widget_add_css_class (t->ph, "walpe-thumb-placeholder");
    gtk_overlay_set_child (GTK_OVERLAY (ov), t->ph);
    t->pic = gtk_picture_new ();
    gtk_picture_set_content_fit (GTK_PICTURE (t->pic), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink (GTK_PICTURE (t->pic), TRUE);
    gtk_widget_set_size_request (t->pic, TILE_W, TILE_H);
    gtk_overlay_add_overlay (GTK_OVERLAY (ov), t->pic);
    GtkWidget *bl = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (bl, "walpe-tile-badges");
    gtk_widget_set_halign (bl, GTK_ALIGN_START);
    gtk_widget_set_valign (bl, GTK_ALIGN_START);
    t->type = badge_new (NULL, "");
    gtk_box_append (GTK_BOX (bl), t->type);
    t->steam = badge_new ("steam", "Steam");
    gtk_box_append (GTK_BOX (bl), t->steam);
    gtk_overlay_add_overlay (GTK_OVERLAY (ov), bl);
    t->active = badge_new ("active", "Active");
    gtk_widget_add_css_class (t->active, "walpe-tile-badges");
    gtk_widget_set_halign (t->active, GTK_ALIGN_END);
    gtk_widget_set_valign (t->active, GTK_ALIGN_START);
    gtk_overlay_add_overlay (GTK_OVERLAY (ov), t->active);
    gtk_box_append (GTK_BOX (box), ov);
    t->title = gtk_label_new (NULL);
    gtk_widget_add_css_class (t->title, "walpe-tile-title");
    gtk_label_set_wrap (GTK_LABEL (t->title), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (t->title), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines (GTK_LABEL (t->title), 2);
    gtk_label_set_ellipsize (GTK_LABEL (t->title), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars (GTK_LABEL (t->title), 10);
    gtk_label_set_max_width_chars (GTK_LABEL (t->title), 18);
    gtk_label_set_xalign (GTK_LABEL (t->title), 0);
    gtk_label_set_yalign (GTK_LABEL (t->title), 0);
    /* reserve two lines so rows line up */
    gtk_widget_set_size_request (t->title, TILE_W, -1);
    gtk_box_append (GTK_BOX (box), t->title);
    gtk_list_item_set_child (item, box);
    g_object_set_data_full (G_OBJECT (item), "tile", t, g_free);
}

static void tile_badges (GtkListItem *item)
{
    Tile *t = g_object_get_data (G_OBJECT (item), "tile");
    WalpeEntry *e = gtk_list_item_get_item (item);
    if (t && e)
        gtk_widget_set_visible (t->active, is_active (e->path));
}

static void refresh_badges (void)
{
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init (&it, U.bound);
    while (g_hash_table_iter_next (&it, &k, NULL))
        tile_badges (k);
}

static void bind_tile (GtkSignalListItemFactory *f, GtkListItem *item, gpointer u)
{
    (void) f; (void) u;
    Tile *t = g_object_get_data (G_OBJECT (item), "tile");
    WalpeEntry *e = gtk_list_item_get_item (item);
    if (!t || !e)
        return;
    gtk_label_set_text (GTK_LABEL (t->title), e->title);
    char *tip = g_strdup_printf ("%s\n%s · %s", e->title, type_label (e->type), e->id);
    gtk_widget_set_tooltip_text (t->title, tip);
    g_free (tip);
    set_badge (t->type, e->type);
    gtk_widget_set_visible (t->steam, e->steam);
    gtk_label_set_text (GTK_LABEL (t->ph), e->preview ? "" : "No preview");
    int sc = gtk_widget_get_scale_factor (t->pic);
    /* image files are their own preview (loose media) */
    thumbs_set (GTK_PICTURE (t->pic), e->preview, TILE_W * sc, TILE_H * sc, THUMB_COVER);
    gtk_accessible_update_property (GTK_ACCESSIBLE (gtk_list_item_get_child (item)),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, e->title, -1);
    g_hash_table_add (U.bound, item);
    tile_badges (item);
}

static void unbind_tile (GtkSignalListItemFactory *f, GtkListItem *item, gpointer u)
{
    (void) f; (void) u;
    Tile *t = g_object_get_data (G_OBJECT (item), "tile");
    if (t)
        thumbs_clear (GTK_PICTURE (t->pic));
    g_hash_table_remove (U.bound, item);
}

static void teardown_tile (GtkSignalListItemFactory *f, GtkListItem *item, gpointer u)
{
    (void) f; (void) u;
    g_hash_table_remove (U.bound, item);
}

/* ============================================================== selection */

static gboolean props_idle_cb (gpointer u)
{
    (void) u;
    U.props_idle = 0;
    WalpeEntry *e = U.shown;
    /* the option schema of this one wallpaper, read now (the scan keeps
     * none); the panel takes its own references */
    walpe_entry_load_props (e);
    walpe_props_panel_show (U.props_panel, e ? e->path : NULL, e ? e->props : NULL,
                            e ? e->localization : NULL, e && is_active (e->path));
    walpe_entry_unload_props (e);
    walpe_props_panel_set_state (U.props_panel, e && is_active (e->path), U.by_path);
    walpe_layers_panel_show (U.layers_panel, e ? e->path : NULL, e ? e->type : NULL);
    walpe_layers_panel_set_state (U.layers_panel, e && is_active (e->path), U.layer_adjust);
    walpe_perf_panel_set_path (U.perf_panel, e ? e->path : NULL);
    return G_SOURCE_REMOVE;
}

static void update_apply_button (void)
{
    WalpeEntry *e = U.shown;
    gboolean active = e && is_active (e->path);
    gboolean on_target = e && is_on_target (e->path);
    if (!U.apply_guard)
        gtk_button_set_label (GTK_BUTTON (U.apply), on_target ? "Reload wallpaper" : "Set as wallpaper");
    gtk_widget_set_sensitive (U.apply, e && !U.apply_guard && g_strcmp0 (e->type, "unsupported"));
    if (U.delete_btn)
        gtk_widget_set_sensitive (U.delete_btn, e != NULL);
    gtk_widget_set_visible (U.active_badge, active);
}

/* description: clamped to 3 lines, "Show more" opens the full text in a
 * height-capped scroller. Its minimum height never depends on the text
 * (a 2000 px changelog once forced the pane taller than the window: GTK
 * then re-measured every frame and input stopped working). */
static void desc_apply (void)
{
    const char *full = U.desc_full ? U.desc_full : "";
    gboolean longish = g_utf8_strlen (full, -1) > 260 || strchr (full, '\n');
    gtk_widget_set_visible (U.desc_more, longish);
    gtk_widget_set_visible (gtk_widget_get_parent (gtk_widget_get_parent (GTK_WIDGET (U.desc))),
                            *full != 0);
    if (U.desc_open) {
        gtk_label_set_lines (U.desc, -1);
        gtk_label_set_ellipsize (U.desc, PANGO_ELLIPSIZE_NONE);
        gtk_label_set_text (U.desc, full);
        gtk_button_set_label (GTK_BUTTON (U.desc_more), "Show less");
    } else {
        gtk_label_set_lines (U.desc, 3);
        gtk_label_set_ellipsize (U.desc, PANGO_ELLIPSIZE_END);
        gtk_label_set_text (U.desc, U.desc_one ? U.desc_one : "");
        gtk_button_set_label (GTK_BUTTON (U.desc_more), "Show more");
    }
}

static void update_details (WalpeEntry *e)
{
    if (e != U.shown) {
        if (U.shown)
            g_object_unref (U.shown);
        U.shown = e ? g_object_ref (e) : NULL;
        /* the options list is rebuilt a moment later: arrowing through the
         * grid stays fluid, only the wallpaper you stop on builds its rows */
        if (U.props_idle)
            g_source_remove (U.props_idle);
        U.props_idle = g_timeout_add (90, props_idle_cb, NULL);
    }
    if (!e) {
        gtk_label_set_text (U.title, "Nothing selected");
        gtk_widget_set_visible (U.type_badge, FALSE);
        gtk_widget_set_visible (U.steam_badge, FALSE);
        gtk_widget_set_visible (U.nopt_badge, FALSE);
        gtk_label_set_text (U.idlabel, "");
        g_free (U.desc_full);
        U.desc_full = NULL;
        g_free (U.desc_one);
        U.desc_one = NULL;
        desc_apply ();
        thumbs_clear (U.preview);
        gtk_label_set_text (GTK_LABEL (U.preview_ph), "");
        update_apply_button ();
        return;
    }
    gtk_label_set_text (U.title, e->title);
    gtk_widget_set_visible (U.type_badge, TRUE);
    set_badge (U.type_badge, e->type);
    gtk_widget_set_visible (U.steam_badge, e->steam);
    guint n = count_options (e);
    char *no = g_strdup_printf ("%u option%s", n, n == 1 ? "" : "s");
    gtk_label_set_text (GTK_LABEL (U.nopt_badge), no);
    g_free (no);
    gtk_widget_set_visible (U.nopt_badge, n > 0);
    gtk_label_set_text (U.idlabel, e->id);
    gtk_widget_set_tooltip_text (GTK_WIDGET (U.idlabel), e->path);
    /* the author's text: bounded, and breakable where it has no spaces
     * (uitext.h) */
    char *plain = walpe_props_plain_text (e->description);
    char *d = walpe_ui_text (plain, 16000, WALPE_UITEXT_PARAGRAPHS);
    g_free (plain);
    /* one paragraph so the 3-line clamp holds (Pango clamps per paragraph) */
    char **parts = g_strsplit (d, "\n", -1);
    g_free (U.desc_one);
    U.desc_one = g_strjoinv ("  ·  ", parts);
    g_strfreev (parts);
    g_free (U.desc_full);
    U.desc_full = d;
    U.desc_open = FALSE;
    desc_apply ();
    gtk_label_set_text (GTK_LABEL (U.preview_ph), e->preview ? "" : "No preview image");
    int sc = gtk_widget_get_scale_factor (GTK_WIDGET (U.preview));
    thumbs_set (U.preview, e->preview, PREVIEW_W * sc, PREVIEW_H * sc, THUMB_CONTAIN);
    update_apply_button ();
}

static void on_desc_more (GtkButton *b, gpointer u)
{
    (void) b; (void) u;
    U.desc_open = !U.desc_open;
    desc_apply ();
}

static void on_selected (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) o; (void) ps; (void) u;
    WalpeEntry *e = selected_entry ();
    if (e)   /* filtered out: keep showing the last one */
        update_details (e);
}

/* ------------------------------------------------------------------ apply */

static gboolean apply_unguard (gpointer u)
{
    (void) u;
    U.apply_guard = 0;
    update_apply_button ();
    return G_SOURCE_REMOVE;
}

static void import_poll_start (void);

static void on_open_reply (JsonObject *reply, const char *err, gpointer user)
{
    char *path = user;
    if (U.apply_guard) {
        g_source_remove (U.apply_guard);
        U.apply_guard = 0;
    }
    if (ui_ipc_ok (reply) && wpj_bool (reply, "converting", FALSE)) {
        /* a video the player cannot show as it is: the engine converts it
         * once (ffmpeg) and switches when the copy is ready; the wallpaper
         * on screen stays until then. Progress comes with status. */
        set_status ("Converting the video to H.264 (once)… the wallpaper changes when it is ready.",
                    FALSE);
        import_poll_start ();
    } else if (ui_ipc_ok (reply)) {
        if (!U.mons->len) {
            g_free (U.current);
            U.current = g_strdup (path);
        }
        char *base = g_path_get_basename (path);
        char *m = g_strdup_printf ("Wallpaper set: %s", U.shown && same_path (U.shown->path, path)
                                                            ? U.shown->title : base);
        set_status (m, FALSE);
        g_free (m);
        g_free (base);
        walpe_props_panel_applied (U.props_panel, path);
        refresh_badges ();
        /* the same wallpaper set again fails with the same words: say them again */
        g_clear_pointer (&U.load_err, g_free);
        sup_poll_now ();
    } else if (!reply && ui_ipc_err_is_down (err) && U.dstate != D_GAVE_UP) {
        g_free (U.pending_apply);
        U.pending_apply = g_strdup (path);   /* sent once the engine is up */
        set_status ("The engine is not running: starting it, then setting the wallpaper…", FALSE);
    } else {
        const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
        char *m = g_strdup_printf ("Could not set the wallpaper: %s", e ? e : "error");
        set_status (m, TRUE);
        g_free (m);
    }
    update_apply_button ();
    g_free (path);
}

static void apply_send (const char *path)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "open");
    json_builder_set_member_name (b, "path");
    json_builder_add_string_value (b, path);
    guint ms = U.mon_dd && gtk_widget_get_visible (U.mon_box)
                   ? gtk_drop_down_get_selected (GTK_DROP_DOWN (U.mon_dd)) : 0;
    if (ms > 0 && ms <= U.mons->len) {
        /* one monitor (per-monitor daemons); omitted = all monitors */
        json_builder_set_member_name (b, "monitor");
        json_builder_add_int_value (b, ((MonInfo *) U.mons->pdata[ms - 1])->index);
    }
    json_builder_end_object (b);
    /* the button comes back after the reply, or after 20 s whatever happens */
    if (U.apply_guard)
        g_source_remove (U.apply_guard);
    U.apply_guard = g_timeout_add_seconds (20, apply_unguard, NULL);
    gtk_button_set_label (GTK_BUTTON (U.apply), "Setting…");
    update_apply_button ();
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW | UI_IPC_AUTOSTART, on_open_reply,
                      g_strdup (path));
    g_object_unref (b);
}

static void do_apply (void)
{
    WalpeEntry *e = U.shown;
    if (!e || U.apply_guard || !g_strcmp0 (e->type, "unsupported"))
        return;
    apply_send (e->path);
}

static void on_apply (GtkButton *b, gpointer u) { (void) b; (void) u; do_apply (); }

static void on_grid_activate (GtkGridView *g, guint pos, gpointer u)
{
    (void) g; (void) u;
    gtk_single_selection_set_selected (U.sel, pos);
    do_apply ();
}

/* ----------------------------------------------------------------- delete
 * "Delete…" next to the title, the Delete key on a tile, and the tiles'
 * context menu (right click, Menu key, Shift+F10). The flow itself -
 * checks, confirmation, engine "forget", trash - is delete.c. */

/* which monitors show `path` now, as a sentence for the dialog; NULL = none */
static char *delete_in_use (const char *path)
{
    if (!U.mons || U.mons->len < 2)
        return is_active (path) ? g_strdup ("It is the current wallpaper.") : NULL;
    GString *s = g_string_new (NULL);
    guint n = 0;
    for (guint i = 0; i < U.mons->len; i++) {
        MonInfo *m = U.mons->pdata[i];
        if (!same_path (m->current, path))
            continue;
        g_string_append_printf (s, "%s%d · %s", n ? " and " : "", m->index + 1,
                                *m->name ? m->name : "Monitor");
        n++;
    }
    char *out = n ? g_strdup_printf ("It is the current wallpaper of monitor%s %s.",
                                     n == 1 ? "" : "s", s->str) : NULL;
    g_string_free (s, TRUE);
    return out;
}

/* engine not running (config.json was edited instead): its last status
 * still names the deleted wallpaper. Take it out, so the "Showing ..." line
 * and the Active badges do not show it until the engine is started */
static void delete_patch_state (const char *path)
{
    if (same_path (U.current, path)) {
        g_free (U.current);
        U.current = g_strdup ("");
    }
    if (!U.last_state)
        return;
    if (same_path (wpj_str (U.last_state, "path", ""), path))
        json_object_set_string_member (U.last_state, "path", "");
    JsonArray *a = wpj_arr (U.last_state, "monitors");
    for (guint i = 0; i < wpj_arr_len (a); i++) {
        JsonObject *o = wpj_arr_obj (a, i);
        if (o && same_path (wpj_str (o, "current", ""), path))
            json_object_set_string_member (o, "current", "");
    }
    g_clear_pointer (&U.mon_sig, g_free);
    parse_monitors (U.last_state);
    refresh_badges ();
}

/* the delete was asked for on a tile (Delete key, context menu): the
 * keyboard focus goes to the tile that takes its place, so Delete / the
 * arrow keys carry on from there. From the details pane's button the focus
 * stays on that button */
static gboolean delete_from_grid;

/* delete.c is done: take the entry out of the grid at once (the rescan
 * that follows confirms it) and move the selection to its neighbour */
static void delete_done (const char *path, gboolean gone)
{
    if (gone && U.dstate != D_UP)
        delete_patch_state (path);
    GListModel *all = G_LIST_MODEL (U.model);
    guint n = g_list_model_get_n_items (all);
    for (guint i = 0; gone && i < n; i++) {
        WalpeEntry *e = g_list_model_get_item (all, i);
        gboolean hit = !g_strcmp0 (e->path, path);
        g_object_unref (e);
        if (!hit)
            continue;
        WalpeEntry *sel = selected_entry ();
        gboolean was_sel = sel && !g_strcmp0 (sel->path, path);
        gboolean was_shown = U.shown && !g_strcmp0 (U.shown->path, path);
        guint pos = gtk_single_selection_get_selected (U.sel);
        g_list_store_remove (U.model, i);
        update_count ();
        guint left = g_list_model_get_n_items (G_LIST_MODEL (U.filtered));
        if (was_sel && left) {
            /* the tile that moved into its place, or the new last one */
            guint np = MIN (pos, left - 1);
            gtk_single_selection_set_selected (U.sel, np);
            gtk_grid_view_scroll_to (GTK_GRID_VIEW (U.grid), np, GTK_LIST_SCROLL_FOCUS, NULL);
        }
        if (was_shown && U.shown && !g_strcmp0 (U.shown->path, path))
            update_details (selected_entry ());   /* NULL: "Nothing selected" */
        /* scroll_to only moves the grid's focus item; the keyboard focus
         * may be anywhere (a removed tile hands it to the GtkPaned) */
        if (delete_from_grid || !gtk_window_get_focus (GTK_WINDOW (U.win)))
            gtk_widget_grab_focus (U.grid);
        break;
    }
    schedule_rescan ();
    sup_poll_now ();   /* the engine switched away: badges, monitor line */
}

static void request_delete (WalpeEntry *e, gboolean from_grid)
{
    if (walpe_ui_delete_busy ())
        from_grid = delete_from_grid;   /* the open dialog is only raised */
    delete_from_grid = from_grid;
    if (!e) {
        set_status ("Select a wallpaper to delete.", FALSE);
        return;
    }
    WalpeDeleteHost host = { GTK_WINDOW (U.win), set_status, delete_done };
    char *use = delete_in_use (e->path);
    walpe_ui_delete (&host, e->path, e->title, U.cfg->wallpapers_dir, use);
    g_free (use);
}

static void on_delete_clicked (GtkButton *b, gpointer u)
{
    (void) b; (void) u;
    request_delete (U.shown, FALSE);
}

static void act_tile_delete (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    request_delete (selected_entry (), TRUE);
}

static void act_tile_apply (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    do_apply ();
}

/* the bound tile under a point of the grid (its whole cell), or NULL */
static GtkListItem *tile_at (double x, double y)
{
    graphene_point_t pt = GRAPHENE_POINT_INIT ((float) x, (float) y);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init (&it, U.bound);
    while (g_hash_table_iter_next (&it, &k, NULL)) {
        GtkWidget *child = gtk_list_item_get_child (k);
        GtkWidget *cell = child ? gtk_widget_get_parent (child) : NULL;
        graphene_rect_t r;
        if (cell && gtk_widget_get_mapped (cell) && gtk_widget_compute_bounds (cell, U.grid, &r) &&
            graphene_rect_contains_point (&r, &pt))
            return k;
    }
    return NULL;
}

/* x, y in grid coordinates */
static void tile_menu_popup (double x, double y)
{
    graphene_point_t in = GRAPHENE_POINT_INIT ((float) x, (float) y), out;
    if (!gtk_widget_compute_point (U.grid, gtk_widget_get_parent (U.tile_menu), &in, &out))
        return;
    GdkRectangle r = { (int) out.x, (int) out.y, 1, 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (U.tile_menu), &r);
    gtk_popover_popup (GTK_POPOVER (U.tile_menu));
}

/* right click: select that tile, then its menu */
static void on_grid_secondary (GtkGestureClick *g, int n_press, double x, double y, gpointer u)
{
    (void) n_press; (void) u;
    GtkListItem *item = tile_at (x, y);
    if (!item)
        return;
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_single_selection_set_selected (U.sel, gtk_list_item_get_position (item));
    /* keyboard focus too, as a left click gives it: the menu returns it
     * there, and Delete / arrows then act on this tile */
    gtk_widget_grab_focus (gtk_widget_get_parent (gtk_list_item_get_child (item)));
    tile_menu_popup (x, y);
}

static gboolean on_grid_key (GtkEventControllerKey *c, guint keyval, guint keycode,
                             GdkModifierType state, gpointer u)
{
    (void) c; (void) keycode; (void) u;
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
    if ((keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete) && !mods) {
        request_delete (selected_entry (), TRUE);
        return GDK_EVENT_STOP;
    }
    if ((keyval == GDK_KEY_Menu && !mods) || (keyval == GDK_KEY_F10 && mods == GDK_SHIFT_MASK)) {
        /* the menu of the selected tile, at its middle */
        guint pos = gtk_single_selection_get_selected (U.sel);
        GHashTableIter it;
        gpointer k;
        g_hash_table_iter_init (&it, U.bound);
        while (pos != GTK_INVALID_LIST_POSITION && g_hash_table_iter_next (&it, &k, NULL)) {
            GtkWidget *child = gtk_list_item_get_child (k);
            graphene_rect_t r;
            if (gtk_list_item_get_position (k) != pos || !child ||
                !gtk_widget_compute_bounds (child, U.grid, &r))
                continue;
            tile_menu_popup (r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
            break;
        }
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

/* build_library: Delete key + context menu for the grid. The popover is a
 * child of the library box (a layout-managed parent presents it) */
static void delete_attach_grid (GtkWidget *library_box)
{
    static const GActionEntry acts[] = {
        { .name = "apply", .activate = act_tile_apply },
        { .name = "delete", .activate = act_tile_delete },
    };
    GSimpleActionGroup *grp = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (grp), acts, G_N_ELEMENTS (acts), NULL);
    gtk_widget_insert_action_group (library_box, "tile", G_ACTION_GROUP (grp));
    g_object_unref (grp);

    GMenu *m = g_menu_new ();
    g_menu_append (m, "Set as wallpaper", "tile.apply");
    GMenu *danger = g_menu_new ();
    g_menu_append (danger, "Delete…", "tile.delete");
    g_menu_append_section (m, NULL, G_MENU_MODEL (danger));
    g_object_unref (danger);
    U.tile_menu = gtk_popover_menu_new_from_model (G_MENU_MODEL (m));
    g_object_unref (m);
    gtk_popover_set_has_arrow (GTK_POPOVER (U.tile_menu), FALSE);
    gtk_popover_set_position (GTK_POPOVER (U.tile_menu), GTK_POS_BOTTOM);
    gtk_widget_set_halign (U.tile_menu, GTK_ALIGN_START);
    gtk_widget_set_parent (U.tile_menu, library_box);

    GtkGesture *rc = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rc), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (rc), GTK_PHASE_CAPTURE);
    g_signal_connect (rc, "pressed", G_CALLBACK (on_grid_secondary), NULL);
    gtk_widget_add_controller (U.grid, GTK_EVENT_CONTROLLER (rc));

    /* on the grid only: Delete in the search box still edits text */
    GtkEventController *kc = gtk_event_controller_key_new ();
    g_signal_connect (kc, "key-pressed", G_CALLBACK (on_grid_key), NULL);
    gtk_widget_add_controller (U.grid, kc);
}

/* build_details: the title with "Delete…" at its right end */
static GtkWidget *delete_title_row (GtkWidget *title)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (row), title);
    U.delete_btn = gtk_button_new_with_mnemonic ("_Delete…");
    gtk_widget_set_valign (U.delete_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (U.delete_btn,
                                 "Move this wallpaper to the trash (Delete on a tile)");
    gtk_widget_set_sensitive (U.delete_btn, FALSE);
    g_signal_connect (U.delete_btn, "clicked", G_CALLBACK (on_delete_clicked), NULL);
    gtk_box_append (GTK_BOX (row), U.delete_btn);
    return row;
}

/* ------------------------------------------------------------------ pause */

static void on_pause_reply (JsonObject *reply, const char *err, gpointer u)
{
    (void) u;
    if (!ui_ipc_ok (reply)) {
        char *m = g_strdup_printf ("Pause/resume failed: %s", reply ? wpj_str (reply, "error", "?") : err);
        set_status (m, TRUE);
        g_free (m);
    }
    sup_poll_now ();   /* the toggle follows the daemon's real state */
}

static void on_pause_toggled (GtkToggleButton *b, gpointer u)
{
    (void) u;
    if (U.pause_building)
        return;
    gboolean p = gtk_toggle_button_get_active (b);
    gtk_button_set_label (GTK_BUTTON (b), p ? "Resume" : "Pause");
    ui_ipc_send (p ? "{\"cmd\":\"pause\"}" : "{\"cmd\":\"resume\"}", 0, on_pause_reply, NULL);
}

/* ======================================================= engine settings */

/* ------------------------------------------------------------- tray icon
 * walpe-tray is its own process: the setting is tray.ini
 * start_with_gui (~/.config/walpe/tray.ini, default on). On: the GUI starts
 * the tray whenever it opens, and a per-user XDG autostart entry starts it
 * at login. Off: tray.ini says so, the per-user entry becomes Hidden=true
 * (which also masks a system-wide /etc/xdg/autostart entry) and the running
 * tray is asked to quit over D-Bus. The tray outlives the GUI. */

static char *tray_binary (void)
{
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname (exe) : NULL;
    char *p = dir ? g_build_filename (dir, "walpe-tray", NULL) : NULL;
    g_free (exe);
    g_free (dir);
    if (p && g_file_test (p, G_FILE_TEST_IS_EXECUTABLE))
        return p;
    g_free (p);
    return g_find_program_in_path ("walpe-tray");
}

static char *tray_autostart_path (void)
{
    return g_build_filename (g_get_user_config_dir (), "autostart", "walpe-tray.desktop", NULL);
}

static gboolean tray_enabled (void)
{
    WalpeTrayConf *tc = walpe_tray_conf_load ();
    gboolean on = tc->start_with_gui;
    walpe_tray_conf_free (tc);
    return on;
}

/* start walpe-tray unless one is already on the bus (a second tray would
 * exit by itself anyway: it is a unique GApplication) */
static gboolean tray_start (char **err)
{
    char *bin = tray_binary ();
    if (!bin) {
        if (err)
            *err = g_strdup ("walpe-tray is not installed");
        return FALSE;
    }
    g_free (bin);
    if (walpe_tray_is_running (500))
        return TRUE;
    GError *e = NULL;
    gboolean ok = walpe_tray_spawn (&e);
    if (!ok && err)
        *err = g_strdup (e ? e->message : "cannot start walpe-tray");
    g_clear_error (&e);
    return ok;
}

/* per-user login entry: Exec= the binary found now, or Hidden=true */
static gboolean tray_autostart_write (gboolean on, char **err)
{
    char *p = tray_autostart_path ();
    char *dir = g_path_get_dirname (p);
    g_mkdir_with_parents (dir, 0755);
    char *txt;
    if (on) {
        char *bin = tray_binary ();
        /* Exec= quoting rules: a path with spaces goes in double quotes */
        GString *ex = g_string_new (NULL);
        const char *src = bin ? bin : "walpe-tray";
        gboolean q = strpbrk (src, " \t\"\\$`") != NULL;
        if (q)
            g_string_append_c (ex, '"');
        for (const char *c = src; *c; c++) {
            if (q && strchr ("\"`$\\", *c))
                g_string_append_c (ex, '\\');
            g_string_append_c (ex, *c);
        }
        if (q)
            g_string_append_c (ex, '"');
        char *exec = g_string_free (ex, FALSE);
        txt = g_strdup_printf ("[Desktop Entry]\nType=Application\nName=WalPE Tray\n"
                               "Comment=Tray icon for WalPE: live wallpapers for Linux (plays Wallpaper Engine content)\n"
                               "Exec=%s\n"
                               "Icon=walpe\nTerminal=false\nNoDisplay=true\n"
                               "X-GNOME-Autostart-enabled=true\nX-GNOME-Autostart-Delay=2\n", exec);
        g_free (exec);
        g_free (bin);
    } else {
        txt = g_strdup ("[Desktop Entry]\nType=Application\nName=WalPE Tray\nExec=walpe-tray\n"
                        "Hidden=true\nX-GNOME-Autostart-enabled=false\n");
    }
    GError *e = NULL;
    gboolean ok = g_file_set_contents (p, txt, -1, &e);
    if (!ok && err)
        *err = g_strdup (e->message);
    g_clear_error (&e);
    g_free (txt);
    g_free (dir);
    g_free (p);
    return ok;
}

static gboolean tray_set (gboolean on, char **err)
{
    WalpeTrayConf *tc = walpe_tray_conf_load ();
    tc->start_with_gui = on;
    walpe_tray_conf_save (tc);
    walpe_tray_conf_free (tc);
    gboolean ok = tray_autostart_write (on, err);
    if (on) {
        if (ok)
            ok = tray_start (err);
        else
            tray_start (NULL);
    } else {
        walpe_tray_request_quit ();
    }
    return ok;
}

typedef struct {
    GtkWidget *tray, *tray_info;
    GtkWidget *fps, *volume, *fs, *covered, *dpms, *transparent, *parallax, *assets, *steam;
    GtkWidget *shared, *fxcache, *texdown, *covtrans, *webaccel, *webaccel_info, *hwdec;
    GtkWidget *vlimit;
    GtkWidget *software, *software_info;   /* "Render on CPU" + what renders now */
    GtkWidget *focus, *focus_fps, *battery, *battery_fps, *previews;   /* opt-in rules */
    GtkWidget *idle;                 /* pause_idle: seconds without input, 0 = off */
    GtkWidget *rule_note[5];         /* under a pause setting nothing in the session can
                                      * drive (status "pause_rules"); order of rule_keys */
    GtkWidget *plowres, *pdensity;   /* particles: reduced-resolution layer, density */
    GtkWidget *fxspec, *fxfast;      /* effect pass programs: fx_specialize, fx_quality = fast */
    GtkWidget *assets_info, *err, *libdir, *shared_info;
    gboolean building;
    guint fps_id, vol_id, pden_id;
} Settings;

static Settings SW;

static void on_set_reply (JsonObject *reply, const char *err, gpointer u)
{
    char *what = u;
    if (ui_ipc_ok (reply)) {
        /* in effect, but the engine could not write config.json */
        const char *se = wpj_str (reply, "save_error", NULL);
        char *m = se ? g_strdup_printf ("%s applied, but not saved (%s): it is lost when the "
                                        "engine restarts.", what, se)
                     : g_strdup_printf ("%s applied.", what);
        set_status (m, se != NULL);
        g_free (m);
        if (SW.err)
            gtk_label_set_text (GTK_LABEL (SW.err), "");
    } else {
        const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
        char *m = g_strdup_printf ("%s not applied: %s", what, e ? e : "error");
        set_status (m, TRUE);
        /* the control shows what is in force again (a switch the engine
         * refused stayed flipped), then the reason */
        settings_follow ();
        if (SW.err)
            gtk_label_set_text (GTK_LABEL (SW.err), m);
        g_free (m);
    }
    g_free (what);
    sup_poll_now ();
}

static void send_set (const char *key, JsonNode *val, const char *what)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "set");
    json_builder_set_member_name (b, key);
    json_builder_add_value (b, val);
    json_builder_end_object (b);
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_AUTOSTART, on_set_reply, g_strdup (what));
    g_object_unref (b);
}

static void set_bool (const char *key, gboolean v, const char *what)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_boolean (n, v);
    send_set (key, n, what);
}

static void set_string (const char *key, const char *v, const char *what)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_string (n, v ? v : "");
    send_set (key, n, what);
}

static gboolean fps_fire (gpointer u)
{
    (void) u;
    SW.fps_id = 0;
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_int (n, gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (SW.fps)));
    send_set ("fps", n, "Frame rate cap");
    return G_SOURCE_REMOVE;
}

/* a choice (GtkDropDown): the daemon gets the value name at that index */
static void on_choice_set (GObject *o, GParamSpec *ps, gpointer key)
{
    (void) ps;
    if (SW.building)
        return;
    const char *const *vals = g_object_get_data (o, "walpe-values");
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (o));
    set_string (key, vals[i], g_object_get_data (o, "walpe-what"));
    if (!strcmp (key, "preview_anim"))
        thumbs_set_anim_mode ((int) i);     /* applies at once, here */
}

static GtkWidget *mk_choice (const char *key, const char *what, const char *const *labels,
                             const char *const *values)
{
    GtkWidget *d = gtk_drop_down_new_from_strings (labels);
    g_object_set_data (G_OBJECT (d), "walpe-values", (gpointer) values);
    g_object_set_data (G_OBJECT (d), "walpe-what", (gpointer) what);
    g_signal_connect (d, "notify::selected", G_CALLBACK (on_choice_set), (gpointer) key);
    return d;
}

/* a number (GtkSpinButton), sent when it was left alone for a moment */
static gboolean num_fire (gpointer sb)
{
    g_object_set_data (sb, "walpe-timer", NULL);
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_int (n, gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (sb)));
    send_set (g_object_get_data (sb, "walpe-key"), n, g_object_get_data (sb, "walpe-what"));
    return G_SOURCE_REMOVE;
}

static void on_num (GtkSpinButton *sb, gpointer u)
{
    (void) u;
    if (SW.building)
        return;
    guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (sb), "walpe-timer"));
    if (id)
        g_source_remove (id);
    id = g_timeout_add (200, num_fire, sb);
    g_object_set_data (G_OBJECT (sb), "walpe-timer", GUINT_TO_POINTER (id));
}

static GtkWidget *mk_num (const char *key, const char *what, int lo, int hi)
{
    GtkWidget *s = gtk_spin_button_new_with_range (lo, hi, 1);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (s), TRUE);
    walpe_num_field_keep_when_empty (s);
    g_object_set_data (G_OBJECT (s), "walpe-key", (gpointer) key);
    g_object_set_data (G_OBJECT (s), "walpe-what", (gpointer) what);
    g_signal_connect (s, "value-changed", G_CALLBACK (on_num), NULL);
    return s;
}

static void on_fps (GtkSpinButton *sb, gpointer u)
{
    (void) sb; (void) u;
    if (SW.building)
        return;
    if (SW.fps_id)
        g_source_remove (SW.fps_id);
    SW.fps_id = g_timeout_add (150, fps_fire, NULL);
}

static gboolean pden_fire (gpointer u)
{
    (void) u;
    SW.pden_id = 0;
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_int (n, gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (SW.pdensity)));
    send_set ("particle_density", n, "Particle density");
    return G_SOURCE_REMOVE;
}

static void on_pdensity (GtkSpinButton *sb, gpointer u)
{
    (void) sb; (void) u;
    if (SW.building)
        return;
    if (SW.pden_id)
        g_source_remove (SW.pden_id);
    SW.pden_id = g_timeout_add (150, pden_fire, NULL);
}

static void on_plowres (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) ps; (void) u;
    if (SW.building)
        return;
    set_string ("particle_lowres",
                walpe_particle_lowres_name ((int) gtk_drop_down_get_selected (GTK_DROP_DOWN (o))),
                "Soft particles");
}

static gboolean vol_fire (gpointer u)
{
    (void) u;
    SW.vol_id = 0;
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_double (n, gtk_range_get_value (GTK_RANGE (SW.volume)) / 100.0);
    send_set ("volume", n, "Volume");
    return G_SOURCE_REMOVE;
}

static void on_volume (GtkRange *r, gpointer u)
{
    (void) r; (void) u;
    if (SW.building)
        return;
    if (SW.vol_id)
        g_source_remove (SW.vol_id);
    SW.vol_id = g_timeout_add (150, vol_fire, NULL);
}

static void on_switch_set (GObject *o, GParamSpec *ps, gpointer key)
{
    (void) ps;
    if (SW.building)
        return;
    set_bool (key, gtk_switch_get_active (GTK_SWITCH (o)),
              g_object_get_data (o, "walpe-what"));
}

static void on_tray_switch (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) ps; (void) u;
    if (SW.building)
        return;
    char *err = NULL;
    gboolean on = gtk_switch_get_active (GTK_SWITCH (o));
    if (tray_set (on, &err))
        set_status (on ? "Tray icon started; it also starts at login." : "Tray icon turned off.", FALSE);
    else {
        char *m = g_strdup_printf ("Tray icon: %s", err ? err : "could not write the autostart entry");
        set_status (m, TRUE);
        g_free (m);
    }
    g_free (err);
}

static void entry_set_commit (GtkWidget *e)
{
    if (SW.building)
        return;
    const char *key = g_object_get_data (G_OBJECT (e), "walpe-key");
    const char *t = gtk_editable_get_text (GTK_EDITABLE (e));
    const char *last = g_object_get_data (G_OBJECT (e), "walpe-last");
    if (!g_strcmp0 (last, t))
        return;
    g_object_set_data_full (G_OBJECT (e), "walpe-last", g_strdup (t), g_free);
    set_string (key, t, g_object_get_data (G_OBJECT (e), "walpe-what"));
}

static void on_entry_set_activate (GtkEntry *e, gpointer u) { (void) u; entry_set_commit (GTK_WIDGET (e)); }

static void on_entry_set_leave (GtkEventControllerFocus *fc, gpointer u)
{
    (void) u;
    entry_set_commit (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (fc)));
}

/* ---- pause settings that depend on what the session reports
 * "Pause for fullscreen apps" can only work where the engine finds out
 * that a window is fullscreen: always on X11, on Wayland only where the
 * compositor offers the protocol for it. The engine's status says per
 * setting whether something drives it ("pause_rules": works, note); a
 * setting that cannot work gets that sentence under its row instead of
 * silently doing nothing. */
static const char *const rule_keys[] = { "pause_fullscreen", "pause_covered", "pause_dpms",
                                         "focus_action", "pause_idle" };

static void rule_note_row (GtkWidget *grid, int row, const char *key)
{
    GtkWidget *l = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 80);
    gtk_widget_add_css_class (l, "walpe-settings-sub");
    gtk_widget_add_css_class (l, "walpe-settings-note");
    gtk_widget_set_visible (l, FALSE);
    gtk_grid_attach (GTK_GRID (grid), l, 0, row, 2, 1);
    for (guint i = 0; i < G_N_ELEMENTS (rule_keys); i++)
        if (!strcmp (rule_keys[i], key))
            SW.rule_note[i] = l;
}

static void rule_notes_fill (JsonObject *st)
{
    JsonObject *rules = st ? wpj_obj (st, "pause_rules") : NULL;
    for (guint i = 0; i < G_N_ELEMENTS (rule_keys); i++) {
        if (!SW.rule_note[i])
            continue;
        JsonObject *o = rules ? wpj_obj (rules, rule_keys[i]) : NULL;
        const char *note = o ? wpj_str (o, "note", NULL) : NULL;
        if (note && *note) {
            /* a sentence of its own: capital first letter */
            char *t = g_strdup (note);
            t[0] = g_ascii_toupper (t[0]);
            gtk_label_set_text (GTK_LABEL (SW.rule_note[i]), t);
            g_free (t);
        }
        gtk_widget_set_visible (SW.rule_note[i], note && *note);
    }
}

static GtkWidget *settings_row (GtkWidget *grid, int row, const char *title, const char *sub,
                                GtkWidget *ctl)
{
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (v, TRUE);
    GtkWidget *t = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (t), 0);
    gtk_label_set_wrap (GTK_LABEL (t), TRUE);
    gtk_label_set_width_chars (GTK_LABEL (t), 34);
    gtk_label_set_max_width_chars (GTK_LABEL (t), 60);
    gtk_box_append (GTK_BOX (v), t);
    GtkWidget *s = NULL;
    if (sub) {
        s = gtk_label_new (sub);
        gtk_label_set_xalign (GTK_LABEL (s), 0);
        gtk_label_set_wrap (GTK_LABEL (s), TRUE);
        gtk_label_set_width_chars (GTK_LABEL (s), 34);
        gtk_label_set_max_width_chars (GTK_LABEL (s), 60);
        gtk_widget_add_css_class (s, "walpe-settings-sub");
        gtk_box_append (GTK_BOX (v), s);
    }
    gtk_widget_set_margin_top (v, 6);
    gtk_widget_set_margin_bottom (v, 6);
    gtk_grid_attach (GTK_GRID (grid), v, 0, row, 1, 1);
    gtk_widget_set_valign (ctl, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (ctl, GTK_ALIGN_END);
    gtk_accessible_update_property (GTK_ACCESSIBLE (ctl), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    gtk_grid_attach (GTK_GRID (grid), ctl, 1, row, 1, 1);
    return s;
}

static void settings_group (GtkWidget *grid, int row, const char *title)
{
    GtkWidget *h = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (h), 0);
    gtk_widget_add_css_class (h, "walpe-settings-group");
    gtk_grid_attach (GTK_GRID (grid), h, 0, row, 2, 1);
}

static GtkWidget *mk_switch (const char *key, const char *what)
{
    GtkWidget *s = gtk_switch_new ();
    g_object_set_data (G_OBJECT (s), "walpe-what", (gpointer) what);
    g_signal_connect (s, "notify::active", G_CALLBACK (on_switch_set), (gpointer) key);
    return s;
}

static GtkWidget *mk_entry (const char *key, const char *what, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (e), placeholder);
    gtk_widget_set_size_request (e, 280, -1);
    g_object_set_data (G_OBJECT (e), "walpe-key", (gpointer) key);
    g_object_set_data (G_OBJECT (e), "walpe-what", (gpointer) what);
    g_signal_connect (e, "activate", G_CALLBACK (on_entry_set_activate), NULL);
    GtkEventController *fc = gtk_event_controller_focus_new ();
    g_signal_connect (fc, "leave", G_CALLBACK (on_entry_set_leave), NULL);
    gtk_widget_add_controller (e, fc);
    return e;
}

static void entry_load (GtkWidget *e, const char *v)
{
    gtk_editable_set_text (GTK_EDITABLE (e), v ? v : "");
    g_object_set_data_full (G_OBJECT (e), "walpe-last", g_strdup (v ? v : ""), g_free);
}

/* "Render on CPU": the line under the switch says what renders now and,
 * when the switch could not be honoured, why (status: gl_renderer,
 * software_active, software_note) */
static void settings_software_info (void)
{
    if (!SW.software_info)
        return;
    JsonObject *st = U.last_state;
    const char *r = st ? wpj_str (st, "gl_renderer", "") : "";
    const char *note = st ? wpj_str (st, "software_note", "") : "";
    gboolean cpu = st && wpj_bool (st, "software_active", FALSE);
    if (st && json_object_has_member (st, "software_render")) {
        /* the switch mirrors the engine (walpe-ctl may have changed it) */
        gboolean was = SW.building;
        SW.building = TRUE;
        gtk_switch_set_active (GTK_SWITCH (SW.software), wpj_bool (st, "software_render", FALSE));
        SW.building = was;
    }
    GString *s = g_string_new ("Uses the processor instead of the graphics card: no GPU load, but "
                               "high CPU load and fewer frames. The engine restarts itself.");
    if (*note)
        g_string_append_printf (s, "\nNot available, rendering on the GPU: %s", note);
    else if (*r)
        g_string_append_printf (s, "\nNow rendering on the %s: %s", cpu ? "CPU" : "GPU", r);
    gtk_label_set_text (GTK_LABEL (SW.software_info), s->str);
    g_string_free (s, TRUE);
}

/* a number the user is changing right now (its send is still waiting) is
 * left alone while the window follows the engine */
static gboolean num_pending (GtkWidget *sb)
{
    return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (sb), "walpe-timer")) != 0;
}

/* the same for a text field with typing in it that was not sent yet (it
 * is sent on Enter or when the field is left) */
static gboolean entry_editing (GtkWidget *e)
{
    const char *last = g_object_get_data (G_OBJECT (e), "walpe-last");
    return last && g_strcmp0 (last, gtk_editable_get_text (GTK_EDITABLE (e))) != 0;
}

/* values: config.json (what the daemon saved) + the last status. follow =
 * the window is open and something else changed a setting: what the user
 * is editing this moment stays, and so does the error line. */
static void settings_fill (gboolean follow)
{
    WalpeConfig *c = walpe_config_load ();
    walpe_config_free (U.cfg);
    U.cfg = c;
    SW.building = TRUE;
    JsonObject *st = U.last_state;
    if (!(follow && SW.fps_id))
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (SW.fps), st ? wpj_num (st, "fps", c->fps) : c->fps);
    if (!(follow && SW.vol_id))
        gtk_range_set_value (GTK_RANGE (SW.volume), 100.0 * (st ? wpj_num (st, "volume", c->volume) : c->volume));
    gtk_switch_set_active (GTK_SWITCH (SW.fs), c->pause_fullscreen);
    gtk_switch_set_active (GTK_SWITCH (SW.covered), c->pause_covered);
    gtk_switch_set_active (GTK_SWITCH (SW.dpms), c->pause_dpms);
    gtk_switch_set_active (GTK_SWITCH (SW.parallax), c->parallax);
    gtk_switch_set_active (GTK_SWITCH (SW.shared), c->render_shared);
    gtk_switch_set_active (GTK_SWITCH (SW.fxcache), st ? wpj_bool (st, "fx_cache", c->fx_cache) : c->fx_cache);
    gtk_switch_set_active (GTK_SWITCH (SW.texdown), c->tex_downscale);
    gtk_switch_set_active (GTK_SWITCH (SW.fxspec),
                           st ? wpj_bool (st, "fx_specialize", c->fx_specialize) : c->fx_specialize);
    gtk_switch_set_active (GTK_SWITCH (SW.fxfast),
                           !g_strcmp0 (st ? wpj_str (st, "fx_quality", c->fx_quality) : c->fx_quality, "fast"));
    gtk_switch_set_active (GTK_SWITCH (SW.webaccel),
                           st ? wpj_bool (st, "web_accel", c->web_accel) : c->web_accel);
    {
        /* what the daemon really does with it (it tests before it trusts) */
        const char *wr = st ? wpj_str (st, "web_render", NULL) : NULL;
        const char *wg = st ? wpj_str (st, "web_webgl", NULL) : NULL;
        char *wi = g_strdup_printf ("Web wallpapers are drawn by the graphics card instead of one CPU "
                                    "thread. Off by default: WebKit crashed with it on some NVIDIA "
                                    "set-ups. The engine tests it at start and falls back by itself; "
                                    "switching restarts the engine.%s%s%s%s",
                                    wr ? "\nNow: " : "", wr ? wr : "",
                                    wg ? "\nWebGL in pages: " : "", wg ? wg : "");
        gtk_label_set_text (GTK_LABEL (SW.webaccel_info), wi);
        g_free (wi);
    }
    gtk_switch_set_active (GTK_SWITCH (SW.hwdec), st ? wpj_bool (st, "video_hwdec", c->video_hwdec) : c->video_hwdec);
    gtk_switch_set_active (GTK_SWITCH (SW.vlimit), st ? wpj_bool (st, "video_limit", c->video_limit) : c->video_limit);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (SW.plowres), (guint) CLAMP (
        walpe_particle_lowres_from_name (st ? wpj_str (st, "particle_lowres", NULL) : NULL,
                                         c->particle_lowres), 0, 2));
    if (!(follow && SW.pden_id))
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (SW.pdensity),
                                   st ? wpj_num (st, "particle_density", c->particle_density)
                                      : c->particle_density);
    gtk_switch_set_active (GTK_SWITCH (SW.covtrans), c->cover_transparent);
    gtk_switch_set_active (GTK_SWITCH (SW.software), c->software_render);
    settings_software_info ();
    gtk_drop_down_set_selected (GTK_DROP_DOWN (SW.focus), (guint) c->focus_action);
    if (!(follow && num_pending (SW.focus_fps)))
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (SW.focus_fps), c->focus_fps);
    gtk_switch_set_active (GTK_SWITCH (SW.battery), c->pause_battery);
    if (!(follow && num_pending (SW.battery_fps)))
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (SW.battery_fps), c->battery_fps);
    if (!(follow && num_pending (SW.idle)))
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (SW.idle), c->pause_idle);
    rule_notes_fill (st);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (SW.previews), (guint) c->preview_anim);
    thumbs_set_anim_mode (c->preview_anim);
    {
        const char *off = st ? wpj_str (st, "shared_off", NULL) : NULL;
        char *si = off && *off
            ? g_strdup_printf ("Same-size monitors showing one scene render it once. Not in use now: %s", off)
            : g_strdup ("Same-size monitors showing one scene render it once (less GPU). "
                        "Turn off if one monitor stutters.");
        gtk_label_set_text (GTK_LABEL (SW.shared_info), si);
        g_free (si);
    }
    if (!(follow && entry_editing (SW.transparent)))
        entry_load (SW.transparent, c->transparent_windows);
    if (!(follow && entry_editing (SW.assets)))
        entry_load (SW.assets, c->we_assets);
    if (!(follow && entry_editing (SW.steam)))
        entry_load (SW.steam, c->steam_user);
    const char *wa = st ? wpj_str (st, "we_assets", "") : "";
    const char *src = st ? wpj_str (st, "we_assets_source", "") : "";
    char *info = *wa ? g_strdup_printf ("In use: %s (%s)", wa, src)
                     : g_strdup ("Not found: scenes that use Wallpaper Engine's built-in effects "
                                 "render without them.");
    gtk_label_set_text (GTK_LABEL (SW.assets_info), info);
    g_free (info);
    gtk_label_set_text (GTK_LABEL (SW.libdir), c->wallpapers_dir);
    {
        char *tb = tray_binary ();
        gtk_switch_set_active (GTK_SWITCH (SW.tray), tb != NULL && tray_enabled ());
        gtk_widget_set_sensitive (SW.tray, tb != NULL);
        gtk_label_set_text (GTK_LABEL (SW.tray_info),
                            tb ? "Starts walpe-tray with WalPE and at login; off quits it."
                               : "walpe-tray is not installed next to walpe or in PATH.");
        g_free (tb);
    }
    if (!follow)
        gtk_label_set_text (GTK_LABEL (SW.err), "");
    SW.building = FALSE;
}

static void settings_load (void)
{
    settings_fill (FALSE);
}

/* the settings window, when it is open, shows the engine's settings as
 * they are now (changed by walpe-ctl, the tray, or refused by the engine) */
static void settings_follow (void)
{
    if (U.settings && gtk_widget_get_visible (U.settings))
        settings_fill (TRUE);
}

static GtkWidget *settings_window (void)
{
    if (U.settings)
        return U.settings;
    GtkWidget *w = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w), "Engine settings");
    gtk_window_set_transient_for (GTK_WINDOW (w), GTK_WINDOW (U.win));
    gtk_window_set_hide_on_close (GTK_WINDOW (w), TRUE);
    gtk_window_set_default_size (GTK_WINDOW (w), 780, 760);
    GtkWidget *sc = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *g = gtk_grid_new ();
    gtk_widget_add_css_class (g, "walpe-settings");
    gtk_grid_set_column_spacing (GTK_GRID (g), 24);
    gtk_grid_set_row_spacing (GTK_GRID (g), 2);
    int r = 0;
    GtkWidget *intro = gtk_label_new ("Every change applies immediately; there is nothing to save.");
    gtk_label_set_xalign (GTK_LABEL (intro), 0);
    gtk_widget_add_css_class (intro, "walpe-settings-sub");
    gtk_grid_attach (GTK_GRID (g), intro, 0, r++, 2, 1);

    settings_group (g, r++, "Performance");
    SW.fps = gtk_spin_button_new_with_range (5, 200, 1);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (SW.fps), TRUE);
    walpe_num_field_keep_when_empty (SW.fps);    /* (emptied and left, it set 5) */
    g_signal_connect (SW.fps, "value-changed", G_CALLBACK (on_fps), NULL);
    settings_row (g, r++, "Frame rate cap",
                  "Scenes and web wallpapers render at most this many frames per second "
                  "(5–200), and never more than the monitor shows. Videos keep their own "
                  "frame rate.", SW.fps);

    SW.shared = mk_switch ("render_shared", "Shared rendering");
    SW.shared_info = settings_row (g, r++, "Render once for identical monitors", "…", SW.shared);
    SW.fxcache = mk_switch ("fx_cache", "Effect cache");
    settings_row (g, r++, "Cache static effect output",
                  "Effects whose result does not change are not recomputed every frame.",
                  SW.fxcache);
    SW.fxspec = mk_switch ("fx_specialize", "Specialised effect shaders");
    settings_row (g, r++, "Specialise effect shaders",
                  "Effect settings that cannot change are compiled into the shaders: a little "
                  "less GPU work per pixel, same picture (each such shader is first compared "
                  "with the plain one on this GPU and only used if it draws the same). "
                  "Reloads the wallpapers.", SW.fxspec);
    SW.fxfast = mk_switch ("fx_fast", "Fast effect quality");
    settings_row (g, r++, "Fast effect quality",
                  "Stacked wave, shake and ripple effects of a layer are drawn in one pass; "
                  "blur and light rays read fewer texels. The picture may differ slightly. "
                  "Reloads the wallpapers.", SW.fxfast);
    SW.texdown = mk_switch ("tex_downscale", "Texture downscaling");
    settings_row (g, r++, "Downscale oversized textures",
                  "Textures larger than needed are stored smaller (less video memory). "
                  "Applies when a wallpaper is loaded.", SW.texdown);
    SW.hwdec = mk_switch ("video_hwdec", "Hardware video decoding");
    settings_row (g, r++, "Decode videos on the graphics card",
                  "Videos (video wallpapers and video layers of scenes) are decoded by a hardware "
                  "decoder (VA-API, NVDEC, VDPAU, Vulkan Video or a V4L2 decoder through FFmpeg, "
                  "whichever this machine offers, the graphics card that renders first) instead of "
                  "the CPU when that works; otherwise the CPU decodes as before. Videos that are "
                  "playing switch at once. One decoder can be chosen by name with "
                  "walpe-ctl set hwdec=NAME.",
                  SW.hwdec);
    SW.vlimit = mk_switch ("video_limit", "Videos follow the frame rate cap");
    settings_row (g, r++, "Videos follow the frame rate cap",
                  "A video wallpaper is shown at most as often as the frame rate cap (and the "
                  "battery / focus limits) allows, and pictures of a video that no monitor shows "
                  "are not decoded. Off: videos keep their own frame rate.", SW.vlimit);
    /* quality presets and the knobs behind them (perf.c) */
    gtk_grid_attach (GTK_GRID (g), walpe_perf_panel_new (FALSE, set_status), 0, r++, 2, 1);

    /* the size each monitor's wallpaper is rendered at, and the limits
     * that follow it (resdialog.c; the same rows as the "Monitor
     * resolution" dialog, applied at once here) */
    settings_group (g, r++, "Resolution");
    gtk_grid_attach (GTK_GRID (g), walpe_res_ui_settings_new (), 0, r++, 2, 1);

    SW.software = mk_switch ("software_render", "Render on CPU");
    SW.software_info = settings_row (g, r++, "Render on CPU (software rendering)", "…",
                                     SW.software);

    SW.webaccel = mk_switch ("web_accel", "Web hardware acceleration");
    SW.webaccel_info = settings_row (g, r++, "Web hardware acceleration", "…", SW.webaccel);
    {
        static const char *modes[] = { "Off", "Automatic", "Everything large", NULL };
        SW.plowres = gtk_drop_down_new_from_strings (modes);
        gtk_widget_set_valign (SW.plowres, GTK_ALIGN_CENTER);
        g_signal_connect (SW.plowres, "notify::selected", G_CALLBACK (on_plowres), NULL);
        settings_row (g, r++, "Soft particles at reduced resolution",
                      "Fog, smoke, glows and light shafts are drawn into a smaller buffer and "
                      "blended onto the picture once (less GPU). Automatic: only where the "
                      "picture stays the same. Everything large: all big particles, slightly "
                      "softer.", SW.plowres);
        SW.pdensity = gtk_spin_button_new_with_range (5, 100, 5);
        gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (SW.pdensity), TRUE);
        walpe_num_field_keep_when_empty (SW.pdensity);
        g_signal_connect (SW.pdensity, "value-changed", G_CALLBACK (on_pdensity), NULL);
        settings_row (g, r++, "Particle density (%)",
                      "How many of a wallpaper's particles are emitted. 100 = as its author "
                      "made it; lower values thin out rain, snow, fog and sparks for less "
                      "GPU and CPU use.", SW.pdensity);
    }

    settings_group (g, r++, "Pausing");
    SW.fs = mk_switch ("pause_fullscreen", "Pause for fullscreen apps");
    settings_row (g, r++, "Pause while a fullscreen app is focused", NULL, SW.fs);
    rule_note_row (g, r++, "pause_fullscreen");
    SW.covered = mk_switch ("pause_covered", "Pause when covered");
    settings_row (g, r++, "Pause a monitor when windows cover it",
                  "Counts as covered from 85 % of the monitor.", SW.covered);
    rule_note_row (g, r++, "pause_covered");
    SW.dpms = mk_switch ("pause_dpms", "Pause while the display sleeps");
    settings_row (g, r++, "Pause while the display sleeps", "DPMS standby / screensaver.", SW.dpms);
    rule_note_row (g, r++, "pause_dpms");
    SW.covtrans = mk_switch ("cover_transparent", "See-through windows cover");
    settings_row (g, r++, "See-through windows count as covering",
                  "On: a translucent terminal over the wallpaper pauses it too. Off: the "
                  "windows listed below and translucent windows never cover.", SW.covtrans);
    SW.transparent = mk_entry ("transparent_windows", "See-through windows", "st,foot");
    settings_row (g, r++, "See-through windows",
                  "Comma-separated window classes (WM_CLASS) that never count as covering, "
                  "e.g. translucent terminals. Applied on Enter.", SW.transparent);

    {
        /* what Wallpaper Engine calls "other application focused"; the
         * fullscreen and covered (maximized) cases are the switches above */
        static const char *const fl[] = { "Keep running", "Limit the frame rate", "Pause", NULL };
        static const char *const fv[] = { "run", "limit", "pause" };
        SW.focus = mk_choice ("focus_action", "While an application is focused", fl, fv);
        settings_row (g, r++, "While an application is focused",
                      "Applies to the monitor the focused window is on. The wallpaper runs "
                      "normally again when the desktop itself has the focus.", SW.focus);
        rule_note_row (g, r++, "focus_action");
        SW.focus_fps = mk_num ("focus_fps", "Frame rate while an application is focused", 1, 200);
        settings_row (g, r++, "Frame rate while an application is focused",
                      "Used by \"Limit the frame rate\" (scenes and web wallpapers).", SW.focus_fps);
        SW.battery = mk_switch ("pause_battery", "Pause on battery");
        settings_row (g, r++, "Pause on battery power", NULL, SW.battery);
        SW.battery_fps = mk_num ("battery_fps", "Frame rate on battery", 0, 200);
        settings_row (g, r++, "Frame rate limit on battery power", "0 = no limit.", SW.battery_fps);
        SW.idle = mk_num ("pause_idle", "Pause after idle time", 0, 86400);
        settings_row (g, r++, "Pause after this many seconds without input",
                      "0 = off. Counts keyboard and pointer idle time as the session reports "
                      "it; a playing video that keeps the screen awake keeps the wallpaper "
                      "running too.", SW.idle);
        rule_note_row (g, r++, "pause_idle");
    }

    settings_group (g, r++, "Library");
    {
        static const char *const pl[] = { "Always", "Under the pointer", "Off", NULL };
        static const char *const pv[] = { "always", "hover", "off" };
        SW.previews = mk_choice ("preview_anim", "Animated previews", pl, pv);
        settings_row (g, r++, "Animated previews",
                      "GIF previews play in the library while their tiles are visible and "
                      "this window is the active one.", SW.previews);
    }

    settings_group (g, r++, "Desktop");
    SW.tray = gtk_switch_new ();
    g_signal_connect (SW.tray, "notify::active", G_CALLBACK (on_tray_switch), NULL);
    SW.tray_info = settings_row (g, r++, "Start tray icon", "…", SW.tray);

    settings_group (g, r++, "Interaction");
    SW.parallax = mk_switch ("parallax", "Mouse parallax");
    settings_row (g, r++, "Mouse parallax", "Scene layers follow the pointer.", SW.parallax);

    settings_group (g, r++, "Audio");
    SW.volume = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value (GTK_SCALE (SW.volume), TRUE);
    gtk_scale_set_value_pos (GTK_SCALE (SW.volume), GTK_POS_RIGHT);
    gtk_widget_set_size_request (SW.volume, 240, -1);
    g_signal_connect (SW.volume, "value-changed", G_CALLBACK (on_volume), NULL);
    settings_row (g, r++, "Wallpaper volume",
                  "The volume of wallpaper sound. Scenes play their sound layers once that is "
                  "switched on: walpe-ctl set scene_sound=on.", SW.volume);

    settings_group (g, r++, "Content");
    SW.assets = mk_entry ("we_assets", "Wallpaper Engine assets folder", "auto-detect");
    SW.assets_info = settings_row (g, r++, "Wallpaper Engine assets folder", "…", SW.assets);
    SW.steam = mk_entry ("steam_user", "Steam account", "account name (never a password)");
    settings_row (g, r++, "Steam account for workshop installs",
                  "steamcmd's cached login is used; WalPE never sees a password.", SW.steam);
    SW.libdir = gtk_label_new ("");
    gtk_label_set_selectable (GTK_LABEL (SW.libdir), TRUE);
    gtk_label_set_ellipsize (GTK_LABEL (SW.libdir), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars (GTK_LABEL (SW.libdir), 40);
    settings_row (g, r++, "Library folder", "Set in config.json (wallpapers_dir).", SW.libdir);

    SW.err = gtk_label_new ("");
    gtk_label_set_wrap (GTK_LABEL (SW.err), TRUE);
    gtk_label_set_xalign (GTK_LABEL (SW.err), 0);
    gtk_widget_add_css_class (SW.err, "error");
    gtk_grid_attach (GTK_GRID (g), SW.err, 0, r++, 2, 1);

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), g);
    gtk_window_set_child (GTK_WINDOW (w), sc);
    U.settings = w;
    return w;
}

static void show_settings (void)
{
    GtkWidget *w = settings_window ();
    settings_load ();
    gtk_window_present (GTK_WINDOW (w));
    sup_poll_now ();
}

/* ================================================================ import */

typedef struct { GPtrArray *files; char *dest; } CopyJob;

static void copy_job_free (gpointer p)
{
    CopyJob *j = p;
    g_ptr_array_unref (j->files);
    g_free (j->dest);
    g_free (j);
}

static void copy_thread (GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    CopyJob *j = data;
    GString *errs = g_string_new (NULL);
    int n = 0;
    for (guint i = 0; i < j->files->len; i++) {
        GError *err = NULL;
        char *dst = walpe_store_import_file (j->files->pdata[i], j->dest, &err);
        if (dst)
            n++;
        else if (err)
            g_string_append_printf (errs, "%s%s", errs->len ? "; " : "", err->message);
        g_free (dst);
        g_clear_error (&err);
    }
    char *msg = errs->len ? g_strdup_printf ("Imported %d file(s); failed: %s", n, errs->str)
                          : g_strdup_printf ("Imported %d file(s).", n);
    g_string_free (errs, TRUE);
    g_task_return_pointer (t, msg, g_free);
}

static void copy_done (GObject *s, GAsyncResult *res, gpointer u)
{
    (void) s; (void) u;
    char *msg = g_task_propagate_pointer (G_TASK (res), NULL);
    set_status (msg, msg && strstr (msg, "failed"));
    g_free (msg);
    refresh_model ();
}

static gboolean path_is_video (const char *p)
{
    static const char *exts[] = { "mp4", "mkv", "webm", "mov", "avi", "m4v",
                                  "ts", "flv", "wmv", "mpg", "mpeg", "ogv", "3gp", NULL };
    const char *dot = strrchr (p, '.');
    if (!dot)
        return FALSE;
    dot++;
    for (int i = 0; exts[i]; i++)
        if (!g_ascii_strcasecmp (dot, exts[i]))
            return TRUE;
    return FALSE;
}

static GQueue import_q = G_QUEUE_INIT;
static guint import_poll_id;
static gboolean import_poll_inflight;
static void import_send_next (void);

static void on_import_status (JsonObject *reply, const char *err, gpointer u)
{
    (void) u; (void) err;
    import_poll_inflight = FALSE;
    JsonObject *st = reply ? wpj_obj (reply, "state") : NULL;
    if (!st) {
        if (import_poll_id) {
            g_source_remove (import_poll_id);
            import_poll_id = 0;
        }
        set_status ("Import: the engine went away.", TRUE);
        /* the videos still waiting are not imported behind the user's
         * back with the next import, whenever that is */
        g_queue_clear_full (&import_q, g_free);
        return;
    }
    if (!wpj_bool (st, "import_active", FALSE)) {
        if (import_poll_id) {
            g_source_remove (import_poll_id);
            import_poll_id = 0;
        }
        const char *e = wpj_str (st, "import_error", NULL);
        gboolean conv = !g_strcmp0 (wpj_str (st, "import_purpose", NULL), "convert");
        if (e && *e)
            set_status (e, TRUE);
        else
            set_status (conv ? "Video converted; wallpaper set." : "Video imported.", FALSE);
        refresh_model ();
        if (conv)
            sup_poll_now ();
        import_send_next ();
        return;
    }
    int pct = (int) wpj_num (st, "import_percent", -1);
    const char *verb = !g_strcmp0 (wpj_str (st, "import_purpose", NULL), "convert")
                           ? "Converting" : "Importing";
    char *m = pct >= 0 ? g_strdup_printf ("%s %s… %d %%", verb, wpj_str (st, "import_file", ""), pct)
                       : g_strdup_printf ("%s…", verb);
    set_status (m, FALSE);
    g_free (m);
}

static gboolean import_tick (gpointer u)
{
    (void) u;
    if (!import_poll_inflight) {
        import_poll_inflight = TRUE;
        ui_ipc_send ("{\"cmd\":\"status\"}", UI_IPC_POLL, on_import_status, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void import_poll_start (void)
{
    if (!import_poll_id)
        import_poll_id = g_timeout_add (500, import_tick, NULL);
}

static void on_add_reply (JsonObject *reply, const char *err, gpointer u)
{
    (void) u;
    if (ui_ipc_ok (reply)) {
        set_status ("Import started…", FALSE);
        import_poll_start ();
    } else {
        const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
        char *m = g_strdup_printf ("Video import failed: %s", e ? e : "error");
        set_status (m, TRUE);
        g_free (m);
        import_send_next ();
    }
}

static void import_send_next (void)
{
    if (import_poll_id)
        return;
    char *path = g_queue_pop_head (&import_q);
    if (!path)
        return;
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "add");
    json_builder_set_member_name (b, "path");
    json_builder_add_string_value (b, path);
    json_builder_end_object (b);
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW | UI_IPC_AUTOSTART, on_add_reply, NULL);
    g_object_unref (b);
    g_free (path);
}

static void on_files_chosen (GObject *src, GAsyncResult *res, gpointer u)
{
    (void) u;
    GListModel *files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (src), res, NULL);
    if (!files)
        return;
    CopyJob *j = g_new0 (CopyJob, 1);
    j->files = g_ptr_array_new_with_free_func (g_free);
    j->dest = g_strdup (U.cfg->wallpapers_dir);
    int nv = 0;
    for (guint i = 0; i < g_list_model_get_n_items (files); i++) {
        GFile *f = g_list_model_get_item (files, i);
        char *path = g_file_get_path (f);
        if (path && path_is_video (path)) {
            g_queue_push_tail (&import_q, path);
            nv++;
        } else if (path) {
            g_ptr_array_add (j->files, path);
        }
        g_object_unref (f);
    }
    g_object_unref (files);
    if (nv)
        import_send_next ();
    if (j->files->len) {
        set_status ("Copying files into the library…", FALSE);
        GTask *t = g_task_new (NULL, NULL, copy_done, NULL);
        g_task_set_task_data (t, j, copy_job_free);
        g_task_run_in_thread (t, copy_thread);
        g_object_unref (t);
    } else {
        copy_job_free (j);
    }
}

static void on_import (void)
{
    GtkFileDialog *d = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (d, "Import wallpapers (video / GIF / image files)");
    gtk_file_dialog_set_modal (d, FALSE);   /* a stuck portal must not lock the window */
    gtk_file_dialog_open_multiple (d, GTK_WINDOW (U.win), NULL, on_files_chosen, NULL);
    g_object_unref (d);
}

static void on_folder_chosen (GObject *src, GAsyncResult *res, gpointer u)
{
    (void) u;
    GFile *f = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (src), res, NULL);
    if (!f)
        return;
    char *dir = g_file_get_path (f);
    g_object_unref (f);
    if (!dir)
        return;
    /* a folder of projects where some were added and some were not (a name
     * the library already has for another wallpaper): only the count of the
     * added ones was shown, the others were dropped without a word */
    guint failed = 0;
    char *why = NULL;
    guint n = walpe_store_import_folder (dir, U.cfg->wallpapers_dir, &failed, &why);
    if (n) {
        char *m = failed ? g_strdup_printf ("Added %u wallpaper folder(s); %u not added: %s", n,
                                            failed, why ? why : "import failed")
                         : g_strdup_printf ("Added %u wallpaper folder(s).", n);
        set_status (m, failed > 0);
        g_free (m);
        refresh_model ();
    } else {
        set_status (why ? why
                    : "No Wallpaper Engine projects there (expected project.json subfolders).", TRUE);
    }
    g_free (why);
    g_free (dir);
}

static void on_add_folder (void)
{
    GtkFileDialog *d = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (d, "Pick a Wallpaper Engine project folder (or a folder of them)");
    gtk_file_dialog_set_modal (d, FALSE);
    gtk_file_dialog_select_folder (d, GTK_WINDOW (U.win), NULL, on_folder_chosen, NULL);
    g_object_unref (d);
}

/* ----------------------------------------------------- workshop install */

static guint install_poll_id;
static gboolean install_inflight;

static void on_install_status (JsonObject *reply, const char *err, gpointer u)
{
    (void) u; (void) err;
    install_inflight = FALSE;
    JsonObject *st = reply ? wpj_obj (reply, "state") : NULL;
    if (!st) {
        return;   /* missed poll: try again next tick (the engine supervisor reports outages) */
    }
    if (wpj_bool (st, "install_active", FALSE)) {
        char *m = g_strdup_printf ("Workshop: %s", wpj_str (st, "install_phase", "…"));
        set_status (m, FALSE);
        g_free (m);
        return;
    }
    if (install_poll_id) {
        g_source_remove (install_poll_id);
        install_poll_id = 0;
    }
    const char *e = wpj_str (st, "install_error", NULL);
    char *m = e ? g_strdup_printf ("Workshop install failed: %s", e)
                : g_strdup_printf ("Installed: %s", wpj_str (st, "install_title", ""));
    set_status (m, e != NULL);
    g_free (m);
    if (!e)
        refresh_model ();
}

static gboolean install_tick (gpointer u)
{
    (void) u;
    if (!install_inflight) {
        install_inflight = TRUE;
        ui_ipc_send ("{\"cmd\":\"status\"}", UI_IPC_POLL, on_install_status, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void on_install_reply (JsonObject *reply, const char *err, gpointer u)
{
    GtkWidget *dlg = u;   /* ref'd */
    GtkLabel *el = g_object_get_data (G_OBJECT (dlg), "err");
    GtkWidget *go = g_object_get_data (G_OBJECT (dlg), "go");
    if (go)
        gtk_widget_set_sensitive (go, TRUE);
    if (ui_ipc_ok (reply)) {
        set_status ("Workshop: starting steamcmd…", FALSE);
        if (!install_poll_id)
            install_poll_id = g_timeout_add (700, install_tick, NULL);
        gtk_window_destroy (GTK_WINDOW (dlg));
    } else {
        const char *e = reply ? wpj_str (reply, "error", "install failed")
                              : (err ? err : "walpe-daemon not reachable");
        if (el)
            gtk_label_set_text (el, e);
        if (!gtk_widget_get_visible (dlg)) {
            /* closed before the engine answered (it had to be started
             * first, or was busy): nobody reads the dialog's own line any
             * more, and the refusal was shown nowhere */
            char *m = g_strdup_printf ("Workshop install failed: %s", e);
            set_status (m, TRUE);
            g_free (m);
        }
    }
    g_object_unref (dlg);
}

static void on_install_go (GtkButton *b, gpointer u)
{
    GtkWidget *dlg = u;
    GtkEditable *ide = g_object_get_data (G_OBJECT (dlg), "id");
    GtkEditable *ue = g_object_get_data (G_OBJECT (dlg), "user");
    GtkLabel *err = g_object_get_data (G_OBJECT (dlg), "err");
    char *id = walpe_workshop_parse_id (gtk_editable_get_text (ide));
    const char *su = gtk_editable_get_text (ue);
    if (!id) {
        gtk_label_set_text (err, "Enter a workshop id (digits) or a steamcommunity.com "
                                 "…/filedetails/?id=… link.");
        return;
    }
    if (!walpe_workshop_valid_user (su)) {
        gtk_label_set_text (err, "Enter your Steam account name (not a password).");
        g_free (id);
        return;
    }
    gtk_widget_set_sensitive (GTK_WIDGET (b), FALSE);
    gtk_label_set_text (err, "");
    set_string ("steam_user", su, "Steam account");
    char *req = g_strdup_printf ("{\"cmd\":\"install\",\"id\":\"%s\"}", id);  /* digits only */
    ui_ipc_send (req, UI_IPC_SLOW | UI_IPC_AUTOSTART, on_install_reply, g_object_ref (dlg));
    g_free (req);
    g_free (id);
}

static void on_install (void)
{
    GtkWidget *dlg = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (dlg), "Install from Steam Workshop");
    gtk_window_set_transient_for (GTK_WINDOW (dlg), GTK_WINDOW (U.win));
    /* not modal: a slow daemon or steamcmd never locks the main window */
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class (box, "walpe-settings");
    GtkWidget *info = gtk_label_new (
        "Downloads with steamcmd using your Steam account (it must own Wallpaper Engine).\n"
        "Log in to steamcmd once in a terminal: steamcmd +login <account> +quit\n"
        "WalPE never asks for or stores your password.");
    gtk_label_set_xalign (GTK_LABEL (info), 0);
    gtk_label_set_wrap (GTK_LABEL (info), TRUE);
    gtk_box_append (GTK_BOX (box), info);
    GtkWidget *id = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (id), "Workshop id or steamcommunity.com link");
    gtk_box_append (GTK_BOX (box), id);
    GtkWidget *ue = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (ue), "Steam account name");
    if (U.last_state)
        gtk_editable_set_text (GTK_EDITABLE (ue), wpj_str (U.last_state, "steam_user", ""));
    gtk_box_append (GTK_BOX (box), ue);
    GtkWidget *err = gtk_label_new (NULL);
    gtk_label_set_wrap (GTK_LABEL (err), TRUE);
    gtk_label_set_xalign (GTK_LABEL (err), 0);
    gtk_widget_add_css_class (err, "error");
    gtk_box_append (GTK_BOX (box), err);
    GtkWidget *go = gtk_button_new_with_mnemonic ("_Install");
    gtk_widget_add_css_class (go, "suggested-action");
    gtk_widget_set_halign (go, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (box), go);
    g_object_set_data (G_OBJECT (dlg), "id", id);
    g_object_set_data (G_OBJECT (dlg), "user", ue);
    g_object_set_data (G_OBJECT (dlg), "err", err);
    g_object_set_data (G_OBJECT (dlg), "go", go);
    g_signal_connect (go, "clicked", G_CALLBACK (on_install_go), dlg);
    g_signal_connect_swapped (id, "activate", G_CALLBACK (gtk_widget_activate), go);
    gtk_window_set_child (GTK_WINDOW (dlg), box);
    gtk_window_set_default_size (GTK_WINDOW (dlg), 480, -1);
    gtk_window_present (GTK_WINDOW (dlg));
}

/* ================================================================ actions */

static void act_find (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    gtk_widget_grab_focus (U.search);
}

static void act_find_option (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    walpe_props_panel_focus_filter (U.props_panel);
}

/* also the tray menu's "Settings…" (D-Bus org.freedesktop.Application) */
static void act_settings (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    if (!U.win)
        g_application_activate (G_APPLICATION (U.app));
    show_settings ();
}

/* "Monitor resolution…": the menu, walpe-tray's entry, the notification
 * about a new monitor and `walpe --resolution` (D-Bus
 * org.freedesktop.Application.ActivateAction). p = the output names to
 * show, none = every connected monitor. */
static void act_monitor_resolution (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) u;
    if (!U.win)
        g_application_activate (G_APPLICATION (U.app));
    if (!U.win)
        return;
    const char **names = p ? g_variant_get_strv (p, NULL) : NULL;
    walpe_res_ui_show_dialog (names);
    g_free (names);
}
static void act_apply (GSimpleAction *a, GVariant *p, gpointer u) { (void) a; (void) p; (void) u; do_apply (); }
static void act_import (GSimpleAction *a, GVariant *p, gpointer u) { (void) a; (void) p; (void) u; on_import (); }
static void act_folder (GSimpleAction *a, GVariant *p, gpointer u) { (void) a; (void) p; (void) u; on_add_folder (); }
static void act_install (GSimpleAction *a, GVariant *p, gpointer u) { (void) a; (void) p; (void) u; on_install (); }
static void act_refresh (GSimpleAction *a, GVariant *p, gpointer u) { (void) a; (void) p; (void) u; refresh_model (); }
static void act_quit (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    if (U.win)
        gtk_window_destroy (GTK_WINDOW (U.win));   /* on_win_destroy does the rest */
}

static const GActionEntry app_actions[] = {
    { .name = "find", .activate = act_find },
    { .name = "find-option", .activate = act_find_option },
    { .name = "settings", .activate = act_settings },
    { .name = WALPE_RES_ACTION, .activate = act_monitor_resolution, .parameter_type = "as" },
    { .name = "apply", .activate = act_apply },
    { .name = "import", .activate = act_import },
    { .name = "add-folder", .activate = act_folder },
    { .name = "install", .activate = act_install },
    { .name = "refresh", .activate = act_refresh },
    { .name = "quit", .activate = act_quit },
};

/* ================================================================= window */

static void load_css (void)
{
    GtkCssProvider *p = gtk_css_provider_new ();
    gtk_css_provider_load_from_resource (p, "/io/github/walpe/ui/style.css");
    gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (p),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (p);
    char *user = g_build_filename (g_get_user_config_dir (), "walpe", "style.css", NULL);
    if (g_file_test (user, G_FILE_TEST_IS_REGULAR)) {
        GtkCssProvider *up = gtk_css_provider_new ();
        gtk_css_provider_load_from_path (up, user);
        gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (up),
                                                    GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_unref (up);
    }
    g_free (user);
}

static void update_cap (void)
{
    int h = 0;
    gtk_window_get_default_size (GTK_WINDOW (U.win), NULL, &h);
    int ah = gtk_widget_get_height (U.win);
    if (ah > 0 && (gtk_window_is_maximized (GTK_WINDOW (U.win)) || h <= 0))
        h = ah;
    if (h <= 0)
        h = 900;
    /* ~35 % of a tall window; less on short ones so the options keep room */
    double frac = h >= 1000 ? 0.35 : h <= 720 ? 0.22 : 0.22 + 0.13 * (h - 720) / 280.0;
    cap_box_set_max (U.capbox, MAX (120, (int) (h * frac)));
}

static void on_win_size (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) o; (void) ps; (void) u;
    update_cap ();
}

static void on_realize (GtkWidget *w, gpointer u)
{
    (void) u;
    GskRenderer *r = gtk_native_get_renderer (GTK_NATIVE (w));
    const char *env = g_getenv ("GSK_RENDERER");
    g_message ("GSK renderer: %s (GSK_RENDERER=%s%s)", r ? G_OBJECT_TYPE_NAME (r) : "none",
               env ? env : "unset", g_getenv ("WALPE_UI_SET_GSK") ? ", WalPE default" : "");
    update_cap ();
}

static GtkWidget *menu_button (void)
{
    GMenu *m = g_menu_new ();
    GMenu *s1 = g_menu_new ();
    g_menu_append (s1, "Import media files…", "app.import");
    g_menu_append (s1, "Add Wallpaper Engine folder…", "app.add-folder");
    g_menu_append (s1, "Install from Steam Workshop…", "app.install");
    g_menu_append (s1, "Rescan library", "app.refresh");
    g_menu_append_section (m, NULL, G_MENU_MODEL (s1));
    GMenu *s2 = g_menu_new ();
    g_menu_append (s2, "Engine settings", "app.settings");
    GMenuItem *res = g_menu_item_new ("Monitor resolution…", NULL);
    g_menu_item_set_action_and_target_value (res, "app." WALPE_RES_ACTION, g_variant_new_strv (NULL, 0));
    g_menu_append_item (s2, res);
    g_object_unref (res);
    g_menu_append (s2, "Quit", "app.quit");
    g_menu_append_section (m, NULL, G_MENU_MODEL (s2));
    GtkWidget *b = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (b), "open-menu-symbolic");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (b), G_MENU_MODEL (m));
    gtk_widget_set_tooltip_text (b, "Menu");
    g_object_unref (s1);
    g_object_unref (s2);
    g_object_unref (m);
    return b;
}

/* GtkGridView keeps max_columns x 30 rows of tiles alive, whatever is on
 * screen. With a fixed limit of 24 that was 720 tiles: (almost) every
 * wallpaper got its widgets and a thumbnail decode at start-up although a
 * dozen were visible. The limit follows the pane instead: the columns that
 * could fit at most (a tile is at least TILE_W wide), never fewer than the
 * grid shows, so the layout is the same. */
static guint grid_cols_idle_id;

static gboolean grid_cols_idle (gpointer user)
{
    GtkAdjustment *adj = user;
    grid_cols_idle_id = 0;
    guint cols = (guint) CLAMP ((int) gtk_adjustment_get_page_size (adj) / TILE_W, 1, 24);
    if (U.grid && cols != gtk_grid_view_get_max_columns (GTK_GRID_VIEW (U.grid)))
        gtk_grid_view_set_max_columns (GTK_GRID_VIEW (U.grid), cols);
    g_object_unref (adj);
    return G_SOURCE_REMOVE;
}

/* the scrolled window's width changed (window resize, pane handle). Not
 * changed from inside the size allocation that reports it. */
static void grid_width_changed (GtkAdjustment *adj, GParamSpec *ps, gpointer user)
{
    (void) ps; (void) user;
    if (!grid_cols_idle_id)
        grid_cols_idle_id = g_idle_add (grid_cols_idle, g_object_ref (adj));
}

static GtkWidget *build_library (void)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (bar, "walpe-library-bar");
    U.search = gtk_search_entry_new ();
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (U.search), "Search wallpapers (Ctrl+F)");
    gtk_widget_set_hexpand (U.search, TRUE);
    g_signal_connect (U.search, "search-changed", G_CALLBACK (on_search_changed), NULL);
    gtk_box_append (GTK_BOX (bar), U.search);
    const char *types[] = { "All types", "Scenes", "Videos", "Web", "Images / GIF",
                            "With options", NULL };
    U.type_dd = gtk_drop_down_new_from_strings (types);
    gtk_widget_set_tooltip_text (U.type_dd, "Filter by type");
    g_signal_connect (U.type_dd, "notify::selected", G_CALLBACK (on_type_changed), NULL);
    gtk_box_append (GTK_BOX (bar), U.type_dd);
    U.count = gtk_label_new ("");
    gtk_widget_add_css_class (U.count, "walpe-count");
    gtk_box_append (GTK_BOX (bar), U.count);
    gtk_box_append (GTK_BOX (box), bar);

    U.model = g_list_store_new (WALPE_TYPE_ENTRY);
    U.filter = gtk_custom_filter_new (filter_match, NULL, NULL);
    U.filtered = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (U.model)),
                                            GTK_FILTER (g_object_ref (U.filter)));
    U.sel = gtk_single_selection_new (G_LIST_MODEL (g_object_ref (U.filtered)));
    gtk_single_selection_set_autoselect (U.sel, FALSE);
    gtk_single_selection_set_can_unselect (U.sel, FALSE);
    g_signal_connect (U.sel, "notify::selected-item", G_CALLBACK (on_selected), NULL);

    GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
    g_signal_connect (f, "setup", G_CALLBACK (setup_tile), NULL);
    g_signal_connect (f, "bind", G_CALLBACK (bind_tile), NULL);
    g_signal_connect (f, "unbind", G_CALLBACK (unbind_tile), NULL);
    g_signal_connect (f, "teardown", G_CALLBACK (teardown_tile), NULL);
    U.grid = gtk_grid_view_new (GTK_SELECTION_MODEL (g_object_ref (U.sel)), f);
    gtk_widget_add_css_class (U.grid, "walpe-grid");
    /* the pane starts 780 px wide (build_window); grid_width_changed keeps
     * the limit at what the pane can hold */
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (U.grid), 780 / TILE_W);
    gtk_grid_view_set_min_columns (GTK_GRID_VIEW (U.grid), 1);
    /* single-click-activate also selects on hover: click selects, Enter /
     * double-click applies */
    gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (U.grid), FALSE);
    g_signal_connect (U.grid, "activate", G_CALLBACK (on_grid_activate), NULL);
    GtkWidget *sc = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (sc, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), U.grid);
    g_signal_connect (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sc)),
                      "notify::page-size", G_CALLBACK (grid_width_changed), NULL);
    gtk_box_append (GTK_BOX (box), sc);
    delete_attach_grid (box);
    gtk_widget_set_size_request (box, 360, -1);
    return box;
}

static GtkWidget *build_details (void)
{
    GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *top = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (top, "walpe-details");

    GtkWidget *ov = gtk_overlay_new ();
    gtk_widget_add_css_class (ov, "walpe-preview");
    U.preview_ph = gtk_label_new ("");
    gtk_widget_add_css_class (U.preview_ph, "walpe-dim");
    gtk_overlay_set_child (GTK_OVERLAY (ov), U.preview_ph);
    U.preview = GTK_PICTURE (gtk_picture_new ());
    gtk_picture_set_content_fit (U.preview, GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink (U.preview, TRUE);
    gtk_overlay_add_overlay (GTK_OVERLAY (ov), GTK_WIDGET (U.preview));
    gtk_overlay_set_measure_overlay (GTK_OVERLAY (ov), GTK_WIDGET (U.preview), TRUE);
    gtk_widget_set_size_request (ov, -1, 120);
    U.capbox = cap_box_new (ov);
    gtk_box_append (GTK_BOX (top), U.capbox);

    U.title = GTK_LABEL (gtk_label_new ("Nothing selected"));
    gtk_label_set_xalign (U.title, 0);
    gtk_label_set_wrap (U.title, TRUE);
    gtk_label_set_wrap_mode (U.title, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines (U.title, 2);
    gtk_label_set_ellipsize (U.title, PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable (U.title, TRUE);
    gtk_widget_set_focusable (GTK_WIDGET (U.title), FALSE);
    gtk_widget_add_css_class (GTK_WIDGET (U.title), "walpe-title");
    gtk_box_append (GTK_BOX (top), delete_title_row (GTK_WIDGET (U.title)));

    GtkWidget *meta = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    U.type_badge = badge_new ("scene", "Scene");
    U.active_badge = badge_new ("active", "Active");
    U.steam_badge = badge_new ("steam", "Steam");
    U.nopt_badge = badge_new ("neutral", "");
    gtk_box_append (GTK_BOX (meta), U.type_badge);
    gtk_box_append (GTK_BOX (meta), U.active_badge);
    gtk_box_append (GTK_BOX (meta), U.steam_badge);
    gtk_box_append (GTK_BOX (meta), U.nopt_badge);
    U.idlabel = GTK_LABEL (gtk_label_new (""));
    gtk_widget_add_css_class (GTK_WIDGET (U.idlabel), "walpe-dim");
    gtk_label_set_selectable (U.idlabel, TRUE);
    gtk_widget_set_focusable (GTK_WIDGET (U.idlabel), FALSE);
    gtk_box_append (GTK_BOX (meta), GTK_WIDGET (U.idlabel));
    GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (spacer, TRUE);
    gtk_box_append (GTK_BOX (meta), spacer);
    U.mon_label = GTK_LABEL (gtk_label_new (""));
    gtk_widget_add_css_class (GTK_WIDGET (U.mon_label), "walpe-dim");
    gtk_widget_set_tooltip_text (GTK_WIDGET (U.mon_label),
                                 "walpe-daemon has no per-monitor target yet: a wallpaper is "
                                 "shown on every monitor.");
    gtk_box_append (GTK_BOX (meta), GTK_WIDGET (U.mon_label));
    U.mons = g_ptr_array_new_with_free_func (mon_info_free);
    U.mon_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    U.mon_area = gtk_drawing_area_new ();
    gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (U.mon_area), 64);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (U.mon_area), 30);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (U.mon_area), mon_area_draw, NULL, NULL);
    gtk_widget_set_valign (U.mon_area, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (U.mon_box), U.mon_area);
    const char *all[] = { "All monitors", NULL };
    U.mon_dd = gtk_drop_down_new_from_strings (all);
    gtk_accessible_update_property (GTK_ACCESSIBLE (U.mon_dd), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Target monitor", -1);
    g_signal_connect (U.mon_dd, "notify::selected", G_CALLBACK (on_mon_selected), NULL);
    gtk_box_append (GTK_BOX (U.mon_box), U.mon_dd);
    gtk_widget_set_visible (U.mon_box, FALSE);
    gtk_box_append (GTK_BOX (meta), U.mon_box);
    U.apply = gtk_button_new_with_mnemonic ("Set as wallpaper");
    gtk_widget_add_css_class (U.apply, "suggested-action");
    gtk_widget_add_css_class (U.apply, "walpe-apply");
    gtk_widget_set_tooltip_text (U.apply, "Set as wallpaper (Enter on a tile, Ctrl+Return)");
    g_signal_connect (U.apply, "clicked", G_CALLBACK (on_apply), NULL);
    gtk_box_append (GTK_BOX (meta), U.apply);
    gtk_box_append (GTK_BOX (top), meta);
    U.mon_now = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_xalign (U.mon_now, 0);
    gtk_label_set_ellipsize (U.mon_now, PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (GTK_WIDGET (U.mon_now), "walpe-dim");
    gtk_widget_set_visible (GTK_WIDGET (U.mon_now), FALSE);
    gtk_box_append (GTK_BOX (top), GTK_WIDGET (U.mon_now));

    U.desc = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_xalign (U.desc, 0);
    gtk_label_set_wrap (U.desc, TRUE);
    gtk_label_set_wrap_mode (U.desc, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines (U.desc, 3);
    gtk_label_set_ellipsize (U.desc, PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable (U.desc, TRUE);
    gtk_widget_set_focusable (GTK_WIDGET (U.desc), FALSE);
    gtk_widget_add_css_class (GTK_WIDGET (U.desc), "walpe-desc");
    GtkWidget *dsc = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (dsc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (dsc), TRUE);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (dsc), 170);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (dsc), GTK_WIDGET (U.desc));
    GtkWidget *drow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append (GTK_BOX (drow), dsc);
    U.desc_more = gtk_button_new_with_label ("Show more");
    gtk_widget_add_css_class (U.desc_more, "flat");
    gtk_widget_set_halign (U.desc_more, GTK_ALIGN_START);
    g_signal_connect (U.desc_more, "clicked", G_CALLBACK (on_desc_more), NULL);
    gtk_box_append (GTK_BOX (drow), U.desc_more);
    gtk_box_append (GTK_BOX (top), drow);
    gtk_box_append (GTK_BOX (right), top);
    gtk_box_append (GTK_BOX (right), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* the wallpaper's own quality settings: one line until opened */
    U.perf_panel = walpe_perf_panel_new (TRUE, props_status);
    gtk_box_append (GTK_BOX (right), U.perf_panel);
    U.props_panel = walpe_props_panel_new (props_status);
    gtk_box_append (GTK_BOX (right), U.props_panel);
    /* scenes only, collapsed: move / resize / hide layers of the wallpaper */
    U.layers_panel = walpe_layers_panel_new (props_status);
    gtk_box_append (GTK_BOX (right), U.layers_panel);
    gtk_widget_set_size_request (right, 460, -1);
    return right;
}

static GtkWidget *build_statusbar (void)
{
    GtkWidget *sb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (sb, "walpe-statusbar");
    U.dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (U.dot, "walpe-dot");
    gtk_widget_set_valign (U.dot, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (sb), U.dot);
    U.engine_lbl = GTK_LABEL (gtk_label_new ("Connecting…"));
    gtk_box_append (GTK_BOX (sb), GTK_WIDGET (U.engine_lbl));
    gtk_box_append (GTK_BOX (sb), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
    U.status = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_xalign (U.status, 0);
    gtk_label_set_ellipsize (U.status, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (GTK_WIDGET (U.status), TRUE);
    gtk_box_append (GTK_BOX (sb), GTK_WIDGET (U.status));
    U.restart_btn = gtk_button_new_with_label ("Start engine");
    gtk_widget_set_visible (U.restart_btn, FALSE);
    g_signal_connect (U.restart_btn, "clicked", G_CALLBACK (on_restart), NULL);
    gtk_box_append (GTK_BOX (sb), U.restart_btn);
    return sb;
}

#ifdef WALPE_HAVE_WORKSHOP_PAGE
static void on_stack_page (GObject *o, GParamSpec *ps, gpointer holder);
#endif

/* The main window is destroyed (Ctrl+Q, its close button, the window
 * manager). GApplication used to go on dispatching what was pending - the
 * status poll's reply, a rescan, the options idle, the timers below - and
 * at its end drains the main loop once more; all of those use the widgets
 * of the window that is gone (GTK criticals on 11 of 12 quits of
 * the sanitizer build; with the memory reused, a use after free). So:
 * nothing of the window's stays scheduled, replies still in flight are
 * dropped, and the application quits at once (g_application_quit skips the
 * final drain). The engine is a process of its own and keeps running. */
static void on_win_destroy (GtkWidget *w, gpointer user)
{
    (void) w; (void) user;
    if (U.closing)
        return;
    U.closing = TRUE;
    ui_ipc_shutdown ();
    guint *ids[] = { &Sup.poll_id, &Sup.retry_id, &U.rescan_id, &U.props_idle, &U.apply_guard,
                     &U.status_clear_id, &import_poll_id, &install_poll_id, &SW.fps_id,
                     &SW.vol_id, &SW.pden_id };
    for (guint i = 0; i < G_N_ELEMENTS (ids); i++) {
        if (*ids[i])
            g_source_remove (*ids[i]);
        *ids[i] = 0;
    }
    if (grid_cols_idle_id) {
        /* its data is a reference on the adjustment: let it run out */
        U.grid = NULL;
    }
    if (U.dirmon) {
        g_file_monitor_cancel (U.dirmon);
        g_clear_object (&U.dirmon);
    }
    if (Sup.sub) {      /* the engine's state lines: no more of them */
        walpe_ipc_sub_free (Sup.sub);
        Sup.sub = NULL;
    }
    walpe_res_ui_shutdown ();   /* its dialog goes with the window, its notification is taken back */
    U.status = NULL;    /* set_status () and friends return early from here on */
    U.win = NULL;
    if (U.app)
        g_application_quit (G_APPLICATION (U.app));
}

static void activate (GtkApplication *app, gpointer user)
{
    (void) user;
    if (U.closing)
        return;
    if (U.win) {
        gtk_window_present (GTK_WINDOW (U.win));
        return;
    }
    load_css ();
    walpe_appearance_follow ();     /* a no-op on X11 */
    U.bound = g_hash_table_new (g_direct_hash, g_direct_equal);
    struct { const char *act; const char *acc[2]; } accels[] = {
        { "app.find", { "<Control>f", NULL } },
        { "app.find-option", { "<Control><Shift>f", NULL } },
        { "app.settings", { "<Control>comma", NULL } },
        { "app.apply", { "<Control>Return", NULL } },
        { "app.import", { "<Control>o", NULL } },
        { "app.refresh", { "F5", NULL } },
        { "app.quit", { "<Control>q", NULL } },
    };
    for (guint i = 0; i < G_N_ELEMENTS (accels); i++)
        gtk_application_set_accels_for_action (app, accels[i].act, accels[i].acc);

    U.win = gtk_application_window_new (app);
    gtk_window_set_title (GTK_WINDOW (U.win), "WalPE");
    gtk_window_set_default_size (GTK_WINDOW (U.win), 1440, 900);
    g_signal_connect (U.win, "realize", G_CALLBACK (on_realize), NULL);
    g_signal_connect (U.win, "destroy", G_CALLBACK (on_win_destroy), NULL);
    g_signal_connect (U.win, "notify::default-height", G_CALLBACK (on_win_size), NULL);
    g_signal_connect (U.win, "notify::maximized", G_CALLBACK (on_win_size), NULL);

    GtkWidget *hdr = gtk_header_bar_new ();
    GtkWidget *imp = gtk_button_new_with_mnemonic ("_Import…");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (imp), "app.import");
    gtk_widget_set_tooltip_text (imp, "Import video, GIF or image files (Ctrl+O)");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (hdr), imp);
    GtkWidget *ws = gtk_button_new_with_label ("Workshop…");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (ws), "app.install");
    gtk_widget_set_tooltip_text (ws, "Install from Steam Workshop");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (hdr), ws);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (hdr), menu_button ());
    GtkWidget *set = gtk_button_new_with_label ("Engine settings");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (set), "app.settings");
    gtk_widget_set_tooltip_text (set, "Frame rate, pausing, parallax, volume… (Ctrl+,)");
    gtk_header_bar_pack_end (GTK_HEADER_BAR (hdr), set);
    U.pause_btn = gtk_toggle_button_new_with_label ("Pause");
    gtk_widget_set_tooltip_text (U.pause_btn, "Pause / resume the wallpaper on all monitors");
    gtk_widget_set_sensitive (U.pause_btn, FALSE);
    g_signal_connect (U.pause_btn, "toggled", G_CALLBACK (on_pause_toggled), NULL);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (hdr), U.pause_btn);
    gtk_window_set_titlebar (GTK_WINDOW (U.win), hdr);

    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    U.stack = gtk_stack_new ();
    gtk_widget_set_vexpand (U.stack, TRUE);
    U.paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand (U.paned, TRUE);
    gtk_paned_set_wide_handle (GTK_PANED (U.paned), TRUE);
    GtkWidget *statusbar = build_statusbar ();   /* first: others report into it */
    gtk_paned_set_start_child (GTK_PANED (U.paned), build_library ());
    gtk_paned_set_end_child (GTK_PANED (U.paned), build_details ());
    gtk_paned_set_resize_start_child (GTK_PANED (U.paned), TRUE);
    gtk_paned_set_resize_end_child (GTK_PANED (U.paned), TRUE);
    gtk_paned_set_shrink_start_child (GTK_PANED (U.paned), FALSE);
    gtk_paned_set_shrink_end_child (GTK_PANED (U.paned), FALSE);
    gtk_paned_set_position (GTK_PANED (U.paned), 780);
    gtk_stack_add_titled (GTK_STACK (U.stack), U.paned, "library", "Library");
    int npages = 1;
#ifdef WALPE_HAVE_WORKSHOP_PAGE
    /* Workshop tab (src/ui/workshop_page.c): browse/install, then the
     * library rescans by itself (dir monitor) */
    {
        /* built when the tab is first opened: as part of activate() it
         * queried Steam (twice), bound 30 tiles and fetched 31 previews at
         * every start of the GUI, for a tab most launches never look at */
        GtkWidget *holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        gtk_stack_add_titled (GTK_STACK (U.stack), holder, "workshop", "Workshop");
        g_signal_connect (U.stack, "notify::visible-child-name", G_CALLBACK (on_stack_page), holder);
    }
    npages++;
#endif
    if (npages > 1) {
        GtkWidget *sw = gtk_stack_switcher_new ();
        gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (sw), GTK_STACK (U.stack));
        gtk_header_bar_set_title_widget (GTK_HEADER_BAR (hdr), sw);
    }
    gtk_box_append (GTK_BOX (outer), U.stack);
    gtk_box_append (GTK_BOX (outer), statusbar);
    gtk_window_set_child (GTK_WINDOW (U.win), outer);

    U.current = g_strdup (U.cfg->current);   /* until the first status reply */
    update_details (NULL);
    sup_set_state (D_UNKNOWN, NULL);
    {
        /* before the first status reply: it may hold monitors to ask about */
        WalpeResUiHost rh = { .app = app, .parent = GTK_WINDOW (U.win), .status = set_status };
        walpe_res_ui_init (&rh);
    }
    ui_ipc_set_down_hook (sup_on_down);
    refresh_model ();

    /* engine: first poll decides whether to start it (async) */
    sup_poll_now ();
    {
        char *sp = walpe_socket_path ();
        Sup.sub = walpe_ipc_sub_new (sp, on_sub_line, NULL);
        g_free (sp);
    }
    g_signal_connect (U.win, "notify::is-active", G_CALLBACK (on_win_active), NULL);
    sup_poll_update ();

    char *dir = walpe_expand_path (U.cfg->wallpapers_dir);
    GFile *gf = g_file_new_for_path (dir);
    U.dirmon = g_file_monitor_directory (gf, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (U.dirmon)
        g_signal_connect (U.dirmon, "changed", G_CALLBACK (on_dir_changed), NULL);
    g_object_unref (gf);
    g_free (dir);

    /* tray.ini start_with_gui: the tray comes up with the GUI (not
     * installed / already running: nothing to do) */
    if (tray_enabled ())
        tray_start (NULL);

    walpe_ui_stats_attach (U.win);
    gtk_window_present (GTK_WINDOW (U.win));
    gtk_widget_grab_focus (U.grid);
    if (U.want_res_dialog) {    /* walpe --resolution */
        U.want_res_dialog = FALSE;
        walpe_res_ui_show_dialog (NULL);
    }
}

#ifdef WALPE_HAVE_WORKSHOP_PAGE
static void on_stack_page (GObject *o, GParamSpec *ps, gpointer holder)
{
    (void) ps;
    if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (o)), "workshop") ||
        gtk_widget_get_first_child (holder))
        return;
    GtkWidget *pg = walpe_workshop_page_new_hosted (schedule_rescan);
    gtk_widget_set_hexpand (pg, TRUE);
    gtk_widget_set_vexpand (pg, TRUE);
    gtk_box_append (GTK_BOX (holder), pg);
}
#endif

/* `walpe --resolution`: the "Monitor resolution" dialog. A window that
 * runs already is told to open it (and this process ends); else this
 * process becomes the window and opens it when it is up. */
static gint on_local_options (GApplication *app, GVariantDict *opts, gpointer u)
{
    (void) u;
    if (!g_variant_dict_contains (opts, "resolution"))
        return -1;
    if (g_application_register (app, NULL, NULL) && g_application_get_is_remote (app)) {
        g_action_group_activate_action (G_ACTION_GROUP (app), WALPE_RES_ACTION,
                                        g_variant_new_strv (NULL, 0));
        GDBusConnection *c = g_application_get_dbus_connection (app);
        if (c)
            g_dbus_connection_flush_sync (c, NULL, NULL);
        return 0;
    }
    U.want_res_dialog = TRUE;
    return -1;
}

int main (int argc, char **argv)
{
    /* the browser needs no GPU: cairo avoids GL context trouble on drivers
     * where GSK's GL renderer misbehaves. A user GSK_RENDERER wins; the
     * renderer in use is logged at startup. */
    if (!g_getenv ("GSK_RENDERER")) {
        g_setenv ("GSK_RENDERER", "cairo", TRUE);
        g_setenv ("WALPE_UI_SET_GSK", "1", TRUE);
        /* ... and then GDK must not start GL either: it did, to pick its
         * visual, and the GUI carried a whole GL driver it never drew with
         * (Mesa: +55 MB resident and four worker threads; on NVIDIA a GL
         * context of its own on the card). Same switches as walpe-tray
         * and the daemon's web module; a user value wins. */
        if (!g_getenv ("GDK_DISABLE") && !g_getenv ("GDK_DEBUG")) {
            if (gtk_get_major_version () == 4 && gtk_get_minor_version () >= 16)
                g_setenv ("GDK_DISABLE", "gl", TRUE);
            else
                g_setenv ("GDK_DEBUG", "gl-disable", TRUE);
            g_setenv ("WALPE_UI_SET_GDK", "1", TRUE);
        }
    }
    U.cfg = walpe_config_load ();
    thumbs_set_anim_mode (U.cfg->preview_anim);
    /* unique: a second "walpe" (or walpe-tray's left click, D-Bus
     * org.freedesktop.Application.Activate) raises the open window */
    U.app = gtk_application_new (WALPE_UI_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (U.app, "activate", G_CALLBACK (activate), NULL);
    /* app actions exist before the first activate: walpe-tray's
     * ActivateAction("settings") can arrive as soon as the name is owned */
    g_action_map_add_action_entries (G_ACTION_MAP (U.app), app_actions, G_N_ELEMENTS (app_actions), NULL);
    g_application_add_main_option (G_APPLICATION (U.app), "resolution", 0, G_OPTION_FLAG_NONE,
                                   G_OPTION_ARG_NONE,
                                   "Open the dialog that sets each monitor's render resolution", NULL);
    g_signal_connect (U.app, "handle-local-options", G_CALLBACK (on_local_options), NULL);
    int rc = g_application_run (G_APPLICATION (U.app), argc, argv);
    /* requests already handed to the IPC workers (an option changed a
     * moment before Ctrl+Q) still go out: on_win_destroy quits at once */
    ui_ipc_flush (500);
    /* no walpe_config_save here: the daemon owns config.json (saving the
     * GUI's startup copy wiped every option changed during the session) */
    walpe_config_free (U.cfg);
    g_object_unref (U.app);
    return rc;
}
