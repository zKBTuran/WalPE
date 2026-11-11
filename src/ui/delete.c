/* delete.c - "Delete wallpaper" from the browser.
 *
 *   check (worker thread)   common/library.c decides whether the entry may
 *                           be deleted and what goes with it: other copies
 *                           of the same workshop item, a video's thumbnail
 *   confirm                 modal dialog: every path, the Steam warning,
 *                           which monitors show it, presets based on it
 *   forget (IPC "forget")   the engine switches away from it and drops it
 *                           from config.json. No engine running: the GUI
 *                           edits config.json itself (nobody else writes it
 *                           then). Engine silent: nothing is deleted
 *   trash (worker thread)   g_file_trash per path, each validated again
 *   on failure              a dialog says why and offers "Delete
 *                           permanently" - never done without that click
 *
 * Steam subscription (version 0.12, common/steamsub.h): a copy in a Steam client
 * library comes back while the account is subscribed. The dialog then offers
 *   - "Also unsubscribe on Steam" when a Steam Web API key is saved: WalPE
 *     asks Steam before it deletes (IPublishedFileService/Unsubscribe); a
 *     refusal is shown after the delete, with a button to the Workshop page
 *   - "Open Workshop page": the item's page in the running Steam client, or
 *     in the web browser when Steam is not running, to unsubscribe by hand
 * A copy that only steamcmd downloaded gets neither: nothing subscribed it.
 *
 * Main-thread rule (main.c): the file work runs in GTask threads, the IPC
 * through uiipc. */
#include "delete.h"
#include "uiipc.h"
#include "common/library.h"
#include "common/steamsub.h"
#include "common/util.h"
#include "common/workshop.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    WalpeDeleteHost host;
    char *path, *title, *library_dir, *in_use;
    WalpeDelPlan *plan;
    gboolean *gone;         /* per plan target: removed */
    GtkWidget *dlg;         /* the open dialog (confirm or failure) */
    guint forget_i;
    gboolean forgot;        /* the engine / config no longer use it */
    gboolean permanent;     /* this pass deletes instead of trashing */
    gboolean touched;       /* a pass ran: the library changed (or may have) */
    /* result of the last pass (worker -> main) */
    guint n_failed;
    char *fail_msg;
    /* Steam subscription */
    gboolean steam_sub;     /* the item may be subscribed: the dialog says so */
    gboolean sub_by_walpe;  /* ... because WalPE subscribed it (steamsub marks) */
    gboolean can_direct;    /* a Web API key is saved: WalPE can ask Steam */
    gboolean unsub_default; /* the box as it was left the last time */
    GtkWidget *unsub_check;
    GtkLabel *page_msg;     /* what "Open Workshop page" did */
    int unsub;              /* 0 not asked, 1 Steam accepted, -1 failed */
    char *unsub_err;
    guint n_trashed;        /* targets this run moved / deleted itself */
} Del;

static Del *cur;   /* one run at a time (main thread) */

gboolean walpe_ui_delete_busy (void)
{
    return cur != NULL;
}

static void del_finish (Del *d)
{
    if (d->touched && d->host.done)
        d->host.done (d->path, d->gone && d->gone[0]);
    if (d->dlg)
        gtk_window_destroy (GTK_WINDOW (d->dlg));
    if (cur == d)
        cur = NULL;
    walpe_del_plan_free (d->plan);
    g_free (d->gone);
    g_free (d->path);
    g_free (d->title);
    g_free (d->library_dir);
    g_free (d->in_use);
    g_free (d->fail_msg);
    g_free (d->unsub_err);
    g_free (d);
}

static void del_status (Del *d, gboolean error, const char *fmt, ...) G_GNUC_PRINTF (3, 4);
static void del_status (Del *d, gboolean error, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    char *m = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    if (d->host.status)
        d->host.status (m, error);
    g_free (m);
}

/* ---------------------------------------------------------------- dialogs */

