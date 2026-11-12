/* resdialog.c - see resdialog.h */
#include "resdialog.h"
#include <string.h>
#include "resmodel.h"
#include "a11yname.h"
#include "uiipc.h"
#include "common/uitext.h"
#include "common/util.h"

#define NOTIFY_ID "monitor-resolution"

/* what the rest of the dialog needs of the screen's height besides the
 * list of monitors (heading, text, two check boxes, buttons, the title bar
 * and a desktop panel), and how tall the list is allowed to get: six rows
 * with their note line */
#define DIALOG_CHROME 320
#define LIST_MAX_HEIGHT 460
#define LIST_MIN_HEIGHT 150

static const char cost_line[] =
    "WalPE renders each monitor's wallpaper at the size chosen here and scales it to the screen. "
    "A larger render size uses more GPU time and memory; a smaller one saves both and is scaled up.";

/* ================================================================ state */

typedef struct Panel Panel;

/* one monitor in a panel */
typedef struct {
    Panel *panel;
    char *name;             /* output name */
    GtkWidget *box, *title, *detail, *note, *dd, *custom, *w_entry, *h_entry;
    GPtrArray *options;     /* WalpeMonresOption*: what dd lists, in its order */
    char *opt_sig;          /* their labels: dd is refilled only when they change */
    WalpeMonres value;      /* what the row stands for */
    gboolean valid;         /* FALSE: "Custom size" with fields that are no size */
    gboolean touched;       /* dialog: the user chose here, the row no longer follows
                             * the engine. Settings: typed and not sent yet. */
    int inflight;           /* settings: requests for this row not answered yet */
} Row;

/* the rows of the monitors: once in the dialog (a choice is kept until
 * "Apply"), once in Engine settings (live: a choice is sent at once) */
struct Panel {
    gboolean live;
    GtkWidget *root, *scroller, *list, *empty, *exceed;
    GPtrArray *rows;        /* Row* */
    char **only;            /* show these outputs alone; NULL = all */
    int building;           /* > 0: widgets are being set, their signals mean nothing */
    void (*changed) (void); /* a row changed */
    /* dialog: what the user chose and has not applied, output name ->
     * WalpeMonres. Rows come and go with the engine's monitor list (an
     * engine that restarts has none for a moment); the choice stays. */
    GHashTable *staged;
};

typedef struct {
    WalpeTexLimit limit;
    gboolean mixed;         /* the "Different per monitor" entry */
} TexItem;

static struct {
    WalpeResUiHost host;
    gboolean ready;
    gboolean heard;             /* a status arrived, or the word that there is no engine */
    gboolean engine;            /* the last status came from a running engine */
    WalpeMonresTracker *tracker;
    WalpeMonresState *st;       /* what the windows show; never NULL while ready */
    char *status_sig;           /* signature of the last status */
    gboolean list_inflight, list_again;
    guint gen;                  /* counts the changes sent: a listing asked for before
                                 * the last of them is out of date when it arrives */
    gboolean asked_this_run;    /* the start-up question was put */
    gboolean startup_pending;   /* ... is due: monitors nobody was asked about at start-up */
    guint announced;            /* monitors named by the notification that is up */
    /* the dialog */
    GtkWidget *dlg;
    guint dlg_serial;           /* which dialog a reply belongs to */
    Panel *dlg_panel;
    GtkWidget *dlg_heading, *dlg_noask, *dlg_err, *dlg_apply, *dlg_keep, *dlg_close;
    gboolean dlg_busy;          /* "Apply" was sent and not answered */
    gboolean dlg_noask_touched;
    int dlg_building;
    /* the rows in Engine settings */
    GtkWidget *set_root;
    Panel *set_panel;
    GtkWidget *prompt_sw, *filter_row, *filter_dd, *tex_row, *tex_dd, *tex_sub;
    GtkWidget *video_row, *video_dd, *video_sub;
    GArray *tex_items;          /* TexItem per entry of tex_dd */
    GArray *video_items;        /* WalpeVideoLimit per entry of video_dd */
    char *tex_sig, *video_sig;
    int set_building;
} R;

typedef enum {
    SENT_LIST,      /* the listing was asked for */
    SENT_LIVE,      /* one row of the settings panel */
    SENT_APPLY,     /* the dialog's buttons */
    SENT_OTHER,     /* a single setting */
} SentKind;

typedef struct {
    SentKind kind;
    guint serial;       /* SENT_APPLY: R.dlg_serial when it was sent */
    guint gen;          /* R.gen when it was sent */
    char *name;         /* SENT_LIVE: the row's output */
    const char *what;   /* for the status bar: "<what> applied." */
} Sent;

static void panel_fill (Panel *p);
static void dialog_update (void);
static void settings_fill (void);
static void request_list (void);

static void say (const char *msg, gboolean error)
{
    if (R.host.status && msg && *msg)
        R.host.status (msg, error);
}

static const WalpeMonres native_value = { WALPE_MONRES_NATIVE, 0, 0, 0 };

/* ================================================================ requests */

static void distribute (void)
{
    if (R.dlg_panel) {
        panel_fill (R.dlg_panel);
        dialog_update ();
    }
    if (R.set_panel) {
        panel_fill (R.set_panel);
        settings_fill ();
    }
}

static gboolean shown_somewhere (void)
{
    return R.dlg != NULL || (R.set_root && gtk_widget_get_mapped (R.set_root));
}

/* a listing arrived (asked for, or as the answer to a change): it is what
 * the windows show from now on */
static gboolean take_listing (JsonObject *reply)
{
    if (!reply || !json_object_has_member (reply, "monitors"))
        return FALSE;
    WalpeMonresState *st = walpe_monres_state_new (reply);
    if (!st->supported) {
        walpe_monres_state_free (st);
        return FALSE;
    }
    /* (the listing does not say whether a fullscreen application has the
     * focus; the status does, and only the announcement needs it) */
    st->fullscreen = R.st ? R.st->fullscreen : FALSE;
    walpe_monres_state_free (R.st);
    R.st = st;
    distribute ();
    return TRUE;
}

static Row *panel_row (Panel *p, const char *name)
{
    for (guint i = 0; p && name && i < p->rows->len; i++) {
        Row *r = p->rows->pdata[i];
        if (!strcmp (r->name, name))
            return r;
    }
    return NULL;
}

static void dialog_sent_done (gboolean ok, const char *msg);

static void on_reply (JsonObject *reply, const char *err, gpointer user)
{
    Sent *s = user;
    gboolean ok = ui_ipc_ok (reply);
    char *msg = NULL;
    if (!R.ready)
        goto out;
    if (s->kind == SENT_LIST) {
        R.list_inflight = FALSE;
        if (ok && s->gen == R.gen)
            take_listing (reply);
        else if (ok)
            R.list_again = TRUE;    /* a change went out meanwhile: ask again */
        else
            /* not answered (an engine that is loading, or restarting itself
             * for a new monitor): the status line that asked for this
             * listing is forgotten, so the next one asks again. Without
             * this an open dialog kept the old list of monitors until
             * something else in the status changed. */
            g_clear_pointer (&R.status_sig, g_free);
        if (R.list_again) {
            R.list_again = FALSE;
            request_list ();
        }
        goto out;
    }
    if (s->kind == SENT_LIVE) {
        Row *r = panel_row (R.set_panel, s->name);
        if (r && r->inflight > 0)
            r->inflight--;
    }
    if (ok) {
        char *note = walpe_ui_text (wpj_str (reply, "note", ""), 300, WALPE_UITEXT_LINE);
        msg = *note ? g_strdup_printf ("%s applied. %s", s->what, note)
                    : g_strdup_printf ("%s applied.", s->what);
        g_free (note);
        say (msg, FALSE);
        if (!take_listing (reply))
            request_list ();
    } else {
        /* the engine's sentence names the value it refused */
        char *why = walpe_ui_text (reply ? wpj_str (reply, "error", "rejected") : err, 300,
                                   WALPE_UITEXT_LINE);
        msg = g_strdup_printf ("%s not applied: %s", s->what, *why ? why : "error");
        g_free (why);
        say (msg, TRUE);
        /* the controls show what is in force again */
        distribute ();
        request_list ();
    }
    if (s->kind == SENT_APPLY && R.dlg && s->serial == R.dlg_serial)
        dialog_sent_done (ok, msg);
out:
    g_free (msg);
    g_free (s->name);
    g_free (s);
}

