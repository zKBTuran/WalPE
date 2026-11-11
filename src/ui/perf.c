/* perf.c - see perf.h */
#include "perf.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include "common/quality.h"
#include "common/util.h"
#include "uiipc.h"
#include "numfield.h"

typedef struct {
    gboolean per_wp;
    char *path;                 /* per_wp: the wallpaper */
    WalpePerfStatus status;
    GtkWidget *root, *expander, *preset, *cost, *estimate;
    GtkWidget *rate, *res, *post, *scale, *tex, *exact, *idle;
    gboolean loading;           /* widgets are being set from the daemon's answer */
    gboolean have;              /* an answer arrived */
    WalpeQuality q;             /* the knobs shown */
    guint poll_id, scale_id, idle_id;
} Perf;

/* preset selector: per wallpaper the first entry drops its own settings */
static const char *const preset_items_global[] = { "Maximum (exact sizes)", "High (default)",
                                                   "Balanced", "Low", "Custom", NULL };
static const char *const preset_items_wp[] = { "As in Engine settings", "Maximum (exact sizes)",
                                               "High (default)", "Balanced", "Low", "Custom", NULL };
static const char *const rate_items[] = { "Every frame", "Every 2nd frame, slow effects only",
                                          "Every 2nd frame, all effects",
                                          "Every 3rd frame, slow effects only",
                                          "Every 3rd frame, all effects", NULL };
static const char *const res_items[] = { "Full", "On-screen size", "Reduced (3/4)", "Half", NULL };
static const char *const res_values[] = { "full", "screen", "reduced", "half" };
static const char *const post_items[] = { "Full", "Reduced", "Off", NULL };
static const char *const post_values[] = { "full", "reduced", "off" };
static const char *const tex_items[] = { "Full", "Half", "Quarter", NULL };
static const char *const tex_values[] = { "full", "half", "quarter" };

static void perf_request (Perf *P);

/* ---------------------------------------------------------------- sending */

static void on_set_reply (JsonObject *reply, const char *err, gpointer user)
{
    GtkWidget *root = user;
    Perf *P = g_object_get_data (G_OBJECT (root), "walpe-perf");
    if (P) {
        if (ui_ipc_ok (reply)) {
            if (P->status)
                P->status ("Quality setting applied.", FALSE);
        } else if (P->status) {
            const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
            char *m = g_strdup_printf ("Quality setting not applied: %s", e ? e : "error");
            P->status (m, TRUE);
            g_free (m);
        }
        perf_request (P);   /* show what the daemon has now */
    }
    g_object_unref (root);
}

/* one knob (val is consumed), or reset = the wallpaper's own settings away */
static void perf_send (Perf *P, const char *key, JsonNode *val, gboolean reset)
{
    if (P->per_wp && !P->path) {
        if (val)
            json_node_unref (val);
        return;
    }
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, P->per_wp ? "setquality" : "set");
    if (P->per_wp) {
        json_builder_set_member_name (b, "path");
        json_builder_add_string_value (b, P->path);
        if (reset) {
            json_builder_set_member_name (b, "reset");
            json_builder_add_boolean_value (b, TRUE);
        } else {
            json_builder_set_member_name (b, "values");
            json_builder_begin_object (b);
            json_builder_set_member_name (b, key);
            json_builder_add_value (b, val);
            json_builder_end_object (b);
        }
    } else {
        json_builder_set_member_name (b, key);
        json_builder_add_value (b, val);
    }
    json_builder_end_object (b);
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_AUTOSTART, on_set_reply,
                      g_object_ref (P->root));
    g_object_unref (b);
}

static void perf_send_str (Perf *P, const char *key, const char *v)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_string (n, v);
    perf_send (P, key, n, FALSE);
}

static void perf_send_int (Perf *P, const char *key, int v)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_int (n, v);
    perf_send (P, key, n, FALSE);
}

/* ---------------------------------------------------------------- widgets */

