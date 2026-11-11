/* layers.c - "Adjust layers" section of the options pane (see layers.h) */
#include "layers.h"
#include "uiipc.h"
#include "numfield.h"
#include "common/util.h"
#include <math.h>
#include <string.h>

#define HL_SECONDS 4.0      /* how long the daemon keeps an outline we do not renew */
#define HL_RENEW_MS 1500
#define LIST_RETRY_MS 700   /* the scene was still loading */
#define LIST_SLOW_MS 2500   /* ... and after LIST_RETRIES quick tries */
#define LIST_RETRIES 8

typedef struct {
    char *key, *name, *type, *detail, *search;
    int index;             /* position in the scene (back to front) */
    int parent;            /* scene index or -1 */
    int depth;
    gboolean shown;        /* drawn right now */
    gboolean has_adj;
    double dx, dy, scale;
    int vis;               /* 0 as the wallpaper sets it, 1 hidden, 2 always shown */
    gboolean filter_ok;
    GtkWidget *row, *name_lbl, *state_lbl;
} LRow;

typedef struct {
    GtkWidget *expander;
    GtkWidget *hint;
    GtkWidget *body;       /* everything below the hint */
    GtkWidget *search, *list, *sel_children, *refresh;
    GtkWidget *x_spin, *y_spin, *step_dd, *scale_spin, *vis_dd;
    GtkWidget *nudge[4];
    GtkWidget *controls;
    GtkWidget *sel_lbl, *reset_sel, *reset_all;
    GtkWidget *missing_box, *missing_lbl;
    WalpeLayersStatus status;
    GPtrArray *rows;       /* LRow*, display order */
    GHashTable *by_key;    /* key -> LRow (borrowed) */
    GPtrArray *missing;    /* saved keys no layer has */
    char *path, *type, *filter;
    gboolean is_current, supported;
    gboolean running;      /* the daemon's word: a scene shows this wallpaper */
    gboolean loaded, loading;
    guint retries, retry_id;
    gboolean updating;     /* widgets are being set by code */
    guint gen;             /* per wallpaper: late replies of another one are dropped */
    guint inflight;
    guint n_saved;
    double shown_dx, shown_dy, shown_scale;   /* what the fields were last set / sent as */
    gboolean hl_on;
    guint hl_timer;
} LP;

static LP *lp_of (GtkWidget *w) { return g_object_get_data (G_OBJECT (w), "walpe-layers"); }
static LRow *lrow_of (gpointer w) { return g_object_get_data (G_OBJECT (w), "walpe-lrow"); }

static void lrow_free (gpointer p)
{
    LRow *r = p;
    g_free (r->key);
    g_free (r->name);
    g_free (r->type);
    g_free (r->detail);
    g_free (r->search);
    g_free (r);
}

static gboolean is_scene (LP *P) { return P->type && !g_ascii_strcasecmp (P->type, "scene"); }

static const char *type_label (const char *t)
{
    static const struct { const char *t, *l; } tab[] = {
        { "image", "Picture" }, { "text", "Text" }, { "particles", "Particles" },
        { "compose", "Effect" }, { "fullscreen", "Full-screen effect" }, { "solid", "Colour" },
        { "video", "Video" }, { "group", "Group" },
    };
    for (guint i = 0; i < G_N_ELEMENTS (tab); i++)
        if (!g_strcmp0 (t, tab[i].t))
            return tab[i].l;
    return "Layer";
}

/* ------------------------------------------------------------ selection */

static guint selected_rows (LP *P, GPtrArray *out /* LRow*, may be NULL */)
{
    guint n = 0;
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row))) {
            n++;
            if (out)
                g_ptr_array_add (out, r);
        }
    }
    return n;
}

static LRow *row_by_index (LP *P, int index)
{
    for (guint i = 0; index >= 0 && i < P->rows->len; i++)
        if (((LRow *) P->rows->pdata[i])->index == index)
            return P->rows->pdata[i];
    return NULL;
}

/* some ancestor of r is selected too: r moves with it */
static gboolean has_selected_ancestor (LP *P, LRow *r)
{
    int hops = 0;
    for (LRow *p = row_by_index (P, r->parent); p && hops < 256; p = row_by_index (P, p->parent), hops++)
        if (gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (p->row)))
            return TRUE;
    return FALSE;
}

/* the first selected layer that is not below another selected one: the
 * fields show its values */
static LRow *first_root (LP *P)
{
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row)) && !has_selected_ancestor (P, r))
            return r;
    }
    return NULL;
}

/* -------------------------------------------------------------- display */

static void row_update (LRow *r)
{
    GString *s = g_string_new (NULL);
    if (r->has_adj && (r->dx != 0 || r->dy != 0))
        g_string_append (s, "moved");
    if (r->has_adj && r->scale != 1.0)
        g_string_append_printf (s, "%sresized", s->len ? ", " : "");
    if (r->has_adj && r->vis == 1)
        g_string_append_printf (s, "%shidden", s->len ? ", " : "");
    else if (r->has_adj && r->vis == 2)
        g_string_append_printf (s, "%salways shown", s->len ? ", " : "");
    else if (!r->shown)
        g_string_append_printf (s, "%snot shown now", s->len ? ", " : "");
    gtk_label_set_text (GTK_LABEL (r->state_lbl), s->str);
    g_string_free (s, TRUE);
    if (r->has_adj)
        gtk_widget_add_css_class (r->name_lbl, "walpe-prop-modified");
    else
        gtk_widget_remove_css_class (r->name_lbl, "walpe-prop-modified");
}

