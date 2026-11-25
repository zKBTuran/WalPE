/* walpe-tray - system tray icon for WalPE (like Wallpaper Engine's tray icon)
 *
 * A separate small process: talks to walpe-daemon over the same IPC as
 * walpe-ctl (asynchronously, with timeouts) and mirrors its status in the
 * icon, tooltip and menu. The status arrives by itself: the tray keeps one
 * subscribed connection (common/ipcsub.h) and the daemon writes to it when
 * something changed. It polls (every 300 ms) only during a start / restart
 * it triggered, and every 2 s only against a daemon too old to push. Shows itself through a
 * StatusNotifierItem when an SNI host is present (KDE, waybar, ...), else
 * as a classic XEmbed tray icon (dwm systray, stalonetray, trayer, ...).
 * On a Wayland session it is always a StatusNotifierItem (XEmbed is an X11
 * protocol); which one is used when is in traysel.h.
 *
 *   walpe-tray [--backend=auto|sni|xembed] [--quit] [--export-icon=FILE]
 */
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

#include "walpe-build-config.h"   /* WALPE_VERSION, WALPE_CODENAME: from meson.build */
#include "common/sourceid.h"      /* --version: which tree it was built from */
#include "tray.h"
#include "trayids.h"
#include "traysel.h"
#include "trayipc.h"
#include "trayconf.h"
#include "common/config.h"
#include "common/ipcsub.h"
#include "common/util.h"
#include "common/uitext.h"
#include "common/workshop.h"
#include "ui/resmodel.h"   /* render resolution per monitor: who is new, the GUI's action */
#include "ui/store.h"

#define POLL_MS        2000
#define FAST_POLL_MS   300
#define IPC_TIMEOUT_MS 1500
#define RECENT_MAX     10

typedef struct {
    int index;
    char *name;
    char *current;
    char *type;
    gboolean paused;
    int follows;           /* -1 = renders itself */
    int geom[4];
} MonInfo;

static struct {
    GtkApplication *app;
    WalpeTrayConf *conf;
    char *backend_opt;

    /* daemon state (last status) */
    gboolean alive;
    gboolean have_status;  /* at least one poll finished */
    char *path;            /* default wallpaper */
    char *type;
    gboolean paused, fs_pause, covered_pause, dpms_pause;
    double volume;
    gboolean mute;         /* the setting "mute": silent whatever the volume is */
    gboolean sound_on;     /* the setting "scene_sound": wallpapers may make sound at all */
    GPtrArray *mons;       /* MonInfo* */
    /* settings from config.json (status does not carry them) */
    gboolean set_fs, set_covered;

    /* transitions */
    gboolean starting, restarting, quitting;
    gint64 transition_until;
    gint64 last_gui_spawn;
    gboolean pending_settings;
    guint ui_watch;
    gboolean ui_running;   /* the GUI owns its bus name */

    /* render resolution per monitor (the GUI owns the dialog) */
    WalpeMonresTracker *res_track;
    GPtrArray *res_new;    /* char*: names of the outputs nobody was asked about */
    char *res_new_label;   /* the menu line for them, NULL = none */
    guint res_announced;   /* monitors named by the notification that is up */

    gboolean poll_inflight;
    guint poll_timer;
    guint poll_ms;         /* interval of poll_timer, 0 = none */
    WalpeIpcSub *sub;      /* state pushes from the daemon */

    GQueue recent;         /* char* paths, most recent first */
    GHashTable *titles;    /* path -> title */
    double volume_before_mute;

    TrayItem *menu;
    guint menu_serial;
    char *menu_sig;
    char *tip_title, *tip_text;
    TrayIconState icon_state;

    const TrayBackend *backend;
    guint watcher_watch;
    gboolean watcher;      /* org.kde.StatusNotifierWatcher has an owner */
    gboolean sni_host;     /* a StatusNotifierWatcher with a registered host */
    guint host_sub;
    GDBusConnection *bus;
    const char *pick_note; /* what tray_pick () last had to say (logged once) */
    /* xdg-activation token of the click being handled (Wayland: without one
     * the compositor may refuse to bring the GUI's window to the front) */
    char *act_token;
    gint64 act_token_us;
} T = { .volume_before_mute = 1.0, .icon_state = TRAY_ICON_DEAD };

static void poll_now (void);
static void rebuild (void);
static void res_update (JsonObject *st);

/* ================================================================ menu tree */

/* the items' ids: by what an item is, not by where it stands (trayids.h) */
static TrayIds menu_ids = TRAY_IDS_INIT;

TrayItem *tray_item_new (TrayItemKind kind, const char *label, const char *action)
{
    TrayItem *it = g_new0 (TrayItem, 1);
    it->kind = kind;
    it->label = g_strdup (label ? label : "");
    it->action = g_strdup (action);
    it->enabled = kind != TRAY_ITEM_INFO;
    it->visible = TRUE;
    if (kind == TRAY_ITEM_SUBMENU)
        it->children = g_ptr_array_new ();
    return it;
}

void tray_item_append (TrayItem *parent, TrayItem *child)
{
    if (!parent->children)
        parent->children = g_ptr_array_new ();
    child->parent = parent;
    g_ptr_array_add (parent->children, child);
}

void tray_item_free (TrayItem *it)
{
    if (!it)
        return;
    if (it->children) {
        for (guint i = 0; i < it->children->len; i++)
            tray_item_free (it->children->pdata[i]);
        g_ptr_array_free (it->children, TRUE);
    }
    g_free (it->label);
    g_free (it->action);
    g_free (it->arg);
    g_free (it);
}

TrayItem *tray_item_find (TrayItem *root, int id)
{
    if (!root)
        return NULL;
    if (root->id == id)
        return root;
    for (guint i = 0; root->children && i < root->children->len; i++) {
        TrayItem *f = tray_item_find (root->children->pdata[i], id);
        if (f)
            return f;
    }
    return NULL;
}

TrayItem *tray_menu_get (void) { return T.menu; }
guint tray_menu_serial (void) { return T.menu_serial; }
const char *tray_tooltip_title (void) { return T.tip_title ? T.tip_title : "WalPE"; }
const char *tray_tooltip_text (void) { return T.tip_text ? T.tip_text : ""; }
TrayIconState tray_icon_state (void) { return T.icon_state; }

/* ================================================================ helpers */

static const char *title_for (const char *path)
{
    if (!path || !*path)
        return "nothing";
    if (!T.titles)
        T.titles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    const char *t = g_hash_table_lookup (T.titles, path);
    if (t)
        return t;
    char *title = NULL;
    if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
        char *pj = g_build_filename (path, "project.json", NULL);
        JsonNode *n = wpj_parse_file (pj);
        if (n && JSON_NODE_HOLDS_OBJECT (n)) {
            const char *s = wpj_str (json_node_get_object (n), "title", NULL);
            if (s && *s)
                title = g_strdup (s);
        }
        if (n)
            json_node_unref (n);
        g_free (pj);
    }
    if (!title) {
        char *b = g_path_get_basename (path);
        char *dot = strrchr (b, '.');
        if (dot && dot != b && !g_file_test (path, G_FILE_TEST_IS_DIR))
            *dot = 0;
        title = b;
    }
    /* menus stay narrow: long workshop titles are cut. The title is the
     * wallpaper author's (or a file name): one clean line, no control or
     * direction characters in the menu and the tooltip (uitext.h) */
    {
        char *line = walpe_ui_text (title, 47, WALPE_UITEXT_LINE);
        g_strstrip (line);
        if (*line) {
            g_free (title);
            title = line;
        } else {
            g_free (line);
            g_free (title);
            title = g_strdup ("(untitled)");
        }
    }
    g_hash_table_insert (T.titles, g_strdup (path), title);
    return title;
}

static char *state_file (const char *name)
{
    return g_build_filename (g_get_user_state_dir (), "walpe", name, NULL);
}

static void recent_load (void)
{
    char *p = state_file ("tray-recent");
    char *data = NULL;
    if (g_file_get_contents (p, &data, NULL, NULL)) {
        char **lines = g_strsplit (data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (g_str_has_prefix (lines[i], "volume_before_mute=")) {
                T.volume_before_mute = g_ascii_strtod (lines[i] + 19, NULL);
                continue;
            }
            if (*lines[i] && g_queue_get_length (&T.recent) < RECENT_MAX)
                g_queue_push_tail (&T.recent, g_strdup (lines[i]));
        }
        g_strfreev (lines);
        g_free (data);
    }
    g_free (p);
}

/* the last write of the list failed (a full disk, the folder in the way):
 * it is tried again with the next status line, as every line used to */