static void on_preset (GtkDropDown *d, GParamSpec *ps, gpointer user)
{
    (void) ps;
    Perf *P = user;
    if (P->loading)
        return;
    int i = (int) gtk_drop_down_get_selected (d);
    if (P->per_wp) {
        if (i == 0) {
            perf_send (P, NULL, NULL, TRUE);
            return;
        }
        i--;
    }
    if (i >= WALPE_PRESET_MAX && i <= WALPE_PRESET_LOW)
        perf_send_str (P, "quality_preset", walpe_preset_name ((WalpePreset) i));
    /* "Custom" is what the single settings make of it: nothing to send */
}

static void on_rate (GtkDropDown *d, GParamSpec *ps, gpointer user)
{
    (void) ps;
    Perf *P = user;
    if (P->loading)
        return;
    int i = (int) gtk_drop_down_get_selected (d);
    int rate = i == 0 ? 1 : i <= 2 ? 2 : 3;
    if (i > 0)   /* the mode first: the divisor is what switches it on */
        perf_send_str (P, "fx_rate_mode", (i == 2 || i == 4) ? "all" : "slow");
    perf_send_int (P, "fx_rate", rate);
}

static void on_choice (GtkDropDown *d, GParamSpec *ps, gpointer user)
{
    (void) ps;
    Perf *P = user;
    if (P->loading)
        return;
    const char *key = g_object_get_data (G_OBJECT (d), "walpe-key");
    const char *const *values = g_object_get_data (G_OBJECT (d), "walpe-values");
    perf_send_str (P, key, values[gtk_drop_down_get_selected (d)]);
}

static void on_exact (GtkSwitch *s, GParamSpec *ps, gpointer user)
{
    (void) ps;
    Perf *P = user;
    if (!P->loading)
        perf_send_int (P, "texture_slack", gtk_switch_get_active (s) ? 0 : WALPE_TEX_SLACK_DEFAULT);
}

static gboolean scale_fire (gpointer user)
{
    Perf *P = user;
    P->scale_id = 0;
    perf_send_int (P, "render_scale", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (P->scale)));
    return G_SOURCE_REMOVE;
}

static gboolean idle_fire (gpointer user)
{
    Perf *P = user;
    P->idle_id = 0;
    int v = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (P->idle));
    /* 0 = no limit of its own; 1..4 are not rates anybody wants */
    perf_send_int (P, "fps_idle", v <= 0 ? 0 : MAX (v, WALPE_QUALITY_FPS_MIN));
    return G_SOURCE_REMOVE;
}

static void on_spin (GtkSpinButton *s, gpointer user)
{
    Perf *P = user;
    if (P->loading)
        return;
    guint *id = GTK_WIDGET (s) == P->scale ? &P->scale_id : &P->idle_id;
    if (*id)
        g_source_remove (*id);
    *id = g_timeout_add (300, GTK_WIDGET (s) == P->scale ? scale_fire : idle_fire, P);
}

static GtkWidget *sub_label (const char *text)
{
    GtkWidget *s = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (s), 0);
    gtk_label_set_wrap (GTK_LABEL (s), TRUE);
    gtk_label_set_width_chars (GTK_LABEL (s), 34);
    gtk_label_set_max_width_chars (GTK_LABEL (s), 60);
    gtk_widget_add_css_class (s, "walpe-settings-sub");
    return s;
}