/* A wallpaper's title as it goes into WalPE's own sentences (dialog
 * headings, status line). The title is whatever project.json says: it is
 * cut to 80 characters (a 10000-character title made the confirmation
 * taller than the screen, its path list and buttons out of sight) and loses
 * control characters and the Unicode direction controls (a title with
 * U+202E mirrored the rest of the sentence). g_free. */
static char *short_title (const char *t)
{
    char *valid = g_utf8_make_valid (t ? t : "", -1);
    GString *s = g_string_new (NULL);
    int n = 0;
    for (const char *p = valid; *p; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (c == '\n' || c == '\t' || c == '\r')
            c = ' ';
        if (c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x061c || c == 0x200e || c == 0x200f ||
            (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069))
            continue;
        if (n == 80) {
            g_string_append (s, "…");
            break;
        }
        g_string_append_unichar (s, c);
        n++;
    }
    g_free (valid);
    return g_string_free (s, FALSE);
}

static GtkWidget *text_label (const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_width_chars (GTK_LABEL (l), 48);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 64);
    if (css)
        gtk_widget_add_css_class (l, css);
    return l;
}

/* "<b>lead</b> rest": the notes that must not be skimmed over */
static GtkWidget *note_label (const char *lead, const char *rest)
{
    GtkWidget *l = text_label (NULL, NULL);
    char *m = g_markup_printf_escaped ("<b>%s</b> %s", lead, rest);
    gtk_label_set_markup (GTK_LABEL (l), m);
    g_free (m);
    return l;
}

static gboolean dlg_escape (GtkWidget *w, GVariant *args, gpointer user)
{
    (void) args; (void) user;
    gtk_window_close (GTK_WINDOW (w));
    return TRUE;
}

/* modal, transient; body box returned through *box */
static GtkWidget *dialog_new (Del *d, const char *wtitle, const char *heading, GtkWidget **box)
{
    GtkWidget *w = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w), wtitle);
    if (d->host.parent) {
        gtk_window_set_transient_for (GTK_WINDOW (w), d->host.parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (w), TRUE);
    }
    gtk_window_set_modal (GTK_WINDOW (w), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (w), FALSE);
    GtkEventController *sc = gtk_shortcut_controller_new ();
    gtk_shortcut_controller_add_shortcut (
        GTK_SHORTCUT_CONTROLLER (sc),
        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                          gtk_callback_action_new (dlg_escape, NULL, NULL)));
    gtk_widget_add_controller (w, sc);
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top (v, 18);
    gtk_widget_set_margin_bottom (v, 16);
    gtk_widget_set_margin_start (v, 20);
    gtk_widget_set_margin_end (v, 20);
    GtkWidget *h = text_label (heading, "title-3");
    gtk_label_set_lines (GTK_LABEL (h), 4);      /* the heading never pushes */
    gtk_label_set_ellipsize (GTK_LABEL (h), PANGO_ELLIPSIZE_END);   /* the rest off screen */
    gtk_box_append (GTK_BOX (v), h);
    gtk_window_set_child (GTK_WINDOW (w), v);
    *box = v;
    return w;
}

/* the paths of the plan that are still there, one per line */
static GtkWidget *paths_label (Del *d)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < d->plan->targets->len; i++) {
        WalpeDelTarget *t = d->plan->targets->pdata[i];
        if (d->gone[i])
            continue;
        if (s->len)
            g_string_append_c (s, '\n');
        g_string_append (s, t->path);
        if (t->kind == WALPE_DEL_LINK)
            g_string_append_printf (s, "  (link to %s)", t->link_target ? t->link_target : "?");
        else if (t->kind == WALPE_DEL_THUMB)
            g_string_append (s, "  (its thumbnail)");
        else if (t->root == WALPE_ROOT_STEAM)
            g_string_append (s, "  (Steam library)");
        else if (t->root == WALPE_ROOT_STEAMCMD)
            g_string_append (s, "  (steamcmd download)");
    }
    GtkWidget *l = text_label (s->str, "monospace");
    gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_CHAR);
    gtk_label_set_selectable (GTK_LABEL (l), TRUE);
    gtk_widget_set_focusable (l, FALSE);
    g_string_free (s, TRUE);
    return l;
}