static gboolean recent_unsaved;

static void recent_save (void)
{
    GString *s = g_string_new (NULL);
    char vb[G_ASCII_DTOSTR_BUF_SIZE];
    g_string_append_printf (s, "volume_before_mute=%s\n",
                            g_ascii_dtostr (vb, sizeof vb, T.volume_before_mute));
    for (GList *l = T.recent.head; l; l = l->next)
        g_string_append_printf (s, "%s\n", (char *) l->data);
    char *p = state_file ("tray-recent");
    char *dir = g_path_get_dirname (p);
    g_mkdir_with_parents (dir, 0700);
    /* the paths of the wallpapers last shown: for the owner alone, like
     * config.json (an existing directory keeps its mode, and the file was
     * written with whatever the umask left: 0644) */
    recent_unsaved = !walpe_write_private (p, s->str, -1, NULL);
    g_free (dir);
    g_free (p);
    g_string_free (s, TRUE);
}

/* a wallpaper deleted since (GUI "Delete", a Steam unsubscribe): the entry
 * is gone while the folder it was in still exists (an unmounted library
 * keeps its entries) */
static gboolean recent_is_deleted (const char *path)
{
    char *dir = g_path_get_dirname (path);
    gboolean gone = g_file_test (dir, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_EXISTS);
    g_free (dir);
    return gone;
}

/* deleted wallpapers leave the list. TRUE when the list changed */
static gboolean recent_drop_deleted (void)
{
    gboolean changed = FALSE;
    for (GList *l = T.recent.head; l;) {
        GList *next = l->next;
        if (recent_is_deleted (l->data)) {
            g_free (l->data);
            g_queue_delete_link (&T.recent, l);
            changed = TRUE;
        }
        l = next;
    }
    return changed;
}

/* TRUE when the list changed */
static gboolean recent_push (const char *path)
{
    if (!path || !*path || recent_is_deleted (path))
        return FALSE;
    if (T.recent.head && !g_strcmp0 (T.recent.head->data, path))
        return FALSE;
    for (GList *l = T.recent.head; l; l = l->next)
        if (!g_strcmp0 (l->data, path)) {
            g_free (l->data);
            g_queue_delete_link (&T.recent, l);
            break;
        }
    g_queue_push_head (&T.recent, g_strdup (path));
    while (g_queue_get_length (&T.recent) > RECENT_MAX)
        g_free (g_queue_pop_tail (&T.recent));
    return TRUE;
}

/* the list as one string: what recent_save would write of it */
static char *recent_sig (void)
{
    GString *s = g_string_new (NULL);
    for (GList *l = T.recent.head; l; l = l->next)
        g_string_append_printf (s, "%s\n", (char *) l->data);
    return g_string_free (s, FALSE);
}

static void mon_free (gpointer p)
{
    MonInfo *m = p;
    g_free (m->name);
    g_free (m->current);
    g_free (m->type);
    g_free (m);
}

static gboolean in_transition (void)
{
    if ((T.starting || T.restarting) && g_get_monotonic_time () > T.transition_until) {
        g_message ("engine did not come back in time");
        T.starting = T.restarting = FALSE;
    }
    return T.starting || T.restarting;
}

/* ================================================================ daemon */

static void spawn_daemon (void)
{
    char *path = walpe_tray_sibling_exe ("walpe-daemon");
    char *argv[] = { path, NULL };
    GError *err = NULL;
    if (!g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        g_warning ("cannot start %s: %s", path, err->message);
        g_error_free (err);
        T.starting = T.restarting = FALSE;
    } else {
        g_message ("started %s", path);
    }
    g_free (path);
}

static void cmd_done (const char *reply, gpointer user)
{
    char *what = user;
    JsonNode *n = reply ? wpj_parse_data (reply, -1) : NULL;
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    if (!o)
        g_message ("%s: no reply from walpe-daemon", what);
    else if (!wpj_bool (o, "ok", FALSE))
        g_message ("%s: %s", what, wpj_str (o, "error", "failed"));
    if (n)
        json_node_unref (n);
    g_free (what);
    poll_now ();
}

static void send_cmd (const char *json)
{
    char *what = g_strdup (json);
    tray_ipc (json, IPC_TIMEOUT_MS, cmd_done, what);
}

static void send_open (const char *path, int monitor)
{
    char *esc = walpe_json_escape (path);
    char *req = monitor >= 0
        ? g_strdup_printf ("{\"cmd\":\"open\",\"path\":\"%s\",\"monitor\":%d}", esc, monitor)
        : g_strdup_printf ("{\"cmd\":\"open\",\"path\":\"%s\"}", esc);
    send_cmd (req);
    g_free (req);
    g_free (esc);
}

static void read_config_flags (void)
{
    /* the daemon saves config.json on every "set": the source of truth
     * for settings the status reply does not report */
    WalpeConfig *c = walpe_config_load ();
    T.set_fs = c->pause_fullscreen;
    T.set_covered = c->pause_covered;
    if (!T.alive) {
        g_free (T.path);
        T.path = g_strdup (c->current);
        T.volume = c->volume;
        T.mute = c->mute;
        T.sound_on = c->scene_sound;
        T.paused = c->paused;
    }
    walpe_config_free (c);
}

static void on_status (const char *reply, gpointer user)
{
    (void) user;
    T.poll_inflight = FALSE;
    if (T.quitting)
        return;
    JsonNode *n = reply ? wpj_parse_data (reply, -1) : NULL;
    JsonObject *st = n && JSON_NODE_HOLDS_OBJECT (n) ? wpj_obj (json_node_get_object (n), "state") : NULL;
    gboolean was_alive = T.alive;
    T.have_status = TRUE;
    if (st) {
        T.alive = TRUE;
        g_free (T.path);
        T.path = g_strdup (wpj_str (st, "path", ""));
        g_free (T.type);
        T.type = g_strdup (wpj_str (st, "type", "none"));
        T.paused = wpj_bool (st, "paused", FALSE);
        T.fs_pause = wpj_bool (st, "fullscreen_pause", FALSE);
        T.covered_pause = wpj_bool (st, "covered_pause", FALSE);
        T.dpms_pause = wpj_bool (st, "dpms_pause", FALSE);
        T.volume = wpj_num (st, "volume", 0);
        T.mute = wpj_bool (st, "mute", FALSE);
        T.sound_on = wpj_bool (st, "scene_sound", FALSE);
        if (T.mons)
            g_ptr_array_set_size (T.mons, 0);
        else
            T.mons = g_ptr_array_new_with_free_func (mon_free);
        /* version 0.11.1: "monitors" = [{index,name,geometry,current,paused,type,follows}];
         * older daemons report a count there (no per-monitor submenu then) */
        JsonNode *mn = json_object_get_member (st, "monitors");
        if (mn && JSON_NODE_HOLDS_ARRAY (mn)) {
            JsonArray *a = json_node_get_array (mn);
            for (guint i = 0; i < json_array_get_length (a); i++) {
                JsonObject *mo = wpj_arr_obj (a, i);
                if (!mo)
                    continue;
                MonInfo *m = g_new0 (MonInfo, 1);
                m->index = (int) wpj_num (mo, "index", i);
                m->name = g_strdup (wpj_str (mo, "name", ""));
                m->current = g_strdup (wpj_str (mo, "current", ""));
                m->type = g_strdup (wpj_str (mo, "type", ""));
                m->paused = wpj_bool (mo, "paused", FALSE);
                m->follows = json_object_has_member (mo, "follows") ? (int) wpj_num (mo, "follows", -1) : -1;
                JsonArray *g = wpj_arr (mo, "geometry");
                for (guint k = 0; g && k < 4 && k < json_array_get_length (g); k++)
                    m->geom[k] = (int) json_array_get_int_element (g, k);
                g_ptr_array_add (T.mons, m);
            }
        }
        /* Saved when the list is another one afterwards, not when a push
         * moved something: with two monitors showing two wallpapers the
         * second push moves its wallpaper in front of the first and the
         * last one (the engine's "path", the first monitor's) moves that
         * back - the same list as before, written again (a new file,
         * two fsync, a rename) for every status line of the engine. */
        char *before = recent_sig ();
        for (guint i = 0; T.mons && i < T.mons->len; i++)
            recent_push (((MonInfo *) T.mons->pdata[i])->current);
        recent_push (T.path);
        recent_drop_deleted ();
        char *after = recent_sig ();
        if (strcmp (before, after) || recent_unsaved)
            recent_save ();
        g_free (before);
        g_free (after);
        if (T.starting || T.restarting) {
            if (T.starting || (T.restarting && !was_alive)) {
                T.starting = T.restarting = FALSE;
                g_message ("engine is running");
            }
        }
    } else {
        T.alive = FALSE;
        if (T.mons)
            g_ptr_array_set_size (T.mons, 0);
        if (T.restarting && was_alive == FALSE && g_get_monotonic_time () < T.transition_until) {
            /* old daemon gone: start the new one once */
            static gint64 spawned_for;
            if (spawned_for != T.transition_until) {
                spawned_for = T.transition_until;
                spawn_daemon ();
            }
        }
    }
    if (was_alive != T.alive)
        g_message ("walpe-daemon %s", T.alive ? "is running" : "is not running");
    res_update (st);
    if (n)
        json_node_unref (n);
    read_config_flags ();
    rebuild ();
}

