/* workshop_page.c - the Workshop page (see workshop_page.h)
 *
 * Grid: GtkGridView over a GListStore of WsEntry (one workshop item each);
 * tiles are recycled, thumbnails are fetched/decoded off the main thread
 * at tile size (walpe_ws_preview_async, disk-cached) and pushed into the
 * entry's "thumb" property, which the bound tile follows - scrolling never
 * waits on the network or a decoder. 30 items per page, Prev/Next.
 *
 * Download = the daemon's "install" IPC (steamcmd with the cached login of
 * the configured steam_user; WalPE never sees a password), progress from
 * "status" (install_phase / install_progress) polled every 0.5 s. Every
 * request to the daemon goes through uiipc's worker threads (ipc_async). Items
 * already in a Steam client library need no download ("Set as wallpaper").
 *
 * Subscribe on Steam when downloading (version 0.12, a setting, off by default,
 * needs the Web API key; common/steamsub.h): steamcmd's download subscribes
 * nothing, so the page asks Steam itself (walpe_ws_subscribe_async).
 *   - Steam client running with Wallpaper Engine installed: subscribe FIRST
 *     and let the client download the item into its own library - one copy,
 *     one download, kept up to date by Steam. If the client shows no sign of
 *     it within WAIT_START_S the page falls back to steamcmd.
 *   - otherwise: steamcmd download as before, then subscribe.
 * Whatever Steam answers is shown; a refused subscription never stops the
 * download. "Open on Steam" shows the item's page in the Steam client. */
#include "workshop_page.h"
#include "thumbs.h"             /* animated previews (thumbs_anim_watch) */
#include "uiipc.h"              /* the daemon is asked off the main thread */
#include "common/workshop.h"
#include "common/steamsub.h"
#include "common/util.h"
#include "common/uitext.h"
#include "common/ipc.h"
#include "common/config.h"
#include <string.h>
#include <glib/gstdio.h>

#define TILE 196
#define WAIT_START_S 45         /* the Steam client must show a sign by then */
#define WAIT_TOTAL_S (30 * 60)  /* ... and deliver within this */

/* ------------------------------------------------------------- WsEntry */

#define WS_TYPE_ENTRY (ws_entry_get_type ())
G_DECLARE_FINAL_TYPE (WsEntry, ws_entry, WS, ENTRY, GObject)

typedef enum { LOCAL_NONE, LOCAL_WALPE, LOCAL_WALPE_OLD, LOCAL_STEAM } Local;

struct _WsEntry {
    GObject parent;
    WalpeWsItem *item;
    GdkPaintable *thumb;
    char *thumb_file;       /* the cached preview download behind thumb */
    gboolean pending, failed;
    Local local;
    char *local_path;
};

G_DEFINE_FINAL_TYPE (WsEntry, ws_entry, G_TYPE_OBJECT)

enum { PROP_0, PROP_THUMB, N_PROPS };
static GParamSpec *entry_props[N_PROPS];

static void ws_entry_get_property (GObject *o, guint id, GValue *v, GParamSpec *ps)
{
    if (id == PROP_THUMB)
        g_value_set_object (v, WS_ENTRY (o)->thumb);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void ws_entry_finalize (GObject *o)
{
    WsEntry *e = WS_ENTRY (o);
    walpe_ws_item_unref (e->item);
    g_clear_object (&e->thumb);
    g_free (e->thumb_file);
    g_free (e->local_path);
    G_OBJECT_CLASS (ws_entry_parent_class)->finalize (o);
}

static void ws_entry_class_init (WsEntryClass *k)
{
    GObjectClass *oc = G_OBJECT_CLASS (k);
    oc->finalize = ws_entry_finalize;
    oc->get_property = ws_entry_get_property;
    entry_props[PROP_THUMB] = g_param_spec_object ("thumb", NULL, NULL, GDK_TYPE_PAINTABLE,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (oc, N_PROPS, entry_props);
}

static void ws_entry_init (WsEntry *e) { (void) e; }

static void entry_set_thumb (WsEntry *e, GdkPaintable *p)
{
    if (g_set_object (&e->thumb, p))
        g_object_notify_by_pspec (G_OBJECT (e), entry_props[PROP_THUMB]);
}

/* ---------------------------------------------------------------- page */

typedef struct {
    WalpeWorkshopHooks hooks;
    char *library_dir;
    char **steam_libs;
    WalpeWsClient *client;
    WalpeWsSettings *settings;

    GtkWidget *root;
    GtkSearchEntry *search;
    GtkDropDown *sort, *resolution;
    GtkToggleButton *filters_btn;
    GtkRevealer *filters;
    GHashTable *chip_state;        /* canonical tag -> 0 off / 1 required / 2 excluded */
    GPtrArray *chips;              /* GtkButton* (data "tag") */
    GtkWidget *mature_chip, *question_chip;

    GListStore *model;
    GtkSingleSelection *sel;
    GtkWidget *grid_sw;
    GtkLabel *page_lbl, *msg;
    GtkWidget *prev, *next, *spinner, *retry;
    GCancellable *search_cancel, *thumbs_cancel;
    guint page, total_pages;
    guint chip_debounce;

    /* details */
    WsEntry *cur;
    gulong cur_thumb_h;        /* cur's notify::thumb -> d_pic until the full preview */
    gboolean d_full;
    GCancellable *detail_cancel;
    GtkWidget *d_box;
    GtkPicture *d_pic;
    GtkLabel *d_title, *d_author, *d_stats, *d_state, *d_err, *d_desc;
    GtkWidget *d_tags, *d_action, *d_steam, *d_prog, *d_cancel;

    /* install */
    char *inst_id;
    guint poll_id;
    gboolean poll_inflight;    /* a status request of the progress poll is out */
    gboolean user_known;       /* settings: the daemon told the account name */
    char *user_shown;          /* settings: the name field when it was opened */
    char *starting_id;         /* a download of this item is being asked for */

    /* subscribed; waiting for the Steam client to deliver the item */
    GCancellable *sub_cancel;  /* cancelled only when the page goes */
    char *wait_id, *wait_title;
    guint64 wait_size;
    guint wait_gen;            /* a cancelled wait's late answers are stale */
    guint wait_src;            /* 1 s poll, 0 while the request is out */
    gint64 wait_since;
    gboolean wait_seen;        /* the client knows the item / is downloading */
    gboolean sub_denied;       /* Steam refused the key for subscribing: not
                                * asked again until the settings are saved */
    GHashTable *notes;         /* item id -> Note: what last happened to its
                                * download / subscription, shown with the item
                                * (the main window's tab has no status line) */

    /* settings */
    GtkEntry *set_user;
    GtkPasswordEntry *set_key;
    GtkSwitch *set_mature, *set_question, *set_subscribe;
    GtkLabel *set_msg;
} Page;

static void run_search (Page *p, guint page);
static void show_details (Page *p, WsEntry *e);
static char *default_ipc (const char *json);

typedef struct { char *msg; gboolean error; } Note;
static void note_show (Page *p, const Note *n);

static void status (Page *p, const char *m)
{
    if (p->hooks.status)
        p->hooks.status (m);
}

static JsonObject *ipc_obj (Page *p, const char *req, JsonNode **root)
{
    char *reply = p->hooks.ipc ? p->hooks.ipc (req) : NULL;
    *root = reply ? wpj_parse_data (reply, -1) : NULL;
    g_free (reply);
    return *root && JSON_NODE_HOLDS_OBJECT (*root) ? json_node_get_object (*root) : NULL;
}

/* Requests to the daemon never run on the GTK main thread. The
 * page used walpe_ipc_request there - connect, write and a wait of up to
 * 3 s for the answer - for the progress poll (twice a second during a
 * download), "Set as wallpaper", starting and cancelling a download and
 * the settings: with an engine that was slow to answer (loading a
 * wallpaper, stopped) the window did not paint or take input meanwhile,
 * and "Set as wallpaper" called a wallpaper that needed more than 3 s to
 * load "not reachable" although it was applied. They go through uiipc now
 * (worker threads, 2.5 s / 8 s limits, like the rest of the GUI); the
 * answer comes back as a call of `done` on the main thread - or not at
 * all when the page is gone by then. reply = NULL: no usable answer.
 * A host's own hooks.ipc (not the GUI's) is still called directly. */
typedef void (*IpcDone) (Page *p, JsonObject *reply, gpointer data);

typedef struct {
    GWeakRef root;          /* the page's widget: gone = the Page is freed */
    IpcDone done;
    gpointer data;
    GDestroyNotify free_data;
} IpcCall;

static void ipc_call_reply (JsonObject *reply, const char *err, gpointer user)
{
    (void) err;
    IpcCall *c = user;
    GObject *root = g_weak_ref_get (&c->root);
    if (root) {
        Page *p = g_object_get_data (root, "walpe-workshop-page");
        if (p && c->done)
            c->done (p, reply, c->data);
        g_object_unref (root);
    }
    if (c->free_data)
        c->free_data (c->data);
    g_weak_ref_clear (&c->root);
    g_free (c);
}

static void ipc_async (Page *p, const char *req, guint flags, IpcDone done, gpointer data,
                       GDestroyNotify free_data)
{
    IpcCall *c = g_new0 (IpcCall, 1);
    g_weak_ref_init (&c->root, p->root);
    c->done = done;
    c->data = data;
    c->free_data = free_data;
    if (p->hooks.ipc != default_ipc) {
        JsonNode *root = NULL;
        JsonObject *o = ipc_obj (p, req, &root);
        ipc_call_reply (o, NULL, c);
        if (root)
            json_node_unref (root);
        return;
    }
    ui_ipc_send (req, flags, ipc_call_reply, c);
}

/* ------------------------------------------------------- local state */

static gboolean has_project (const char *dir, char **out)
{
    char *pj = g_build_filename (dir, "project.json", NULL);
    gboolean ok = g_file_test (pj, G_FILE_TEST_IS_REGULAR);
    g_free (pj);
    if (ok && out)
        *out = g_strdup (dir);
    return ok;
}

static void entry_local (Page *p, WsEntry *e)
{
    g_clear_pointer (&e->local_path, g_free);
    e->local = LOCAL_NONE;
    const char *id = e->item->id;
    char *d = p->library_dir ? g_build_filename (p->library_dir, id, NULL) : NULL;
    if (d && has_project (d, &e->local_path)) {
        /* copies get fresh mtimes: older than the item's update = outdated */
        char *pj = g_build_filename (d, "project.json", NULL);
        GStatBuf st;
        e->local = g_stat (pj, &st) == 0 && e->item->time_updated > 0 &&
                           (gint64) st.st_mtime < e->item->time_updated
                       ? LOCAL_WALPE_OLD : LOCAL_WALPE;
        g_free (pj);
    }
    g_free (d);
    for (int i = 0; e->local == LOCAL_NONE && p->steam_libs && p->steam_libs[i]; i++) {
        char *s = g_build_filename (p->steam_libs[i], id, NULL);
        if (has_project (s, &e->local_path))
            e->local = LOCAL_STEAM;
        g_free (s);
    }
}

static const char *badge_text (Local l)
{
    switch (l) {
    case LOCAL_WALPE: return "Installed";
    case LOCAL_WALPE_OLD: return "Update";
    case LOCAL_STEAM: return "In Steam";
    default: return NULL;
    }
}

static const char *type_of (const WalpeWsItem *it)
{
    static const char *const types[] = { "Scene", "Video", "Web", "Application", "Preset", NULL };
    for (int i = 0; types[i]; i++)
        if (walpe_ws_item_has_tag (it, types[i]))
            return types[i];
    return "";
}

static const char *resolution_of (const WalpeWsItem *it)
{
    guint n;
    const WalpeWsTagCategory *c = walpe_ws_tag_categories (&n);
    for (guint i = 0; i < n; i++)
        if (!strcmp (c[i].name, "Resolution"))
            for (const char *const *t = c[i].tags; *t; t++)
                if (walpe_ws_item_has_tag (it, *t))
                    return *t;
    return NULL;
}

static char *fmt_count (guint64 n)
{
    if (n >= 1000000)
        return g_strdup_printf ("%.1fM", n / 1e6);
    if (n >= 10000)
        return g_strdup_printf ("%.0fK", n / 1e3);
    if (n >= 1000)
        return g_strdup_printf ("%.1fK", n / 1e3);
    return g_strdup_printf ("%" G_GUINT64_FORMAT, n);
}

/* ------------------------------------------------------------- tiles */

static void thumb_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    WsEntry *e = user;
    char *file = walpe_ws_preview_file (res);
    GdkPixbuf *pb = walpe_ws_preview_finish (res, NULL);
    e->pending = FALSE;
    if (pb) {
        GdkTexture *t = thumbs_texture_for_pixbuf (pb);
        g_free (e->thumb_file);
        e->thumb_file = file;   /* the cached download: plays when it is an animated GIF */
        file = NULL;
        entry_set_thumb (e, GDK_PAINTABLE (t));
        g_object_unref (t);
        g_object_unref (pb);
    } else {
        e->failed = TRUE;   /* no retry loop on scroll; next search retries */
    }
    g_free (file);
    g_object_unref (e);
}

static void tile_setup (GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void) f; (void) user;
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class (box, "ws-tile");
    GtkWidget *ov = gtk_overlay_new ();
    GtkWidget *pic = gtk_picture_new ();
    gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request (pic, TILE, TILE);
    gtk_overlay_set_child (GTK_OVERLAY (ov), pic);
    GtkWidget *badge = gtk_label_new (NULL);
    gtk_widget_add_css_class (badge, "ws-badge");
    gtk_widget_set_halign (badge, GTK_ALIGN_START);
    gtk_widget_set_valign (badge, GTK_ALIGN_START);
    gtk_overlay_add_overlay (GTK_OVERLAY (ov), badge);
    GtkWidget *title = gtk_label_new (NULL);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (title), 22);
    gtk_label_set_xalign (GTK_LABEL (title), 0);
    GtkWidget *cap = gtk_label_new (NULL);
    gtk_label_set_ellipsize (GTK_LABEL (cap), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign (GTK_LABEL (cap), 0);
    gtk_widget_add_css_class (cap, "dim-label");
    gtk_widget_add_css_class (cap, "caption");
    gtk_box_append (GTK_BOX (box), ov);
    gtk_box_append (GTK_BOX (box), title);
    gtk_box_append (GTK_BOX (box), cap);
    gtk_list_item_set_child (li, box);
    g_object_set_data (G_OBJECT (li), "pic", pic);
    g_object_set_data (G_OBJECT (li), "badge", badge);
    g_object_set_data (G_OBJECT (li), "title", title);
    g_object_set_data (G_OBJECT (li), "cap", cap);
}