static GtkWidget *button_row (GtkWidget *box, const char *cancel, const char *go,
                              GtkWidget **cancel_out)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (row, GTK_ALIGN_END);
    gtk_widget_set_margin_top (row, 6);
    GtkWidget *c = gtk_button_new_with_mnemonic (cancel);
    gtk_box_append (GTK_BOX (row), c);
    GtkWidget *g = gtk_button_new_with_mnemonic (go);
    gtk_widget_add_css_class (g, "destructive-action");
    gtk_box_append (GTK_BOX (row), g);
    gtk_box_append (GTK_BOX (box), row);
    *cancel_out = c;
    return g;
}

static void del_forget_next (Del *d);
static void del_run_pass (Del *d);
static void unsub_failed_dialog (Del *d);

/* the dialog went away without a decision (Cancel, Escape, close button) */
static gboolean on_dlg_close (GtkWindow *w, gpointer user)
{
    Del *d = user;
    (void) w;
    d->dlg = NULL;      /* being destroyed by the default handler */
    d->unsub_check = NULL;
    d->page_msg = NULL;
    del_finish (d);
    return FALSE;
}

static void on_cancel (GtkButton *b, gpointer user)
{
    (void) b;
    gtk_window_close (GTK_WINDOW (((Del *) user)->dlg));
}

/* a decision: the dialog closes, the run goes on */
static void dlg_decided (Del *d)
{
    GtkWidget *w = d->dlg;
    d->dlg = NULL;
    g_signal_handlers_disconnect_by_func (w, on_dlg_close, d);
    gtk_window_destroy (GTK_WINDOW (w));
}

/* ------------------------------------------------------ Steam subscription */

static WalpeWsClient *unsub_client;   /* one for the life of the GUI */

static void del_start (Del *d)
{
    del_status (d, FALSE, "Deleting “%s”…", d->title);
    d->forget_i = 0;
    del_forget_next (d);
}

static void on_unsubscribed (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Del *d = user;
    GError *err = NULL;
    if (walpe_ws_subscribe_finish (res, &err)) {
        d->unsub = 1;
        walpe_steam_sub_mark (d->plan->workshop_id, FALSE);
    } else {
        d->unsub = -1;
        d->unsub_err = g_strdup (err ? err->message : "no answer");
        g_clear_error (&err);
    }
    /* the delete goes on either way: it is what was asked for. A refused
     * unsubscribe is reported once the files are gone (unsub_failed_dialog) */
    del_start (d);
}

/* ask Steam first, then delete: a running Steam client drops its copy
 * itself when told, and the trash pass copes with that (target_vanished) */
static void del_unsubscribe (Del *d)
{
    if (!unsub_client)
        unsub_client = walpe_ws_client_new ();
    WalpeWsSettings *st = walpe_ws_settings_load ();   /* the key as it is now */
    walpe_ws_client_set_api_key (unsub_client, st->api_key);
    walpe_ws_settings_free (st);
    del_status (d, FALSE, "Unsubscribing from “%s” on Steam…", d->title);
    walpe_ws_subscribe_async (unsub_client, d->plan->workshop_id, FALSE, NULL, on_unsubscribed,
                              d);
}

/* "Open Workshop page": Steam client when it runs, else the web browser */
static const char *open_page (Del *d, char **errmsg)
{
    WalpeSteamPageWhere where;
    GError *err = NULL;
    if (!walpe_steam_open_item_page (d->plan->workshop_id, &where, &err)) {
        char *url = walpe_steam_item_web_url (d->plan->workshop_id);
        *errmsg = g_strdup_printf ("The Workshop page could not be opened (%s). Its address: %s",
                                   err ? err->message : "?", url ? url : "?");
        g_free (url);
        g_clear_error (&err);
        return *errmsg;
    }
    return where == WALPE_STEAM_PAGE_CLIENT
               ? "Opened in the Steam client. Click “Subscribed” there to unsubscribe."
               : "Steam is not running: opened in your web browser. Log in there and click "
                 "“Subscribed” to unsubscribe.";
}