static void set_hint (LP *P)
{
    const char *t;
    if (!P->path)
        t = "";
    else if (!P->is_current)
        t = P->n_saved ? "This wallpaper is not on a monitor right now. Its saved layer adjustments "
                         "are applied when you set it; set it to change them."
                       : "Set this wallpaper on a monitor to move, resize or hide its layers.";
    else if ((P->loading || !P->loaded) && P->retries > LIST_RETRIES)
        t = "The wallpaper has not handed over its layers yet. It may still be loading; this "
            "section fills in when it is ready.";
    else if (P->loading || !P->loaded)
        t = "Reading the layers of the running wallpaper…";
    else if (!P->rows->len)
        t = "This wallpaper has no layers that can be adjusted.";
    else
        t = "Pick a layer (Ctrl-click or Shift-click for several), then move, resize or hide it. "
            "What you pick is outlined on the wallpaper, with the layers that hang below it and "
            "move along. Changes show at once and are saved for this wallpaper.";
    gtk_label_set_text (GTK_LABEL (P->hint), t);
}

static void spin_set (GtkWidget *spin, double v)
{
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), v);
}

/* fields <- the first selected layer; buttons <- what can be done */
static void sync_controls (LP *P)
{
    LRow *r = first_root (P);
    GPtrArray *sel = g_ptr_array_new ();
    guint n = selected_rows (P, sel);
    gboolean any_adj = FALSE;
    for (guint i = 0; i < sel->len; i++)
        any_adj = any_adj || ((LRow *) sel->pdata[i])->has_adj;
    g_ptr_array_unref (sel);
    gboolean live = P->is_current && P->loaded;
    P->updating = TRUE;
    P->shown_dx = r && r->has_adj ? r->dx : 0;
    P->shown_dy = r && r->has_adj ? r->dy : 0;
    P->shown_scale = r && r->has_adj ? r->scale : 1;
    spin_set (P->x_spin, P->shown_dx);
    spin_set (P->y_spin, P->shown_dy);
    spin_set (P->scale_spin, P->shown_scale);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->vis_dd), r && r->has_adj ? (guint) r->vis : 0);
    P->updating = FALSE;
    gtk_widget_set_sensitive (P->controls, live && n > 0);
    gtk_widget_set_sensitive (P->sel_children, live && n > 0);
    gtk_widget_set_sensitive (P->reset_sel, live && any_adj);
    gtk_widget_set_sensitive (P->reset_all, P->n_saved > 0);
    char *t = n == 0 ? g_strdup ("Nothing selected")
            : n == 1 ? g_strdup ("1 layer selected")
                     : g_strdup_printf ("%u layers selected: they move together", n);
    gtk_label_set_text (GTK_LABEL (P->sel_lbl), t);
    g_free (t);
}

static void update_missing (LP *P)
{
    guint n = P->missing->len;
    gtk_widget_set_visible (P->missing_box, n > 0);
    if (n) {
        char *t = g_strdup_printf (n == 1 ? "%u saved adjustment belongs to a layer this wallpaper "
                                            "no longer has. It is ignored."
                                          : "%u saved adjustments belong to layers this wallpaper "
                                            "no longer has. They are ignored.", n);
        gtk_label_set_text (GTK_LABEL (P->missing_lbl), t);
        g_free (t);
    }
}

/* ------------------------------------------------------------ outline */

static void hl_send (LP *P, gboolean on)
{
    if (!P->path)
        return;
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "layeradj");
    json_builder_set_member_name (b, "op");
    json_builder_add_string_value (b, "highlight");
    json_builder_set_member_name (b, "path");
    json_builder_add_string_value (b, P->path);
    json_builder_set_member_name (b, "seconds");
    json_builder_add_double_value (b, HL_SECONDS);
    /* with what hangs below them: that moves along (and a group has no
     * picture of its own to outline) */
    json_builder_set_member_name (b, "children");
    json_builder_add_boolean_value (b, TRUE);
    json_builder_set_member_name (b, "keys");
    json_builder_begin_array (b);
    for (guint i = 0; on && i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row)))
            json_builder_add_string_value (b, r->key);
    }
    json_builder_end_array (b);
    json_builder_end_object (b);
    ui_ipc_send_node (json_builder_get_root (b), 0, NULL, NULL);
    g_object_unref (b);
    P->hl_on = on;
}

static gboolean hl_wanted (LP *P)
{
    return P->path && P->supported && P->is_current && P->loaded &&
           gtk_expander_get_expanded (GTK_EXPANDER (P->expander)) &&
           gtk_widget_get_mapped (P->expander) && selected_rows (P, NULL) > 0;
}

static gboolean hl_tick (gpointer u)
{
    LP *P = u;
    if (hl_wanted (P)) {
        hl_send (P, TRUE);   /* renew: the daemon drops it by itself otherwise */
        return G_SOURCE_CONTINUE;
    }
    if (P->hl_on)
        hl_send (P, FALSE);
    P->hl_timer = 0;
    return G_SOURCE_REMOVE;
}

/* outline what is selected now, or take the outline away */
static void hl_update (LP *P)
{
    if (hl_wanted (P)) {
        hl_send (P, TRUE);
        if (!P->hl_timer)
            P->hl_timer = g_timeout_add (HL_RENEW_MS, hl_tick, P);
    } else if (P->hl_on) {
        hl_send (P, FALSE);
        if (P->hl_timer) {
            g_source_remove (P->hl_timer);
            P->hl_timer = 0;
        }
    }
}