/* a tile's picture: the still, and - when the preview the CDN sent is an
 * animated GIF - its animation under the library's rules (thumbs.h: only
 * while visible in the active window, same limits, the same setting) */
static void tile_show_thumb (GtkPicture *pic, WsEntry *e)
{
    thumbs_anim_unwatch (pic);
    gtk_picture_set_paintable (pic, e->thumb);
    if (e->thumb && e->thumb_file) {
        int sc = gtk_widget_get_scale_factor (GTK_WIDGET (pic));
        thumbs_anim_watch (pic, e->thumb_file, TILE * sc, TILE * sc, THUMB_COVER);
    }
}

static void tile_thumb_changed (WsEntry *e, GParamSpec *ps, GtkPicture *pic)
{
    (void) ps;
    tile_show_thumb (pic, e);
}

static void tile_badge (GtkListItem *li, WsEntry *e)
{
    GtkWidget *badge = g_object_get_data (G_OBJECT (li), "badge");
    const char *b = badge_text (e->local);
    gtk_label_set_text (GTK_LABEL (badge), b ? b : "");
    gtk_widget_set_visible (badge, b != NULL);
}

static void tile_bind (GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void) f;
    Page *p = user;
    WsEntry *e = gtk_list_item_get_item (li);
    GtkPicture *pic = g_object_get_data (G_OBJECT (li), "pic");
    gtk_label_set_text (g_object_get_data (G_OBJECT (li), "title"),
                        e->item->title ? e->item->title : e->item->id);
    const char *res = resolution_of (e->item);
    char *subs = fmt_count (e->item->subscriptions);
    char *cap = g_strdup_printf ("%s%s%s · %s subs", type_of (e->item), res ? " · " : "",
                                 res ? res : "", subs);
    gtk_label_set_text (g_object_get_data (G_OBJECT (li), "cap"), cap);
    g_free (cap);
    g_free (subs);
    tile_badge (li, e);
    tile_show_thumb (pic, e);
    gulong h = g_signal_connect (e, "notify::thumb", G_CALLBACK (tile_thumb_changed), pic);
    g_object_set_data (G_OBJECT (li), "handler", GSIZE_TO_POINTER (h));
    if (!e->thumb && !e->pending && !e->failed && e->item->preview_url && p->client) {
        e->pending = TRUE;
        walpe_ws_preview_async (p->client, e->item->id, e->item->preview_url, TRUE, TILE, TILE,
                                p->thumbs_cancel, thumb_done, g_object_ref (e));
    }
}

static void tile_unbind (GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void) f; (void) user;
    WsEntry *e = gtk_list_item_get_item (li);
    gulong h = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (li), "handler"));
    if (e && h)
        g_signal_handler_disconnect (e, h);
    g_object_set_data (G_OBJECT (li), "handler", NULL);
    thumbs_anim_unwatch (g_object_get_data (G_OBJECT (li), "pic"));
    gtk_picture_set_paintable (g_object_get_data (G_OBJECT (li), "pic"), NULL);
}

/* ------------------------------------------------------------ search */

static void set_busy (Page *p, gboolean busy, const char *msg)
{
    gtk_widget_set_visible (p->spinner, busy);
    if (busy)
        gtk_spinner_start (GTK_SPINNER (p->spinner));
    else
        gtk_spinner_stop (GTK_SPINNER (p->spinner));
    gtk_label_set_text (p->msg, msg ? msg : "");
    gtk_widget_set_visible (p->retry, FALSE);
}

static void update_pager (Page *p, guint64 total, const char *source)
{
    char *t = fmt_count (total);
    char *s;
    if (p->total_pages)
        s = g_strdup_printf ("Page %u of %u · %s items · %s", p->page, p->total_pages, t,
                             !g_strcmp0 (source, "webapi") ? "Steam Web API" : "workshop page");
    else
        s = g_strdup_printf ("Page %u", p->page);
    gtk_label_set_text (p->page_lbl, s);
    g_free (s);
    g_free (t);
    gtk_widget_set_sensitive (p->prev, p->page > 1);
    gtk_widget_set_sensitive (p->next, p->total_pages == 0 || p->page < p->total_pages);
}

static void fill_model (Page *p, GPtrArray *items)
{
    /* new page: drop the old tiles' pending thumbnail work */
    g_cancellable_cancel (p->thumbs_cancel);
    g_object_unref (p->thumbs_cancel);
    p->thumbs_cancel = g_cancellable_new ();
    GPtrArray *objs = g_ptr_array_new_with_free_func (g_object_unref);
    for (guint i = 0; i < items->len; i++) {
        WsEntry *e = g_object_new (WS_TYPE_ENTRY, NULL);
        e->item = walpe_ws_item_ref (g_ptr_array_index (items, i));
        entry_local (p, e);
        g_ptr_array_add (objs, e);
    }
    g_list_store_splice (p->model, 0, g_list_model_get_n_items (G_LIST_MODEL (p->model)),
                         objs->pdata, objs->len);
    g_ptr_array_unref (objs);
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (p->grid_sw));
    gtk_adjustment_set_value (va, 0);
    if (items->len == 0)
        show_details (p, NULL);
}

static void search_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GError *err = NULL;
    WalpeWsResults *r = walpe_ws_search_finish (res, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free (err);   /* superseded or page closed: p may be gone */
        return;
    }
    Page *p = user;
    if (!r) {
        set_busy (p, FALSE, err->message);
        gtk_widget_set_visible (p->retry, TRUE);
        g_error_free (err);
        return;
    }
    p->page = r->page;
    p->total_pages = r->total_pages;
    fill_model (p, r->items);
    set_busy (p, FALSE, r->items->len ? r->notice
                        : "No wallpapers match. Try fewer tags or another search.");
    update_pager (p, r->total, r->source);
    walpe_ws_results_free (r);
}

static void details_one_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GError *err = NULL;
    GPtrArray *a = walpe_ws_details_finish (res, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free (err);
        return;
    }
    Page *p = user;
    if (!a) {
        set_busy (p, FALSE, err->message);
        gtk_widget_set_visible (p->retry, TRUE);
        g_error_free (err);
        return;
    }
    GPtrArray *ok = g_ptr_array_new ();
    for (guint i = 0; i < a->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (a, i);
        if (it->result == 1 && it->title)
            g_ptr_array_add (ok, it);
    }
    p->page = 1;
    p->total_pages = 1;
    fill_model (p, ok);
    set_busy (p, FALSE, ok->len ? NULL : "No public workshop item with that id.");
    update_pager (p, ok->len, "webapi");
    g_ptr_array_unref (ok);
    g_ptr_array_unref (a);
}

static void strv_add (GPtrArray *a, const char *s)
{
    for (guint i = 0; i < a->len; i++)
        if (!g_strcmp0 (g_ptr_array_index (a, i), s))
            return;
    g_ptr_array_add (a, (gpointer) s);
}