static void on_open_page (GtkButton *b, gpointer user)
{
    Del *d = user;
    (void) b;
    char *e = NULL;
    const char *m = open_page (d, &e);
    if (d->page_msg) {
        gtk_label_set_text (d->page_msg, m);
        gtk_widget_set_visible (GTK_WIDGET (d->page_msg), TRUE);
    }
    del_status (d, e != NULL, "%s", m);
    g_free (e);
}

static GtkWidget *page_button_row (Del *d, GtkWidget *box, const char *label)
{
    GtkWidget *b = gtk_button_new_with_mnemonic (label);
    gtk_widget_set_halign (b, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (b, "steam://url/CommunityFilePage/… in the running Steam "
                                    "client, otherwise the steamcommunity.com page in your "
                                    "web browser");
    g_signal_connect (b, "clicked", G_CALLBACK (on_open_page), d);
    gtk_box_append (GTK_BOX (box), b);
    GtkWidget *m = text_label (NULL, "dim-label");
    gtk_label_set_selectable (GTK_LABEL (m), TRUE);   /* the address, when it failed */
    gtk_widget_set_focusable (m, FALSE);
    gtk_widget_set_visible (m, FALSE);
    d->page_msg = GTK_LABEL (m);
    gtk_box_append (GTK_BOX (box), m);
    return b;
}

/* the part of the confirmation about the Steam subscription */
static void steam_section (Del *d, GtkWidget *box)
{
    if (d->plan->steam_client)
        gtk_box_append (GTK_BOX (box), note_label (
            "Steam will download it again.",
            "It is in Steam's Workshop folder: while your account is subscribed to the item, "
            "the Steam client restores it. It stays away once you have unsubscribed."));
    else
        gtk_box_append (GTK_BOX (box), note_label (
            "WalPE subscribed you to it on Steam.",
            "That was when it downloaded this item. Unless you have unsubscribed since, it "
            "stays in your Steam subscriptions (and a Steam client with Wallpaper Engine "
            "installed downloads it) until you unsubscribe."));
    if (d->can_direct) {
        d->unsub_check = gtk_check_button_new_with_mnemonic ("Also _unsubscribe on Steam");
        gtk_check_button_set_active (GTK_CHECK_BUTTON (d->unsub_check), d->unsub_default);
        gtk_box_append (GTK_BOX (box), d->unsub_check);
        GtkWidget *cap = text_label (
            "WalPE asks Steam with your Steam Web API key (IPublishedFileService/Unsubscribe, "
            "a call Valve does not document). If Steam does not accept it you are told, and "
            "can unsubscribe on the Workshop page instead.", "dim-label");
        gtk_widget_set_margin_start (cap, 24);
        gtk_box_append (GTK_BOX (box), cap);
        page_button_row (d, box, "Open _Workshop page");
    } else {
        gtk_box_append (GTK_BOX (box), text_label (
            walpe_ws_available ()
                ? "To unsubscribe, open the item's Workshop page and click the green "
                  "“Subscribed” button there; until you do, the item returns. (WalPE can ask "
                  "Steam itself once a Steam Web API key is saved in the Workshop tab's "
                  "settings; it never handles your Steam login.)"
                : "To unsubscribe, open the item's Workshop page and click the green "
                  "“Subscribed” button there; until you do, the item returns.", NULL));
        page_button_row (d, box, "Open _Workshop page to unsubscribe");
    }
}

static void on_confirm (GtkButton *b, gpointer user)
{
    Del *d = user;
    (void) b;
    gboolean unsub = d->unsub_check &&
                     gtk_check_button_get_active (GTK_CHECK_BUTTON (d->unsub_check));
    if (d->unsub_check && unsub != d->unsub_default)
        walpe_ws_settings_set_unsubscribe_on_delete (unsub);   /* the box remembers */
    d->unsub_check = NULL;
    d->page_msg = NULL;
    dlg_decided (d);
    if (unsub)
        del_unsubscribe (d);
    else
        del_start (d);
}

static void on_permanent (GtkButton *b, gpointer user)
{
    Del *d = user;
    (void) b;
    d->page_msg = NULL;
    dlg_decided (d);
    d->permanent = TRUE;
    del_status (d, FALSE, "Deleting “%s” permanently…", d->title);
    del_run_pass (d);
}

static void dlg_present (Del *d, GtkWidget *w, GtkWidget *focus)
{
    d->dlg = w;
    g_signal_connect (w, "close-request", G_CALLBACK (on_dlg_close), d);
    /* the safe button has the focus: Enter never deletes */
    gtk_window_set_focus (GTK_WINDOW (w), focus);
    gtk_window_present (GTK_WINDOW (w));
}

static void confirm_dialog (Del *d)
{
    WalpeDelPlan *pl = d->plan;
    WalpeDelTarget *first = pl->targets->pdata[0];
    guint n = pl->targets->len;
    char *head = g_strdup_printf ("Delete “%s”?", d->title);
    GtkWidget *box;
    GtkWidget *w = dialog_new (d, "Delete wallpaper", head, &box);
    g_free (head);

    const char *what;
    if (first->kind == WALPE_DEL_LINK && n == 1)
        what = "The link in the library is moved to the trash. What it points to is not "
               "touched.";
    else if (first->kind == WALPE_DEL_FILE)
        what = n > 1 ? "These files are moved to the trash:" : "This file is moved to the trash:";
    else if (n > 1)
        what = "This wallpaper is stored in more than one place. All of these are moved to "
               "the trash:";
    else
        what = "This folder is moved to the trash:";
    gtk_box_append (GTK_BOX (box), text_label (what, NULL));
    gtk_box_append (GTK_BOX (box), paths_label (d));

    if (d->in_use) {
        char *m = g_strdup_printf ("%s WalPE switches away from it first; a monitor with no "
                                   "other wallpaper to fall back to stays black until you set "
                                   "one.", d->in_use);
        gtk_box_append (GTK_BOX (box), note_label ("It is in use.", m));
        g_free (m);
    }
    /* Steam restores only what lies in a Steam client folder (or what WalPE
     * subscribed itself); a steamcmd download subscribed nothing */
    if (d->steam_sub)
        steam_section (d, box);
    else if (pl->steam_client)   /* a Steam folder that is not named by an id */
        gtk_box_append (GTK_BOX (box), note_label (
            "Steam will download it again.",
            "It is in Steam's Workshop folder: while your account is subscribed to the item, "
            "the Steam client restores it. Unsubscribe in Steam to keep it away."));
    guint nd = pl->dependents ? g_strv_length (pl->dependents) : 0;
    if (nd) {
        char *lead = g_strdup_printf ("%u preset%s will stop working:", nd, nd == 1 ? "" : "s");
        GString *s = g_string_new (NULL);
        for (guint i = 0; i < nd && i < 5; i++) {
            char *dt = short_title (pl->dependents[i]);
            g_string_append_printf (s, "%s“%s”", i ? ", " : "", dt);
            g_free (dt);
        }
        if (nd > 5)
            g_string_append_printf (s, " and %u more", nd - 5);
        g_string_append_printf (s, " (based on this wallpaper).");
        gtk_box_append (GTK_BOX (box), note_label (lead, s->str));
        g_string_free (s, TRUE);
        g_free (lead);
    }
    gtk_box_append (GTK_BOX (box), text_label (
        "You can restore it from the trash (file manager, or “gio trash --list” and "
        "“gio trash --restore”). The options you saved for it are forgotten.", "dim-label"));

    GtkWidget *cancel;
    GtkWidget *go = button_row (box, "_Cancel", "_Delete", &cancel);
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), d);
    g_signal_connect (go, "clicked", G_CALLBACK (on_confirm), d);
    dlg_present (d, w, cancel);
}