/* title + hint on the left, the control on the right (as the settings rows) */
static void perf_row (GtkWidget *grid, int row, const char *title, const char *sub, GtkWidget *ctl)
{
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (v, TRUE);
    GtkWidget *t = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (t), 0);
    gtk_label_set_wrap (GTK_LABEL (t), TRUE);
    gtk_label_set_width_chars (GTK_LABEL (t), 34);
    gtk_label_set_max_width_chars (GTK_LABEL (t), 60);
    gtk_box_append (GTK_BOX (v), t);
    if (sub)
        gtk_box_append (GTK_BOX (v), sub_label (sub));
    gtk_widget_set_margin_top (v, 6);
    gtk_widget_set_margin_bottom (v, 6);
    gtk_grid_attach (GTK_GRID (grid), v, 0, row, 1, 1);
    gtk_widget_set_valign (ctl, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (ctl, GTK_ALIGN_END);
    gtk_accessible_update_property (GTK_ACCESSIBLE (ctl), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    if (sub)
        gtk_widget_set_tooltip_text (ctl, sub);
    gtk_grid_attach (GTK_GRID (grid), ctl, 1, row, 1, 1);
}

static GtkWidget *mk_choice (Perf *P, const char *key, const char *const *items,
                             const char *const *values)
{
    GtkWidget *d = gtk_drop_down_new_from_strings (items);
    g_object_set_data (G_OBJECT (d), "walpe-key", (gpointer) key);
    g_object_set_data (G_OBJECT (d), "walpe-values", (gpointer) values);
    g_signal_connect (d, "notify::selected", G_CALLBACK (on_choice), P);
    return d;
}

/* ------------------------------------------------------------- the answer */

/* "Balanced -17 %, Low -61 %": pixels shaded per second against High,
 * summed over the monitors (per_wp: the ones showing the wallpaper) */
static char *perf_estimate_text (Perf *P, JsonObject *q)
{
    static const char *const names[] = { "high", "balanced", "low", "current" };
    double sum[4] = { 0 }, frame_high = 0;
    int n = 0, scenes = 0;
    JsonArray *ms = wpj_arr (q, "monitors");
    for (guint i = 0; i < wpj_arr_len (ms); i++) {
        JsonObject *m = wpj_arr_obj (ms, i);
        if (!m || json_object_has_member (m, "follows"))
            continue;
        if (P->per_wp && g_strcmp0 (wpj_str (m, "path", ""), P->path))
            continue;
        if (!g_strcmp0 (wpj_str (m, "type", ""), "scene"))
            scenes++;
        JsonObject *es = wpj_obj (m, "estimate");
        if (!es)
            continue;
        n++;
        for (int k = 0; k < 4; k++)
            sum[k] += wpj_num (wpj_obj (es, names[k]), "px_s", 0);
        frame_high += wpj_num (wpj_obj (es, "high"), "px_frame", 0);
    }
    if (!n || sum[0] <= 0) {
        if (P->per_wp && !scenes)
            return g_strdup ("Estimate: shown while this wallpaper is on screen (scenes only).");
        return g_strdup (scenes ? "Estimate: measuring the wallpapers on screen…"
                                : "Estimate: needs a scene wallpaper on screen; for videos and web "
                                  "pages only the frame rate limit applies.");
    }
    double pct[4] = { 0 };
    for (int i = 1; i < 4; i++) {
        pct[i] = 100 * (sum[i] / sum[0] - 1);
        if (pct[i] > -0.5 && pct[i] < 0.5)
            pct[i] = 0;   /* no "-0 %" */
    }
    return g_strdup_printf ("Estimated GPU work for %s (pixels shaded per second; High = %.1f "
                            "million per frame): Balanced %+.0f %%, Low %+.0f %%, as set now "
                            "%+.0f %%. Measured on this machine's wallpapers, not a promise for "
                            "others.", P->per_wp ? "this wallpaper" : "the wallpapers on screen",
                            frame_high / 1e6, pct[1], pct[2], pct[3]);
}

/* what preset p costs the viewer: the daemon's sentence for the global
 * preset (it names what this build sets with it), the knobs' own for a
 * wallpaper's preset */
static const char *perf_cost (Perf *P, JsonObject *q, WalpePreset p)
{
    JsonObject *ps = q ? wpj_obj (q, "presets") : NULL;
    JsonObject *e = ps && p <= WALPE_PRESET_LOW ? wpj_obj (ps, walpe_preset_name (p)) : NULL;
    const char *s = e ? wpj_str (e, P->per_wp ? "cost_own" : "cost", NULL) : NULL;
    return s && *s ? s : walpe_preset_cost (p);
}

static void perf_load (Perf *P, JsonObject *q)
{
    WalpeQuality v;
    walpe_quality_preset (WALPE_PRESET_HIGH, &v);
    walpe_quality_load_json (q, &v, NULL);
    gboolean own = FALSE;
    if (P->per_wp) {
        JsonObject *ws = wpj_obj (q, "wallpapers");
        JsonObject *o = ws && P->path ? wpj_obj (ws, P->path) : NULL;
        if (o) {
            guint set = 0;
            /* only its own keys: a "quality_preset" label is not one */
            JsonObject *c = json_object_new ();
            GList *ks = json_object_get_members (o);
            for (GList *k = ks; k; k = k->next)
                if (strcmp (k->data, "quality_preset"))
                    json_object_set_member (c, k->data,
                                            json_node_copy (json_object_get_member (o, k->data)));
            g_list_free (ks);
            walpe_quality_load_json (c, &v, &set);
            json_object_unref (c);
            own = set != 0;
        }
    }
    P->q = v;
    P->have = TRUE;
    P->loading = TRUE;
    WalpePreset pr = walpe_quality_preset_of (&v);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->preset),
                                P->per_wp ? (own ? (guint) pr + 1 : 0) : (guint) pr);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->rate),
                                v.fx_rate <= 1 ? 0 : (guint) ((v.fx_rate - 2) * 2 + 1 + (v.fx_rate_all ? 1 : 0)));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->res), (guint) v.fx_res);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->post), (guint) v.post);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (P->tex), (guint) v.tex_res);
    gtk_switch_set_active (GTK_SWITCH (P->exact), v.tex_slack == 0);
    if (!P->scale_id)
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (P->scale), v.render_scale);
    if (!P->idle_id)
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (P->idle), v.fps_idle);
    P->loading = FALSE;
    char *cost = P->per_wp && !own
        ? g_strdup ("This wallpaper uses the quality settings of Engine settings > Performance.")
        : g_strdup_printf ("%s: %s", walpe_preset_label (pr), perf_cost (P, q, pr));
    gtk_label_set_text (GTK_LABEL (P->cost), cost);
    g_free (cost);
    /* what each preset costs the viewer: on the selector, in plain words */
    char *tip = g_strdup_printf ("Maximum: %s\n\nHigh: %s\n\nBalanced: %s\n\nLow: %s\n\n%s%s",
                                 perf_cost (P, q, WALPE_PRESET_MAX),
                                 perf_cost (P, q, WALPE_PRESET_HIGH),
                                 perf_cost (P, q, WALPE_PRESET_BALANCED),
                                 perf_cost (P, q, WALPE_PRESET_LOW), walpe_quality_picture_note (),
                                 P->per_wp ? "\n\nSettings of the whole engine (particles, effect "
                                             "shaders) stay as in Engine settings." : "");
    const char *old = gtk_widget_get_tooltip_text (P->preset);
    if (g_strcmp0 (old, tip))
        gtk_widget_set_tooltip_text (P->preset, tip);
    g_free (tip);
    char *est = perf_estimate_text (P, q);
    gtk_label_set_text (GTK_LABEL (P->estimate), est);
    g_free (est);
    if (P->expander) {
        char *t = g_strdup_printf ("Quality for this wallpaper: %s",
                                   own ? walpe_preset_label (pr) : "as in Engine settings");
        gtk_expander_set_label (GTK_EXPANDER (P->expander), t);
        g_free (t);
    }
}