static void run_search (Page *p, guint page)
{
    if (!p->client) {
        set_busy (p, FALSE, "WalPE was built without libsoup-3.0: workshop browsing is "
                            "unavailable (Download by id still works: Install from Workshop…).");
        return;
    }
    g_cancellable_cancel (p->search_cancel);
    g_object_unref (p->search_cancel);
    p->search_cancel = g_cancellable_new ();
    const char *text = gtk_editable_get_text (GTK_EDITABLE (p->search));
    /* a pasted id or filedetails link opens that item */
    char *id = walpe_workshop_parse_id (text);
    if (id) {
        const char *ids[] = { id, NULL };
        set_busy (p, TRUE, "Looking up item…");
        walpe_ws_details_async (p->client, ids, p->search_cancel, details_one_done, p);
        g_free (id);
        return;
    }
    GPtrArray *req = g_ptr_array_new (), *ex = g_ptr_array_new ();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, p->chip_state);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        if (GPOINTER_TO_INT (v) == 1)
            strv_add (req, k);
        else if (GPOINTER_TO_INT (v) == 2)
            strv_add (ex, k);
    }
    GtkStringObject *rs = gtk_drop_down_get_selected_item (p->resolution);
    guint ri = gtk_drop_down_get_selected (p->resolution);
    if (rs && ri > 0)
        strv_add (req, gtk_string_object_get_string (rs));
    if (p->settings->hide_mature)
        strv_add (ex, "Mature");
    if (p->settings->hide_questionable)
        strv_add (ex, "Questionable");
    g_ptr_array_add (req, NULL);
    g_ptr_array_add (ex, NULL);
    guint s = gtk_drop_down_get_selected (p->sort);
    WalpeWsQuery q = {
        .text = text,
        .sort = s < WALPE_WS_SORT_N ? (WalpeWsSort) s : WALPE_WS_SORT_TREND,
        .page = MAX (1u, page),
        .required = (const char *const *) req->pdata,
        .excluded = (const char *const *) ex->pdata,
    };
    set_busy (p, TRUE, "Searching the Steam Workshop…");
    walpe_ws_search_async (p->client, &q, p->search_cancel, search_done, p);
    g_ptr_array_unref (req);
    g_ptr_array_unref (ex);
}

static void on_search_changed (GtkSearchEntry *e, Page *p) { (void) e; run_search (p, 1); }
static void on_sort_changed (GObject *o, GParamSpec *ps, Page *p) { (void) o; (void) ps; run_search (p, 1); }
static void on_prev (GtkButton *b, Page *p) { (void) b; if (p->page > 1) run_search (p, p->page - 1); }
static void on_next (GtkButton *b, Page *p) { (void) b; run_search (p, p->page + 1); }
static void on_retry (GtkButton *b, Page *p) { (void) b; run_search (p, p->page ? p->page : 1); }

/* ------------------------------------------------------------- chips */

static void chip_style (GtkWidget *chip, int st)
{
    gtk_widget_remove_css_class (chip, "req");
    gtk_widget_remove_css_class (chip, "excl");
    if (st == 1)
        gtk_widget_add_css_class (chip, "req");
    else if (st == 2)
        gtk_widget_add_css_class (chip, "excl");
    const char *tag = g_object_get_data (G_OBJECT (chip), "tag");
    char *l = g_strdup_printf ("%s%s", st == 1 ? "+ " : st == 2 ? "− " : "", tag);
    gtk_button_set_label (GTK_BUTTON (chip), l);
    g_free (l);
}

static gboolean chip_debounced (gpointer user)
{
    Page *p = user;
    p->chip_debounce = 0;
    run_search (p, 1);
    return G_SOURCE_REMOVE;
}

/* click: off -> required -> excluded -> off. "single" categories (Steam
 * shows them as a <select>) allow one required tag at a time. */
static void on_chip (GtkButton *b, Page *p)
{
    const char *tag = g_object_get_data (G_OBJECT (b), "tag");
    const WalpeWsTagCategory *cat = g_object_get_data (G_OBJECT (b), "cat");
    int st = (GPOINTER_TO_INT (g_hash_table_lookup (p->chip_state, tag)) + 1) % 3;
    if (st == 1 && cat->single) {
        for (guint i = 0; i < p->chips->len; i++) {
            GtkWidget *c = g_ptr_array_index (p->chips, i);
            const char *ct = g_object_get_data (G_OBJECT (c), "tag");
            if (c != GTK_WIDGET (b) && g_object_get_data (G_OBJECT (c), "cat") == cat &&
                GPOINTER_TO_INT (g_hash_table_lookup (p->chip_state, ct)) == 1) {
                g_hash_table_insert (p->chip_state, (gpointer) ct, GINT_TO_POINTER (0));
                chip_style (c, 0);
            }
        }
    }
    g_hash_table_insert (p->chip_state, (gpointer) tag, GINT_TO_POINTER (st));
    chip_style (GTK_WIDGET (b), st);
    if (p->chip_debounce)
        g_source_remove (p->chip_debounce);
    p->chip_debounce = g_timeout_add (350, chip_debounced, p);
}

/* age chips follow the hide settings */
static void sync_age_chips (Page *p)
{
    struct { GtkWidget *w; gboolean hide; } a[] = {
        { p->mature_chip, p->settings->hide_mature },
        { p->question_chip, p->settings->hide_questionable } };
    for (int i = 0; i < 2; i++) {
        if (!a[i].w)
            continue;
        const char *tag = g_object_get_data (G_OBJECT (a[i].w), "tag");
        if (a[i].hide) {
            g_hash_table_insert (p->chip_state, (gpointer) tag, GINT_TO_POINTER (2));
            chip_style (a[i].w, 2);
            gtk_widget_set_tooltip_text (a[i].w, "Hidden by the Workshop settings (⚙)");
        } else {
            if (GPOINTER_TO_INT (g_hash_table_lookup (p->chip_state, tag)) == 2) {
                g_hash_table_insert (p->chip_state, (gpointer) tag, GINT_TO_POINTER (0));
                chip_style (a[i].w, 0);
            }
            gtk_widget_set_tooltip_text (a[i].w, NULL);
        }
        gtk_widget_set_sensitive (a[i].w, !a[i].hide);
    }
}

static GtkWidget *build_filters (Page *p)
{
    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start (outer, 10);
    gtk_widget_set_margin_end (outer, 10);
    gtk_widget_set_margin_bottom (outer, 6);
    guint n;
    const WalpeWsTagCategory *cats = walpe_ws_tag_categories (&n);
    for (guint c = 0; c < n; c++) {
        if (!strcmp (cats[c].name, "Resolution"))
            continue;   /* a dropdown in the toolbar */
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l = gtk_label_new (cats[c].name);
        gtk_widget_add_css_class (l, "dim-label");
        gtk_label_set_xalign (GTK_LABEL (l), 0);
        gtk_widget_set_valign (l, GTK_ALIGN_START);
        gtk_widget_set_margin_top (l, 4);
        gtk_widget_set_size_request (l, 100, -1);
        GtkWidget *fb = gtk_flow_box_new ();
        gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (fb), GTK_SELECTION_NONE);
        gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (fb), 30);
        gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (fb), 2);
        gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (fb), 2);
        gtk_widget_set_hexpand (fb, TRUE);
        for (const char *const *t = cats[c].tags; *t; t++) {
            GtkWidget *chip = gtk_button_new_with_label (*t);
            gtk_widget_add_css_class (chip, "ws-chip");
            gtk_widget_set_focus_on_click (chip, FALSE);
            g_object_set_data (G_OBJECT (chip), "tag", (gpointer) *t);
            g_object_set_data (G_OBJECT (chip), "cat", (gpointer) &cats[c]);
            gtk_widget_set_tooltip_text (chip, "Click: require · again: exclude · again: off");
            g_signal_connect (chip, "clicked", G_CALLBACK (on_chip), p);
            gtk_flow_box_append (GTK_FLOW_BOX (fb), chip);
            g_ptr_array_add (p->chips, chip);
            if (!strcmp (*t, "Mature"))
                p->mature_chip = chip;
            if (!strcmp (*t, "Questionable"))
                p->question_chip = chip;
        }
        gtk_box_append (GTK_BOX (row), l);
        gtk_box_append (GTK_BOX (row), fb);
        gtk_box_append (GTK_BOX (outer), row);
    }
    GtkWidget *sw = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (sw), 300);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sw), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), outer);
    return sw;
}

/* ----------------------------------------------------------- details */

static void detail_pic_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GError *err = NULL;
    GdkPixbuf *pb = walpe_ws_preview_finish (res, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free (err);
        return;
    }
    g_clear_error (&err);
    Page *p = user;
    if (pb) {
        GdkTexture *t = thumbs_texture_for_pixbuf (pb);
        p->d_full = TRUE;
        thumbs_anim_unwatch (p->d_pic);
        gtk_picture_set_paintable (p->d_pic, GDK_PAINTABLE (t));
        /* the full preview is the item's own file: often an animated GIF */
        char *file = walpe_ws_preview_file (res);
        if (file) {
            int sc = gtk_widget_get_scale_factor (GTK_WIDGET (p->d_pic));
            thumbs_anim_watch (p->d_pic, file, 320 * sc, 320 * sc, THUMB_CONTAIN);
            g_free (file);
        }
        g_object_unref (t);
        g_object_unref (pb);
    }
}

static void detail_author_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GError *err = NULL;
    char *name = walpe_ws_author_finish (res, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free (err);
        return;
    }
    g_clear_error (&err);
    Page *p = user;
    if (name && p->cur) {
        if (!p->cur->item->author)
            p->cur->item->author = g_strdup (name);
        char *s = g_strdup_printf ("by %s", name);
        gtk_label_set_text (p->d_author, s);
        g_free (s);
    }
    g_free (name);
}

/* the shown item is being fetched by the Steam client after a subscribe */
static gboolean waiting_this (Page *p)
{
    return p->wait_id && p->cur && !g_strcmp0 (p->wait_id, p->cur->item->id);
}

static gboolean installing_this (Page *p)
{
    return (p->poll_id && p->cur && !g_strcmp0 (p->inst_id, p->cur->item->id)) ||
           (p->starting_id && p->cur && !g_strcmp0 (p->starting_id, p->cur->item->id)) ||
           waiting_this (p);
}

/* the setting is on and usable (it needs the Web API key) */
static gboolean subscribe_on (Page *p)
{
    return p->client && p->settings->subscribe_on_download && !p->sub_denied &&
           walpe_ws_client_has_api_key (p->client);
}

/* subscribing alone gets the item: a running Steam client with Wallpaper
 * Engine installed downloads what the account subscribes to */
static gboolean steam_can_deliver (Page *p)
{
    return subscribe_on (p) && !p->wait_id && walpe_steam_client_running () &&
           walpe_steam_we_installed ();
}

static void update_action (Page *p)
{
    WsEntry *e = p->cur;
    if (!e)
        return;
    const char *state = NULL, *action = NULL;
    char *dl = g_format_size (e->item->file_size);
    char *dl_label = e->item->file_size ? g_strdup_printf ("Download (%s)", dl)
                                        : g_strdup ("Download");
    switch (e->local) {
    case LOCAL_WALPE:
        state = walpe_steam_sub_is_marked (e->item->id)
                    ? "Installed in your WalPE library. WalPE subscribed you to it on Steam."
                    : "Installed in your WalPE library.";
        action = "Set as wallpaper";
        break;
    case LOCAL_WALPE_OLD:
        state = "Installed, but the workshop has a newer version.";
        action = "Update";
        break;
    case LOCAL_STEAM:
        state = "Already in your Steam library (subscribed in the Steam client) - no download "
                "needed.";
        action = "Set as wallpaper";
        break;
    default:
        action = dl_label;
        if (waiting_this (p))
            state = !p->wait_src ? "Asking Steam to subscribe your account…"
                    : p->wait_seen ? "Subscribed on Steam: the Steam client is downloading it."
                    : "Subscribed on Steam: waiting for the Steam client to download it.";
        else if (steam_can_deliver (p))
            state = "“Subscribe on Steam” is on and Steam is running: WalPE subscribes your "
                    "account and the Steam client downloads the item.";
        else if (subscribe_on (p))
            state = "“Subscribe on Steam” is on: after the download WalPE subscribes your "
                    "account to this item.";
        break;
    }
    if (walpe_ws_item_has_tag (e->item, "Application"))
        state = "Application wallpapers are Windows programs: WalPE cannot run them.";
    gtk_label_set_text (p->d_state, state ? state : "");
    gtk_widget_set_visible (GTK_WIDGET (p->d_state), state != NULL);
    gtk_button_set_label (GTK_BUTTON (p->d_action), action);
    gboolean busy = installing_this (p);
    gtk_widget_set_sensitive (p->d_action, !busy);
    gtk_widget_set_visible (p->d_prog, busy);
    gtk_widget_set_visible (p->d_cancel, busy);
    g_free (dl);
    g_free (dl_label);
}