static void failure_dialog (Del *d)
{
    gboolean any_gone = FALSE;
    for (guint i = 0; i < d->plan->targets->len; i++)
        any_gone |= d->gone[i];
    char *head = d->permanent
                     ? g_strdup_printf ("“%s” could not be deleted", d->title)
                     : g_strdup_printf ("“%s” could not be moved to the trash", d->title);
    GtkWidget *box;
    GtkWidget *w = dialog_new (d, "Delete wallpaper", head, &box);
    g_free (head);
    gtk_box_append (GTK_BOX (box), text_label (d->fail_msg ? d->fail_msg : "Unknown error.",
                                               "error"));
    gtk_box_append (GTK_BOX (box), text_label (any_gone ? "Still there:"
                                                        : "Nothing was deleted. Still there:",
                                               NULL));
    gtk_box_append (GTK_BOX (box), paths_label (d));
    if (d->forgot && d->in_use && !d->gone[0])
        gtk_box_append (GTK_BOX (box), text_label (
            "WalPE already switched away from this wallpaper; set it again if you keep it.",
            NULL));
    if (d->unsub == 1) {
        gtk_box_append (GTK_BOX (box), text_label (
            "You were unsubscribed from it on Steam (the Workshop page subscribes again).",
            NULL));
    } else if (d->unsub == -1) {
        char *m = g_strdup_printf ("Steam did not accept the unsubscribe either: %s",
                                   d->unsub_err ? d->unsub_err : "?");
        gtk_box_append (GTK_BOX (box), text_label (m, NULL));
        g_free (m);
    }
    GtkWidget *cancel;
    if (!d->permanent) {
        gtk_box_append (GTK_BOX (box), text_label (
            "It can be deleted without the trash instead. That cannot be undone.", NULL));
        GtkWidget *go = button_row (box, "_Keep", "Delete _permanently", &cancel);
        g_signal_connect (go, "clicked", G_CALLBACK (on_permanent), d);
    } else {
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign (row, GTK_ALIGN_END);
        cancel = gtk_button_new_with_mnemonic ("_Close");
        gtk_box_append (GTK_BOX (row), cancel);
        gtk_box_append (GTK_BOX (box), row);
    }
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), d);
    dlg_present (d, w, cancel);
}

