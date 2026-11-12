/* perf_panel_test.c - the quality panel of a wallpaper (src/ui/perf.c) and
 * the engine's answers, without an engine: the requests end in this file.
 *
 * Needs a display (GTK); the case `panel` of scripts/lifecheck.py runs it
 * on its Xvfb. Exit status 77 without one.
 *
 *  - an answer that says what the last one said leaves the line's label
 *    widget alone. gtk_expander_set_label () makes a new label widget for
 *    every call, also for the text that is there: the panel called it for
 *    each answer, and an open panel gets one every 2 s - a layout and a
 *    frame of the whole window each time.
 *  - an answer that says something else changes the label's text. */
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "ui/perf.h"
#include "ui/uiipc.h"
#include "ui/winstate.h"

static UiIpcDone last_cb;
static gpointer last_user;
static guint n_requests;
static int fails;

/* ---- the part of uiipc.c the panel uses */
void ui_ipc_send (const char *json, guint flags, UiIpcDone cb, gpointer user)
{
    (void) flags;
    if (json && strstr (json, "\"quality\"")) {
        n_requests++;
        last_cb = cb;
        last_user = user;
    }
}

void ui_ipc_send_node (JsonNode *req, guint flags, UiIpcDone cb, gpointer user)
{
    (void) flags; (void) cb; (void) user;
    json_node_unref (req);
}

/* (the panel asks which failure it was, to ask a stopped engine less often:
 * the answers of this test never fail) */
gboolean ui_ipc_err_is_down (const char *err)
{
    (void) err;
    return FALSE;
}

/* (winstate.c: what a window manager does to a hidden window. The test's
 * window stays on screen; the panel's timer is not what is tested here) */
void walpe_winstate_watch (GdkDisplay *d)
{
    (void) d;
}

gboolean ui_ipc_ok (JsonObject *reply)
{
    return reply && json_object_has_member (reply, "ok") && json_object_get_boolean_member (reply, "ok");
}

static void check (gboolean ok, const char *what)
{
    g_print ("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += ok ? 0 : 1;
}

static void spin (guint ms)
{
    gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;
    while (g_get_monotonic_time () < end)
        g_main_context_iteration (NULL, FALSE), g_usleep (2000);
}

/* the engine's answer to the request that waits, if one does */
static gboolean answer (const char *json)
{
    if (!last_cb)
        return FALSE;
    UiIpcDone cb = last_cb;
    gpointer user = last_user;
    last_cb = NULL;
    JsonParser *p = json_parser_new ();
    gboolean ok = json_parser_load_from_data (p, json, -1, NULL);
    if (ok)
        cb (json_node_get_object (json_parser_get_root (p)), NULL, user);
    g_object_unref (p);
    return ok;
}

int main (void)
{
    if (!gtk_init_check ()) {
        g_print ("perf-panel-test: skipped (no display)\n");
        return 77;
    }
    static const char path[] = "/nowhere/a wallpaper";
    static const char plain[] = "{\"ok\":true,\"quality\":{\"monitors\":[]}}";
    static const char own[] = "{\"ok\":true,\"quality\":{\"monitors\":[],\"wallpapers\":"
                              "{\"/nowhere/a wallpaper\":{\"fx_res\":\"half\"}}}}";
    GtkWidget *win = gtk_window_new ();
    GtkWidget *panel = walpe_perf_panel_new (TRUE, NULL);
    gtk_window_set_child (GTK_WINDOW (win), panel);
    walpe_perf_panel_set_path (panel, path);
    gtk_window_present (GTK_WINDOW (win));
    for (int i = 0; i < 500 && !last_cb; i++)
        spin (10);
    check (last_cb != NULL, "the panel asks the engine when it is shown");
    check (GTK_IS_EXPANDER (panel), "a wallpaper's panel is one line that opens");
    if (fails)
        return 1;
    GtkExpander *ex = GTK_EXPANDER (panel);
    answer (plain);
    char *first = g_strdup (gtk_expander_get_label (ex));
    check (first && strstr (first, "as in Engine settings") != NULL, "the line names what the wallpaper uses");
    gtk_expander_set_expanded (ex, TRUE);     /* asks at once */
    spin (50);
    GtkWidget *label = gtk_expander_get_label_widget (ex);
    g_object_ref (label);
    guint replaced = 0, answered = 0;
    for (int i = 0; i < 4; i++) {
        if (!last_cb)
            walpe_perf_panel_set_path (panel, path);    /* (the same wallpaper: asks again) */
        answered += answer (plain) ? 1 : 0;
        spin (20);
        replaced += gtk_expander_get_label_widget (ex) != label ? 1 : 0;
    }
    check (answered == 4, "four more answers that say the same");
    if (replaced)
        g_print ("     the label widget was not the first one after %u of 4 answers\n", replaced);
    check (replaced == 0, "answers that say the same leave the line's label widget alone");
    check (!g_strcmp0 (first, gtk_expander_get_label (ex)), "... and its text");
    if (!last_cb)
        walpe_perf_panel_set_path (panel, path);
    answer (own);
    spin (20);
    check (g_strcmp0 (first, gtk_expander_get_label (ex)) != 0 &&
           strstr (gtk_expander_get_label (ex), "Quality for this wallpaper: ") != NULL,
           "an answer with settings of the wallpaper's own changes the line");
    g_object_unref (label);
    g_free (first);
    gtk_window_destroy (GTK_WINDOW (win));
    spin (50);
    g_print ("perf-panel-test: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