static void detail_thumb_changed (WsEntry *e, GParamSpec *ps, Page *p)
{
    (void) ps;
    if (!p->d_full) {
        thumbs_anim_unwatch (p->d_pic);
        gtk_picture_set_paintable (p->d_pic, e->thumb);
    }
}

static void show_details (Page *p, WsEntry *e)
{
    g_cancellable_cancel (p->detail_cancel);
    g_object_unref (p->detail_cancel);
    p->detail_cancel = g_cancellable_new ();
    if (p->cur && p->cur_thumb_h)
        g_signal_handler_disconnect (p->cur, p->cur_thumb_h);
    p->cur_thumb_h = 0;
    p->d_full = FALSE;
    g_set_object (&p->cur, e);
    if (e)
        p->cur_thumb_h = g_signal_connect (e, "notify::thumb", G_CALLBACK (detail_thumb_changed), p);
    gtk_widget_set_visible (p->d_box, e != NULL);
    if (!e)
        return;
    WalpeWsItem *it = e->item;
    /* three lines of heading at most; 256 x "W" (what the parser lets
     * through) made the pane a third wider and six lines tall */
    char *head = walpe_ui_text (it->title ? it->title : it->id, 120, WALPE_UITEXT_LINE);
    gtk_label_set_text (p->d_title, head);
    g_free (head);
    char *by = it->author ? g_strdup_printf ("by %s", it->author) : g_strdup ("");
    gtk_label_set_text (p->d_author, by);
    g_free (by);
    if (!it->author && it->creator && p->client)
        walpe_ws_author_async (p->client, it->creator, p->detail_cancel, detail_author_done, p);
    GString *s = g_string_new (NULL);
    if (it->stars >= 0) {
        for (int i = 0; i < 5; i++)
            g_string_append (s, i < it->stars ? "★" : "☆");
        if (it->votes >= 0)
            g_string_append_printf (s, " (%d votes)", it->votes);
    } else if (it->score >= 0) {
        g_string_append_printf (s, "%.0f%% positive (%d votes)", it->score * 100, it->votes);
    } else {
        g_string_append (s, "not rated yet");
    }
    char *subs = fmt_count (it->subscriptions);
    char *size = g_format_size (it->file_size);
    g_string_append_printf (s, "  ·  %s subscribers  ·  %s", subs, size);
    if (it->time_updated > 0) {
        GDateTime *dt = g_date_time_new_from_unix_local (it->time_updated);
        char *d = dt ? g_date_time_format (dt, "%Y-%m-%d") : NULL;
        if (d)
            g_string_append_printf (s, "  ·  updated %s", d);
        g_free (d);
        if (dt)
            g_date_time_unref (dt);
    }
    gtk_label_set_text (p->d_stats, s->str);
    g_string_free (s, TRUE);
    g_free (subs);
    g_free (size);
    /* tags */
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child (p->d_tags)))
        gtk_flow_box_remove (GTK_FLOW_BOX (p->d_tags), c);
    /* Wallpaper Engine's own tags are short and an item has a dozen: a
     * chip shows 40 characters, the pane 30 chips (one tag of 255
     * characters turned the chips into a column of full-width bars). The
     * age ratings come first and are never among the ones left out. */
    guint n_tags = g_strv_length (it->tags), shown_tags = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (char **t = it->tags; *t; t++) {
            gboolean age = !g_ascii_strcasecmp (*t, "Mature") ||
                           !g_ascii_strcasecmp (*t, "Questionable");
            if (age != (pass == 0))
                continue;
            gboolean more = pass == 1 && shown_tags >= 30;
            char *chip = more ? g_strdup_printf ("+%u more", n_tags - shown_tags)
                              : walpe_ui_text (*t, 40, WALPE_UITEXT_LINE);
            GtkWidget *l = gtk_label_new (chip);
            g_free (chip);
            gtk_widget_add_css_class (l, "ws-tag");
            if (age)
                gtk_widget_add_css_class (l, "warn");
            gtk_flow_box_append (GTK_FLOW_BOX (p->d_tags), l);
            if (more)
                break;
            shown_tags++;
        }
    }
    /* whatever the author wrote, up to Steam's 8000 characters: without
     * break opportunities (8000 x "W") the wrapped label cost 9 s of CPU for
     * three frames, with the first result selected by itself (uitext.h) */
    char *bb = walpe_ws_bbcode_to_text (it->description);
    char *desc = walpe_ui_text (bb, 16000, WALPE_UITEXT_PARAGRAPHS);
    gtk_label_set_text (p->d_desc, desc);
    g_free (desc);
    g_free (bb);
    note_show (p, g_hash_table_lookup (p->notes, it->id));
    /* small thumb at once, the full preview when it arrives */
    thumbs_anim_unwatch (p->d_pic);
    gtk_picture_set_paintable (p->d_pic, e->thumb);
    if (it->preview_url && p->client)
        walpe_ws_preview_async (p->client, it->id, it->preview_url, FALSE, 420, 420,
                                p->detail_cancel, detail_pic_done, p);
    update_action (p);
}

static void on_selected (GtkSingleSelection *sel, GParamSpec *ps, Page *p)
{
    (void) ps;
    show_details (p, gtk_single_selection_get_selected_item (sel));
}

static void detail_error (Page *p, const char *m)
{
    gtk_widget_add_css_class (GTK_WIDGET (p->d_err), "error");
    gtk_label_set_text (p->d_err, m);
    gtk_widget_set_visible (GTK_WIDGET (p->d_err), m && *m);
}

static void note_free (gpointer n)
{
    g_free (((Note *) n)->msg);
    g_free (n);
}

static void note_show (Page *p, const Note *n)
{
    detail_error (p, n ? n->msg : NULL);
    if (n && !n->error)
        gtk_widget_remove_css_class (GTK_WIDGET (p->d_err), "error");
}

/* A message about item `id` (NULL forgets it): kept with the item and shown
 * in the details pane whenever that item is; also on the status line. */
static void item_note (Page *p, const char *id, const char *m, gboolean error)
{
    Note *n = NULL;
    if (m && *m) {
        n = g_new0 (Note, 1);
        n->msg = g_strdup (m);
        n->error = error;
        g_hash_table_insert (p->notes, g_strdup (id), n);
        status (p, m);
    } else {
        g_hash_table_remove (p->notes, id);
    }
    if (p->cur && !g_strcmp0 (p->cur->item->id, id))
        note_show (p, n);
}

static void item_error (Page *p, const char *id, const char *m)
{
    item_note (p, id, m, TRUE);
}

/* --------------------------------------------------------- download */

static void refresh_local (Page *p)
{
    g_strfreev (p->steam_libs);
    p->steam_libs = walpe_workshop_steam_libraries ();
    guint n = g_list_model_get_n_items (G_LIST_MODEL (p->model));
    for (guint i = 0; i < n; i++) {
        WsEntry *e = g_list_model_get_item (G_LIST_MODEL (p->model), i);
        Local before = e->local;
        entry_local (p, e);
        if (before != e->local)   /* rebind the tile (badge) */
            g_list_model_items_changed (G_LIST_MODEL (p->model), i, 1, 1);
        g_object_unref (e);
    }
}

/* ---------------------------------------- subscribe on Steam (opt-in) */

static void start_download_full (Page *p, const char *id, guint64 size, const char *ok_note,
                                 gboolean ok_note_error, const char *fail_note);

/* one request to Steam; the page may be gone when the answer comes
 * (sub_cancel), a cancelled wait's answer is stale (gen) */
typedef struct {
    Page *p;
    char *id, *title;
    guint64 size;
    guint gen;
} SubReq;

static SubReq *sub_req (Page *p, const char *id, const char *title, guint64 size)
{
    SubReq *r = g_new0 (SubReq, 1);
    r->p = p;
    r->id = g_strdup (id);
    /* it goes into WalPE's own sentences (uitext.h) */
    r->title = walpe_ui_text (title && *title ? title : id, 80, WALPE_UITEXT_LINE);
    r->size = size;
    r->gen = p->wait_gen;
    return r;
}

static void sub_req_free (SubReq *r)
{
    g_free (r->id);
    g_free (r->title);
    g_free (r);
}

/* TRUE + *ok: the answer is in and the page is still there. A refusal of
 * the key itself (HTTP 401/403, EResult access denied) ends the asking for
 * this session: repeating it only earns the address a rate limit. */
static gboolean sub_answer (GAsyncResult *res, SubReq *r, gboolean *ok, char **why)
{
    GError *err = NULL;
    *ok = walpe_ws_subscribe_finish (res, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free (err);   /* page closed: r->p may be gone */
        sub_req_free (r);
        return FALSE;
    }
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        r->p->sub_denied = TRUE;
        *why = g_strdup_printf ("%s. WalPE does not ask Steam again until you save the "
                                "Workshop settings (⚙)", err->message);
    } else {
        *why = *ok ? NULL : g_strdup (err ? err->message : "no answer");
    }
    g_clear_error (&err);
    return TRUE;
}

/* after a steamcmd install: the item is here, now subscribe the account */
static void installed_subscribed (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    SubReq *r = user;
    gboolean ok;
    char *why = NULL;
    if (!sub_answer (res, r, &ok, &why))
        return;
    Page *p = r->p;
    if (ok) {
        walpe_steam_sub_mark (r->id, TRUE);
        char *m = g_strdup_printf ("Installed: %s. You are now subscribed to it on Steam.",
                                   r->title);
        status (p, m);
        g_free (m);
    } else {
        char *m = g_strdup_printf ("“%s” is installed, but you are not subscribed to it on "
                                   "Steam: %s. “Open on Steam” shows its page, where you can "
                                   "subscribe yourself.", r->title, why);
        item_error (p, r->id, m);
        g_free (m);
    }
    g_free (why);
    update_action (p);
    sub_req_free (r);
}

static void wait_clear (Page *p)
{
    if (p->wait_src)
        g_source_remove (p->wait_src);
    p->wait_src = 0;
    g_clear_pointer (&p->wait_id, g_free);
    g_clear_pointer (&p->wait_title, g_free);
    p->wait_gen++;
}