/* json is consumed; NULL = nothing to send. (Not named send: that is the
 * C library's, and a header that declares it would end this file's build.) */
static void post (char *json, SentKind kind, const char *name, const char *what, guint flags)
{
    if (!json || !R.ready) {
        g_free (json);
        return;
    }
    if (kind != SENT_LIST)
        R.gen++;
    Sent *s = g_new0 (Sent, 1);
    s->kind = kind;
    s->serial = R.dlg_serial;
    s->gen = R.gen;
    s->name = g_strdup (name);
    s->what = what;
    ui_ipc_send (json, flags, on_reply, s);
    g_free (json);
}

/* ask for the listing; one request at a time, and one more when something
 * changed while it was on its way */
static void request_list (void)
{
    if (!R.ready)
        return;
    if (R.list_inflight) {
        R.list_again = TRUE;
        return;
    }
    R.list_inflight = TRUE;
    post (walpe_monres_request_list (), SENT_LIST, NULL, NULL, UI_IPC_POLL);
}

/* ================================================================ one row */

static void row_free (gpointer p)
{
    Row *r = p;
    g_free (r->name);
    g_free (r->opt_sig);
    if (r->options)
        g_ptr_array_unref (r->options);
    g_free (r);
}

static const WalpeMonresMon *row_mon (Row *r)
{
    return R.st ? walpe_monres_state_find (R.st, r->name) : NULL;
}

static gboolean row_is_custom (Row *r)
{
    return r->value.kind == WALPE_MONRES_CUSTOM;
}

/* dialog: the row's choice is remembered under the monitor's name */
static void row_stage (Row *r)
{
    Panel *p = r->panel;
    if (p->live || !r->touched || !r->valid)
        return;
    g_hash_table_replace (p->staged, g_strdup (r->name), g_memdup2 (&r->value, sizeof (r->value)));
}

/* the line under a row's name: what the choice costs, or what is wrong
 * with the typed size. Also what a screen reader says about the controls. */
static void row_note (Row *r, const char *text, gboolean error)
{
    gboolean has = text && *text;
    gtk_label_set_text (GTK_LABEL (r->note), has ? text : "");
    gtk_widget_set_visible (r->note, has);
    if (error)
        gtk_widget_add_css_class (r->note, "error");
    else
        gtk_widget_remove_css_class (r->note, "error");
    GtkWidget *ws[] = { r->dd, r->w_entry, r->h_entry };
    for (guint i = 0; i < G_N_ELEMENTS (ws); i++)
        gtk_accessible_update_property (GTK_ACCESSIBLE (ws[i]), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                        has ? text : "", -1);
    for (guint i = 1; i < G_N_ELEMENTS (ws); i++)
        gtk_accessible_update_state (GTK_ACCESSIBLE (ws[i]), GTK_ACCESSIBLE_STATE_INVALID,
                                     error ? GTK_ACCESSIBLE_INVALID_TRUE : GTK_ACCESSIBLE_INVALID_FALSE,
                                     -1);
}

/* r->valid and the note line, from the row's value (and, for a custom
 * size, from what the two fields hold) */
static void row_validate (Row *r)
{
    const WalpeMonresMon *m = row_mon (r);
    if (!row_is_custom (r)) {
        int w = 0, h = 0;
        char *note = NULL;
        r->valid = TRUE;
        if (m && m->choice_raw && !r->touched) {
            char *raw = walpe_ui_text (m->choice_raw, 40, WALPE_UITEXT_LINE);
            note = g_strdup_printf ("The stored value “%s” is not one this version knows; the "
                                    "monitor's own size is shown.", raw);
            g_free (raw);
        } else if (r->value.kind != WALPE_MONRES_NATIVE && walpe_monres_mon_size (m, &r->value, &w, &h)) {
            note = walpe_monres_cost_note (m, w, h);
        }
        row_note (r, note, FALSE);
        g_free (note);
        return;
    }
    int w = 0, h = 0;
    char *msg = NULL;
    WalpeMonresCheck c = walpe_monres_check_custom (R.st, m,
                                                    gtk_editable_get_text (GTK_EDITABLE (r->w_entry)),
                                                    gtk_editable_get_text (GTK_EDITABLE (r->h_entry)),
                                                    &w, &h, &msg);
    r->valid = c != WALPE_MONRES_CHECK_BAD;
    if (r->valid) {
        r->value.w = w;
        r->value.h = h;
        if (c == WALPE_MONRES_CHECK_OK)
            msg = walpe_monres_cost_note (m, w, h);
    }
    row_note (r, msg, !r->valid);
    g_free (msg);
}

/* the controls from r->value */
static void row_show_value (Row *r)
{
    Panel *p = r->panel;
    int idx = walpe_monres_options_find (r->options, &r->value);
    if (idx < 0) {
        r->value = native_value;
        idx = 0;
    }
    p->building++;
    gtk_drop_down_set_selected (GTK_DROP_DOWN (r->dd), (guint) idx);
    gtk_widget_set_visible (r->custom, row_is_custom (r));
    if (row_is_custom (r)) {
        char wb[16] = "", hb[16] = "";
        if (r->value.w > 0)
            g_snprintf (wb, sizeof (wb), "%d", r->value.w);
        if (r->value.h > 0)
            g_snprintf (hb, sizeof (hb), "%d", r->value.h);
        gtk_editable_set_text (GTK_EDITABLE (r->w_entry), wb);
        gtk_editable_set_text (GTK_EDITABLE (r->h_entry), hb);
    }
    p->building--;
    row_validate (r);
}

/* settings: the row's value goes to the engine */
static void row_send (Row *r)
{
    const WalpeMonresMon *m = row_mon (r);
    r->touched = FALSE;
    if (!m || !r->valid)
        return;
    if (m->knows && !m->choice_raw && m->asked && walpe_monres_equal (&m->choice, &r->value))
        return;                 /* it is what the engine has */
    WalpeMonresAssign a = { .name = r->name, .set_value = TRUE, .value = r->value, .asked = -1 };
    r->inflight++;
    post (walpe_monres_request_apply (&a, 1), SENT_LIVE, r->name, "Render resolution",
          UI_IPC_AUTOSTART | UI_IPC_SLOW);
}