static void poll_now (void)
{
    if (T.poll_inflight)
        return;
    T.poll_inflight = TRUE;
    tray_ipc ("{\"cmd\":\"status\"}", IPC_TIMEOUT_MS, on_status, NULL);
}

/* The poll timer runs only when it has to: every 300 ms while a start /
 * restart triggered here is under way, every 2 s against a daemon that
 * cannot push its state (older build), never otherwise - the subscription
 * delivers every change, and tells when the daemon went or came back. */
static gboolean poll_tick (gpointer user);

static void poll_reschedule (void)
{
    if (T.quitting) {
        if (T.poll_timer)
            g_source_remove (T.poll_timer);
        T.poll_timer = 0;
        T.poll_ms = 0;
        return;
    }
    guint ms = in_transition () ? FAST_POLL_MS
               : (!T.sub || walpe_ipc_sub_unsupported (T.sub)) ? POLL_MS : 0;
    if (ms == T.poll_ms)
        return;
    if (T.poll_timer)
        g_source_remove (T.poll_timer);
    T.poll_ms = ms;
    T.poll_timer = ms ? g_timeout_add (ms, poll_tick, NULL) : 0;
}

static gboolean poll_tick (gpointer user)
{
    (void) user;
    if (T.quitting) {
        T.poll_timer = 0;
        T.poll_ms = 0;
        return G_SOURCE_REMOVE;
    }
    poll_now ();
    guint ms = in_transition () ? FAST_POLL_MS
               : (!T.sub || walpe_ipc_sub_unsupported (T.sub)) ? POLL_MS : 0;
    if (ms != T.poll_ms) {
        T.poll_timer = 0;       /* this source ends */
        T.poll_ms = 0;
        poll_reschedule ();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* the daemon's status line (pushed), or NULL: it went away */
static void on_sub (const char *line, gpointer user)
{
    (void) user;
    if (T.quitting)
        return;
    /* the engine's last line when it leaves on request ({"event":"quit"}):
     * no state in it and nothing to act on - the end of the connection
     * follows, and that is what a restart started here waits for */
    if (line && strstr (line, "\"event\":\"quit\"") && !strstr (line, "\"state\""))
        return;
    on_status (line, NULL);
    poll_reschedule ();
}

static void begin_transition (gboolean restart)
{
    if (restart)
        T.restarting = TRUE;
    else
        T.starting = TRUE;
    T.transition_until = g_get_monotonic_time () + 12 * G_USEC_PER_SEC;
    poll_reschedule ();
    rebuild ();
}

/* ================================================================ library */

typedef struct { gboolean random; char *current; } LibJob;

static void lib_job_free (gpointer p)
{
    LibJob *j = p;
    g_free (j->current);
    g_free (j);
}

/* worker thread: scan the library like the GUI does (WalPE library +
 * Steam WE workshop folders, each item once), pick the next / a random one */
static void lib_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    LibJob *j = data;
    WalpeConfig *cfg = walpe_config_load ();
    GList *all = walpe_store_scan (cfg->wallpapers_dir);
    GHashTable *have = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = all; l; l = l->next)
        g_hash_table_add (have, g_path_get_basename (((WalpeEntry *) l->data)->path));
    char **libs = walpe_workshop_steam_libraries ();
    for (int i = 0; libs[i]; i++) {
        GList *se = walpe_store_scan (libs[i]);
        for (GList *l = se; l; l = l->next) {
            WalpeEntry *e = l->data;
            char *base = g_path_get_basename (e->path);
            if (g_hash_table_contains (have, base)) {
                g_free (base);
                g_object_unref (e);
                continue;
            }
            g_hash_table_add (have, base);
            all = g_list_append (all, e);
        }
        g_list_free (se);
    }
    g_strfreev (libs);
    g_hash_table_unref (have);
    walpe_config_free (cfg);

    GPtrArray *ok = g_ptr_array_new ();
    int cur = -1;
    for (GList *l = all; l; l = l->next) {
        WalpeEntry *e = l->data;
        if (!g_strcmp0 (e->type, "unsupported"))
            continue;
        if (j->current && !g_strcmp0 (e->path, j->current))
            cur = ok->len;
        g_ptr_array_add (ok, e->path);
    }
    char *pick = NULL;
    if (ok->len == 1 && cur < 0)
        pick = g_strdup (ok->pdata[0]);
    else if (ok->len > 1) {
        int k;
        if (j->random) {
            do
                k = g_random_int_range (0, ok->len);
            while (k == cur);
        } else {
            k = (cur + 1) % ok->len;
        }
        pick = g_strdup (ok->pdata[k]);
    }
    g_ptr_array_free (ok, TRUE);
    g_list_free_full (all, g_object_unref);
    g_task_return_pointer (task, pick, g_free);
}

static void lib_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src; (void) user;
    char *pick = g_task_propagate_pointer (G_TASK (res), NULL);
    if (pick) {
        g_message ("switching to %s", pick);
        send_open (pick, -1);
    } else {
        g_message ("library: nothing else to switch to");
    }
    g_free (pick);
}

static void library_pick (gboolean random)
{
    LibJob *j = g_new0 (LibJob, 1);
    j->random = random;
    j->current = g_strdup (T.path);
    GTask *t = g_task_new (NULL, NULL, lib_done, NULL);
    g_task_set_task_data (t, j, lib_job_free);
    g_task_run_in_thread (t, lib_thread);
    g_object_unref (t);
}

/* ================================================================ GUI */

static void ui_call (const char *method, GVariant *params, GAsyncReadyCallback cb)
{
    if (!T.bus) {
        GTask *t = g_task_new (NULL, NULL, cb, NULL);
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "no session bus");
        g_object_unref (t);
        if (params)
            g_variant_unref (g_variant_ref_sink (params));
        return;
    }
    g_dbus_connection_call (T.bus, WALPE_UI_APP_ID, WALPE_UI_OBJECT, "org.freedesktop.Application",
                            method, params, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, cb, NULL);
}

static gboolean finish_call (GObject *src, GAsyncResult *res)
{
    GError *e = NULL;
    GVariant *r = NULL;
    if (G_IS_TASK (res))
        g_task_propagate_boolean (G_TASK (res), &e);
    else
        r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (r)
        g_variant_unref (r);
    gboolean ok = !e;
    g_clear_error (&e);
    return ok;
}

gboolean tray_x11_raise_class (const char *res_class); /* popup.c */

/* ---- activation token
 * Wayland compositors let a window come to the front only for a token that
 * proves a user action (xdg-activation). The tray has no surface the click
 * happened on, so the panel hands its own token over before it calls
 * Activate / an item's event (StatusNotifierItem.ProvideXdgActivationToken,
 * sni.c). It is good for one use and only for a moment. */
#define ACT_TOKEN_MAX_AGE_US (10 * G_USEC_PER_SEC)
/* A token is a short opaque word (compositors build it from digits, a
 * counter, a host name). It arrives from whoever calls the method on the
 * session bus and goes into the environment of the GUI that is started
 * and into a D-Bus call. One that is not such a word is not passed on:
 * a string of a few hundred kilobytes in the environment makes the GUI's
 * start fail (the kernel's limit for one string), and control characters
 * have no business in either place. */
#define ACT_TOKEN_MAX_LEN 512

static gboolean act_token_ok (const char *token)
{
    gsize n = 0;
    for (const guchar *p = (const guchar *) token; p && *p; p++)
        if (*p < 0x20 || *p == 0x7f || ++n > ACT_TOKEN_MAX_LEN)
            return FALSE;
    return n > 0;
}

void tray_set_activation_token (const char *token)
{
    g_free (T.act_token);
    T.act_token = act_token_ok (token) ? g_strdup (token) : NULL;
    T.act_token_us = g_get_monotonic_time ();
}