/* the files are gone but Steam refused (or never got) the unsubscribe */
static void unsub_failed_dialog (Del *d)
{
    /* the library changed: the grid follows now, not when this is closed */
    if (d->host.done)
        d->host.done (d->path, d->gone && d->gone[0]);
    d->touched = FALSE;
    char *head = g_strdup_printf ("“%s” was deleted, but you are still subscribed", d->title);
    GtkWidget *box;
    GtkWidget *w = dialog_new (d, "Delete wallpaper", head, &box);
    g_free (head);
    char *m = g_strdup_printf ("Steam did not accept the unsubscribe: %s",
                               d->unsub_err ? d->unsub_err : "?");
    gtk_box_append (GTK_BOX (box), text_label (m, "error"));
    g_free (m);
    gtk_box_append (GTK_BOX (box), text_label (
        d->plan->steam_client
            ? "Steam will download the item again until you unsubscribe. Open its Workshop "
              "page and click the green “Subscribed” button there."
            : "The item stays in your Steam subscriptions until you unsubscribe. Open its "
              "Workshop page and click the green “Subscribed” button there.", NULL));
    GtkWidget *open = page_button_row (d, box, "Open _Workshop page to unsubscribe");
    gtk_widget_add_css_class (open, "suggested-action");
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (row, GTK_ALIGN_END);
    gtk_widget_set_margin_top (row, 6);
    GtkWidget *close = gtk_button_new_with_mnemonic ("_Close");
    gtk_box_append (GTK_BOX (row), close);
    gtk_box_append (GTK_BOX (box), row);
    g_signal_connect (close, "clicked", G_CALLBACK (on_cancel), d);
    dlg_present (d, w, open);
}