static void on_quality_reply (JsonObject *reply, const char *err, gpointer user)
{
    (void) err;
    GtkWidget *root = user;
    Perf *P = g_object_get_data (G_OBJECT (root), "walpe-perf");
    JsonObject *q = reply && ui_ipc_ok (reply) ? wpj_obj (reply, "quality") : NULL;
    if (P && q)
        perf_load (P, q);
    else if (P && !P->have)
        gtk_label_set_text (GTK_LABEL (P->estimate),
                            "The wallpaper service is not running: the settings show once it is.");
    g_object_unref (root);
}

static void perf_request (Perf *P)
{
    if (P->per_wp && !P->path)
        return;
    ui_ipc_send ("{\"cmd\":\"quality\"}", UI_IPC_POLL, on_quality_reply, g_object_ref (P->root));
}

/* the daemon only counts while somebody asks: ask while the panel shows */
static gboolean perf_poll (gpointer user)
{
    Perf *P = user;
    if (!P->expander || gtk_expander_get_expanded (GTK_EXPANDER (P->expander)))
        perf_request (P);
    return G_SOURCE_CONTINUE;
}

static void on_map (GtkWidget *w, gpointer user)
{
    (void) w;
    Perf *P = user;
    perf_request (P);
    if (!P->poll_id)
        P->poll_id = g_timeout_add_seconds (2, perf_poll, P);
}