static void on_row_selected (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps;
    Row *r = user;
    Panel *p = r->panel;
    if (p->building)
        return;
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (o));
    if (!r->options || i >= r->options->len)
        return;
    const WalpeMonresOption *opt = r->options->pdata[i];
    if (opt->value.kind == WALPE_MONRES_CUSTOM) {
        if (!row_is_custom (r)) {
            /* the fields start with the size that was selected: the
             * monitor's own, the first time */
            int w = 0, h = 0;
            walpe_monres_mon_size (row_mon (r), &r->value, &w, &h);
            r->value.kind = WALPE_MONRES_CUSTOM;
            r->value.lines = 0;
            r->value.w = w;
            r->value.h = h;
        }
        r->touched = TRUE;      /* settings: sent when the fields are confirmed */
        row_show_value (r);
        gtk_widget_grab_focus (r->w_entry);
    } else {
        r->value = opt->value;
        r->touched = !p->live;
        row_show_value (r);
        if (p->live)
            row_send (r);
    }
    row_stage (r);
    if (p->changed)
        p->changed ();
}

static void on_row_typed (GtkEditable *e, gpointer user)
{
    (void) e;
    Row *r = user;
    if (r->panel->building)
        return;
    r->touched = TRUE;
    row_validate (r);
    row_stage (r);
    if (r->panel->changed)
        r->panel->changed ();
}

/* settings: Enter in a field, or the field is left */
static void row_commit (Row *r)
{
    Panel *p = r->panel;
    if (p->building || !p->live || !r->touched || !row_is_custom (r) || !r->valid)
        return;
    row_send (r);
}

static void on_row_activate (GtkEntry *e, gpointer user)
{
    (void) e;
    row_commit (user);
}

static void on_row_leave (GtkEventControllerFocus *fc, gpointer user)
{
    (void) fc;
    row_commit (user);
}

static GtkWidget *dim_label (const char *text, gboolean wrap)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    if (wrap) {
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_label_set_width_chars (GTK_LABEL (l), 34);
        gtk_label_set_max_width_chars (GTK_LABEL (l), 60);
    } else {
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
    }
    gtk_widget_add_css_class (l, "walpe-settings-sub");
    return l;
}