/* ------------------------------------------------------- trash / remove */

/* the entry is gone while the folder it was in is still there */
static gboolean target_vanished (const char *path)
{
    struct stat st;
    if (lstat (path, &st) == 0 || errno != ENOENT)
        return FALSE;
    char *dir = g_path_get_dirname (path);
    gboolean parent = g_file_test (dir, G_FILE_TEST_IS_DIR);
    g_free (dir);
    return parent;
}

static void pass_thread (GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    Del *d = data;   /* the main thread waits for this task: no locking */
    GString *msg = g_string_new (NULL);
    guint failed = 0;
    for (guint i = 0; i < d->plan->targets->len; i++) {
        WalpeDelTarget *tg = d->plan->targets->pdata[i];
        if (d->gone[i])
            continue;
        if (failed && i == 1 && tg->kind == WALPE_DEL_THUMB)
            continue;   /* the video stays: so does its thumbnail */
        GError *err = NULL;
        gboolean ok = d->permanent ? walpe_library_remove (tg, d->library_dir, &err)
                                   : walpe_library_trash (tg, d->library_dir, &err);
        if (ok && tg->kind != WALPE_DEL_THUMB) {
            d->n_trashed++;
        } else if (!ok && tg->steam_client && target_vanished (tg->path)) {
            /* unsubscribed (here or on the Workshop page): the Steam client
             * removes its copy itself and may get there first */
            ok = TRUE;
        }
        if (ok) {
            d->gone[i] = TRUE;
        } else if (tg->kind == WALPE_DEL_THUMB) {
            /* a leftover thumbnail is not worth a dialog */
        } else {
            failed++;
            g_string_append_printf (msg, "%s%s: %s", msg->len ? "\n" : "", tg->path,
                                    err ? err->message : "failed");
        }
        g_clear_error (&err);
    }
    d->n_failed = failed;
    g_free (d->fail_msg);
    d->fail_msg = g_string_free (msg, FALSE);
    g_task_return_boolean (t, failed == 0);
}

static void pass_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Del *d = user;
    gboolean ok = g_task_propagate_boolean (G_TASK (res), NULL);
    d->touched = TRUE;
    if (ok) {
        const char *did = d->permanent ? "Deleted" : "Moved";
        const char *where = d->permanent ? " permanently" : " to the trash";
        if (d->unsub == -1) {
            del_status (d, TRUE, "%s “%s”%s, but you are still subscribed to it on Steam.",
                        did, d->title, where);
            unsub_failed_dialog (d);
            return;
        }
        if (!d->n_trashed)   /* every copy was Steam's, and Steam was faster */
            del_status (d, FALSE, "Steam had already removed “%s”%s.", d->title,
                        d->unsub == 1 ? "; you are unsubscribed from it" : "");
        else
            del_status (d, FALSE, "%s “%s”%s%s.", did, d->title, where,
                        d->unsub == 1 ? " and unsubscribed from it on Steam" : "");
        del_finish (d);
        return;
    }
    del_status (d, TRUE, "“%s” could not be %s: %s", d->title,
                d->permanent ? "deleted" : "moved to the trash", d->fail_msg);
    failure_dialog (d);
}

static void del_run_pass (Del *d)
{
    GTask *t = g_task_new (NULL, NULL, pass_done, d);
    g_task_set_task_data (t, d, NULL);
    g_task_run_in_thread (t, pass_thread);
    g_object_unref (t);
}

/* ----------------------------------------------------------------- forget */