/* ------------------------------------------------------------- requests */

typedef struct { LP *P; guint gen; char *what; } OpCtx;

static void request_list (LP *P);

/* the daemon's "adjustments" -> the rows */
static void apply_adjustments (LP *P, JsonArray *aa)
{
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        r->has_adj = FALSE;
        r->dx = r->dy = 0;
        r->scale = 1;
        r->vis = 0;
    }
    g_ptr_array_set_size (P->missing, 0);
    P->n_saved = wpj_arr_len (aa);
    for (guint i = 0; i < wpj_arr_len (aa); i++) {
        JsonObject *ao = wpj_arr_obj (aa, i);
        const char *key = ao ? wpj_str (ao, "key", NULL) : NULL;
        if (!key)
            continue;
        LRow *r = g_hash_table_lookup (P->by_key, key);
        if (wpj_bool (ao, "missing", FALSE) || !r) {
            if (wpj_bool (ao, "missing", FALSE))
                g_ptr_array_add (P->missing, g_strdup (key));
            continue;
        }
        const char *v = wpj_str (ao, "visible", "default");
        r->has_adj = TRUE;
        r->dx = wpj_num (ao, "dx", 0);
        r->dy = wpj_num (ao, "dy", 0);
        r->scale = wpj_num (ao, "scale", 1);
        r->vis = !strcmp (v, "hidden") ? 1 : !strcmp (v, "shown") ? 2 : 0;
    }
    for (guint i = 0; i < P->rows->len; i++)
        row_update (P->rows->pdata[i]);
    update_missing (P);
}

static void on_op (JsonObject *reply, const char *err, gpointer user)
{
    OpCtx *c = user;
    LP *P = c->P;
    if (P->inflight)
        P->inflight--;
    if (c->gen == P->gen) {
        if (ui_ipc_ok (reply)) {
            apply_adjustments (P, wpj_arr (reply, "adjustments"));
            P->status (c->what, FALSE);
            if (!P->inflight)
                sync_controls (P);   /* not while more of the user's changes are on their way */
            set_hint (P);
        } else {
            const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
            char *m = g_strdup_printf ("Layer not changed: %s", e ? e : "error");
            P->status (m, TRUE);
            g_free (m);
            request_list (P);        /* show what is really saved */
        }
    }
    g_free (c->what);
    g_free (c);
}

static JsonBuilder *op_begin (LP *P, const char *op, gboolean with_keys)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "layeradj");
    json_builder_set_member_name (b, "op");
    json_builder_add_string_value (b, op);
    json_builder_set_member_name (b, "path");
    json_builder_add_string_value (b, P->path);
    if (with_keys) {
        json_builder_set_member_name (b, "keys");
        json_builder_begin_array (b);
        for (guint i = 0; i < P->rows->len; i++) {
            LRow *r = P->rows->pdata[i];
            if (gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row)))
                json_builder_add_string_value (b, r->key);
        }
        json_builder_end_array (b);
    }
    return b;
}

static void op_send (LP *P, JsonBuilder *b, const char *what)
{
    json_builder_end_object (b);
    OpCtx *c = g_new0 (OpCtx, 1);
    c->P = P;
    c->gen = P->gen;
    c->what = g_strdup (what);
    P->inflight++;
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW, on_op, c);
    g_object_unref (b);
}

/* ------------------------------------------------------- the layer list */

static gboolean filter_func (GtkListBoxRow *row, gpointer user)
{
    (void) user;
    LRow *r = lrow_of (row);
    return !r || r->filter_ok;
}

static void apply_filter (LP *P)
{
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        r->filter_ok = !P->filter || !*P->filter || strstr (r->search, P->filter) != NULL;
    }
    gtk_list_box_invalidate_filter (GTK_LIST_BOX (P->list));
}

static GtkWidget *mk_label (const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    if (css)
        gtk_widget_add_css_class (l, css);
    return l;
}

static void build_row (LP *P, LRow *r)
{
    r->row = gtk_list_box_row_new ();
    GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (h, 6 + 18 * MIN (r->depth, 8));
    gtk_widget_set_margin_end (h, 6);
    gtk_widget_set_margin_top (h, 3);
    gtk_widget_set_margin_bottom (h, 3);
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (v, TRUE);
    r->name_lbl = mk_label (r->name, "walpe-prop-title");
    gtk_label_set_ellipsize (GTK_LABEL (r->name_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (v), r->name_lbl);
    if (r->detail && *r->detail) {
        GtkWidget *d = mk_label (r->detail, "walpe-prop-sub");
        gtk_label_set_ellipsize (GTK_LABEL (d), PANGO_ELLIPSIZE_END);
        gtk_box_append (GTK_BOX (v), d);
    }
    gtk_box_append (GTK_BOX (h), v);
    r->state_lbl = mk_label ("", "walpe-prop-sub");
    gtk_widget_set_valign (r->state_lbl, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (h), r->state_lbl);
    GtkWidget *badge = mk_label (type_label (r->type), "walpe-badge");
    gtk_widget_add_css_class (badge, "neutral");
    gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (h), badge);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (r->row), h);
    g_object_set_data (G_OBJECT (r->row), "walpe-lrow", r);
    row_update (r);
    P->updating = TRUE;
    gtk_list_box_append (GTK_LIST_BOX (P->list), r->row);
    P->updating = FALSE;
    (void) P;
}