/* the token, if one was handed over recently; the caller owns it (g_free) */
static char *take_activation_token (void)
{
    char *t = T.act_token;
    T.act_token = NULL;
    if (t && g_get_monotonic_time () - T.act_token_us > ACT_TOKEN_MAX_AGE_US)
        g_clear_pointer (&t, g_free);
    return t;
}

static void spawn_gui (void)
{
    gint64 now = g_get_monotonic_time ();
    if (now - T.last_gui_spawn < 5 * G_USEC_PER_SEC)
        return; /* double clicks: one window */
    T.last_gui_spawn = now;
    char *path = walpe_tray_sibling_exe ("walpe");
    char *argv[] = { path, NULL };
    GError *err = NULL;
    /* a token goes to the new process the way launchers pass it (GTK reads
     * both names at start-up); without one the environment is inherited
     * untouched, as before */
    char **envp = NULL;
    char *token = take_activation_token ();
    if (token) {
        envp = g_get_environ ();
        envp = g_environ_setenv (envp, "XDG_ACTIVATION_TOKEN", token, TRUE);
        envp = g_environ_setenv (envp, "DESKTOP_STARTUP_ID", token, TRUE);
        g_free (token);
    }
    gboolean spawned = g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    g_strfreev (envp);
    if (!spawned) {
        g_warning ("cannot start %s: %s", path, err->message);
        g_error_free (err);
    } else {
        g_message ("started %s", path);
    }
    g_free (path);
}

static void on_activated (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    if (finish_call (src, res)) {
        g_message ("raised the running WalPE window (D-Bus Activate)");
        g_free (take_activation_token ());   /* used */
        return;
    }
    /* a GUI from before version 0.12 is not a unique GApplication: find its
     * X11 window, else start one */
    if (tray_x11_raise_class ("walpe"))
        g_message ("raised the running WalPE window (_NET_ACTIVE_WINDOW)");
    else
        spawn_gui ();
}

/* org.freedesktop.Application platform data: the activation token under
 * the two names GApplication knows ("activation-token", and
 * "desktop-startup-id" for toolkits from before xdg-activation); empty
 * when the panel gave none. peek = leave the token for a call that
 * follows (the fallback that starts the GUI). */
static GVariant *platform_data (gboolean peek)
{
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    char *token = peek ? g_strdup (T.act_token) : take_activation_token ();
    if (token && peek && g_get_monotonic_time () - T.act_token_us > ACT_TOKEN_MAX_AGE_US)
        g_clear_pointer (&token, g_free);
    if (token) {
        g_variant_builder_add (&b, "{sv}", "activation-token", g_variant_new_string (token));
        g_variant_builder_add (&b, "{sv}", "desktop-startup-id", g_variant_new_string (token));
        g_free (token);
    }
    return g_variant_builder_end (&b);
}

void tray_open_gui (void)
{
    tray_popup_hide ();
    /* the token stays until the reply: when no GUI runs, spawn_gui () needs it */
    ui_call ("Activate", g_variant_new ("(@a{sv})", platform_data (TRUE)), on_activated);
}

static void on_settings_called (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    if (finish_call (src, res)) {
        g_free (take_activation_token ());   /* used */
        return;
    }
    /* not running: start it, run the action once its name appears */
    T.pending_settings = TRUE;
    if (tray_x11_raise_class ("walpe"))
        T.pending_settings = FALSE; /* old GUI: raising is all we can do */
    else
        spawn_gui ();
}

static void open_settings (void)
{
    GVariant *p = g_variant_new ("(sav@a{sv})", "settings", NULL, platform_data (TRUE));
    ui_call ("ActivateAction", p, on_settings_called);
}

static void ui_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer u)
{
    (void) c; (void) name; (void) owner; (void) u;
    if (T.quitting)
        return;
    T.ui_running = TRUE;
    if (T.pending_settings) {
        T.pending_settings = FALSE;
        open_settings ();
    }
}

static void ui_vanished (GDBusConnection *c, const char *name, gpointer u)
{
    (void) c; (void) name; (void) u;
    T.ui_running = FALSE;
}

/* ================================================================ monitor resolution
 * The dialog belongs to the GUI. The tray has the menu entry that opens
 * it, names monitors nobody was asked about in its menu, and - when the
 * GUI is not running - announces a monitor that is connected with one
 * desktop notification. It never opens a window by itself, and announces
 * nothing while a fullscreen application has the focus (resmodel.c). */

#define RES_NOTIFY_ID "monitor-resolution"

static void on_resolution_called (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    if (finish_call (src, res)) {
        g_free (take_activation_token ());   /* used (it went along in the call) */
        return;
    }
    /* A window that is there but could not be told (no session bus: it is
     * not a unique application then; or it did not answer in time and takes
     * the call when it can) is raised, as for "Settings…": starting
     * `walpe --resolution` next to it would be a second window program on
     * the same library and engine. Its menu has "Monitor resolution…". */
    if (tray_x11_raise_class ("walpe"))
        return;
    /* no GUI: start one that opens the dialog when it is up (it lists
     * every monitor; the ones not asked about are marked) */
    gint64 now = g_get_monotonic_time ();
    if (now - T.last_gui_spawn < 5 * G_USEC_PER_SEC)
        return;
    T.last_gui_spawn = now;
    char *path = walpe_tray_sibling_exe ("walpe");
    char *argv[] = { path, "--resolution", NULL };
    GError *err = NULL;
    if (!g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        g_warning ("cannot start %s: %s", path, err->message);
        g_error_free (err);
    } else {
        g_message ("started %s --resolution", path);
    }
    g_free (path);
}

/* names: output names separated by line breaks, NULL or "" = all monitors */
static void open_resolution (const char *names)
{
    char **v = names && *names ? g_strsplit (names, "\n", 64) : NULL;
    GVariantBuilder av;
    g_variant_builder_init (&av, G_VARIANT_TYPE ("av"));
    g_variant_builder_add (&av, "v", g_variant_new_strv ((const char *const *) v, v ? -1 : 0));
    /* (the click's activation token goes along, as for "settings": the
     * dialog can then come to the front on a Wayland session) */
    GVariant *p = g_variant_new ("(sav@a{sv})", WALPE_MONRES_ACTION, &av, platform_data (TRUE));
    g_strfreev (v);
    tray_popup_hide ();
    ui_call ("ActivateAction", p, on_resolution_called);
}

static void res_announce (const WalpeMonresState *rs, GPtrArray *names)
{
    /* no session bus (a bare X session started without one): no
     * notification service, and GIO would log criticals about its missing
     * connection; the menu line for these monitors remains */
    if (!g_application_get_dbus_connection (G_APPLICATION (T.app))) {
        walpe_monres_tracker_unshown (T.res_track, names);
        return;
    }
    char *title = NULL, *body = NULL;
    walpe_monres_announce_text (rs, names, &title, &body);
    GString *arg = g_string_new (NULL);
    for (guint i = 0; i < names->len; i++)
        g_string_append_printf (arg, "%s%s", i ? "\n" : "", (const char *) names->pdata[i]);
    GNotification *n = g_notification_new (title);
    g_notification_set_body (n, body);
    g_notification_set_default_action_and_target (n, "app.resolution", "s", arg->str);
    g_notification_add_button_with_target (n, "Choose…", "app.resolution", "s", arg->str);
    g_application_send_notification (G_APPLICATION (T.app), RES_NOTIFY_ID, n);
    g_object_unref (n);
    T.res_announced += names->len;
    g_message ("announced: %s", title);
    g_string_free (arg, TRUE);
    g_free (title);
    g_free (body);
}

/* every status (st == NULL: no engine) */
static void res_update (JsonObject *st)
{
    WalpeMonresState *rs = walpe_monres_state_new (st);
    if (!T.res_track)
        T.res_track = walpe_monres_tracker_new ();
    if (!T.res_new)
        T.res_new = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_set_size (T.res_new, 0);
    g_clear_pointer (&T.res_new_label, g_free);
    if (walpe_monres_state_unasked (rs, T.res_new) == 1) {
        char *t = walpe_monres_mon_title (walpe_monres_state_find (rs, T.res_new->pdata[0]));
        T.res_new_label = g_strdup_printf ("Choose the render resolution of %s…", t);
        g_free (t);
    } else if (T.res_new->len > 1) {
        T.res_new_label = g_strdup_printf ("Choose the render resolution of %u monitors…",
                                           T.res_new->len);
    }
    GPtrArray *back = NULL;
    GPtrArray *news = walpe_monres_tracker_feed (T.res_track, rs, NULL, &back);
    if (back) {
        T.res_announced = T.res_announced > back->len ? T.res_announced - back->len : 0;
        if (!T.res_announced)
            g_application_withdraw_notification (G_APPLICATION (T.app), RES_NOTIFY_ID);
        g_ptr_array_unref (back);
    }
    if (news) {
        /* a running GUI announces it itself: one notification, not two.
         * Then nothing of the tray's is up for these monitors, and the
         * tracker is told so (T.res_announced counts what the tray sent:
         * taking back what it never sent withdrew the notification of
         * another monitor that was still to be asked about). */
        if (T.ui_running)
            walpe_monres_tracker_unshown (T.res_track, news);
        else
            res_announce (rs, news);
        g_ptr_array_unref (news);
    }
    walpe_monres_state_free (rs);
}