static GtkWidget *size_entry (Row *r)
{
    GtkWidget *e = gtk_entry_new ();
    gtk_entry_set_input_purpose (GTK_ENTRY (e), GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_max_length (GTK_ENTRY (e), 5);
    gtk_editable_set_width_chars (GTK_EDITABLE (e), 6);
    gtk_editable_set_max_width_chars (GTK_EDITABLE (e), 6);
    gtk_entry_set_alignment (GTK_ENTRY (e), 1);
    /* the dialog: Enter in a field is the default button ("Apply") */
    gtk_entry_set_activates_default (GTK_ENTRY (e), !r->panel->live);
    g_signal_connect (e, "changed", G_CALLBACK (on_row_typed), r);
    if (r->panel->live)
        g_signal_connect (e, "activate", G_CALLBACK (on_row_activate), r);
    return e;
}

static Row *row_new (Panel *p, const WalpeMonresMon *m)
{
    Row *r = g_new0 (Row, 1);
    r->panel = p;
    r->name = g_strdup (m->name);
    r->valid = TRUE;
    r->value = m->knows && !m->choice_raw ? m->choice : native_value;

    r->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class (r->box, "walpe-res-row");
    GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (left, TRUE);
    gtk_widget_set_valign (left, GTK_ALIGN_CENTER);
    r->title = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (r->title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (r->title), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (left), r->title);
    r->detail = dim_label ("", FALSE);
    gtk_box_append (GTK_BOX (left), r->detail);
    r->note = dim_label ("", TRUE);
    gtk_widget_set_visible (r->note, FALSE);
    gtk_box_append (GTK_BOX (left), r->note);
    gtk_box_append (GTK_BOX (r->box), left);

    GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign (right, GTK_ALIGN_CENTER);
    r->dd = gtk_drop_down_new (NULL, NULL);
    gtk_widget_set_size_request (r->dd, 250, -1);
    gtk_widget_set_halign (r->dd, GTK_ALIGN_END);
    g_signal_connect (r->dd, "notify::selected", G_CALLBACK (on_row_selected), r);
    gtk_box_append (GTK_BOX (right), r->dd);
    r->custom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign (r->custom, GTK_ALIGN_END);
    r->w_entry = size_entry (r);
    r->h_entry = size_entry (r);
    gtk_box_append (GTK_BOX (r->custom), r->w_entry);
    gtk_box_append (GTK_BOX (r->custom), gtk_label_new ("×"));
    gtk_box_append (GTK_BOX (r->custom), r->h_entry);
    gtk_box_append (GTK_BOX (r->custom), gtk_label_new ("pixels"));
    if (p->live) {
        /* settings: the typed size is sent when the focus leaves the two
         * fields. One controller on their box, not one per field: Tab from
         * the width to the height is not "left" (a controller on each field
         * sent the new width with the old height on the way - a size nobody
         * asked for, applied, and its scenes loaded for it). */
        GtkEventController *fc = gtk_event_controller_focus_new ();
        g_signal_connect (fc, "leave", G_CALLBACK (on_row_leave), r);
        gtk_widget_add_controller (r->custom, fc);
    }
    gtk_widget_set_visible (r->custom, FALSE);
    gtk_box_append (GTK_BOX (right), r->custom);
    gtk_box_append (GTK_BOX (r->box), right);
    return r;
}

/* labels, the list of choices and - unless the user is at it - the value,
 * from the engine's state */
static void row_fill (Row *r, const WalpeMonresMon *m)
{
    Panel *p = r->panel;
    char *title = walpe_monres_mon_title (m);
    char *detail = walpe_monres_mon_detail (m);
    if (!m->asked) {
        char *t = g_strdup_printf ("%s  ·  not chosen yet", title);
        g_free (title);
        title = t;
    }
    if (p->live && m->render_w > 0 && (m->render_w != m->mode_w || m->render_h != m->mode_h)) {
        char *d = g_strdup_printf ("%s%srendered at %d × %d now", detail, *detail ? " · " : "",
                                   m->render_w, m->render_h);
        g_free (detail);
        detail = d;
    }
    gtk_label_set_text (GTK_LABEL (r->title), title);
    gtk_label_set_text (GTK_LABEL (r->detail), detail);
    gtk_widget_set_visible (r->detail, *detail != 0);

    /* what each control is, for a screen reader: the visible labels sit
     * in another column */
    char *plain = walpe_monres_mon_title (m);
    char *a = g_strdup_printf ("Render resolution of monitor %s", plain);
    walpe_a11y_name (r->dd, a, r->title);   /* a choice: named by the row's title label */
    g_free (a);
    a = g_strdup_printf ("Width in pixels, monitor %s", plain);
    gtk_accessible_update_property (GTK_ACCESSIBLE (r->w_entry), GTK_ACCESSIBLE_PROPERTY_LABEL, a, -1);
    g_free (a);
    a = g_strdup_printf ("Height in pixels, monitor %s", plain);
    gtk_accessible_update_property (GTK_ACCESSIBLE (r->h_entry), GTK_ACCESSIBLE_PROPERTY_LABEL, a, -1);
    g_free (a);
    g_free (plain);
    g_free (title);
    g_free (detail);

    if (!r->touched && !r->inflight)
        r->value = m->knows && !m->choice_raw ? m->choice : native_value;

    /* a preset larger than the monitor that the row stands for stays in
     * its list whatever the check box says */
    GPtrArray *opts = walpe_monres_options (m, gtk_check_button_get_active (GTK_CHECK_BUTTON (p->exceed)));
    if (r->value.kind == WALPE_MONRES_LINES && walpe_monres_options_find (opts, &r->value) < 0) {
        g_ptr_array_unref (opts);
        opts = walpe_monres_options (m, TRUE);
    }
    GString *sig = g_string_new (NULL);
    for (guint i = 0; i < opts->len; i++)
        g_string_append_printf (sig, "%s\n", ((WalpeMonresOption *) opts->pdata[i])->label);
    if (g_strcmp0 (sig->str, r->opt_sig)) {
        GtkStringList *sl = gtk_string_list_new (NULL);
        for (guint i = 0; i < opts->len; i++)
            gtk_string_list_append (sl, ((WalpeMonresOption *) opts->pdata[i])->label);
        if (r->options)
            g_ptr_array_unref (r->options);
        r->options = opts;
        g_free (r->opt_sig);
        r->opt_sig = g_string_free (sig, FALSE);
        gtk_drop_down_set_model (GTK_DROP_DOWN (r->dd), G_LIST_MODEL (sl));
        g_object_unref (sl);
    } else {
        /* the same labels can stand for other sizes (the mode changed and
         * changed back): the list is the new one all the same */
        if (r->options)
            g_ptr_array_unref (r->options);
        r->options = opts;
        g_string_free (sig, TRUE);
    }
    /* a field the user is typing in is not written over */
    if (row_is_custom (r) && r->touched && gtk_widget_get_visible (r->custom))
        row_validate (r);
    else
        row_show_value (r);
}

/* ================================================================ a panel */

static gboolean panel_shows (Panel *p, const char *name)
{
    /* a monitor without a name cannot be named in a request, and one whose
     * name the engine does not take makes it refuse the whole request:
     * "Apply" would fail for every monitor in the list */
    if (!walpe_monres_name_ok (name))
        return FALSE;
    return !p->only || !p->only[0] || g_strv_contains ((const char *const *) p->only, name);
}

static void panel_fill (Panel *p)
{
    const WalpeMonresState *st = R.st;
    GPtrArray *show = g_ptr_array_new ();
    guint unusable = 0;         /* monitors that cannot be listed: see panel_shows */
    for (guint i = 0; st && st->supported && i < st->mons->len; i++) {
        WalpeMonresMon *m = st->mons->pdata[i];
        if (panel_shows (p, m->name))
            g_ptr_array_add (show, m);
        else if (!walpe_monres_name_ok (m->name) && !(p->only && p->only[0]))
            unusable++;
    }
    gboolean same = show->len == p->rows->len;
    for (guint i = 0; same && i < show->len; i++)
        same = !strcmp (((Row *) p->rows->pdata[i])->name, ((WalpeMonresMon *) show->pdata[i])->name);

    p->building++;
    if (!same) {
        /* another set of monitors: the rows are made again. What the user
         * had chosen for a monitor that is still there stays chosen. */
        GPtrArray *old = p->rows;
        p->rows = g_ptr_array_new_with_free_func (row_free);
        for (guint i = 0; i < old->len; i++)
            gtk_box_remove (GTK_BOX (p->list), ((Row *) old->pdata[i])->box);
        for (guint i = 0; i < show->len; i++) {
            const WalpeMonresMon *m = show->pdata[i];
            Row *r = row_new (p, m);
            const WalpeMonres *kept = g_hash_table_lookup (p->staged, r->name);
            if (kept) {
                r->value = *kept;
                r->touched = TRUE;
            }
            g_ptr_array_add (p->rows, r);
            gtk_box_append (GTK_BOX (p->list), r->box);
        }
        g_ptr_array_unref (old);
    }
    for (guint i = 0; i < show->len; i++)
        row_fill (p->rows->pdata[i], show->pdata[i]);
    p->building--;

    static const char unusable_text[] =
        "Not listed: a monitor whose output name cannot be stored (longer than 64 bytes, or not "
        "plain text). It is rendered at its own resolution.";
    const char *why = NULL;
    if (!show->len) {
        if (!R.heard)
            why = "Asking the engine for its monitors…";
        else if (!R.engine)
            why = "The engine is not running: its monitors show here once it is.";
        else if (!st || !st->supported)
            why = "The running engine does not report render resolutions. It is an older build: "
                  "restart the engine to use this.";
        else if (p->only && p->only[0])
            why = "That monitor is not connected any more.";
        else if (unusable)
            why = unusable_text;
        else
            why = "The engine reports no monitor.";
    } else if (unusable) {
        why = unusable_text;
    }
    gtk_label_set_text (GTK_LABEL (p->empty), why ? why : "");
    gtk_widget_set_visible (p->empty, why != NULL);
    gtk_widget_set_visible (p->scroller ? p->scroller : p->list, show->len > 0);
    gtk_widget_set_visible (p->exceed, show->len > 0);
    g_ptr_array_unref (show);
    if (p->changed)
        p->changed ();
}

static void on_exceed_toggled (GtkCheckButton *b, gpointer user)
{
    (void) b;
    Panel *p = user;
    if (!p->building)
        panel_fill (p);
}

/* the panel's widgets are going: nothing looks at it any more, and what
 * its rows still signal while they are taken apart means nothing */
static void on_panel_destroy (GtkWidget *w, gpointer user)
{
    (void) w;
    Panel *p = user;
    if (R.dlg_panel == p)
        R.dlg_panel = NULL;
    if (R.set_panel == p)
        R.set_panel = NULL;
    p->building++;
    p->changed = NULL;
}

/* with the root widget's last reference: every row widget is gone by then */
static void panel_free (gpointer user)
{
    Panel *p = user;
    g_ptr_array_unref (p->rows);
    g_hash_table_unref (p->staged);
    g_strfreev (p->only);
    g_free (p);
}

/* max_height > 0: the rows scroll in a box of at most that height (the
 * dialog); 0: they are as tall as they are (the settings window scrolls) */
static Panel *panel_new (gboolean live, int max_height)
{
    Panel *p = g_new0 (Panel, 1);
    p->live = live;
    p->rows = g_ptr_array_new_with_free_func (row_free);
    p->staged = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    p->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    p->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    if (max_height > 0) {
        p->scroller = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (p->scroller), GTK_POLICY_NEVER,
                                        GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (p->scroller), TRUE);
        gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (p->scroller), max_height);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (p->scroller), p->list);
        gtk_widget_set_vexpand (p->scroller, TRUE);
        gtk_accessible_update_property (GTK_ACCESSIBLE (p->scroller), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        "Monitors", -1);
        gtk_box_append (GTK_BOX (p->root), p->scroller);
    } else {
        gtk_box_append (GTK_BOX (p->root), p->list);
    }
    p->empty = dim_label ("", TRUE);
    gtk_widget_set_visible (p->empty, FALSE);
    gtk_box_append (GTK_BOX (p->root), p->empty);
    p->exceed = gtk_check_button_new_with_mnemonic ("_Offer sizes larger than the monitor");
    gtk_widget_set_tooltip_text (p->exceed,
                                 "Rendering more pixels than the monitor has and scaling down smooths "
                                 "edges; GPU time and memory grow with the number of pixels.");
    g_signal_connect (p->exceed, "toggled", G_CALLBACK (on_exceed_toggled), p);
    gtk_box_append (GTK_BOX (p->root), p->exceed);
    g_signal_connect (p->root, "destroy", G_CALLBACK (on_panel_destroy), p);
    g_object_set_data_full (G_OBJECT (p->root), "walpe-res-panel", p, panel_free);
    return p;
}

/* ================================================================ the dialog */