typedef struct { LP *P; JsonArray *la; GArray **kids; gboolean *seen; guint n; } Walk;

/* a layer, then the layers below it (front-most first) */
static void walk (Walk *w, int i, int depth)
{
    if (i < 0 || (guint) i >= w->n || w->seen[i] || depth > 64)
        return;
    w->seen[i] = TRUE;
    JsonObject *lo = wpj_arr_obj (w->la, (guint) i);
    const char *type = lo ? wpj_str (lo, "type", "") : "";
    /* not something to place: the camera; sounds, lights and empty groups ("none") */
    if (lo && strcmp (type, "camera") && strcmp (type, "none")) {
        LRow *r = g_new0 (LRow, 1);
        const char *name = wpj_str (lo, "name", ""), *text = wpj_str (lo, "text", NULL);
        const char *file = wpj_str (lo, "file", NULL);
        r->key = g_strdup (wpj_str (lo, "key", ""));
        r->name = g_strdup (*name ? name : "(no name)");
        r->type = g_strdup (type);
        if (text) {
            char *one = g_strdup (text);
            g_strdelimit (one, "\n\r\t", ' ');
            r->detail = g_strdup_printf ("“%s”", one);
            g_free (one);
        } else if (file) {
            r->detail = g_strdup (file);
        }
        r->index = i;
        r->parent = (int) wpj_num (lo, "parent", -1);
        r->depth = depth;
        r->shown = wpj_bool (lo, "shown", TRUE);
        r->scale = 1;
        r->filter_ok = TRUE;
        char *all = g_strdup_printf ("%s %s %s %s %s", r->name, r->detail ? r->detail : "",
                                     type_label (type), type, r->key);
        r->search = g_utf8_casefold (all, -1);
        g_free (all);
        g_ptr_array_add (w->P->rows, r);
        g_hash_table_insert (w->P->by_key, r->key, r);
        build_row (w->P, r);
        depth++;
    }
    for (int k = (int) w->kids[i]->len - 1; k >= 0; k--)
        walk (w, g_array_index (w->kids[i], int, k), depth);
}

static void clear_rows (LP *P)
{
    P->updating = TRUE;
    for (guint i = 0; i < P->rows->len; i++)
        g_object_set_data (G_OBJECT (((LRow *) P->rows->pdata[i])->row), "walpe-lrow", NULL);
    gtk_list_box_remove_all (GTK_LIST_BOX (P->list));
    g_hash_table_remove_all (P->by_key);
    g_ptr_array_set_size (P->rows, 0);
    P->updating = FALSE;
}

static gboolean retry_cb (gpointer u)
{
    LP *P = u;
    P->retry_id = 0;
    P->loading = FALSE;
    if (gtk_expander_get_expanded (GTK_EXPANDER (P->expander)))
        request_list (P);
    else
        set_hint (P);   /* closed meanwhile: opening it asks again (maybe_load) */
    return G_SOURCE_REMOVE;
}

static void on_list (JsonObject *reply, const char *err, gpointer user)
{
    OpCtx *c = user;
    LP *P = c->P;
    gboolean mine = c->gen == P->gen;
    g_free (c);
    if (!mine)
        return;
    P->loading = FALSE;
    if (!ui_ipc_ok (reply)) {
        const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
        char *m = g_strdup_printf ("The layers could not be read: %s", e ? e : "error");
        gtk_label_set_text (GTK_LABEL (P->hint), m);
        g_free (m);
        return;
    }
    JsonArray *la = wpj_arr (reply, "layers");
    guint n = wpj_arr_len (la);
    P->running = wpj_bool (reply, "running", FALSE);
    if (!n && P->is_current && (wpj_bool (reply, "loading", FALSE) ||
                                (P->running && !wpj_bool (reply, "fresh", FALSE)))) {
        /* the wallpaper is still loading (a big scene takes seconds), or
         * its frame did not come in time: ask again - quickly at first,
         * then slowly for as long as the section is there to show it */
        if (P->retries <= LIST_RETRIES)
            P->retries++;
        if (!P->retry_id)
            P->retry_id = g_timeout_add (P->retries > LIST_RETRIES ? LIST_SLOW_MS : LIST_RETRY_MS,
                                         retry_cb, P);
        P->loading = TRUE;
        set_hint (P);
        return;
    }
    /* keep what was selected (same wallpaper, list read again) */
    GHashTable *keep = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row)))
            g_hash_table_add (keep, g_strdup (r->key));
    }
    clear_rows (P);
    Walk w = { P, la, g_new0 (GArray *, n + 1), g_new0 (gboolean, n + 1), n };
    for (guint i = 0; i <= n; i++)
        w.kids[i] = g_array_new (FALSE, FALSE, sizeof (int));
    GArray *roots = w.kids[n];
    for (guint i = 0; i < n; i++) {
        int par = (int) wpj_num (wpj_arr_obj (la, i), "parent", -1), v = (int) i;
        g_array_append_val (par >= 0 && (guint) par < n && par != v ? w.kids[par] : roots, v);
    }
    for (int k = (int) roots->len - 1; k >= 0; k--)
        walk (&w, g_array_index (roots, int, k), 0);
    for (guint i = 0; i < n; i++)
        walk (&w, (int) i, 0);   /* whatever a parent cycle left out */
    for (guint i = 0; i <= n; i++)
        g_array_unref (w.kids[i]);
    g_free (w.kids);
    g_free (w.seen);
    P->loaded = TRUE;
    apply_adjustments (P, wpj_arr (reply, "adjustments"));
    apply_filter (P);
    P->updating = TRUE;
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (g_hash_table_contains (keep, r->key))
            gtk_list_box_select_row (GTK_LIST_BOX (P->list), GTK_LIST_BOX_ROW (r->row));
    }
    P->updating = FALSE;
    g_hash_table_unref (keep);
    gtk_widget_set_visible (P->body, P->rows->len > 0);
    set_hint (P);
    sync_controls (P);
    hl_update (P);
}