/* ================================================================ actions */

static GtkWidget *quit_win;
static GtkWidget *quit_detail_lbl;

static void on_quit_reply (const char *reply, gpointer user)
{
    (void) reply; (void) user;
    g_application_quit (G_APPLICATION (T.app));
}

static void tray_quit_gui (void)
{
    if (!T.bus)
        return;
    g_dbus_connection_call (T.bus, WALPE_UI_APP_ID, WALPE_UI_OBJECT, "org.freedesktop.Application",
                            "ActivateAction", g_variant_new ("(sava{sv})", "quit", NULL, NULL),
                            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, NULL, NULL, NULL);
}

static void quit_all (void)
{
    if (T.quitting)
        return;
    T.quitting = TRUE;
    if (T.poll_timer) {
        g_source_remove (T.poll_timer);
        T.poll_timer = 0;
        T.poll_ms = 0;
    }
    if (T.sub) {
        walpe_ipc_sub_free (T.sub);
        T.sub = NULL;
    }
    if (quit_win)
        gtk_window_destroy (GTK_WINDOW (quit_win));
    tray_popup_hide ();
    tray_quit_gui ();
    g_message ("quit: stopping walpe-daemon");
    tray_ipc ("{\"cmd\":\"quit\"}", IPC_TIMEOUT_MS, on_quit_reply, NULL);
}

static void on_confirm (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    int b = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (src), res, NULL);
    if (b == 1)
        quit_all ();
    g_application_release (G_APPLICATION (T.app));
}

static const char *quit_detail (void)
{
    return T.alive ? "This stops the wallpaper engine (the desktop goes back to its plain background) "
                     "and removes the tray icon."
                   : "This removes the tray icon.";
}

/* ---- "Quit WalPE?" on a Wayland session
 * The question is a window of the tray's own there, not a GtkAlertDialog:
 * a Wayland compositor brings a new window to the front only for an
 * activation token, and the token for this click is the one the panel
 * handed over right before the menu's event (KDE Plasma does that for
 * every entry of the menu; ProvideXdgActivationToken in sni.c). The alert
 * dialog makes its window itself and takes none, so the question could
 * come up without the focus or behind the windows that are open - with
 * nothing else on the screen saying that "Quit WalPE" waits for an answer.
 * Without a token (a panel that gives none) the window is shown as any new
 * window is. */

static void on_quit_answer (GtkButton *b, gpointer user)
{
    (void) user;
    gboolean quit = g_object_get_data (G_OBJECT (b), "walpe-quit") != NULL;
    if (quit_win)
        gtk_window_destroy (GTK_WINDOW (quit_win));
    if (quit)
        quit_all ();
}

static void ask_quit_window (void)
{
    char *token = take_activation_token ();
    if (!quit_win) {
        GtkWidget *w = quit_win = gtk_window_new ();
        g_object_add_weak_pointer (G_OBJECT (w), (gpointer *) &quit_win);
        gtk_window_set_application (GTK_WINDOW (w), T.app);
        gtk_window_set_title (GTK_WINDOW (w), "Quit WalPE?");
        gtk_window_set_resizable (GTK_WINDOW (w), FALSE);
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_top (box, 18);
        gtk_widget_set_margin_bottom (box, 18);
        gtk_widget_set_margin_start (box, 18);
        gtk_widget_set_margin_end (box, 18);
        GtkWidget *head = gtk_label_new ("Quit WalPE?");
        gtk_widget_add_css_class (head, "title-2");
        gtk_box_append (GTK_BOX (box), head);
        GtkWidget *detail = quit_detail_lbl = gtk_label_new (quit_detail ());
        g_object_add_weak_pointer (G_OBJECT (detail), (gpointer *) &quit_detail_lbl);
        gtk_label_set_wrap (GTK_LABEL (detail), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (detail), 48);
        gtk_label_set_xalign (GTK_LABEL (detail), 0);
        gtk_box_append (GTK_BOX (box), detail);
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_set_homogeneous (GTK_BOX (row), TRUE);
        gtk_widget_set_margin_top (row, 6);
        GtkWidget *no = gtk_button_new_with_label ("Cancel");
        GtkWidget *yes = gtk_button_new_with_label ("Quit");
        g_object_set_data (G_OBJECT (yes), "walpe-quit", GINT_TO_POINTER (1));
        g_signal_connect (no, "clicked", G_CALLBACK (on_quit_answer), NULL);
        g_signal_connect (yes, "clicked", G_CALLBACK (on_quit_answer), NULL);
        gtk_box_append (GTK_BOX (row), no);
        gtk_box_append (GTK_BOX (row), yes);
        gtk_box_append (GTK_BOX (box), row);
        gtk_window_set_child (GTK_WINDOW (w), box);
        /* as in the alert dialog: Return is "Quit", Escape is "Cancel" */
        gtk_window_set_default_widget (GTK_WINDOW (w), yes);
        gtk_window_set_focus (GTK_WINDOW (w), yes);
        GtkEventController *keys = gtk_shortcut_controller_new ();
        gtk_shortcut_controller_add_shortcut (
            GTK_SHORTCUT_CONTROLLER (keys),
            gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                              gtk_named_action_new ("window.close")));
        gtk_widget_add_controller (w, keys);
    } else if (quit_detail_lbl) {
        gtk_label_set_text (GTK_LABEL (quit_detail_lbl), quit_detail ());
    }
    /* before it is shown: the token goes out with the window's first
     * appearance. For a question that is already open (a second click on
     * "Quit WalPE") it brings that window to the front. */
    if (token)
        gtk_window_set_startup_id (GTK_WINDOW (quit_win), token);
    gtk_window_present (GTK_WINDOW (quit_win));
    g_free (token);
}

static gboolean session_is_x11 (void);

static void ask_quit (void)
{
    if (T.quitting)
        return;
    if (!T.conf->confirm_quit || !gdk_display_get_default ()) {
        quit_all ();
        return;
    }
    if (!session_is_x11 ()) {
        ask_quit_window ();
        return;
    }
    GtkAlertDialog *d = gtk_alert_dialog_new ("Quit WalPE?");
    gtk_alert_dialog_set_detail (d, quit_detail ());
    const char *btns[] = { "Cancel", "Quit", NULL };
    gtk_alert_dialog_set_buttons (d, btns);
    gtk_alert_dialog_set_cancel_button (d, 0);
    gtk_alert_dialog_set_default_button (d, 1);
    g_application_hold (G_APPLICATION (T.app));
    gtk_alert_dialog_choose (d, NULL, NULL, on_confirm, NULL);
    g_object_unref (d);
}

gboolean tray_confirm_quit (void)
{
    return T.conf && T.conf->confirm_quit;
}

gboolean tray_engine_alive (void)
{
    return T.alive;
}

/* Wallpaper sound is switched on and silent by one of the two settings the
 * menu's "Mute" / "Unmute" stands for: the volume is 0, or "mute" is on
 * (walpe-ctl, the settings window). Only the volume was looked at: with
 * "mute" on the entry read "Mute", a click on it set the volume to 0 as
 * well, and the "Unmute" that followed brought the volume back and left
 * "mute" on - still silent, and the entry said "Mute" again. */
static gboolean tray_silent (void)
{
    return T.mute || T.volume <= 0.001;
}

/* The third state, and up to 0.14.0 the one a new configuration was in
 * (sound is on by default now, at volume 0.5): wallpaper sound is switched
 * off (the setting scene_sound). That is not "muted": the entry read
 * "Unmute" all the same (the volume was 0 by default), a click set the
 * volume to 1, the entry then read "Mute" - and nothing was heard, because
 * nothing here switched the sound on. The entry names that state and
 * switches it on. */