/* <a Steam client library>/431960/<id> with its project.json: delivered */
static gboolean steam_has_item (Page *p, const char *id)
{
    g_strfreev (p->steam_libs);   /* the first item creates the folder */
    p->steam_libs = walpe_workshop_steam_libraries ();
    gboolean found = FALSE;
    for (int i = 0; p->steam_libs[i] && !found; i++) {
        char *d = g_build_filename (p->steam_libs[i], id, NULL);
        found = has_project (d, NULL);
        g_free (d);
    }
    return found;
}

static guint64 tree_size (const char *path, int *budget)
{
    GStatBuf st;
    if (--*budget < 0 || g_lstat (path, &st) != 0)
        return 0;
    if (S_ISREG (st.st_mode))
        return (guint64) st.st_size;
    if (!S_ISDIR (st.st_mode))
        return 0;
    guint64 sum = 0;
    GDir *d = g_dir_open (path, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *c = g_build_filename (path, n, NULL);
        sum += tree_size (c, budget);
        g_free (c);
    }
    if (d)
        g_dir_close (d);
    return sum;
}

/* signs that the Steam client took the subscription up: the item's folder
 * under steamapps/workshop/{downloads,temp,content}/431960, or its id in
 * the app's workshop manifest. *bytes = what is on disk so far. */
static gboolean steam_fetching (const char *id, guint64 *bytes)
{
    char **roots = walpe_steam_library_roots ();
    gboolean seen = FALSE;
    int budget = 20000;
    *bytes = 0;
    for (int i = 0; roots[i]; i++) {
        const char *subs[] = { "downloads", "temp", "content", NULL };
        for (int k = 0; subs[k]; k++) {
            char *d = g_build_filename (roots[i], "steamapps", "workshop", subs[k],
                                        WALPE_WE_APPID, id, NULL);
            if (g_file_test (d, G_FILE_TEST_IS_DIR)) {
                seen = TRUE;
                guint64 sz = tree_size (d, &budget);   /* once: MAX() evaluates twice */
                *bytes = MAX (*bytes, sz);
            }
            g_free (d);
        }
        char *acf = g_build_filename (roots[i], "steamapps", "workshop",
                                      "appworkshop_" WALPE_WE_APPID ".acf", NULL);
        char *data = NULL;
        gsize len = 0;
        if (!seen && g_file_get_contents (acf, &data, &len, NULL) && len < 16 * 1024 * 1024) {
            char *needle = g_strdup_printf ("\"%s\"", id);
            seen = strstr (data, needle) != NULL;
            g_free (needle);
        }
        g_free (data);
        g_free (acf);
    }
    g_strfreev (roots);
    return seen;
}

static guint wait_start_secs (void)
{
    const char *e = g_getenv ("WALPE_STEAM_WAIT_START");   /* tests */
    guint v = e ? (guint) g_ascii_strtoull (e, NULL, 10) : 0;
    return v ? v : WAIT_START_S;
}

static gboolean wait_poll (gpointer user)
{
    Page *p = user;
    gboolean shown = waiting_this (p);
    if (steam_has_item (p, p->wait_id)) {
        char *id = g_strdup (p->wait_id);
        char *m = g_strdup_printf ("You are subscribed to “%s” on Steam, and the Steam client "
                                   "downloaded it: nothing was downloaded twice.",
                                   p->wait_title);
        p->wait_src = 0;
        wait_clear (p);
        item_note (p, id, m, FALSE);
        g_free (m);
        g_free (id);
        refresh_local (p);
        if (p->hooks.library_changed)
            p->hooks.library_changed ();
        update_action (p);
        return G_SOURCE_REMOVE;
    }
    double el = (double) (g_get_monotonic_time () - p->wait_since) / 1e6;
    guint64 got = 0;
    if (steam_fetching (p->wait_id, &got) && !p->wait_seen) {
        p->wait_seen = TRUE;
        update_action (p);
    }
    if (!p->wait_seen && el > wait_start_secs ()) {
        /* the client did not react (no Wallpaper Engine there after all, or
         * downloads paused): fetch it ourselves; the account stays subscribed */
        char *id = g_strdup (p->wait_id);
        guint64 size = p->wait_size;
        char *m = g_strdup_printf ("You are subscribed to “%s” on Steam. The Steam client did "
                                   "not start downloading it, so WalPE uses steamcmd for the "
                                   "download.", p->wait_title);
        p->wait_src = 0;
        wait_clear (p);
        /* the note is shown once the download has started; when it could
         * not, start_download_full says why */
        start_download_full (p, id, size, m, FALSE, NULL);
        g_free (m);
        g_free (id);
        return G_SOURCE_REMOVE;
    }
    if (el > WAIT_TOTAL_S) {
        char *id = g_strdup (p->wait_id);
        char *m = g_strdup_printf ("You are subscribed to “%s”, but the Steam client has not "
                                   "finished downloading it. It is listed here once Steam is "
                                   "done.", p->wait_title);
        p->wait_src = 0;
        wait_clear (p);
        item_error (p, id, m);
        g_free (m);
        g_free (id);
        update_action (p);
        return G_SOURCE_REMOVE;
    }
    char *text;
    if (got && p->wait_size)
        text = g_strdup_printf ("Steam is downloading: %.1f of %.1f MB", (double) got / 1048576.0,
                                (double) p->wait_size / 1048576.0);
    else
        text = g_strdup (p->wait_seen ? "Steam is downloading…"
                                      : "subscribed; waiting for the Steam client…");
    if (shown) {
        if (got && p->wait_size)
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->d_prog),
                                           MIN (0.99, (double) got / (double) p->wait_size));
        else
            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (p->d_prog));
        gtk_progress_bar_set_text (GTK_PROGRESS_BAR (p->d_prog), text);
    }
    char *m = g_strdup_printf ("Workshop: %s", text);
    status (p, m);
    g_free (m);
    g_free (text);
    return G_SOURCE_CONTINUE;
}

static void delivery_subscribed (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    SubReq *r = user;
    gboolean ok;
    char *why = NULL;
    if (!sub_answer (res, r, &ok, &why))
        return;
    Page *p = r->p;
    if (r->gen != p->wait_gen) {
        /* cancelled meanwhile: cancel_wait already asked to unsubscribe */
    } else if (ok) {
        walpe_steam_sub_mark (r->id, TRUE);
        p->wait_since = g_get_monotonic_time ();
        p->wait_seen = FALSE;
        p->wait_src = g_timeout_add (1000, wait_poll, p);
        char *m = g_strdup_printf ("Subscribed to “%s” on Steam; waiting for the Steam client "
                                   "to download it…", r->title);
        status (p, m);
        g_free (m);
        if (waiting_this (p))
            gtk_progress_bar_set_text (GTK_PROGRESS_BAR (p->d_prog),
                                       "subscribed; waiting for the Steam client…");
    } else {
        /* no subscription, but the wallpaper was asked for: steamcmd */
        wait_clear (p);
        char *started = g_strdup_printf ("WalPE downloads “%s” with steamcmd; you are not "
                                         "subscribed to it on Steam: %s.", r->title, why);
        char *failed = g_strdup_printf ("You are not subscribed to “%s” on Steam (%s), and the "
                                        "steamcmd download could not start: check the Steam "
                                        "account in the Workshop settings and that walpe-daemon "
                                        "runs.", r->title, why);
        start_download_full (p, r->id, r->size, started, TRUE, failed);
        g_free (started);
        g_free (failed);
    }
    g_free (why);
    update_action (p);
    sub_req_free (r);
}

/* Steam is running and has Wallpaper Engine: subscribing is the download */
static void start_steam_delivery (Page *p, WsEntry *e)
{
    wait_clear (p);
    p->wait_id = g_strdup (e->item->id);
    p->wait_title = walpe_ui_text (e->item->title ? e->item->title : e->item->id, 80,
                                   WALPE_UITEXT_LINE);
    p->wait_size = e->item->file_size;
    item_note (p, p->wait_id, NULL, FALSE);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->d_prog), 0);
    gtk_progress_bar_set_text (GTK_PROGRESS_BAR (p->d_prog), "subscribing on Steam…");
    walpe_ws_subscribe_async (p->client, p->wait_id, TRUE, p->sub_cancel, delivery_subscribed,
                              sub_req (p, p->wait_id, p->wait_title, p->wait_size));
    update_action (p);
}

static void cancel_unsubscribed (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    SubReq *r = user;
    gboolean ok;
    char *why = NULL;
    if (!sub_answer (res, r, &ok, &why))
        return;
    Page *p = r->p;
    char *m;
    if (ok) {
        walpe_steam_sub_mark (r->id, FALSE);
        m = g_strdup_printf ("Cancelled: unsubscribed from “%s” again.", r->title);
        item_note (p, r->id, m, FALSE);
    } else {
        m = g_strdup_printf ("Cancelled, but Steam did not accept the unsubscribe (%s): you "
                             "are still subscribed to “%s”. “Open on Steam” shows its page.",
                             why, r->title);
        item_error (p, r->id, m);
    }
    g_free (m);
    g_free (why);
    sub_req_free (r);
}

/* Cancel while the Steam client is to deliver: take the subscription back
 * (a subscription that stays would make Steam download the item anyway) */
static void cancel_wait (Page *p)
{
    SubReq *r = sub_req (p, p->wait_id, p->wait_title, 0);
    wait_clear (p);
    status (p, "Workshop: cancelled; unsubscribing on Steam again…");
    walpe_ws_subscribe_async (p->client, r->id, FALSE, p->sub_cancel, cancel_unsubscribed, r);
    update_action (p);
}

/* The progress poll: a "status" request twice a second while a download
 * runs, one at a time, answered on uiipc's poll lane (a daemon that does
 * not answer costs a worker thread 2.5 s, not the window) */
static void poll_install_reply (Page *p, JsonObject *o, gpointer data)
{
    (void) data;
    p->poll_inflight = FALSE;
    if (!p->poll_id)
        return;     /* stopped meanwhile */
    JsonObject *st = o ? wpj_obj (o, "state") : NULL;
    if (!st)
        return;     /* daemon busy/restarting: keep asking */
    gboolean active = wpj_bool (st, "install_active", FALSE);
    const char *id = wpj_str (st, "install_id", "");
    const char *phase = wpj_str (st, "install_phase", "");
    double prog = wpj_num (st, "install_progress", -1);
    gboolean mine = !g_strcmp0 (id, p->inst_id);
    gboolean shown = p->cur && !g_strcmp0 (p->cur->item->id, p->inst_id);
    if (active && mine) {
        if (shown) {
            if (prog >= 0)
                gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->d_prog), MIN (prog, 1.0));
            else
                gtk_progress_bar_pulse (GTK_PROGRESS_BAR (p->d_prog));
            gtk_progress_bar_set_text (GTK_PROGRESS_BAR (p->d_prog), phase);
        }
        char *m = g_strdup_printf ("Workshop: %s", phase);
        status (p, m);
        g_free (m);
        return;
    }
    g_source_remove (p->poll_id);
    p->poll_id = 0;
    const char *err = mine ? wpj_str (st, "install_error", NULL) : "another install replaced it";
    if (err) {
        char *m = g_strdup_printf ("Download failed: %s", err);
        if (shown)
            detail_error (p, m);
        status (p, m);
        g_free (m);
    } else {
        /* the installed project.json's title, as the daemon read it */
        char *title = walpe_ui_text (wpj_str (st, "install_title", id), 80, WALPE_UITEXT_LINE);
        char *m = g_strdup_printf ("Installed: %s", title);
        status (p, m);
        g_free (m);
        if (p->hooks.library_changed)
            p->hooks.library_changed ();
        refresh_local (p);
        /* steamcmd subscribed nothing: ask Steam, if the user wants that */
        if (subscribe_on (p) && !walpe_steam_sub_is_marked (id))
            walpe_ws_subscribe_async (p->client, id, TRUE, p->sub_cancel, installed_subscribed,
                                      sub_req (p, id, title, 0));
        g_free (title);
    }
    update_action (p);
}