static void request_list (LP *P)
{
    if (!P->path || P->loading)
        return;
    P->loading = TRUE;
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "layers");
    json_builder_set_member_name (b, "path");
    json_builder_add_string_value (b, P->path);
    json_builder_end_object (b);
    OpCtx *c = g_new0 (OpCtx, 1);
    c->P = P;
    c->gen = P->gen;
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW, on_list, c);
    g_object_unref (b);
    set_hint (P);
}

/* read the list when the section is open and there is something to read */
static void maybe_load (LP *P)
{
    if (P->path && is_scene (P) && P->supported && !P->loaded && !P->loading &&
        gtk_expander_get_expanded (GTK_EXPANDER (P->expander)))
        request_list (P);
}

/* -------------------------------------------------------------- handlers */

static void on_selection (GtkListBox *lb, gpointer user)
{
    (void) lb;
    LP *P = user;
    if (P->updating)
        return;
    sync_controls (P);
    hl_update (P);
}

static void send_move (LP *P, double dx, double dy)
{
    JsonBuilder *b = op_begin (P, "move", TRUE);
    json_builder_set_member_name (b, "dx");
    json_builder_add_double_value (b, dx);
    json_builder_set_member_name (b, "dy");
    json_builder_add_double_value (b, dy);
    op_send (P, b, "Layer moved.");
}

static void on_offset (GtkSpinButton *sb, gpointer user)
{
    LP *P = user;
    if (P->updating || !P->path)
        return;
    gboolean is_x = GTK_WIDGET (sb) == P->x_spin;
    double v = gtk_spin_button_get_value (sb);
    double d = v - (is_x ? P->shown_dx : P->shown_dy);
    if (fabs (d) < 0.005)
        return;
    if (is_x)
        P->shown_dx = v;
    else
        P->shown_dy = v;
    send_move (P, is_x ? d : 0, is_x ? 0 : d);
}

static void on_scale (GtkSpinButton *sb, gpointer user)
{
    LP *P = user;
    if (P->updating || !P->path)
        return;
    double v = gtk_spin_button_get_value (sb);
    if (v <= 0 || P->shown_scale <= 0 || fabs (v - P->shown_scale) < 0.0005)
        return;
    double f = v / P->shown_scale;
    P->shown_scale = v;
    JsonBuilder *b = op_begin (P, "scale", TRUE);
    json_builder_set_member_name (b, "factor");
    json_builder_add_double_value (b, f);
    op_send (P, b, "Layer resized.");
}

static double step_px (LP *P)
{
    static const double steps[] = { 1, 5, 10, 50, 100 };
    guint s = gtk_drop_down_get_selected (GTK_DROP_DOWN (P->step_dd));
    return steps[s < G_N_ELEMENTS (steps) ? s : 2];
}

static void on_step (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) o; (void) ps;
    LP *P = user;
    double s = step_px (P);
    gtk_spin_button_set_increments (GTK_SPIN_BUTTON (P->x_spin), s, s * 10);
    gtk_spin_button_set_increments (GTK_SPIN_BUTTON (P->y_spin), s, s * 10);
}

static void on_nudge (GtkButton *btn, gpointer user)
{
    LP *P = user;
    double s = step_px (P);
    /* left, up, down, right - as on the screen (scene y grows upwards) */
    if (GTK_WIDGET (btn) == P->nudge[0])
        spin_set (P->x_spin, gtk_spin_button_get_value (GTK_SPIN_BUTTON (P->x_spin)) - s);
    else if (GTK_WIDGET (btn) == P->nudge[1])
        spin_set (P->y_spin, gtk_spin_button_get_value (GTK_SPIN_BUTTON (P->y_spin)) + s);
    else if (GTK_WIDGET (btn) == P->nudge[2])
        spin_set (P->y_spin, gtk_spin_button_get_value (GTK_SPIN_BUTTON (P->y_spin)) - s);
    else
        spin_set (P->x_spin, gtk_spin_button_get_value (GTK_SPIN_BUTTON (P->x_spin)) + s);
}

static void on_vis (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps;
    LP *P = user;
    if (P->updating || !P->path)
        return;
    static const char *const names[] = { "default", "hidden", "shown" };
    guint v = gtk_drop_down_get_selected (GTK_DROP_DOWN (o));
    if (v > 2)
        return;
    JsonBuilder *b = op_begin (P, "set", FALSE);
    json_builder_set_member_name (b, "layers");
    json_builder_begin_array (b);
    guint n = 0;
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (!gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row)))
            continue;
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "key");
        json_builder_add_string_value (b, r->key);
        json_builder_set_member_name (b, "visible");
        json_builder_add_string_value (b, names[v]);
        json_builder_end_object (b);
        n++;
    }
    json_builder_end_array (b);
    if (!n) {
        g_object_unref (b);
        return;
    }
    op_send (P, b, v == 1 ? "Layer hidden." : v == 2 ? "Layer always shown."
                                                     : "Layer shown as the wallpaper sets it.");
}