static const char *tray_sound_label (void)
{
    return !T.sound_on ? "Switch wallpaper sound on" : tray_silent () ? "Unmute" : "Mute";
}

void tray_dispatch (const char *action, const char *arg)
{
    if (!action)
        return;
    g_message ("menu: %s%s%s", action, arg ? " " : "", arg ? arg : "");
    if (!strcmp (action, "open-gui")) {
        tray_open_gui ();
    } else if (!strcmp (action, "pause-toggle")) {
        send_cmd (T.paused ? "{\"cmd\":\"resume\"}" : "{\"cmd\":\"pause\"}");
        T.paused = !T.paused; /* optimistic: the next poll confirms */
        rebuild ();
    } else if (!strcmp (action, "mute-toggle")) {
        char vb[G_ASCII_DTOSTR_BUF_SIZE];
        if (T.sound_on && !tray_silent ()) {
            T.volume_before_mute = T.volume;
            recent_save ();
            send_cmd ("{\"cmd\":\"set\",\"volume\":0}");
        } else {
            /* heard (again): the sound switched on, "mute" off, and a volume
             * above 0 - the one that is set, else the one from before the
             * menu's "Mute", else one half as the settings window sets it */
            double v = T.volume;
            if (v <= 0.001)
                v = !T.sound_on ? 0.5 : T.volume_before_mute > 0.001 ? T.volume_before_mute : 1.0;
            char *req = g_strdup_printf ("{\"cmd\":\"set\",\"scene_sound\":true,\"mute\":false,\"volume\":%s}",
                                         g_ascii_dtostr (vb, sizeof vb, v));
            send_cmd (req);
            g_free (req);
        }
    } else if (!strcmp (action, "next")) {
        library_pick (FALSE);
    } else if (!strcmp (action, "random")) {
        library_pick (TRUE);
    } else if (!strcmp (action, "open") && arg) {
        send_open (arg, -1);
    } else if (!strcmp (action, "open-mon") && arg) {
        /* arg = "<index>\t<path>" */
        const char *tab = strchr (arg, '\t');
        if (tab)
            send_open (tab + 1, atoi (arg));
    } else if (!strcmp (action, "toggle-fs")) {
        send_cmd (T.set_fs ? "{\"cmd\":\"set\",\"pause_fullscreen\":false}"
                           : "{\"cmd\":\"set\",\"pause_fullscreen\":true}");
    } else if (!strcmp (action, "toggle-covered")) {
        send_cmd (T.set_covered ? "{\"cmd\":\"set\",\"pause_covered\":false}"
                                : "{\"cmd\":\"set\",\"pause_covered\":true}");
    } else if (!strcmp (action, "settings")) {
        open_settings ();
    } else if (!strcmp (action, "resolution")) {
        open_resolution (arg);
    } else if (!strcmp (action, "start")) {
        begin_transition (FALSE);
        spawn_daemon ();
    } else if (!strcmp (action, "restart")) {
        if (!T.alive) {
            begin_transition (FALSE);
            spawn_daemon ();
        } else {
            begin_transition (TRUE);
            /* on_status starts the new daemon once the old one is gone */
            tray_ipc ("{\"cmd\":\"quit\"}", IPC_TIMEOUT_MS, NULL, NULL);
        }
    } else if (!strcmp (action, "quit")) {
        ask_quit ();
    } else if (!strcmp (action, "quit-now")) {
        quit_all ();
    }
}

/* ================================================================ state -> UI */

static TrayItem *add (TrayItem *parent, TrayItemKind k, const char *label, const char *action)
{
    TrayItem *it = tray_item_new (k, label, action);
    tray_item_append (parent, it);
    return it;
}

static void add_sep (TrayItem *parent)
{
    add (parent, TRAY_ITEM_SEPARATOR, NULL, NULL);
}

static const char *mon_label (MonInfo *m, char *buf, gsize n)
{
    if (m->name && *m->name && !g_str_has_prefix (m->name, "mon"))
        g_snprintf (buf, n, "Monitor %d (%s)", m->index + 1, m->name);
    else
        g_snprintf (buf, n, "Monitor %d", m->index + 1);
    return buf;
}

static TrayItem *build_menu (void)
{
    TrayItem *root = tray_item_new (TRAY_ITEM_SUBMENU, "WalPE", NULL);
    gboolean busy = in_transition ();
    gboolean live = T.alive && !busy;

    TrayItem *it = add (root, TRAY_ITEM_NORMAL, "Open WalPE", "open-gui");
    it->is_default = TRUE;
    if (T.res_new_label && T.res_new && T.res_new->len) {
        /* a monitor nobody was asked about: its render resolution can be
         * chosen from here (the dialog opens for these monitors) */
        GString *names = g_string_new (NULL);
        for (guint i = 0; i < T.res_new->len; i++)
            g_string_append_printf (names, "%s%s", i ? "\n" : "", (const char *) T.res_new->pdata[i]);
        it = add (root, TRAY_ITEM_NORMAL, T.res_new_label, "resolution");
        it->arg = g_string_free (names, FALSE);
    }
    add_sep (root);

    if (!T.alive || busy) {
        it = add (root, TRAY_ITEM_NORMAL,
                  T.restarting ? "Restarting engine…" : T.starting ? "Starting engine…" : "Start engine",
                  "start");
        it->enabled = !busy;
        add_sep (root);
    }

    it = add (root, TRAY_ITEM_NORMAL, T.paused ? "Resume all wallpapers" : "Pause all wallpapers",
              "pause-toggle");
    it->enabled = live;
    it = add (root, TRAY_ITEM_NORMAL, tray_sound_label (), "mute-toggle");
    it->enabled = live;
    it = add (root, TRAY_ITEM_NORMAL, "Next wallpaper", "next");
    it->enabled = live;
    it = add (root, TRAY_ITEM_NORMAL, "Random wallpaper", "random");
    it->enabled = live;

    /* per-monitor submenus (status "monitors" array, version 0.11.1) */
    if (live && T.mons && T.mons->len >= 2) {
        TrayItem *ms = add (root, TRAY_ITEM_SUBMENU, "Monitors", NULL);
        ms->name = "monitors";
        for (guint i = 0; i < T.mons->len; i++) {
            MonInfo *m = T.mons->pdata[i];
            char nb[96];
            char *lbl = g_strdup_printf ("%s: %s%s", mon_label (m, nb, sizeof nb),
                                         title_for (m->current), m->paused ? " (paused)" : "");
            TrayItem *sub = add (ms, TRAY_ITEM_SUBMENU, lbl, NULL);
            g_free (lbl);
            if (m->geom[2] > 0) {
                char *g = g_strdup_printf ("%d×%d at %d,%d%s", m->geom[2], m->geom[3], m->geom[0],
                                           m->geom[1], m->follows >= 0 ? " · shared rendering" : "");
                add (sub, TRAY_ITEM_INFO, g, NULL);
                g_free (g);
            }
            int shown = 0;
            /* the current one first, then the recent list */
            TrayItem *c = add (sub, TRAY_ITEM_RADIO, title_for (m->current), "open-mon");
            c->checked = TRUE;
            c->arg = g_strdup_printf ("%d\t%s", m->index, m->current);
            for (GList *l = T.recent.head; l && shown < 8; l = l->next) {
                const char *p = l->data;
                if (!g_strcmp0 (p, m->current))
                    continue;
                TrayItem *r = add (sub, TRAY_ITEM_RADIO, title_for (p), "open-mon");
                r->arg = g_strdup_printf ("%d\t%s", m->index, p);
                shown++;
            }
            if (T.path && *T.path && g_strcmp0 (m->current, T.path)) {
                add_sep (sub);
                char *d = g_strdup_printf ("Use the default (%s)", title_for (T.path));
                TrayItem *r = add (sub, TRAY_ITEM_NORMAL, d, "open-mon");
                r->arg = g_strdup_printf ("%d\t%s", m->index, T.path);
                g_free (d);
            }
        }
    }
    /* recent wallpapers on every monitor */
    if (T.recent.length > 0) {
        TrayItem *rs = add (root, TRAY_ITEM_SUBMENU,
                            T.mons && T.mons->len >= 2 ? "Recent (all monitors)" : "Recent wallpapers",
                            NULL);
        rs->name = "recent";
        rs->enabled = live;
        int shown = 0;
        for (GList *l = T.recent.head; l && shown < 8; l = l->next, shown++) {
            TrayItem *r = add (rs, TRAY_ITEM_RADIO, title_for (l->data), "open");
            r->arg = g_strdup (l->data);
            gboolean all_this = T.mons && T.mons->len > 0;
            for (guint i = 0; T.mons && i < T.mons->len; i++)
                all_this &= !g_strcmp0 (((MonInfo *) T.mons->pdata[i])->current, l->data);
            r->checked = all_this || (!T.mons && !g_strcmp0 (T.path, l->data));
        }
    }
    add_sep (root);

    it = add (root, TRAY_ITEM_CHECK, "Pause on fullscreen", "toggle-fs");
    it->checked = T.set_fs;
    it->enabled = live;
    it = add (root, TRAY_ITEM_CHECK, "Pause when covered", "toggle-covered");
    it->checked = T.set_covered;
    it->enabled = live;
    add_sep (root);

    add (root, TRAY_ITEM_NORMAL, "Settings…", "settings");
    add (root, TRAY_ITEM_NORMAL, "Monitor resolution…", "resolution");
    if (T.alive && !busy)
        add (root, TRAY_ITEM_NORMAL, "Restart engine", "restart");
    add_sep (root);
    add (root, TRAY_ITEM_NORMAL, "Quit WalPE", "quit");
    return root;
}