static gboolean poll_install (gpointer user)
{
    Page *p = user;
    if (!p->poll_inflight) {
        p->poll_inflight = TRUE;
        ipc_async (p, "{\"cmd\":\"status\"}", UI_IPC_POLL, poll_install_reply, NULL, NULL);
    }
    return G_SOURCE_CONTINUE;
}

/* Starting a download: "status" (is an account name set?), then "install".
 * ok_note: shown with the item once the download runs (as an error when
 * ok_note_error); fail_note: shown instead of the reason when it could not
 * start. Both may be NULL. */
typedef struct {
    char *id;
    guint64 size;
    char *ok_note, *fail_note;
    gboolean ok_note_error;
} DlReq;

static void dl_req_free (gpointer data)
{
    DlReq *r = data;
    g_free (r->id);
    g_free (r->ok_note);
    g_free (r->fail_note);
    g_free (r);
}

static void download_not_started (Page *p, DlReq *r, const char *why)
{
    if (!g_strcmp0 (p->starting_id, r->id))
        g_clear_pointer (&p->starting_id, g_free);
    item_error (p, r->id, r->fail_note ? r->fail_note : why);
    update_action (p);
}

static void download_install_reply (Page *p, JsonObject *ro, gpointer data)
{
    DlReq *r = data;
    if (!ro || !wpj_bool (ro, "ok", FALSE)) {
        download_not_started (p, r, ro ? wpj_str (ro, "error", "install failed")
                                       : "walpe-daemon is not reachable");
        return;
    }
    if (!g_strcmp0 (p->starting_id, r->id))
        g_clear_pointer (&p->starting_id, g_free);
    g_free (p->inst_id);
    p->inst_id = g_strdup (r->id);
    item_note (p, r->id, r->ok_note, r->ok_note_error);
    if (p->cur && !g_strcmp0 (p->cur->item->id, r->id)) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->d_prog), 0);
        gtk_progress_bar_set_text (GTK_PROGRESS_BAR (p->d_prog), "starting steamcmd…");
    }
    if (!p->poll_id)
        p->poll_id = g_timeout_add (500, poll_install, p);
    update_action (p);
}

static void download_status_reply (Page *p, JsonObject *o, gpointer data)
{
    DlReq *r = data;
    const char *user = o ? wpj_str (wpj_obj (o, "state"), "steam_user", "") : NULL;
    if (!o) {
        download_not_started (p, r, "walpe-daemon is not reachable: downloads run in the daemon.");
    } else if (!user || !*user) {
        download_not_started (p, r, "Set your Steam account name in the Workshop settings (⚙) "
                                    "first. Then log in to steamcmd once in a terminal: "
                                    "steamcmd +login <name>");
    } else {
        char *req = g_strdup_printf ("{\"cmd\":\"install\",\"id\":\"%s\",\"size\":%" G_GUINT64_FORMAT "}",
                                     r->id, r->size);   /* id: digits only */
        DlReq *n = g_new0 (DlReq, 1);
        n->id = g_strdup (r->id);
        n->size = r->size;
        n->ok_note = g_strdup (r->ok_note);
        n->fail_note = g_strdup (r->fail_note);
        n->ok_note_error = r->ok_note_error;
        ipc_async (p, req, UI_IPC_SLOW | UI_IPC_AUTOSTART, download_install_reply, n, dl_req_free);
        g_free (req);
    }
}

static void start_download_full (Page *p, const char *id, guint64 size, const char *ok_note,
                                 gboolean ok_note_error, const char *fail_note)
{
    if (p->starting_id)
        return;     /* one at a time: the answer to the first click is on its way */
    DlReq *r = g_new0 (DlReq, 1);
    r->id = g_strdup (id);   /* id may be p->inst_id itself */
    r->size = size;
    r->ok_note = g_strdup (ok_note);
    r->fail_note = g_strdup (fail_note);
    r->ok_note_error = ok_note_error;
    p->starting_id = g_strdup (id);
    if (p->cur && !g_strcmp0 (p->cur->item->id, id)) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->d_prog), 0);
        gtk_progress_bar_set_text (GTK_PROGRESS_BAR (p->d_prog), "asking the engine…");
    }
    update_action (p);
    ipc_async (p, "{\"cmd\":\"status\"}", UI_IPC_AUTOSTART, download_status_reply, r, dl_req_free);
}

static void start_download (Page *p, WsEntry *e)
{
    start_download_full (p, e->item->id, e->item->file_size, NULL, FALSE, NULL);
}

static void apply_reply (Page *p, JsonObject *o, gpointer data)
{
    (void) data;
    gboolean ok = o && wpj_bool (o, "ok", FALSE);
    /* a video the player cannot show as it is gets converted first (once) */
    gboolean conv = ok && wpj_bool (o, "converting", FALSE);
    detail_error (p, ok ? NULL : o ? wpj_str (o, "error", "apply failed")
                                   : "walpe-daemon did not answer (not running, or still "
                                     "loading the wallpaper)");
    status (p, conv ? "Converting the video to H.264 (once); the wallpaper changes when it is ready."
                    : ok ? "Applied." : "Apply failed.");
    gtk_widget_set_sensitive (p->d_action, !installing_this (p));
}

/* "open" on the lane the Library uses for it (8 s; it was 3 s here, on the
 * main thread) */
static void apply_wallpaper (Page *p, const char *path)
{
    char *esc = walpe_json_escape (path);
    char *req = g_strdup_printf ("{\"cmd\":\"open\",\"path\":\"%s\"}", esc);
    status (p, "Setting the wallpaper…");
    gtk_widget_set_sensitive (p->d_action, FALSE);
    ipc_async (p, req, UI_IPC_SLOW | UI_IPC_AUTOSTART, apply_reply, NULL, NULL);
    g_free (req);
    g_free (esc);
}

static void on_action (GtkButton *b, Page *p)
{
    (void) b;
    WsEntry *e = p->cur;
    if (!e)
        return;
    if ((e->local == LOCAL_WALPE || e->local == LOCAL_STEAM) && e->local_path)
        apply_wallpaper (p, e->local_path);
    else if (e->local == LOCAL_NONE && steam_can_deliver (p))
        start_steam_delivery (p, e);   /* subscribe; the Steam client downloads */
    else
        start_download (p, e);
}

static void on_cancel_install (GtkButton *b, Page *p)
{
    (void) b;
    if (waiting_this (p)) {
        cancel_wait (p);
        return;
    }
    ipc_async (p, "{\"cmd\":\"install_cancel\"}", UI_IPC_AUTOSTART, NULL, NULL, NULL);
}

static void on_open_steam (GtkButton *b, Page *p)
{
    (void) b;
    if (!p->cur)
        return;
    /* the page in the running Steam client (steam://url/CommunityFilePage/),
     * where Subscribe / Unsubscribe work with the logged-in account; the
     * web page in the browser when Steam is not running */
    WalpeSteamPageWhere where;
    GError *err = NULL;
    if (walpe_steam_open_item_page (p->cur->item->id, &where, &err)) {
        status (p, where == WALPE_STEAM_PAGE_CLIENT
                       ? "Opened the Workshop page in the Steam client."
                       : "Steam is not running: opened the Workshop page in your web browser.");
        return;
    }
    g_clear_error (&err);
    char *url = g_strdup_printf ("https://steamcommunity.com/sharedfiles/filedetails/?id=%s",
                                 p->cur->item->id);
    GtkUriLauncher *l = gtk_uri_launcher_new (url);
    gtk_uri_launcher_launch (l, GTK_WINDOW (gtk_widget_get_root (p->root)), NULL, NULL, NULL);
    g_object_unref (l);
    g_free (url);
}

static GtkWidget *build_details (Page *p)
{
    GtkWidget *sw = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request (sw, 360, -1);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    p->d_box = box;
    p->d_pic = GTK_PICTURE (gtk_picture_new ());
    gtk_picture_set_content_fit (p->d_pic, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request (GTK_WIDGET (p->d_pic), 320, 320);
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (p->d_pic));
#define LBL(field, cls) do { p->field = GTK_LABEL (gtk_label_new (NULL)); \
        gtk_label_set_xalign (p->field, 0); gtk_label_set_wrap (p->field, TRUE); \
        gtk_label_set_wrap_mode (p->field, PANGO_WRAP_WORD_CHAR); \
        if (cls) gtk_widget_add_css_class (GTK_WIDGET (p->field), cls); \
        gtk_box_append (GTK_BOX (box), GTK_WIDGET (p->field)); } while (0)
    LBL (d_title, "title-3");
    gtk_label_set_selectable (p->d_title, TRUE);
    LBL (d_author, "dim-label");
    LBL (d_stats, NULL);
    p->d_tags = gtk_flow_box_new ();
    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (p->d_tags), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (p->d_tags), 12);
    gtk_box_append (GTK_BOX (box), p->d_tags);
    LBL (d_state, "ws-state");
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    p->d_action = gtk_button_new_with_label ("Download");
    gtk_widget_add_css_class (p->d_action, "suggested-action");
    gtk_widget_set_hexpand (p->d_action, TRUE);
    g_signal_connect (p->d_action, "clicked", G_CALLBACK (on_action), p);
    p->d_steam = gtk_button_new_with_label ("Open on Steam");
    g_signal_connect (p->d_steam, "clicked", G_CALLBACK (on_open_steam), p);
    gtk_box_append (GTK_BOX (row), p->d_action);
    gtk_box_append (GTK_BOX (row), p->d_steam);
    gtk_box_append (GTK_BOX (box), row);
    GtkWidget *prow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    p->d_prog = gtk_progress_bar_new ();
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (p->d_prog), TRUE);
    gtk_progress_bar_set_ellipsize (GTK_PROGRESS_BAR (p->d_prog), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (p->d_prog, TRUE);
    gtk_widget_set_valign (p->d_prog, GTK_ALIGN_CENTER);
    p->d_cancel = gtk_button_new_with_label ("Cancel");
    g_signal_connect (p->d_cancel, "clicked", G_CALLBACK (on_cancel_install), p);
    gtk_box_append (GTK_BOX (prow), p->d_prog);
    gtk_box_append (GTK_BOX (prow), p->d_cancel);
    gtk_box_append (GTK_BOX (box), prow);
    LBL (d_err, "error");
    gtk_label_set_selectable (p->d_err, TRUE);   /* copy the steamcmd command */
    LBL (d_desc, NULL);
    gtk_label_set_selectable (p->d_desc, TRUE);
#undef LBL
    gtk_widget_set_visible (p->d_prog, FALSE);
    gtk_widget_set_visible (p->d_cancel, FALSE);
    gtk_widget_set_visible (box, FALSE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), box);
    return sw;
}