static void dialog_update (void)
{
    if (!R.dlg || !R.dlg_panel)
        return;
    Panel *p = R.dlg_panel;
    gboolean ok = R.st && R.st->supported && p->rows->len > 0, valid = TRUE;
    guint unasked = 0;
    for (guint i = 0; i < p->rows->len; i++) {
        Row *r = p->rows->pdata[i];
        const WalpeMonresMon *m = row_mon (r);
        valid = valid && r->valid;
        if (m && !m->asked)
            unasked++;
    }
    gtk_widget_set_sensitive (R.dlg_apply, ok && valid && !R.dlg_busy);
    gtk_widget_set_sensitive (R.dlg_keep, ok && !R.dlg_busy);
    gtk_button_set_label (GTK_BUTTON (R.dlg_close), unasked ? "Not _now" : "_Close");
    gtk_label_set_text (GTK_LABEL (R.dlg_heading),
                        !unasked ? "Render resolution per monitor"
                        : p->rows->len > 1 ? "Choose a render resolution for each monitor"
                                           : "Choose a render resolution for this monitor");
    gtk_widget_set_sensitive (R.dlg_noask, R.st && R.st->supported);
    if (!R.dlg_noask_touched && R.st) {
        R.dlg_building++;
        gtk_check_button_set_active (GTK_CHECK_BUTTON (R.dlg_noask), !R.st->prompt);
        R.dlg_building--;
    }
}

static void on_dlg_noask (GtkCheckButton *b, gpointer user)
{
    (void) b; (void) user;
    if (!R.dlg_building)
        R.dlg_noask_touched = TRUE;
}

/* "Apply" (the rows as they are) or "Keep native for all" */
static void dialog_send (gboolean all_native)
{
    Panel *p = R.dlg_panel;
    if (!R.dlg || !p || R.dlg_busy || !p->rows->len)
        return;
    WalpeMonresAssign *a = g_new0 (WalpeMonresAssign, p->rows->len);
    for (guint i = 0; i < p->rows->len; i++) {
        Row *r = p->rows->pdata[i];
        a[i].name = r->name;
        a[i].set_value = TRUE;
        a[i].value = all_native ? native_value : r->value;
        a[i].asked = -1;        /* a monitor that gets a value counts as asked */
    }
    char *req = walpe_monres_request_apply (a, p->rows->len);
    g_free (a);
    if (!req)
        return;
    R.dlg_busy = TRUE;
    gtk_label_set_text (GTK_LABEL (R.dlg_err), "");
    dialog_update ();
    /* scenes on a monitor that gets another size load again: that can take
     * a moment, and a daemon that is not running is started for it */
    post (req, SENT_APPLY, NULL, "Render resolution", UI_IPC_AUTOSTART | UI_IPC_SLOW);
}

static void dialog_sent_done (gboolean ok, const char *msg)
{
    R.dlg_busy = FALSE;
    if (!ok) {
        gtk_label_set_text (GTK_LABEL (R.dlg_err), msg);
        dialog_update ();
        return;
    }
    /* the check box is part of what the two buttons apply */
    gboolean noask = gtk_check_button_get_active (GTK_CHECK_BUTTON (R.dlg_noask));
    if (R.dlg_noask_touched && R.st && noask == R.st->prompt)
        post (walpe_monres_request_prompt (!noask), SENT_OTHER, NULL, "Asking about new monitors",
              UI_IPC_AUTOSTART);
    gtk_window_destroy (GTK_WINDOW (R.dlg));
}

static void on_dlg_apply (GtkButton *b, gpointer user)
{
    (void) b; (void) user;
    dialog_send (FALSE);
}

static void on_dlg_keep (GtkButton *b, gpointer user)
{
    (void) b; (void) user;
    dialog_send (TRUE);
}

static void on_dlg_close (GtkButton *b, gpointer user)
{
    (void) b; (void) user;
    if (R.dlg)
        gtk_window_close (GTK_WINDOW (R.dlg));
}

static gboolean on_dlg_escape (GtkWidget *w, GVariant *args, gpointer user)
{
    (void) args; (void) user;
    gtk_window_close (GTK_WINDOW (w));
    return TRUE;
}

static void on_dlg_destroy (GtkWidget *w, gpointer user)
{
    (void) user;
    if (R.dlg != w)
        return;
    R.dlg = NULL;
    R.dlg_busy = FALSE;
    R.dlg_heading = R.dlg_noask = R.dlg_err = R.dlg_apply = R.dlg_keep = R.dlg_close = NULL;
}

/* how tall the list of monitors may get on the screen the dialog opens
 * on, so that the whole dialog fits a 1280 x 720 monitor */
static int list_height_cap (void)
{
    int h = 0;
    GdkDisplay *d = gdk_display_get_default ();
    GdkSurface *s = R.host.parent ? gtk_native_get_surface (GTK_NATIVE (R.host.parent)) : NULL;
    GdkMonitor *mon = d && s ? gdk_display_get_monitor_at_surface (d, s) : NULL;
    if (mon)
        g_object_ref (mon);
    else if (d && g_list_model_get_n_items (gdk_display_get_monitors (d)) > 0)
        mon = g_list_model_get_item (gdk_display_get_monitors (d), 0);
    if (mon) {
        GdkRectangle g;
        gdk_monitor_get_geometry (mon, &g);
        h = g.height;
        g_object_unref (mon);
    }
    return CLAMP (h > 0 ? h - DIALOG_CHROME : LIST_MAX_HEIGHT - 60, LIST_MIN_HEIGHT, LIST_MAX_HEIGHT);
}