static void sig_item (GString *s, TrayItem *it)
{
    g_string_append_printf (s, "%d|%s|%s|%s|%d%d%d%d{", it->kind, it->label, it->action ? it->action : "",
                            it->arg ? it->arg : "", it->enabled, it->visible, it->checked, it->is_default);
    for (guint i = 0; it->children && i < it->children->len; i++)
        sig_item (s, it->children->pdata[i]);
    g_string_append_c (s, '}');
}

static void rebuild (void)
{
    /* tooltip + icon */
    gboolean busy = in_transition ();
    TrayIconState ist;
    GString *tip = g_string_new (NULL);
    if (busy) {
        ist = TRAY_ICON_DEAD;
        g_string_append (tip, T.restarting ? "Restarting the engine…" : "Starting the engine…");
    } else if (!T.alive) {
        ist = TRAY_ICON_DEAD;
        g_string_append (tip, T.have_status ? "Engine not running (right-click: Start engine)"
                                            : "Connecting to the engine…");
    } else {
        if (T.paused)
            ist = TRAY_ICON_PAUSED;
        else if (T.fs_pause || T.covered_pause || T.dpms_pause)
            ist = TRAY_ICON_AUTOPAUSED;
        else
            ist = TRAY_ICON_RUNNING;
        if (T.mons && T.mons->len >= 2) {
            for (guint i = 0; i < T.mons->len; i++) {
                MonInfo *m = T.mons->pdata[i];
                char nb[96];
                g_string_append_printf (tip, "%s%s: %s%s", i ? "\n" : "", mon_label (m, nb, sizeof nb),
                                        title_for (m->current), m->paused ? " (paused)" : "");
            }
        } else {
            g_string_append (tip, title_for (T.path));
            if (T.type && *T.type && g_strcmp0 (T.type, "none"))
                g_string_append_printf (tip, " (%s)", T.type);
        }
        if (T.paused)
            g_string_append (tip, "\nPaused");
        else if (T.fs_pause)
            g_string_append (tip, "\nPaused: a fullscreen app is running");
        else if (T.covered_pause)
            g_string_append (tip, "\nPaused while covered by windows");
        else if (T.dpms_pause)
            g_string_append (tip, "\nPaused: display asleep");
        if (T.sound_on && tray_silent ())
            g_string_append (tip, " · muted");
    }
    gboolean changed = ist != T.icon_state || g_strcmp0 (tip->str, T.tip_text);
    T.icon_state = ist;
    g_free (T.tip_text);
    T.tip_text = g_string_free (tip, FALSE);
    if (!T.tip_title)
        T.tip_title = g_strdup ("WalPE");

    TrayItem *m = build_menu ();
    GString *s = g_string_new (NULL);
    sig_item (s, m);
    if (g_strcmp0 (s->str, T.menu_sig)) {
        tray_item_free (T.menu);
        tray_ids_number (&menu_ids, m);
        T.menu = m;
        g_free (T.menu_sig);
        T.menu_sig = g_string_free (s, FALSE);
        T.menu_serial++;
        changed = TRUE;
        if (tray_popup_visible ())
            tray_popup_refresh ();
    } else {
        tray_item_free (m);
        g_string_free (s, TRUE);
    }
    if (changed && T.backend && T.backend->update)
        T.backend->update ();
}

/* ================================================================ backends */

static void use_backend (const TrayBackend *b)
{
    if (T.backend == b)
        return;
    if (T.backend) {
        g_message ("stopping the %s backend", T.backend->name);
        T.backend->stop ();
    }
    T.backend = NULL;
    if (b && b->start ()) {
        T.backend = b;
        g_message ("using the %s backend", b->name);
        if (b->update)
            b->update ();
    } else if (b) {
        g_warning ("the %s backend could not start", b->name);
    }
}

/* the toolkit talks X11 (by type name: no X header here, and right in a
 * build of GTK without the X11 backend) */
static gboolean session_is_x11 (void)
{
    GdkDisplay *d = gdk_display_get_default ();
    return d && g_str_has_prefix (G_OBJECT_TYPE_NAME (d), "GdkX11");
}

/* traysel.h decides; what it has to say is logged when it changes */
static void pick_backend (const char *want)
{
    const char *xd = g_getenv ("DISPLAY");
    TrayEnv e = {
        .want = want,
        .have_bus = T.bus != NULL,
        .watcher = T.watcher,
        .host = T.sni_host,
        .session_x11 = session_is_x11 (),
        .x_display = xd && *xd,
#ifdef WALPE_TRAY_XEMBED
        .built_xembed = TRUE,
#endif
    };
    const char *note = NULL;
    TrayPick p = tray_pick (&e, &note);
    if (note && note != T.pick_note)
        g_message ("%s", note);
    T.pick_note = note;
    switch (p) {
    case TRAY_PICK_SNI:
        use_backend (&tray_backend_sni);
        break;
#ifdef WALPE_TRAY_XEMBED
    case TRAY_PICK_XEMBED:
        use_backend (&tray_backend_xembed);
        break;
#endif
    default:
        use_backend (NULL);
        break;
    }
}

static void choose_auto (void)
{
    pick_backend ("auto");
}

static void on_host_prop (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
    gboolean host = FALSE;
    if (r) {
        GVariant *v = NULL;
        g_variant_get (r, "(v)", &v);
        if (v && g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN))
            host = g_variant_get_boolean (v);
        if (v)
            g_variant_unref (v);
        g_variant_unref (r);
    }
    g_message ("StatusNotifierWatcher present, host registered: %s", host ? "yes" : "no");
    T.sni_host = host;
    choose_auto ();
}

static void check_host (void)
{
    g_dbus_connection_call (T.bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.kde.StatusNotifierWatcher",
                                           "IsStatusNotifierHostRegistered"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, NULL,
                            on_host_prop, NULL);
}

static void on_host_registered (GDBusConnection *c, const char *s, const char *p, const char *i,
                                const char *sig, GVariant *params, gpointer u)
{
    (void) c; (void) s; (void) p; (void) i; (void) sig; (void) params; (void) u;
    T.watcher = TRUE;
    T.sni_host = TRUE;
    choose_auto ();
}

static void watcher_appeared (GDBusConnection *c, const char *name, const char *owner, gpointer u)
{
    (void) c; (void) name; (void) owner; (void) u;
    T.watcher = TRUE;
    check_host ();
}

static void watcher_vanished (GDBusConnection *c, const char *name, gpointer u)
{
    (void) c; (void) name; (void) u;
    T.watcher = FALSE;
    T.sni_host = FALSE;
    choose_auto ();
}

static void start_backends (void)
{
    const char *want = T.backend_opt ? T.backend_opt : g_getenv ("WALPE_TRAY_BACKEND");
    if (!want || !*want)
        want = T.conf->backend;
    if (!g_strcmp0 (want, "sni") || !g_strcmp0 (want, "xembed")) {
        pick_backend (want);
        return;
    }
    if (!T.bus) {
        /* nothing to follow: X11 = XEmbed as before, Wayland = no icon (said once) */
        pick_backend ("auto");
        return;
    }
    /* auto: on X11, SNI when a watcher with a host exists and XEmbed
     * otherwise; on Wayland SNI. Follow the watcher coming and going */
    T.host_sub = g_dbus_connection_signal_subscribe (T.bus, NULL, "org.kde.StatusNotifierWatcher",
                                                     "StatusNotifierHostRegistered",
                                                     "/StatusNotifierWatcher", NULL,
                                                     G_DBUS_SIGNAL_FLAGS_NONE, on_host_registered,
                                                     NULL, NULL);
    T.watcher_watch = g_bus_watch_name_on_connection (T.bus, "org.kde.StatusNotifierWatcher",
                                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                      watcher_appeared, watcher_vanished, NULL, NULL);
}