/* ---------------------------------------------------------- settings */

/* the daemon keeps the account name; its answer comes a moment after the
 * rest was saved */
static void settings_user_saved (Page *p, JsonObject *o, gpointer data)
{
    (void) data;
    if (o && wpj_bool (o, "ok", FALSE))
        return;
    const char *cur = gtk_label_get_text (p->set_msg);
    char *m = g_strconcat ("Could not save the account name (daemon not reachable). ",
                           cur && strcmp (cur, "Saved.") ? cur : "", NULL);
    gtk_label_set_text (p->set_msg, m);
    g_free (m);
}

static void on_settings_save (GtkButton *b, Page *p)
{
    (void) b;
    GString *msg = g_string_new (NULL);
    const char *user = gtk_editable_get_text (GTK_EDITABLE (p->set_user));
    if (*user && !walpe_workshop_valid_user (user)) {
        g_string_append (msg, "Steam account names are letters, digits and _. ");
    } else if (*user || p->user_known) {
        /* an empty field is only sent when the daemon's own value was seen
         * (Save before its answer arrived must not wipe the name) */
        char *req = g_strdup_printf ("{\"cmd\":\"set\",\"steam_user\":\"%s\"}", user);  /* validated */
        ipc_async (p, req, UI_IPC_AUTOSTART, settings_user_saved, NULL, NULL);
        g_free (req);
    }
    const char *key = gtk_editable_get_text (GTK_EDITABLE (p->set_key));
    if (*key) {
        if (walpe_ws_valid_api_key (key)) {
            g_free (p->settings->api_key);
            p->settings->api_key = g_strdup (key);
        } else {
            g_string_append (msg, "That is not a Steam Web API key (32 hex digits); not saved. ");
        }
    }
    p->settings->hide_mature = gtk_switch_get_active (p->set_mature);
    p->settings->hide_questionable = gtk_switch_get_active (p->set_question);
    gboolean sub = gtk_switch_get_active (p->set_subscribe);
    if (sub && !p->settings->api_key) {
        sub = FALSE;
        gtk_switch_set_active (p->set_subscribe, FALSE);
        g_string_append (msg, "Subscribing on Steam needs the Steam Web API key: left off. ");
    }
    p->settings->subscribe_on_download = sub;
    p->sub_denied = FALSE;   /* a new key, or a second try: ask Steam again */
    /* the delete dialog keeps its own setting in the same file */
    WalpeWsSettings *disk = walpe_ws_settings_load ();
    p->settings->unsubscribe_on_delete = disk->unsubscribe_on_delete;
    walpe_ws_settings_free (disk);
    GError *err = NULL;
    if (!walpe_ws_settings_save (p->settings, &err)) {
        g_string_append_printf (msg, "%s ", err->message);
        g_error_free (err);
    }
    walpe_ws_client_set_api_key (p->client, p->settings->api_key);
    gtk_editable_set_text (GTK_EDITABLE (p->set_key), "");
    g_object_set (p->set_key, "placeholder-text",
                  p->settings->api_key ? "key saved (hidden) - type to replace" : "optional", NULL);
    gtk_label_set_text (p->set_msg, msg->len ? msg->str : "Saved.");
    g_string_free (msg, TRUE);
    sync_age_chips (p);
    update_action (p);
    run_search (p, 1);
}

static void on_key_remove (GtkButton *b, Page *p)
{
    (void) b;
    g_clear_pointer (&p->settings->api_key, g_free);
    p->settings->subscribe_on_download = FALSE;   /* it needs the key */
    gtk_switch_set_active (p->set_subscribe, FALSE);
    WalpeWsSettings *disk = walpe_ws_settings_load ();
    p->settings->unsubscribe_on_delete = disk->unsubscribe_on_delete;
    walpe_ws_settings_free (disk);
    walpe_ws_settings_save (p->settings, NULL);
    walpe_ws_client_set_api_key (p->client, NULL);
    g_object_set (p->set_key, "placeholder-text", "optional", NULL);
    gtk_label_set_text (p->set_msg, "API key removed: searching the public workshop page.");
    run_search (p, 1);
}

static void settings_status_reply (Page *p, JsonObject *o, gpointer data)
{
    (void) data;
    if (!o)
        return;
    p->user_known = TRUE;
    /* not over what was typed while the answer was on its way */
    if (!g_strcmp0 (gtk_editable_get_text (GTK_EDITABLE (p->set_user)), p->user_shown))
        gtk_editable_set_text (GTK_EDITABLE (p->set_user),
                               wpj_str (wpj_obj (o, "state"), "steam_user", ""));
}

static void on_settings_shown (GtkPopover *pop, Page *p)
{
    (void) pop;
    p->user_known = FALSE;
    g_free (p->user_shown);
    p->user_shown = g_strdup (gtk_editable_get_text (GTK_EDITABLE (p->set_user)));
    ipc_async (p, "{\"cmd\":\"status\"}", 0, settings_status_reply, NULL, NULL);
    gtk_label_set_text (p->set_msg, "");
}

static GtkWidget *note (const char *text)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 46);
    gtk_widget_add_css_class (l, "dim-label");
    gtk_widget_add_css_class (l, "caption");
    return l;
}

static GtkWidget *switch_row (const char *label, GtkSwitch **out, gboolean on)
{
    GtkWidget *r = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new (label);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_widget_set_hexpand (l, TRUE);
    *out = GTK_SWITCH (gtk_switch_new ());
    gtk_switch_set_active (*out, on);
    gtk_box_append (GTK_BOX (r), l);
    gtk_box_append (GTK_BOX (r), GTK_WIDGET (*out));
    return r;
}

static GtkWidget *build_settings (Page *p)
{
    GtkWidget *mb = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (mb), "emblem-system-symbolic");
    gtk_widget_set_tooltip_text (mb, "Workshop settings");
    GtkWidget *pop = gtk_popover_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (box, 8);
    gtk_widget_set_margin_bottom (box, 8);
    gtk_widget_set_margin_start (box, 8);
    gtk_widget_set_margin_end (box, 8);
    GtkWidget *h = gtk_label_new ("Steam account for downloads");
    gtk_label_set_xalign (GTK_LABEL (h), 0);
    gtk_widget_add_css_class (h, "heading");
    gtk_box_append (GTK_BOX (box), h);
    p->set_user = GTK_ENTRY (gtk_entry_new ());
    gtk_entry_set_placeholder_text (p->set_user, "Steam account name");
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (p->set_user));
    gtk_box_append (GTK_BOX (box), note (
        "Downloads run steamcmd with this account's cached login. Log in once in a "
        "terminal: steamcmd +login <name> (password and Steam Guard are typed there). "
        "WalPE never asks for, stores or sees your password."));
    h = gtk_label_new ("Steam Web API key (optional)");
    gtk_label_set_xalign (GTK_LABEL (h), 0);
    gtk_widget_add_css_class (h, "heading");
    gtk_widget_set_margin_top (h, 6);
    gtk_box_append (GTK_BOX (box), h);
    GtkWidget *kr = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    p->set_key = GTK_PASSWORD_ENTRY (gtk_password_entry_new ());
    gtk_password_entry_set_show_peek_icon (p->set_key, TRUE);
    g_object_set (p->set_key, "placeholder-text",
                  p->settings->api_key ? "key saved (hidden) - type to replace" : "optional", NULL);
    gtk_widget_set_hexpand (GTK_WIDGET (p->set_key), TRUE);
    GtkWidget *rm = gtk_button_new_with_label ("Remove");
    g_signal_connect (rm, "clicked", G_CALLBACK (on_key_remove), p);
    gtk_box_append (GTK_BOX (kr), GTK_WIDGET (p->set_key));
    gtk_box_append (GTK_BOX (kr), rm);
    gtk_box_append (GTK_BOX (box), kr);
    gtk_box_append (GTK_BOX (box), note (
        "Not needed for browsing. With a key, search uses IPublishedFileService/QueryFiles; "
        "without one, the public workshop page. Stored in ~/.config/walpe/workshop.json "
        "(mode 0600), never logged."));
    GtkWidget *ss = switch_row ("Subscribe on Steam when downloading", &p->set_subscribe,
                                p->settings->subscribe_on_download);
    gtk_widget_set_margin_top (ss, 6);
    gtk_box_append (GTK_BOX (box), ss);
    gtk_box_append (GTK_BOX (box), note (
        "Off by default; needs the key above. steamcmd downloads without subscribing, so "
        "WalPE asks Steam itself to subscribe your account (IPublishedFileService/Subscribe, a "
        "call Valve does not document): the item then shows under your Steam subscriptions, "
        "and deleting it here can unsubscribe again. With Steam running and Wallpaper Engine "
        "installed there, WalPE only subscribes and Steam downloads the item (one copy)."));
    GtkWidget *sr = switch_row ("Hide Mature items", &p->set_mature, p->settings->hide_mature);
    gtk_widget_set_margin_top (sr, 6);
    gtk_box_append (GTK_BOX (box), sr);
    gtk_box_append (GTK_BOX (box), switch_row ("Hide Questionable items", &p->set_question,
                                               p->settings->hide_questionable));
    GtkWidget *save = gtk_button_new_with_label ("Save");
    gtk_widget_add_css_class (save, "suggested-action");
    g_signal_connect (save, "clicked", G_CALLBACK (on_settings_save), p);
    gtk_box_append (GTK_BOX (box), save);
    p->set_msg = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_wrap (p->set_msg, TRUE);
    gtk_label_set_xalign (p->set_msg, 0);
    gtk_label_set_max_width_chars (p->set_msg, 46);
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (p->set_msg));
    gtk_popover_set_child (GTK_POPOVER (pop), box);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (mb), pop);
    g_signal_connect (pop, "show", G_CALLBACK (on_settings_shown), p);
    return mb;
}

/* -------------------------------------------------------------- page */

static void page_free (gpointer data)
{
    Page *p = data;
    g_cancellable_cancel (p->search_cancel);
    g_cancellable_cancel (p->thumbs_cancel);
    g_cancellable_cancel (p->detail_cancel);
    g_object_unref (p->search_cancel);
    g_object_unref (p->thumbs_cancel);
    g_object_unref (p->detail_cancel);
    g_cancellable_cancel (p->sub_cancel);   /* Steam keeps what it was asked */
    g_object_unref (p->sub_cancel);
    if (p->wait_src)
        g_source_remove (p->wait_src);
    g_free (p->wait_id);
    g_free (p->wait_title);
    g_hash_table_unref (p->notes);
    if (p->poll_id)
        g_source_remove (p->poll_id);   /* the daemon keeps installing */
    if (p->chip_debounce)
        g_source_remove (p->chip_debounce);
    if (p->cur && p->cur_thumb_h)
        g_signal_handler_disconnect (p->cur, p->cur_thumb_h);
    g_clear_object (&p->cur);
    g_object_unref (p->model);
    g_hash_table_unref (p->chip_state);
    g_ptr_array_unref (p->chips);
    walpe_ws_client_free (p->client);
    walpe_ws_settings_free (p->settings);
    g_strfreev (p->steam_libs);
    g_free (p->library_dir);
    g_free (p->inst_id);
    g_free (p->starting_id);
    g_free (p->user_shown);
    g_free (p);
}