static void on_forget_reply (JsonObject *reply, const char *err, gpointer user)
{
    Del *d = user;
    if (ui_ipc_ok (reply)) {
        d->forget_i++;
        del_forget_next (d);
        return;
    }
    if (!reply && ui_ipc_err_is_down (err)) {
        /* no engine: nobody else writes config.json now */
        walpe_config_forget_offline ((const char *const *) d->plan->forget);
        d->forgot = TRUE;
        del_run_pass (d);
        return;
    }
    const char *e = reply ? wpj_str (reply, "error", "rejected") : (err ? err : "no answer");
    const char *sub = d->unsub == 1 ? " You were already unsubscribed from it on Steam." : "";
    if (reply && !g_strcmp0 (e, "unknown cmd"))
        del_status (d, TRUE, "“%s” was not deleted: the running engine is older than this "
                    "window. Restart it (walpe-ctl quit) and try again.%s", d->title, sub);
    else
        del_status (d, TRUE, "“%s” was not deleted: the engine did not let go of it (%s). "
                    "Try again in a moment.%s", d->title, e, sub);
    del_finish (d);
}

static void del_forget_next (Del *d)
{
    const char *p = d->plan->forget[d->forget_i];
    if (!p) {
        d->forgot = TRUE;
        del_run_pass (d);
        return;
    }
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "forget");
    json_builder_set_member_name (b, "path");
    json_builder_add_string_value (b, p);
    json_builder_end_object (b);
    /* not AUTOSTART: a stopped engine is not started just to forget */
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW, on_forget_reply, d);
    g_object_unref (b);
}

/* ------------------------------------------------------------------ check */

static void plan_thread (GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    Del *d = data;
    GError *err = NULL;
    WalpeDelPlan *pl = walpe_library_delete_plan (d->path, d->library_dir, &err);
    if (pl && pl->workshop_id) {
        /* file reads: here, not on the main thread */
        d->sub_by_walpe = walpe_steam_sub_is_marked (pl->workshop_id);
        d->steam_sub = pl->steam_client || d->sub_by_walpe;
        if (d->steam_sub && walpe_ws_available ()) {
            WalpeWsSettings *st = walpe_ws_settings_load ();
            d->can_direct = st->api_key != NULL;
            d->unsub_default = st->unsubscribe_on_delete;
            walpe_ws_settings_free (st);
        }
    }
    if (pl)
        g_task_return_pointer (t, pl, (GDestroyNotify) walpe_del_plan_free);
    else
        g_task_return_error (t, err);
}

static void plan_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Del *d = user;
    GError *err = NULL;
    d->plan = g_task_propagate_pointer (G_TASK (res), &err);
    if (!d->plan) {
        del_status (d, TRUE, "“%s” cannot be deleted from here: %s", d->title,
                    err ? err->message : "not a library entry");
        g_clear_error (&err);
        del_finish (d);
        return;
    }
    d->gone = g_new0 (gboolean, d->plan->targets->len);
    confirm_dialog (d);
}

void walpe_ui_delete (const WalpeDeleteHost *host, const char *path, const char *title,
                      const char *library_dir, const char *in_use)
{
    if (cur) {
        if (cur->dlg)
            gtk_window_present (GTK_WINDOW (cur->dlg));
        return;
    }
    if (!path || !*path)
        return;
    Del *d = g_new0 (Del, 1);
    d->host = *host;
    d->path = g_strdup (path);
    d->title = short_title (title && *title ? title : path);
    if (!*d->title) {          /* a title of nothing but control characters */
        g_free (d->title);
        d->title = short_title (path);
    }
    d->library_dir = g_strdup (library_dir);
    d->in_use = in_use && *in_use ? g_strdup (in_use) : NULL;
    cur = d;
    GTask *t = g_task_new (NULL, NULL, plan_done, d);
    g_task_set_task_data (t, d, NULL);
    g_task_run_in_thread (t, plan_thread);
    g_object_unref (t);
}