static void dialog_open (const char *const *only)
{
    if (!R.ready)
        return;
    if (R.dlg) {
        /* open already: it shows what was asked for now */
        g_strfreev (R.dlg_panel->only);
        R.dlg_panel->only = only && only[0] ? g_strdupv ((char **) only) : NULL;
        panel_fill (R.dlg_panel);
        dialog_update ();
        gtk_window_present (GTK_WINDOW (R.dlg));
        request_list ();
        return;
    }
    GtkWidget *w = gtk_window_new ();
    R.dlg = w;
    R.dlg_serial++;
    R.dlg_busy = FALSE;
    R.dlg_noask_touched = FALSE;
    gtk_window_set_title (GTK_WINDOW (w), "Monitor resolution");
    if (R.host.parent) {
        gtk_window_set_transient_for (GTK_WINDOW (w), R.host.parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (w), TRUE);
    }
    /* not modal: the library stays usable, and a slow engine never locks
     * the main window behind a question */
    gtk_window_set_default_size (GTK_WINDOW (w), 680, -1);
    GtkEventController *sc = gtk_shortcut_controller_new ();
    gtk_shortcut_controller_add_shortcut (
        GTK_SHORTCUT_CONTROLLER (sc),
        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                          gtk_callback_action_new (on_dlg_escape, NULL, NULL)));
    gtk_widget_add_controller (w, sc);
    g_signal_connect (w, "destroy", G_CALLBACK (on_dlg_destroy), NULL);

    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class (v, "walpe-settings");
    R.dlg_heading = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (R.dlg_heading), 0);
    gtk_label_set_wrap (GTK_LABEL (R.dlg_heading), TRUE);
    gtk_widget_add_css_class (R.dlg_heading, "title-3");
    gtk_box_append (GTK_BOX (v), R.dlg_heading);
    GtkWidget *intro = gtk_label_new (cost_line);
    gtk_label_set_xalign (GTK_LABEL (intro), 0);
    gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (intro), 80);
    gtk_box_append (GTK_BOX (v), intro);

    Panel *p = panel_new (FALSE, list_height_cap ());
    p->only = only && only[0] ? g_strdupv ((char **) only) : NULL;
    p->changed = dialog_update;
    R.dlg_panel = p;
    gtk_box_append (GTK_BOX (v), p->root);

    R.dlg_noask = gtk_check_button_new_with_mnemonic ("_Do not ask again when a new monitor is connected");
    gtk_widget_set_tooltip_text (R.dlg_noask,
                                 "New monitors are then rendered at their own resolution without a "
                                 "question. Engine settings has the same choice and this switch.");
    g_signal_connect (R.dlg_noask, "toggled", G_CALLBACK (on_dlg_noask), NULL);
    gtk_box_append (GTK_BOX (v), R.dlg_noask);

    R.dlg_err = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (R.dlg_err), 0);
    gtk_label_set_wrap (GTK_LABEL (R.dlg_err), TRUE);
    gtk_widget_add_css_class (R.dlg_err, "error");
    gtk_accessible_update_property (GTK_ACCESSIBLE (R.dlg_err), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Error", -1);
    gtk_box_append (GTK_BOX (v), R.dlg_err);

    GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (btns, GTK_ALIGN_END);
    gtk_widget_set_margin_top (btns, 4);
    R.dlg_close = gtk_button_new_with_mnemonic ("Not _now");
    gtk_widget_set_tooltip_text (R.dlg_close, "Changes nothing: every monitor keeps what it has");
    g_signal_connect (R.dlg_close, "clicked", G_CALLBACK (on_dlg_close), NULL);
    gtk_box_append (GTK_BOX (btns), R.dlg_close);
    R.dlg_keep = gtk_button_new_with_mnemonic ("_Keep native for all");
    gtk_widget_set_tooltip_text (R.dlg_keep,
                                 "Every monitor listed is rendered at its own resolution");
    g_signal_connect (R.dlg_keep, "clicked", G_CALLBACK (on_dlg_keep), NULL);
    gtk_box_append (GTK_BOX (btns), R.dlg_keep);
    R.dlg_apply = gtk_button_new_with_mnemonic ("_Apply");
    gtk_widget_add_css_class (R.dlg_apply, "suggested-action");
    g_signal_connect (R.dlg_apply, "clicked", G_CALLBACK (on_dlg_apply), NULL);
    gtk_box_append (GTK_BOX (btns), R.dlg_apply);
    gtk_box_append (GTK_BOX (v), btns);

    gtk_window_set_child (GTK_WINDOW (w), v);
    gtk_window_set_default_widget (GTK_WINDOW (w), R.dlg_apply);
    panel_fill (p);
    dialog_update ();
    gtk_window_present (GTK_WINDOW (w));
    request_list ();
}

/* ================================================================ settings */

static const char tex_text[] =
    "Layer images are kept in video memory at the size the render resolution needs. A number "
    "also caps their longer side; No limit keeps them as large as their files. Applies when a "
    "wallpaper is loaded.";
static const char video_text[] =
    "Videos are imported and converted to at most this size. Automatic: the largest render "
    "resolution among the monitors that show the video.";

/* title and hint on the left, the control on the right, as the other rows
 * of the settings window; one widget, so the row can be hidden */
static GtkWidget *set_row (const char *title, const char *sub, GtkWidget *ctl, GtkWidget **sub_out)
{
    GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (v, TRUE);
    GtkWidget *t = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (t), 0);
    gtk_label_set_wrap (GTK_LABEL (t), TRUE);
    gtk_label_set_width_chars (GTK_LABEL (t), 34);
    gtk_label_set_max_width_chars (GTK_LABEL (t), 60);
    gtk_box_append (GTK_BOX (v), t);
    GtkWidget *s = dim_label (sub, TRUE);
    gtk_box_append (GTK_BOX (v), s);
    gtk_widget_set_margin_top (v, 6);
    gtk_widget_set_margin_bottom (v, 6);
    gtk_box_append (GTK_BOX (h), v);
    gtk_widget_set_valign (ctl, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (ctl, GTK_ALIGN_END);
    walpe_a11y_name (ctl, title, t);
    gtk_box_append (GTK_BOX (h), ctl);
    if (sub_out)
        *sub_out = s;
    return h;
}

/* a drop-down's entries; set again only when they changed (an open list
 * is not closed by a status line that changes nothing) */
static void dd_set_items (GtkWidget *dd, GPtrArray *labels, char **sig_store)
{
    char *sig = g_strjoinv ("\n", (char **) labels->pdata);     /* labels ends with NULL */
    if (g_strcmp0 (sig, *sig_store)) {
        GtkStringList *sl = gtk_string_list_new ((const char *const *) labels->pdata);
        gtk_drop_down_set_model (GTK_DROP_DOWN (dd), G_LIST_MODEL (sl));
        g_object_unref (sl);
        g_free (*sig_store);
        *sig_store = sig;
    } else {
        g_free (sig);
    }
}

static void tex_fill (void)
{
    static const int sizes[] = { 1024, 2048, 4096, 8192, 16384 };
    gboolean any = FALSE;
    WalpeTexLimit common = { WALPE_LIMIT_AUTO, 0 };
    gboolean same = walpe_monres_state_texture (R.st, &common, &any);
    gtk_widget_set_visible (R.tex_row, any);
    if (!any)
        return;
    GPtrArray *labels = g_ptr_array_new_with_free_func (g_free);
    g_array_set_size (R.tex_items, 0);
    TexItem it = { { WALPE_LIMIT_AUTO, 0 }, FALSE };
    guint sel = 0;
    g_array_append_val (R.tex_items, it);
    g_ptr_array_add (labels, g_strdup ("Automatic"));
    gboolean placed = !(same && common.kind == WALPE_LIMIT_VALUE);
    for (guint i = 0; i <= G_N_ELEMENTS (sizes); i++) {
        /* a stored number that is none of the usual ones gets an entry of
         * its own, in its place */
        if (!placed && (i == G_N_ELEMENTS (sizes) || common.px <= sizes[i])) {
            placed = TRUE;
            if (i == G_N_ELEMENTS (sizes) || common.px != sizes[i]) {
                it.limit = common;
                g_array_append_val (R.tex_items, it);
                g_ptr_array_add (labels, g_strdup_printf ("%d pixels", common.px));
            }
        }
        if (i == G_N_ELEMENTS (sizes))
            break;
        it.limit.kind = WALPE_LIMIT_VALUE;
        it.limit.px = sizes[i];
        g_array_append_val (R.tex_items, it);
        g_ptr_array_add (labels, g_strdup_printf ("%d pixels", sizes[i]));
    }
    it.limit.kind = WALPE_LIMIT_NONE;
    it.limit.px = 0;
    g_array_append_val (R.tex_items, it);
    g_ptr_array_add (labels, g_strdup ("No limit"));
    if (!same) {
        it.mixed = TRUE;
        g_array_append_val (R.tex_items, it);
        g_ptr_array_add (labels, g_strdup ("Different per monitor"));
        sel = R.tex_items->len - 1;
    } else {
        for (guint i = 0; i < R.tex_items->len; i++)
            if (walpe_tex_limit_equal (&g_array_index (R.tex_items, TexItem, i).limit, &common))
                sel = i;
    }
    g_ptr_array_add (labels, NULL);
    R.set_building++;
    dd_set_items (R.tex_dd, labels, &R.tex_sig);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (R.tex_dd), sel);
    R.set_building--;
    g_ptr_array_unref (labels);

    int cap = 0;
    for (guint i = 0; i < R.st->mons->len; i++)
        cap = MAX (cap, ((WalpeMonresMon *) R.st->mons->pdata[i])->tex_now);
    char *t = cap > 0 ? g_strdup_printf ("%s\nIn force now: no texture longer than %d pixels.",
                                         tex_text, cap)
                      : g_strdup (tex_text);
    gtk_label_set_text (GTK_LABEL (R.tex_sub), t);
    g_free (t);
}