static void on_unmap (GtkWidget *w, gpointer user)
{
    (void) w;
    Perf *P = user;
    if (P->poll_id)
        g_source_remove (P->poll_id);
    P->poll_id = 0;
}

static void on_expanded (GtkExpander *e, GParamSpec *ps, gpointer user)
{
    (void) ps;
    if (gtk_expander_get_expanded (e))
        perf_request (user);
}

static void perf_free (gpointer user)
{
    Perf *P = user;
    if (P->poll_id)
        g_source_remove (P->poll_id);
    if (P->scale_id)
        g_source_remove (P->scale_id);
    if (P->idle_id)
        g_source_remove (P->idle_id);
    g_free (P->path);
    g_free (P);
}

GtkWidget *walpe_perf_panel_new (gboolean per_wallpaper, WalpePerfStatus status)
{
    Perf *P = g_new0 (Perf, 1);
    P->per_wp = per_wallpaper;
    P->status = status;
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *g = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (g), 12);
    int r = 0;

    P->preset = gtk_drop_down_new_from_strings (per_wallpaper ? preset_items_wp : preset_items_global);
    g_signal_connect (P->preset, "notify::selected", G_CALLBACK (on_preset), P);
    perf_row (g, r++, "Quality preset",
              per_wallpaper ? "This wallpaper's own choice; it replaces the one in Engine settings "
                              "(particle and shader settings stay as there)."
                            : "Trades picture quality for less GPU work. Sets all of \"Single "
                              "settings\" at once.", P->preset);
    /* the tooltip (what each preset costs the viewer) is set with the
     * daemon's answer: perf_load */
    P->cost = sub_label ("…");
    gtk_grid_attach (GTK_GRID (g), P->cost, 0, r++, 2, 1);
    P->estimate = sub_label ("…");
    gtk_widget_set_margin_top (P->estimate, 4);
    gtk_grid_attach (GTK_GRID (g), P->estimate, 0, r++, 2, 1);
    gtk_box_append (GTK_BOX (box), g);

    GtkWidget *kg = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (kg), 12);
    r = 0;
    P->rate = gtk_drop_down_new_from_strings (rate_items);
    g_signal_connect (P->rate, "notify::selected", G_CALLBACK (on_rate), P);
    perf_row (kg, r++, "Effect update rate",
              "Animated effects are redrawn every 2nd or 3rd frame and shown unchanged in "
              "between; everything else keeps the full frame rate. \"Slow effects only\" picks "
              "the effects whose motion is slow enough not to show it; \"all effects\" makes "
              "fast ones (blinking, shaking) stutter.", P->rate);
    P->res = mk_choice (P, "fx_res", res_items, res_values);
    perf_row (kg, r++, "Effect resolution",
              "Full: as always. On-screen size: effects are computed at the size the layer has "
              "on screen instead of a little larger (hard to see). Reduced / Half: at 3/4 / 1/2 "
              "of that and drawn enlarged: visibly softer.", P->res);
    P->post = mk_choice (P, "post_processing", post_items, post_values);
    perf_row (kg, r++, "Post-processing",
              "Reduced: blur and glow are computed on half-size buffers (softer). Off: purely "
              "decorative full-screen passes (bloom, film grain, vignette, colour fringes) are "
              "left out as well. Effects that carry content (masks, reveals, water) always stay.",
              P->post);
    P->scale = gtk_spin_button_new_with_range (WALPE_RENDER_SCALE_MIN, 100, 5);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (P->scale), TRUE);
    /* (emptied and left, the field set its minimum, 25 %; the idle frame
     * rate below is left as it is: its minimum, 0, means "none") */
    walpe_num_field_keep_when_empty (P->scale);
    g_signal_connect (P->scale, "value-changed", G_CALLBACK (on_spin), P);
    perf_row (kg, r++, "Render scale (%)",
              "Scenes are drawn at this share of the monitor's resolution and scaled up: "
              "everything gets softer, the GPU work falls with the square of it.", P->scale);
    P->tex = mk_choice (P, "tex_res", tex_items, tex_values);
    perf_row (kg, r++, "Texture resolution",
              "Layer images are stored at half or quarter size: less video memory, softer "
              "picture. Reloads the wallpaper.", P->tex);
    P->exact = gtk_switch_new ();
    g_signal_connect (P->exact, "notify::active", G_CALLBACK (on_exact), P);
    perf_row (kg, r++, "Exact texture sizes",
              "Off (default): a reduced image or effect may be drawn up to 2 % enlarged, which "
              "saves a size step. On: never (preset Maximum). Reloads the wallpaper.", P->exact);
    P->idle = gtk_spin_button_new_with_range (0, WALPE_QUALITY_FPS_MAX, 1);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (P->idle), TRUE);
    g_signal_connect (P->idle, "value-changed", G_CALLBACK (on_spin), P);
    perf_row (kg, r++, "Frame rate while the desktop is idle",
              "0 = the frame rate cap. The limits for a focused application and for battery "
              "power can only lower it further. Changes no picture, only how smooth motion is.",
              P->idle);
    GtkWidget *note = sub_label (walpe_quality_picture_note ());
    gtk_widget_set_margin_top (note, 4);
    gtk_grid_attach (GTK_GRID (kg), note, 0, r++, 2, 1);
    GtkWidget *ex = gtk_expander_new ("Single settings");
    gtk_expander_set_child (GTK_EXPANDER (ex), kg);
    gtk_box_append (GTK_BOX (box), ex);

    if (per_wallpaper) {
        /* one line in the options pane until it is opened */
        P->expander = gtk_expander_new ("Quality for this wallpaper");
        gtk_expander_set_child (GTK_EXPANDER (P->expander), box);
        gtk_widget_set_margin_start (box, 12);
        g_signal_connect (P->expander, "notify::expanded", G_CALLBACK (on_expanded), P);
        P->root = P->expander;
        gtk_widget_set_visible (P->root, FALSE);
    } else {
        P->root = box;
    }
    g_object_set_data_full (G_OBJECT (P->root), "walpe-perf", P, perf_free);
    g_signal_connect (P->root, "map", G_CALLBACK (on_map), P);
    g_signal_connect (P->root, "unmap", G_CALLBACK (on_unmap), P);
    return P->root;
}