static void on_reset_sel (GtkButton *btn, gpointer user)
{
    (void) btn;
    LP *P = user;
    if (!P->path || !selected_rows (P, NULL))
        return;
    op_send (P, op_begin (P, "reset", TRUE), "Selected layers are back where the wallpaper puts them.");
}

static void on_reset_all (GtkButton *btn, gpointer user)
{
    (void) btn;
    LP *P = user;
    if (!P->path)
        return;
    op_send (P, op_begin (P, "reset", FALSE), "Every layer is back where the wallpaper puts it.");
}

static void on_remove_missing (GtkButton *btn, gpointer user)
{
    (void) btn;
    LP *P = user;
    if (!P->path || !P->missing->len)
        return;
    JsonBuilder *b = op_begin (P, "reset", FALSE);
    json_builder_set_member_name (b, "keys");
    json_builder_begin_array (b);
    for (guint i = 0; i < P->missing->len; i++)
        json_builder_add_string_value (b, P->missing->pdata[i]);
    json_builder_end_array (b);
    op_send (P, b, "Unused layer adjustments removed.");
}

static void on_children (GtkButton *btn, gpointer user)
{
    (void) btn;
    LP *P = user;
    P->updating = TRUE;
    for (guint i = 0; i < P->rows->len; i++) {
        LRow *r = P->rows->pdata[i];
        if (!gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (r->row)) && has_selected_ancestor (P, r))
            gtk_list_box_select_row (GTK_LIST_BOX (P->list), GTK_LIST_BOX_ROW (r->row));
    }
    P->updating = FALSE;
    sync_controls (P);
    hl_update (P);
}

static void on_refresh (GtkButton *btn, gpointer user)
{
    (void) btn;
    LP *P = user;
    P->retries = 0;
    request_list (P);
}

static void on_search (GtkSearchEntry *e, gpointer user)
{
    LP *P = user;
    g_free (P->filter);
    P->filter = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (e)), -1);
    g_strstrip (P->filter);
    apply_filter (P);
}

static void on_expanded (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) o; (void) ps;
    LP *P = user;
    maybe_load (P);
    hl_update (P);
}

static void on_map_change (GtkWidget *w, gpointer user)
{
    (void) w;
    hl_update (user);
}

/* a spin button that shows "420" and "-372.25", not "420.00" */
static gboolean on_spin_output (GtkSpinButton *sb, gpointer user)
{
    (void) user;
    char buf[40];
    g_snprintf (buf, sizeof buf, "%.2f", gtk_spin_button_get_value (sb));
    char *dot = strchr (buf, '.');
    if (dot) {
        char *e = buf + strlen (buf) - 1;
        while (e > dot && *e == '0')
            *e-- = 0;
        if (e == dot)
            *e = 0;
    }
    if (!strcmp (buf, "-0"))
        strcpy (buf, "0");
    if (strcmp (buf, gtk_editable_get_text (GTK_EDITABLE (sb))))
        gtk_editable_set_text (GTK_EDITABLE (sb), buf);
    return TRUE;
}

/* ---------------------------------------------------------------- public */

void walpe_layers_panel_show (GtkWidget *panel, const char *path, const char *type)
{
    LP *P = lp_of (panel);
    if (!g_strcmp0 (P->path, path) && !g_strcmp0 (P->type, type))
        return;
    if (P->hl_on)
        hl_send (P, FALSE);   /* the outline belongs to the wallpaper we leave */
    if (P->hl_timer) {
        g_source_remove (P->hl_timer);
        P->hl_timer = 0;
    }
    if (P->retry_id) {
        g_source_remove (P->retry_id);
        P->retry_id = 0;
    }
    P->gen++;
    g_free (P->path);
    g_free (P->type);
    P->path = g_strdup (path);
    P->type = g_strdup (type);
    P->loaded = P->loading = FALSE;
    P->retries = 0;
    P->inflight = 0;
    P->n_saved = 0;
    clear_rows (P);
    g_ptr_array_set_size (P->missing, 0);
    update_missing (P);
    gtk_widget_set_visible (P->body, FALSE);
    gtk_widget_set_visible (P->expander, path && is_scene (P) && P->supported);
    set_hint (P);
    sync_controls (P);
    maybe_load (P);
}

void walpe_layers_panel_set_state (GtkWidget *panel, gboolean is_current, gboolean supported)
{
    LP *P = lp_of (panel);
    gboolean was = P->is_current, was_sup = P->supported;
    P->is_current = is_current;
    P->supported = supported;
    gtk_widget_set_visible (P->expander, P->path && is_scene (P) && supported);
    if (was == is_current && was_sup == supported) {
        maybe_load (P);
        return;   /* a status poll that changed nothing */
    }
    if (was != is_current) {
        /* set on a monitor, or taken off: the list is another one now */
        P->loaded = FALSE;
        P->retries = 0;
        if (!is_current) {
            clear_rows (P);
            gtk_widget_set_visible (P->body, FALSE);
        }
        set_hint (P);
        sync_controls (P);
    }
    maybe_load (P);
    hl_update (P);
}

void walpe_layers_panel_expand (GtkWidget *panel, gboolean open)
{
    gtk_expander_set_expanded (GTK_EXPANDER (lp_of (panel)->expander), open);
}