static void video_fill (void)
{
    const WalpeMonresState *st = R.st;
    gtk_widget_set_visible (R.video_row, st->video_known);
    if (!st->video_known)
        return;
    GPtrArray *labels = g_ptr_array_new_with_free_func (g_free);
    g_array_set_size (R.video_items, 0);
    WalpeVideoLimit v = { WALPE_LIMIT_AUTO, { WALPE_MONRES_NATIVE, 0, 0, 0 } };
    guint sel = 0;
    g_array_append_val (R.video_items, v);
    g_ptr_array_add (labels, g_strdup ("Automatic"));
    v.kind = WALPE_LIMIT_VALUE;
    v.size.kind = WALPE_MONRES_LINES;
    for (guint i = 0; i < WALPE_MONRES_N_PRESETS; i++) {
        const char *label = NULL;
        int w = 0, h = 0;
        v.size.lines = walpe_monres_preset (i, &label);
        walpe_monres_size (&v.size, 0, 0, &w, &h);      /* a preset is a 16:9 box here */
        g_array_append_val (R.video_items, v);
        g_ptr_array_add (labels, g_strdup_printf ("%s (%d × %d)", label, w, h));
    }
    if (st->video.kind == WALPE_LIMIT_VALUE && st->video.size.kind == WALPE_MONRES_CUSTOM) {
        g_array_append_val (R.video_items, st->video);
        g_ptr_array_add (labels, g_strdup_printf ("%d × %d", st->video.size.w, st->video.size.h));
    }
    v.kind = WALPE_LIMIT_NONE;
    v.size = native_value;
    g_array_append_val (R.video_items, v);
    g_ptr_array_add (labels, g_strdup ("No limit"));
    for (guint i = 0; i < R.video_items->len; i++) {
        const WalpeVideoLimit *e = &g_array_index (R.video_items, WalpeVideoLimit, i);
        if (e->kind == st->video.kind &&
            (e->kind != WALPE_LIMIT_VALUE || walpe_monres_equal (&e->size, &st->video.size)))
            sel = i;
    }
    g_ptr_array_add (labels, NULL);
    R.set_building++;
    dd_set_items (R.video_dd, labels, &R.video_sig);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (R.video_dd), sel);
    R.set_building--;
    g_ptr_array_unref (labels);

    char *t;
    if (!st->video_now_known)
        t = g_strdup (video_text);
    else if (st->video_now_w > 0 && st->video_now_h > 0)
        t = g_strdup_printf ("%s\nIn force now: at most %d × %d.", video_text, st->video_now_w,
                             st->video_now_h);
    else
        t = g_strdup_printf ("%s\nIn force now: no limit.", video_text);
    gtk_label_set_text (GTK_LABEL (R.video_sub), t);
    g_free (t);
}

static void settings_fill (void)
{
    if (!R.set_root || !R.st)
        return;
    const WalpeMonresState *st = R.st;
    R.set_building++;
    gtk_switch_set_active (GTK_SWITCH (R.prompt_sw), st->prompt);
    gtk_widget_set_sensitive (R.prompt_sw, st->supported);
    gtk_widget_set_visible (R.filter_row, st->scale_filter != NULL);
    if (st->scale_filter)
        gtk_drop_down_set_selected (GTK_DROP_DOWN (R.filter_dd),
                                    !strcmp (st->scale_filter, "nearest") ? 1 : 0);
    R.set_building--;
    tex_fill ();
    video_fill ();
}

static void on_prompt_switch (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps; (void) user;
    if (R.set_building)
        return;
    post (walpe_monres_request_prompt (gtk_switch_get_active (GTK_SWITCH (o))), SENT_OTHER, NULL,
          "Asking about new monitors", UI_IPC_AUTOSTART);
}

static void on_filter_selected (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps; (void) user;
    if (R.set_building)
        return;
    post (walpe_monres_request_filter (gtk_drop_down_get_selected (GTK_DROP_DOWN (o)) == 1
                                           ? "nearest" : "bilinear"),
          SENT_OTHER, NULL, "Scaling", UI_IPC_AUTOSTART);
}

/* one row for a value the engine keeps per monitor: the choice goes to
 * every connected monitor. Each keeps its resolution and whether it was
 * asked about. */
static void on_tex_selected (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps; (void) user;
    if (R.set_building || !R.st)
        return;
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (o));
    if (i >= R.tex_items->len)
        return;
    const TexItem *it = &g_array_index (R.tex_items, TexItem, i);
    if (it->mixed)
        return;
    guint n = R.st->mons->len;
    WalpeMonresAssign *a = g_new0 (WalpeMonresAssign, n);
    for (guint k = 0; k < n; k++) {
        const WalpeMonresMon *m = R.st->mons->pdata[k];
        /* (no name = left out: one name the engine does not take and it
         * refuses the request for every monitor) */
        a[k].name = walpe_monres_name_ok (m->name) ? m->name : NULL;
        a[k].set_value = m->knows && !m->choice_raw;
        a[k].value = m->choice;
        a[k].set_tex = TRUE;
        a[k].tex = it->limit;
        a[k].asked = m->asked ? 1 : 0;
    }
    post (walpe_monres_request_apply (a, n), SENT_OTHER, NULL, "Largest texture size",
          UI_IPC_AUTOSTART | UI_IPC_SLOW);
    g_free (a);
}

static void on_video_selected (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps; (void) user;
    if (R.set_building)
        return;
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (o));
    if (i >= R.video_items->len)
        return;
    post (walpe_monres_request_video (&g_array_index (R.video_items, WalpeVideoLimit, i)), SENT_OTHER,
          NULL, "Largest video size", UI_IPC_AUTOSTART);
}

/* shown: the listing is asked for (a status line only tells that
 * something changed) */
static void on_set_map (GtkWidget *w, gpointer user)
{
    (void) w; (void) user;
    request_list ();
}

static void on_set_destroy (GtkWidget *w, gpointer user)
{
    (void) w; (void) user;
    R.set_root = NULL;
    R.prompt_sw = R.filter_row = R.filter_dd = R.tex_row = R.tex_dd = R.tex_sub = NULL;
    R.video_row = R.video_dd = R.video_sub = NULL;
}