void walpe_perf_panel_set_path (GtkWidget *panel, const char *path)
{
    Perf *P = panel ? g_object_get_data (G_OBJECT (panel), "walpe-perf") : NULL;
    if (!P || !P->per_wp)
        return;
    if (g_strcmp0 (P->path, path)) {
        /* a number changed a moment ago still waits to be sent (on_spin):
         * it belongs to the wallpaper it was typed for, so it goes out
         * now (it used to go to the wallpaper selected next) */
        if (P->scale_id) {
            g_source_remove (P->scale_id);
            scale_fire (P);
        }
        if (P->idle_id) {
            g_source_remove (P->idle_id);
            idle_fire (P);
        }
        g_free (P->path);
        P->path = g_strdup (path);
        P->have = FALSE;
        /* nothing of the wallpaper shown before stays on the panel while
         * the answer for this one is on its way (or never comes: no
         * engine) */
        gtk_expander_set_label (GTK_EXPANDER (P->expander), "Quality for this wallpaper");
        gtk_label_set_text (GTK_LABEL (P->cost), "…");
        gtk_label_set_text (GTK_LABEL (P->estimate), "…");
    }
    gtk_widget_set_visible (P->root, path != NULL);
    if (path && gtk_widget_get_mapped (P->root))
        perf_request (P);
}