static void load_css_once (void)
{
    static gboolean done;
    if (done || !gdk_display_get_default ())
        return;
    done = TRUE;
    const char *css =
        ".ws-chip { padding: 1px 9px; min-height: 22px; border-radius: 12px; font-size: 90%; }"
        ".ws-chip.req { background: #3584e4; color: white; }"
        ".ws-chip.excl { background: alpha(#e01b24, 0.22); text-decoration-line: line-through; }"
        ".ws-badge { background: rgba(0,0,0,0.68); color: white; border-radius: 6px;"
        "  padding: 1px 6px; margin: 5px; font-size: 80%; font-weight: bold; }"
        ".ws-tile picture { border-radius: 8px; }"
        ".ws-tag { padding: 1px 8px; border-radius: 10px; background: alpha(currentColor,0.10);"
        "  font-size: 85%; }"
        ".ws-tag.warn { background: alpha(#e5a50a, 0.35); }"
        ".ws-state { font-weight: bold; }";
    GtkCssProvider *pr = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (pr, css);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (pr),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (pr);
}

GtkWidget *walpe_workshop_page_new_with_hooks (const WalpeWorkshopHooks *hooks)
{
    load_css_once ();
    Page *p = g_new0 (Page, 1);
    if (hooks)
        p->hooks = *hooks;
    p->library_dir = hooks && hooks->library_dir ? walpe_expand_path (hooks->library_dir) : NULL;
    p->hooks.library_dir = p->library_dir;
    p->steam_libs = walpe_workshop_steam_libraries ();
    p->settings = walpe_ws_settings_load ();
    p->client = walpe_ws_available () ? walpe_ws_client_new () : NULL;
    walpe_ws_client_set_api_key (p->client, p->settings->api_key);
    p->search_cancel = g_cancellable_new ();
    p->thumbs_cancel = g_cancellable_new ();
    p->detail_cancel = g_cancellable_new ();
    p->sub_cancel = g_cancellable_new ();
    p->notes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, note_free);
    p->chip_state = g_hash_table_new (g_str_hash, g_str_equal);
    p->chips = g_ptr_array_new ();
    p->page = 1;

    GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    p->root = root;
    g_object_set_data_full (G_OBJECT (root), "walpe-workshop-page", p, page_free);

    /* toolbar */
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top (bar, 8);
    gtk_widget_set_margin_bottom (bar, 6);
    gtk_widget_set_margin_start (bar, 10);
    gtk_widget_set_margin_end (bar, 10);
    p->search = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
    gtk_search_entry_set_placeholder_text (p->search, "Search the workshop, or paste an id / link");
    gtk_search_entry_set_search_delay (p->search, 600);
    gtk_widget_set_hexpand (GTK_WIDGET (p->search), TRUE);
    g_signal_connect (p->search, "search-changed", G_CALLBACK (on_search_changed), p);
    const char *sorts[WALPE_WS_SORT_N + 1];
    for (int i = 0; i < WALPE_WS_SORT_N; i++)
        sorts[i] = walpe_ws_sort_label ((WalpeWsSort) i);
    sorts[WALPE_WS_SORT_N] = NULL;
    p->sort = GTK_DROP_DOWN (gtk_drop_down_new_from_strings (sorts));
    gtk_widget_set_tooltip_text (GTK_WIDGET (p->sort), "Sort");
    g_signal_connect (p->sort, "notify::selected", G_CALLBACK (on_sort_changed), p);
    /* resolution: Steam's own <select> */
    GPtrArray *res = g_ptr_array_new ();
    g_ptr_array_add (res, (gpointer) "Any resolution");
    guint nc;
    const WalpeWsTagCategory *cats = walpe_ws_tag_categories (&nc);
    for (guint c = 0; c < nc; c++)
        if (!strcmp (cats[c].name, "Resolution"))
            for (const char *const *t = cats[c].tags; *t; t++)
                g_ptr_array_add (res, (gpointer) *t);
    g_ptr_array_add (res, NULL);
    p->resolution = GTK_DROP_DOWN (gtk_drop_down_new_from_strings ((const char *const *) res->pdata));
    g_ptr_array_unref (res);
    gtk_widget_set_tooltip_text (GTK_WIDGET (p->resolution), "Resolution");
    g_signal_connect (p->resolution, "notify::selected", G_CALLBACK (on_sort_changed), p);
    p->filters_btn = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Tags"));
    gtk_widget_set_tooltip_text (GTK_WIDGET (p->filters_btn), "Filter by Wallpaper Engine tags");
    gtk_box_append (GTK_BOX (bar), GTK_WIDGET (p->search));
    gtk_box_append (GTK_BOX (bar), GTK_WIDGET (p->sort));
    gtk_box_append (GTK_BOX (bar), GTK_WIDGET (p->resolution));
    gtk_box_append (GTK_BOX (bar), GTK_WIDGET (p->filters_btn));
    gtk_box_append (GTK_BOX (bar), build_settings (p));
    gtk_box_append (GTK_BOX (root), bar);

    p->filters = GTK_REVEALER (gtk_revealer_new ());
    gtk_revealer_set_child (p->filters, build_filters (p));
    g_object_bind_property (p->filters_btn, "active", p->filters, "reveal-child", G_BINDING_DEFAULT);
    gtk_box_append (GTK_BOX (root), GTK_WIDGET (p->filters));
    sync_age_chips (p);

    /* grid + details */
    GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand (paned, TRUE);
    GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    p->model = g_list_store_new (WS_TYPE_ENTRY);
    p->sel = gtk_single_selection_new (G_LIST_MODEL (g_object_ref (p->model)));
    g_signal_connect (p->sel, "notify::selected-item", G_CALLBACK (on_selected), p);
    GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
    g_signal_connect (f, "setup", G_CALLBACK (tile_setup), p);
    g_signal_connect (f, "bind", G_CALLBACK (tile_bind), p);
    g_signal_connect (f, "unbind", G_CALLBACK (tile_unbind), p);
    GtkWidget *grid = gtk_grid_view_new (GTK_SELECTION_MODEL (p->sel), f);
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (grid), 12);
    gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (grid), FALSE);
    p->grid_sw = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (p->grid_sw), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (p->grid_sw), grid);
    gtk_widget_set_vexpand (p->grid_sw, TRUE);
    gtk_box_append (GTK_BOX (left), p->grid_sw);

    GtkWidget *pager = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top (pager, 4);
    gtk_widget_set_margin_bottom (pager, 6);
    gtk_widget_set_margin_start (pager, 10);
    gtk_widget_set_margin_end (pager, 10);
    p->prev = gtk_button_new_from_icon_name ("go-previous-symbolic");
    gtk_widget_set_tooltip_text (p->prev, "Previous page");
    g_signal_connect (p->prev, "clicked", G_CALLBACK (on_prev), p);
    p->next = gtk_button_new_from_icon_name ("go-next-symbolic");
    gtk_widget_set_tooltip_text (p->next, "Next page");
    g_signal_connect (p->next, "clicked", G_CALLBACK (on_next), p);
    p->page_lbl = GTK_LABEL (gtk_label_new (""));
    p->spinner = gtk_spinner_new ();
    p->msg = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_ellipsize (p->msg, PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign (p->msg, 0);
    gtk_widget_set_hexpand (GTK_WIDGET (p->msg), TRUE);
    p->retry = gtk_button_new_with_label ("Retry");
    g_signal_connect (p->retry, "clicked", G_CALLBACK (on_retry), p);
    gtk_box_append (GTK_BOX (pager), p->prev);
    gtk_box_append (GTK_BOX (pager), GTK_WIDGET (p->page_lbl));
    gtk_box_append (GTK_BOX (pager), p->next);
    gtk_box_append (GTK_BOX (pager), p->spinner);
    gtk_box_append (GTK_BOX (pager), GTK_WIDGET (p->msg));
    gtk_box_append (GTK_BOX (pager), p->retry);
    gtk_box_append (GTK_BOX (left), pager);
    gtk_widget_set_visible (p->retry, FALSE);
    gtk_widget_set_sensitive (p->prev, FALSE);

    gtk_paned_set_start_child (GTK_PANED (paned), left);
    gtk_paned_set_end_child (GTK_PANED (paned), build_details (p));
    gtk_paned_set_resize_end_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_position (GTK_PANED (paned), 760);
    gtk_box_append (GTK_BOX (root), paned);

    run_search (p, 1);   /* trending, Mature hidden */
    return root;
}

static GtkWidget *ws_window;

void walpe_workshop_window_present (GtkWindow *parent, const WalpeWorkshopHooks *hooks)
{
    if (ws_window) {
        gtk_window_present (GTK_WINDOW (ws_window));
        return;
    }
    ws_window = gtk_window_new ();
    g_object_add_weak_pointer (G_OBJECT (ws_window), (gpointer *) &ws_window);
    gtk_window_set_title (GTK_WINDOW (ws_window), "Steam Workshop — WalPE");
    gtk_window_set_transient_for (GTK_WINDOW (ws_window), parent);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (ws_window), TRUE);
    gtk_window_set_default_size (GTK_WINDOW (ws_window), 1180, 800);
    if (parent && gtk_window_get_application (parent))
        gtk_window_set_application (GTK_WINDOW (ws_window), gtk_window_get_application (parent));
    gtk_window_set_child (GTK_WINDOW (ws_window), walpe_workshop_page_new_with_hooks (hooks));
    gtk_window_present (GTK_WINDOW (ws_window));
}

/* the GUI's Workshop tab: talks to the daemon itself and uses the
 * configured library dir; the library view picks up new items through its
 * directory monitor. This function marks "the GUI's own IPC": the page
 * then sends through uiipc (ipc_async) and never calls it on the main
 * thread; it is the synchronous hook only for a host that asks for one. */
static char *default_ipc (const char *json)
{
    char *sock = walpe_socket_path ();
    char *reply = walpe_ipc_request (sock, json, 3000);
    g_free (sock);
    return reply;
}

GtkWidget *walpe_workshop_page_new_hosted (void (*library_changed) (void))
{
    GtkWidget *w = walpe_workshop_page_new ();
    Page *p = g_object_get_data (G_OBJECT (w), "walpe-workshop-page");
    p->hooks.library_changed = library_changed;
    return w;
}

GtkWidget *walpe_workshop_page_new (void)
{
    /* wallpapers_dir read straight from config.json: walpe_config_load()
     * would replace the GUI's own config singleton */
    char *cp = g_build_filename (g_get_user_config_dir (), "walpe", "config.json", NULL);
    JsonNode *root = wpj_parse_file (cp);
    g_free (cp);
    const char *d = root && JSON_NODE_HOLDS_OBJECT (root)
        ? wpj_str (json_node_get_object (root), "wallpapers_dir", NULL) : NULL;
    WalpeWorkshopHooks h = { default_ipc, NULL, NULL, d && *d ? d : walpe_default_dir () };
    GtkWidget *w = walpe_workshop_page_new_with_hooks (&h);   /* copies the dir */
    if (root)
        json_node_unref (root);
    return w;
}