/* ================================================================ app */

static void act_quit (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    g_message ("quit requested (tray only, the engine keeps running)");
    g_application_quit (G_APPLICATION (T.app));
}

static void act_open (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) p; (void) u;
    tray_open_gui ();
}

static void act_menu (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) u;
    /* testing / keyboard launchers: "walpe-tray --menu" pops the menu at
     * the pointer */
    int x = -1, y = -1;
    if (p)
        g_variant_get (p, "(ii)", &x, &y);
    tray_popup_show (x, y);
}

static void act_dispatch (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) u;
    tray_dispatch (g_variant_get_string (p, NULL), NULL);
}

/* the "new monitor" notification was clicked: p = its output names */
static void act_resolution (GSimpleAction *a, GVariant *p, gpointer u)
{
    (void) a; (void) u;
    const char *names = p ? g_variant_get_string (p, NULL) : NULL;
    tray_dispatch ("resolution", names && *names ? names : NULL);
}

static void on_startup (GApplication *app, gpointer u)
{
    (void) u;
    g_application_hold (app); /* no windows: live until Quit */
    T.bus = g_application_get_dbus_connection (app);
    if (T.bus)
        g_object_ref (T.bus);
    static const GActionEntry acts[] = {
        { "quit", act_quit, NULL, NULL, NULL, { 0 } },
        { "open", act_open, NULL, NULL, NULL, { 0 } },
        { "menu", act_menu, "(ii)", NULL, NULL, { 0 } },
        { "dispatch", act_dispatch, "s", NULL, NULL, { 0 } },
        { "resolution", act_resolution, "s", NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries (G_ACTION_MAP (app), acts, G_N_ELEMENTS (acts), NULL);
    recent_load ();
    read_config_flags ();
    if (T.bus)
        T.ui_watch = g_bus_watch_name_on_connection (T.bus, WALPE_UI_APP_ID, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     ui_appeared, ui_vanished, NULL, NULL);
    rebuild ();
    start_backends ();
    poll_now ();
    {
        char *sp = walpe_socket_path ();
        T.sub = walpe_ipc_sub_new (sp, on_sub, NULL);
        g_free (sp);
    }
    poll_reschedule ();
}

static void on_activate (GApplication *app, gpointer u)
{
    /* a second "walpe-tray" (autostart + .xinitrc): nothing to do, the
     * first instance stays (GApplication uniqueness) */
    (void) app; (void) u;
}

static gboolean on_term_signal (gpointer u)
{
    (void) u;
    T.quitting = TRUE;
    if (T.app)
        g_application_quit (G_APPLICATION (T.app));
    return G_SOURCE_REMOVE;
}

static void on_shutdown (GApplication *app, gpointer u)
{
    (void) app; (void) u;
    T.quitting = TRUE;
    if (T.poll_timer) {
        g_source_remove (T.poll_timer);
        T.poll_timer = 0;
        T.poll_ms = 0;
    }
    if (T.sub) {
        walpe_ipc_sub_free (T.sub);
        T.sub = NULL;
    }
    if (T.ui_watch) {
        g_bus_unwatch_name (T.ui_watch);
        T.ui_watch = 0;
    }
    if (quit_win)
        gtk_window_destroy (GTK_WINDOW (quit_win));
    tray_popup_hide ();
    if (T.backend)
        T.backend->stop ();
    T.backend = NULL;
    if (T.bus) {
        g_dbus_connection_flush_sync (T.bus, NULL, NULL);
        g_clear_object (&T.bus);
    }
}

int main (int argc, char **argv)
{
    char *export_icon = NULL, *icon_state = NULL, *backend = NULL;
    int icon_size = 256;
    gboolean quit = FALSE, menu = FALSE, version = FALSE;
    char *action = NULL;
    GOptionEntry entries[] = {
        { "backend", 0, 0, G_OPTION_ARG_STRING, &backend, "Tray protocol: auto (default), sni or xembed", "NAME" },
        { "quit", 0, 0, G_OPTION_ARG_NONE, &quit, "Close the running walpe-tray (the engine keeps running)", NULL },
        { "menu", 0, 0, G_OPTION_ARG_NONE, &menu, "Show the running tray's menu at the pointer", NULL },
        { "action", 0, 0, G_OPTION_ARG_STRING, &action, "Run a menu action in the running tray (pause-toggle, next, ...)", "ACTION" },
        { "export-icon", 0, 0, G_OPTION_ARG_FILENAME, &export_icon, "Write the icon as PNG and exit", "FILE" },
        { "icon-size", 0, 0, G_OPTION_ARG_INT, &icon_size, "Size for --export-icon (default 256)", "PX" },
        { "icon-state", 0, 0, G_OPTION_ARG_STRING, &icon_state, "running, paused, autopaused or dead", "STATE" },
        { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Print the version", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL },
    };
    GOptionContext *oc = g_option_context_new ("- WalPE tray icon");
    g_option_context_add_main_entries (oc, entries, NULL);
    GError *err = NULL;
    if (!g_option_context_parse (oc, &argc, &argv, &err)) {
        g_printerr ("%s\n", err->message);
        return 1;
    }
    g_option_context_free (oc);
    if (version) {
        g_print ("walpe-tray %s \"%s\" (source %s)\n", WALPE_VERSION, WALPE_CODENAME, walpe_source_id ());
        return 0;
    }
    if (export_icon) {
        TrayIconState st = TRAY_ICON_RUNNING;
        if (!g_strcmp0 (icon_state, "paused")) st = TRAY_ICON_PAUSED;
        else if (!g_strcmp0 (icon_state, "autopaused")) st = TRAY_ICON_AUTOPAUSED;
        else if (!g_strcmp0 (icon_state, "dead")) st = TRAY_ICON_DEAD;
        return tray_icon_export_png (export_icon, icon_size, st) ? 0 : 1;
    }

    /* a tray needs no GPU: the menu and tooltip are a few cairo-drawn
     * widgets. Without this GTK opens an EGL/GL context on start-up
     * (~100 MB RSS with Mesa, ~30 MB without). User overrides win. */
    g_setenv ("GSK_RENDERER", "cairo", FALSE);
    if (!g_getenv ("GDK_DISABLE") && !g_getenv ("GDK_DEBUG")) {
        if (gtk_get_major_version () == 4 && gtk_get_minor_version () >= 16)
            g_setenv ("GDK_DISABLE", "gl", FALSE);
        else
            g_setenv ("GDK_DEBUG", "gl-disable", FALSE);
    }
    T.conf = walpe_tray_conf_load ();
    T.backend_opt = backend;
    T.app = gtk_application_new (WALPE_TRAY_APP_ID, G_APPLICATION_DEFAULT_FLAGS);

    if (quit || menu || action) {
        /* remote control of the running instance */
        if (!g_application_register (G_APPLICATION (T.app), NULL, NULL) ||
            !g_application_get_is_remote (G_APPLICATION (T.app))) {
            g_printerr ("walpe-tray is not running\n");
            return 2;
        }
        if (quit)
            g_action_group_activate_action (G_ACTION_GROUP (T.app), "quit", NULL);
        if (menu)
            g_action_group_activate_action (G_ACTION_GROUP (T.app), "menu", g_variant_new ("(ii)", -1, -1));
        if (action)
            g_action_group_activate_action (G_ACTION_GROUP (T.app), "dispatch", g_variant_new_string (action));
        GDBusConnection *c = g_application_get_dbus_connection (G_APPLICATION (T.app));
        if (c)
            g_dbus_connection_flush_sync (c, NULL, NULL);
        return 0; /* no unref: a registered GApplication finalized outside
                   * g_application_run () only warns */
    }

    g_signal_connect (T.app, "startup", G_CALLBACK (on_startup), NULL);
    g_signal_connect (T.app, "activate", G_CALLBACK (on_activate), NULL);
    g_signal_connect (T.app, "shutdown", G_CALLBACK (on_shutdown), NULL);
    g_unix_signal_add (SIGINT, on_term_signal, NULL);
    g_unix_signal_add (SIGTERM, on_term_signal, NULL);
    char *args[] = { argv[0], NULL };
    int rc = g_application_run (G_APPLICATION (T.app), 1, args);
    g_object_unref (T.app);
    walpe_tray_conf_free (T.conf);
    return rc;
}