GtkWidget *walpe_res_ui_settings_new (void)
{
    if (R.set_root)
        return R.set_root;
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    R.set_root = v;
    GtkWidget *intro = dim_label (cost_line, TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (intro), 90);
    gtk_box_append (GTK_BOX (v), intro);
    GtkWidget *scale = dim_label ("The quality presets' render scale applies on top of the size chosen "
                                  "here.", TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (scale), 90);
    gtk_box_append (GTK_BOX (v), scale);

    Panel *p = panel_new (TRUE, 0);
    R.set_panel = p;
    gtk_widget_set_margin_top (p->root, 4);
    gtk_box_append (GTK_BOX (v), p->root);

    R.prompt_sw = gtk_switch_new ();
    g_signal_connect (R.prompt_sw, "notify::active", G_CALLBACK (on_prompt_switch), NULL);
    gtk_box_append (GTK_BOX (v), set_row ("Ask about new monitors",
                                          "A monitor that is connected for the first time is rendered at "
                                          "its own resolution; WalPE asks once which render resolution it "
                                          "should have.", R.prompt_sw, NULL));

    static const char *const filters[] = { "Smooth", "Sharp pixels", NULL };
    R.filter_dd = gtk_drop_down_new_from_strings (filters);
    g_signal_connect (R.filter_dd, "notify::selected", G_CALLBACK (on_filter_selected), NULL);
    R.filter_row = set_row ("Scaling to the screen",
                            "How a picture rendered at another size than the monitor's is scaled to it. "
                            "Sharp pixels gives exact pixel blocks at whole multiples and looks blocky "
                            "otherwise.", R.filter_dd, NULL);
    gtk_box_append (GTK_BOX (v), R.filter_row);

    R.tex_items = g_array_new (FALSE, TRUE, sizeof (TexItem));
    R.tex_dd = gtk_drop_down_new (NULL, NULL);
    g_signal_connect (R.tex_dd, "notify::selected", G_CALLBACK (on_tex_selected), NULL);
    R.tex_row = set_row ("Largest texture size", tex_text, R.tex_dd, &R.tex_sub);
    gtk_box_append (GTK_BOX (v), R.tex_row);

    R.video_items = g_array_new (FALSE, TRUE, sizeof (WalpeVideoLimit));
    R.video_dd = gtk_drop_down_new (NULL, NULL);
    g_signal_connect (R.video_dd, "notify::selected", G_CALLBACK (on_video_selected), NULL);
    R.video_row = set_row ("Largest video size", video_text, R.video_dd, &R.video_sub);
    gtk_box_append (GTK_BOX (v), R.video_row);

    g_signal_connect (v, "map", G_CALLBACK (on_set_map), NULL);
    g_signal_connect (v, "destroy", G_CALLBACK (on_set_destroy), NULL);
    if (R.st) {
        panel_fill (p);
        settings_fill ();
    }
    return v;
}

/* ================================================================ announcing */

static void announce (const WalpeMonresState *st, GPtrArray *names)
{
    /* (no session bus - a bare X session started without one - means no
     * notification service: GIO would log criticals about its missing
     * connection, here and again when the notification is taken back) */
    if (!R.host.app || !names->len ||
        !g_application_get_dbus_connection (G_APPLICATION (R.host.app))) {
        walpe_monres_tracker_unshown (R.tracker, names);
        return;
    }
    char *title = NULL, *body = NULL;
    walpe_monres_announce_text (st, names, &title, &body);
    GNotification *n = g_notification_new (title);
    g_notification_set_body (n, body);
    GVariant *target = g_variant_ref_sink (
        g_variant_new_strv ((const char *const *) names->pdata, (gssize) names->len));
    g_notification_set_default_action_and_target_value (n, "app." WALPE_RES_ACTION, target);
    g_notification_add_button_with_target_value (n, "Choose…", "app." WALPE_RES_ACTION, target);
    g_variant_unref (target);
    g_application_send_notification (G_APPLICATION (R.host.app), NOTIFY_ID, n);
    g_object_unref (n);
    R.announced += names->len;
    say (title, FALSE);
    g_free (title);
    g_free (body);
}

/* ================================================================ API */

void walpe_res_ui_init (const WalpeResUiHost *host)
{
    if (R.ready || !host)
        return;
    R.host = *host;
    R.tracker = walpe_monres_tracker_new ();
    R.st = walpe_monres_state_new (NULL);
    R.asked_this_run = FALSE;
    R.startup_pending = FALSE;
    R.heard = FALSE;
    R.ready = TRUE;
}

/* A status line tells about the monitors, not about the limits and the
 * video settings: until the listing that is asked for next arrives, those
 * stay as the last listing had them (their rows in the settings window do
 * not come and go). */
static void keep_globals (WalpeMonresState *st, const WalpeMonresState *listing)
{
    st->min_w = listing->min_w;
    st->min_h = listing->min_h;
    st->max_side = listing->max_side;
    st->max_pixels = listing->max_pixels;
    st->video_known = listing->video_known;
    st->video = listing->video;
    st->video_now_known = listing->video_now_known;
    st->video_now_w = listing->video_now_w;
    st->video_now_h = listing->video_now_h;
    if (!st->scale_filter)
        st->scale_filter = g_strdup (listing->scale_filter);
}

void walpe_res_ui_status (JsonObject *state)
{
    if (!R.ready)
        return;
    R.heard = TRUE;
    R.engine = state != NULL;
    WalpeMonresState *s = walpe_monres_state_new (state);
    guint startup = 0;
    GPtrArray *back = NULL;
    GPtrArray *news = walpe_monres_tracker_feed (R.tracker, s, &startup, &back);

    char *body = walpe_monres_state_signature (s);
    char *sig = g_strdup_printf ("%d|%s", R.engine, body);
    g_free (body);
    gboolean changed = g_strcmp0 (sig, R.status_sig) != 0;
    gboolean fs = s->fullscreen;
    gboolean supported = s->supported;
    if (changed) {
        g_free (R.status_sig);
        R.status_sig = sig;
        /* what the status tells is shown at once where no listing is
         * shown: nowhere at all, or there is none, or this engine has none */
        if (!shown_somewhere () || !R.st->full || !supported) {
            if (supported && R.st->supported)
                keep_globals (s, R.st);
            walpe_monres_state_free (R.st);
            R.st = s;
            s = NULL;
            distribute ();
        }
        if (supported && shown_somewhere ())
            request_list ();
    } else {
        g_free (sig);
    }
    R.st->fullscreen = fs;

    /* the question at start-up: once per run, when the window comes up
     * and monitors are there that nobody was asked about. Like the
     * announcement it waits while a fullscreen application has the focus
     * (the window may have been started long before this engine). */
    if (startup)
        R.startup_pending = TRUE;
    if (R.startup_pending && !R.asked_this_run && R.engine && !fs) {
        R.startup_pending = FALSE;
        if (walpe_monres_state_unasked (s ? s : R.st, NULL) > 0) {
            R.asked_this_run = TRUE;
            dialog_open (NULL);
        }
    }
    if (back) {
        R.announced = R.announced > back->len ? R.announced - back->len : 0;
        if (!R.announced && R.host.app)
            g_application_withdraw_notification (G_APPLICATION (R.host.app), NOTIFY_ID);
        g_ptr_array_unref (back);
    }
    if (news) {
        /* a dialog that lists every monitor shows the new one already. What
         * is not announced is not counted in R.announced either: the tracker
         * is told, so it does not ask to take it back later (that used to
         * withdraw the notification of another monitor that was still up). */
        if (R.dlg && R.dlg_panel && !R.dlg_panel->only)
            walpe_monres_tracker_unshown (R.tracker, news);
        else
            announce (s ? s : R.st, news);
        g_ptr_array_unref (news);
    }
    walpe_monres_state_free (s);
}

void walpe_res_ui_show_dialog (const char *const *only)
{
    R.asked_this_run = TRUE;    /* it is open: the start-up question is this */
    dialog_open (only);
}

void walpe_res_ui_shutdown (void)
{
    if (!R.ready)
        return;
    R.ready = FALSE;
    if (R.announced && R.host.app)
        g_application_withdraw_notification (G_APPLICATION (R.host.app), NOTIFY_ID);
    R.announced = 0;
    if (R.dlg)
        gtk_window_destroy (GTK_WINDOW (R.dlg));
    g_clear_pointer (&R.tracker, walpe_monres_tracker_free);
    g_clear_pointer (&R.st, walpe_monres_state_free);
    g_clear_pointer (&R.status_sig, g_free);
    R.host.parent = NULL;
    R.host.status = NULL;
}