static void lp_free (gpointer p)
{
    LP *P = p;
    if (P->hl_timer)
        g_source_remove (P->hl_timer);
    if (P->retry_id)
        g_source_remove (P->retry_id);
    g_ptr_array_unref (P->rows);
    g_ptr_array_unref (P->missing);
    g_hash_table_unref (P->by_key);
    g_free (P->path);
    g_free (P->type);
    g_free (P->filter);
    g_free (P);
}

static GtkWidget *mk_spin (LP *P, double lo, double hi, double step, const char *tip,
                           GCallback changed)
{
    GtkWidget *s = gtk_spin_button_new_with_range (lo, hi, step);
    gtk_spin_button_set_digits (GTK_SPIN_BUTTON (s), 2);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (s), TRUE);
    walpe_num_field_keep_when_empty (s);
    gtk_editable_set_width_chars (GTK_EDITABLE (s), 7);
    gtk_widget_set_tooltip_text (s, tip);
    g_signal_connect (s, "output", G_CALLBACK (on_spin_output), NULL);
    g_signal_connect (s, "value-changed", changed, P);
    return s;
}

GtkWidget *walpe_layers_panel_new (WalpeLayersStatus status)
{
    LP *P = g_new0 (LP, 1);
    P->status = status;
    P->rows = g_ptr_array_new_with_free_func (lrow_free);
    P->missing = g_ptr_array_new_with_free_func (g_free);
    P->by_key = g_hash_table_new (g_str_hash, g_str_equal);
    P->shown_scale = 1;

    P->expander = gtk_expander_new (NULL);
    GtkWidget *title = mk_label ("Adjust layers", "walpe-section-title");
    gtk_expander_set_label_widget (GTK_EXPANDER (P->expander), title);
    gtk_widget_set_tooltip_text (title, "Move, resize or hide parts of this wallpaper");
    gtk_widget_set_margin_start (P->expander, 12);
    gtk_widget_set_margin_end (P->expander, 12);
    gtk_widget_set_margin_top (P->expander, 6);
    gtk_widget_set_margin_bottom (P->expander, 8);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (box, 6);
    gtk_widget_set_margin_start (box, 4);
    gtk_widget_set_margin_end (box, 4);
    P->hint = mk_label ("", "walpe-options-hint");
    gtk_label_set_wrap (GTK_LABEL (P->hint), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (P->hint), PANGO_WRAP_WORD_CHAR);
    gtk_box_append (GTK_BOX (box), P->hint);

    P->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    P->search = gtk_search_entry_new ();
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (P->search), "Find a layer");
    gtk_widget_set_hexpand (P->search, TRUE);
    g_signal_connect (P->search, "search-changed", G_CALLBACK (on_search), P);
    gtk_box_append (GTK_BOX (bar), P->search);
    P->sel_children = gtk_button_new_with_label ("Add its children");
    gtk_widget_set_tooltip_text (P->sel_children,
                                 "Also select every layer that hangs below the selected ones");
    g_signal_connect (P->sel_children, "clicked", G_CALLBACK (on_children), P);
    gtk_box_append (GTK_BOX (bar), P->sel_children);
    P->refresh = gtk_button_new_with_label ("Reload list");
    gtk_widget_set_tooltip_text (P->refresh, "Read the layers of the running wallpaper again");
    g_signal_connect (P->refresh, "clicked", G_CALLBACK (on_refresh), P);
    gtk_box_append (GTK_BOX (bar), P->refresh);
    gtk_box_append (GTK_BOX (P->body), bar);

    P->list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (P->list), GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (P->list), FALSE);
    gtk_list_box_set_filter_func (GTK_LIST_BOX (P->list), filter_func, P, NULL);
    gtk_accessible_update_property (GTK_ACCESSIBLE (P->list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Layers of the wallpaper", -1);
    g_signal_connect (P->list, "selected-rows-changed", G_CALLBACK (on_selection), P);
    GtkWidget *sc = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (sc), 130);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (sc), 210);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sc), TRUE);
    gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (sc), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), P->list);
    gtk_box_append (GTK_BOX (P->body), sc);

    P->missing_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    P->missing_lbl = mk_label ("", "walpe-prop-sub");
    gtk_label_set_wrap (GTK_LABEL (P->missing_lbl), TRUE);
    gtk_widget_set_hexpand (P->missing_lbl, TRUE);
    gtk_box_append (GTK_BOX (P->missing_box), P->missing_lbl);
    GtkWidget *rm = gtk_button_new_with_label ("Remove");
    gtk_widget_set_valign (rm, GTK_ALIGN_CENTER);
    g_signal_connect (rm, "clicked", G_CALLBACK (on_remove_missing), P);
    gtk_box_append (GTK_BOX (P->missing_box), rm);
    gtk_widget_set_visible (P->missing_box, FALSE);
    gtk_box_append (GTK_BOX (P->body), P->missing_box);

    /* move / size / visibility of what is selected */
    P->controls = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (P->controls), 8);
    gtk_grid_set_row_spacing (GTK_GRID (P->controls), 6);
    const char *tip_xy = "Offset in the wallpaper's own pixels, added to where the wallpaper puts "
                         "the layer. X: + is right. Y: + is up.";
    gtk_grid_attach (GTK_GRID (P->controls), mk_label ("Move", "walpe-prop-title"), 0, 0, 1, 1);
    GtkWidget *mv = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (mv), mk_label ("X", NULL));
    P->x_spin = mk_spin (P, -100000, 100000, 10, tip_xy, G_CALLBACK (on_offset));
    gtk_accessible_update_property (GTK_ACCESSIBLE (P->x_spin), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "X offset", -1);
    gtk_box_append (GTK_BOX (mv), P->x_spin);
    gtk_box_append (GTK_BOX (mv), mk_label ("Y", NULL));
    P->y_spin = mk_spin (P, -100000, 100000, 10, tip_xy, G_CALLBACK (on_offset));
    gtk_accessible_update_property (GTK_ACCESSIBLE (P->y_spin), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Y offset", -1);
    gtk_box_append (GTK_BOX (mv), P->y_spin);
    gtk_grid_attach (GTK_GRID (P->controls), mv, 1, 0, 1, 1);
    /* second line (the pane stays narrow): nudge buttons and their step */
    mv = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    static const char *const arrows[4] = { "←", "↑", "↓", "→" };
    static const char *const arrow_tip[4] = { "Move left by one step", "Move up by one step",
                                              "Move down by one step", "Move right by one step" };
    for (int i = 0; i < 4; i++) {
        P->nudge[i] = gtk_button_new_with_label (arrows[i]);
        gtk_widget_set_tooltip_text (P->nudge[i], arrow_tip[i]);
        g_signal_connect (P->nudge[i], "clicked", G_CALLBACK (on_nudge), P);
        gtk_box_append (GTK_BOX (mv), P->nudge[i]);
    }
    GtkWidget *sl = mk_label ("Step", NULL);
    gtk_widget_set_margin_start (sl, 8);
    gtk_box_append (GTK_BOX (mv), sl);
    static const char *const steps[] = { "1 px", "5 px", "10 px", "50 px", "100 px", NULL };
    P->step_dd = gtk_drop_down_new_from_strings (steps);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->step_dd), 2);
    gtk_widget_set_tooltip_text (P->step_dd, "How far one arrow click moves the selected layers");
    g_signal_connect (P->step_dd, "notify::selected", G_CALLBACK (on_step), P);
    gtk_box_append (GTK_BOX (mv), P->step_dd);
    gtk_grid_attach (GTK_GRID (P->controls), mv, 1, 1, 1, 1);

    gtk_grid_attach (GTK_GRID (P->controls), mk_label ("Size", "walpe-prop-title"), 0, 2, 1, 1);
    GtkWidget *sz = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    P->scale_spin = mk_spin (P, 0.05, 20, 0.05,
                             "1 = the wallpaper's own size, 2 = twice as large, 0.5 = half. Several "
                             "selected layers are resized as one group.", G_CALLBACK (on_scale));
    gtk_accessible_update_property (GTK_ACCESSIBLE (P->scale_spin), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Size factor", -1);
    gtk_box_append (GTK_BOX (sz), P->scale_spin);
    gtk_box_append (GTK_BOX (sz), mk_label ("×", NULL));
    GtkWidget *vl = mk_label ("Visibility", "walpe-prop-title");
    gtk_widget_set_margin_start (vl, 14);
    gtk_box_append (GTK_BOX (sz), vl);
    static const char *const vis[] = { "As the wallpaper sets it", "Hidden", "Always shown", NULL };
    P->vis_dd = gtk_drop_down_new_from_strings (vis);
    gtk_widget_set_tooltip_text (P->vis_dd, "Hide the selected layers, or show them even where the "
                                            "wallpaper would hide them");
    g_signal_connect (P->vis_dd, "notify::selected", G_CALLBACK (on_vis), P);
    gtk_box_append (GTK_BOX (sz), P->vis_dd);
    gtk_grid_attach (GTK_GRID (P->controls), sz, 1, 2, 1, 1);
    gtk_box_append (GTK_BOX (P->body), P->controls);

    GtkWidget *foot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    P->sel_lbl = mk_label ("Nothing selected", "walpe-prop-sub");
    gtk_widget_set_hexpand (P->sel_lbl, TRUE);
    gtk_box_append (GTK_BOX (foot), P->sel_lbl);
    P->reset_sel = gtk_button_new_with_label ("Reset selected");
    gtk_widget_set_tooltip_text (P->reset_sel,
                                 "Put the selected layers back where the wallpaper has them");
    g_signal_connect (P->reset_sel, "clicked", G_CALLBACK (on_reset_sel), P);
    gtk_box_append (GTK_BOX (foot), P->reset_sel);
    P->reset_all = gtk_button_new_with_label ("Reset all");
    gtk_widget_set_tooltip_text (P->reset_all,
                                 "Drop every layer adjustment of this wallpaper (its options stay)");
    g_signal_connect (P->reset_all, "clicked", G_CALLBACK (on_reset_all), P);
    gtk_box_append (GTK_BOX (foot), P->reset_all);
    gtk_box_append (GTK_BOX (P->body), foot);

    gtk_widget_set_visible (P->body, FALSE);
    gtk_box_append (GTK_BOX (box), P->body);
    gtk_expander_set_child (GTK_EXPANDER (P->expander), box);
    gtk_expander_set_expanded (GTK_EXPANDER (P->expander), FALSE);
    g_signal_connect (P->expander, "notify::expanded", G_CALLBACK (on_expanded), P);
    g_signal_connect (P->expander, "map", G_CALLBACK (on_map_change), P);
    g_signal_connect (P->expander, "unmap", G_CALLBACK (on_map_change), P);
    gtk_widget_set_visible (P->expander, FALSE);
    on_step (NULL, NULL, P);
    sync_controls (P);
    g_object_set_data_full (G_OBJECT (P->expander), "walpe-layers", P, lp_free);
    return P->expander;
}
