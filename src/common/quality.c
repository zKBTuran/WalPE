/* quality.c - see quality.h */
#include "quality.h"
#include "common/pace.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- presets */

/* Max = byte for byte the picture from before the knobs: every knob inert
 * and exact texture sizes.
 * High (the default) = every knob of this file inert; reduced textures
 * and effect sizes may be magnified by 2 % (texture_slack).
 * Balanced costs nothing that is easy to see: effects classified slow are
 * redrawn every 2nd frame, effect passes work at the size the layer is
 * shown at, blur and bloom on smaller buffers.
 * Low takes what is visible too: every animated effect at half rate,
 * cosmetic post passes off, 3/4 render resolution, half-size textures,
 * 30 frames per second at most. */
static const WalpeQuality presets[4] = {
    [WALPE_PRESET_MAX] = { .fx_rate = 1, .fx_rate_all = 0, .fps_idle = 0, .post = WALPE_POST_FULL,
                           .render_scale = 100, .tex_res = WALPE_TEXRES_FULL,
                           .fx_res = WALPE_FXRES_FULL, .tex_slack = 0 },
    [WALPE_PRESET_HIGH] = { .fx_rate = 1, .fx_rate_all = 0, .fps_idle = 0, .post = WALPE_POST_FULL,
                            .render_scale = 100, .tex_res = WALPE_TEXRES_FULL,
                            .fx_res = WALPE_FXRES_FULL, .tex_slack = WALPE_TEX_SLACK_DEFAULT },
    [WALPE_PRESET_BALANCED] = { .fx_rate = 2, .fx_rate_all = 0, .fps_idle = 0,
                                .post = WALPE_POST_REDUCED, .render_scale = 100,
                                .tex_res = WALPE_TEXRES_FULL, .fx_res = WALPE_FXRES_SCREEN,
                                .tex_slack = WALPE_TEX_SLACK_DEFAULT },
    [WALPE_PRESET_LOW] = { .fx_rate = 2, .fx_rate_all = 1, .fps_idle = 30, .post = WALPE_POST_OFF,
                           .render_scale = 75, .tex_res = WALPE_TEXRES_HALF,
                           .fx_res = WALPE_FXRES_SCREEN, .tex_slack = WALPE_TEX_SLACK_DEFAULT },
};

void walpe_quality_preset (WalpePreset p, WalpeQuality *q)
{
    *q = presets[p >= WALPE_PRESET_MAX && p <= WALPE_PRESET_LOW ? p : WALPE_PRESET_HIGH];
}

gboolean walpe_quality_equal (const WalpeQuality *a, const WalpeQuality *b)
{
    return a->fx_rate == b->fx_rate && !a->fx_rate_all == !b->fx_rate_all &&
           a->fps_idle == b->fps_idle && a->post == b->post &&
           a->render_scale == b->render_scale && a->tex_res == b->tex_res &&
           a->fx_res == b->fx_res && a->tex_slack == b->tex_slack;
}

WalpePreset walpe_quality_preset_of (const WalpeQuality *q)
{
    for (int p = WALPE_PRESET_MAX; p <= WALPE_PRESET_LOW; p++) {
        WalpeQuality v = presets[p];
        /* with the divisor off its mode says nothing */
        if (q->fx_rate <= 1 && v.fx_rate <= 1)
            v.fx_rate_all = q->fx_rate_all;
        if (walpe_quality_equal (q, &v))
            return (WalpePreset) p;
    }
    return WALPE_PRESET_CUSTOM;
}

static const char *const preset_names[] = { "max", "high", "balanced", "low", "custom" };
static const char *const preset_labels[] = { "Maximum (exact sizes)", "High (default)", "Balanced",
                                             "Low", "Custom" };
/* what the viewer gives up, in plain words */
static const char *const preset_costs[] = {
    "Nothing: every frame is drawn exactly, every effect in every frame, textures and "
    "effects at their exact sizes. Needs the most GPU work and video memory.",
    "Next to nothing: every effect is drawn in every frame at full quality; a reduced "
    "texture or effect may be drawn up to 2 % enlarged, so fine detail can sit a fraction of "
    "a pixel elsewhere than at Maximum.",
    "Slow effects (slow pulsing, swaying, drifting light) are redrawn only every second frame "
    "and can move in slightly coarser steps; effects are computed at the size the layer has on "
    "screen instead of a little larger; blur and glow are computed on smaller buffers and look "
    "a little softer. Fast effects, video and layers without effects stay as they are.",
    "Every animated effect is redrawn only every second frame, so fast effects (blinking, "
    "shaking, flicker) visibly stutter; purely decorative full-screen passes (bloom, film "
    "grain, vignette, colour fringes) are left out; the whole picture is drawn at 75 % of the "
    "monitor's resolution and layer images at half size, so everything is softer; the "
    "wallpaper runs at 30 frames per second at most, so all motion is less smooth.",
    "Your own mix of the settings below.",
};

/* Settings of other parts that the global preset sets with it. One row
 * each: the `walpe-ctl set` key, the type of its value, the value for max,
 * high, balanced, low, and what the viewer gives up with that value. The
 * daemon passes the rows on with the preset (ipc_set), nothing else is
 * needed; a build without the key ignores the row and its sentence. All of
 * them are settings of the whole daemon: a wallpaper's own preset does not
 * touch them. Decoder choices (video_hwdec) do not belong here: not a
 * quality level.
 *
 * Particles (particle_lowres off | auto | all, particle_density 5..100,
 * particle_instancing): max = the exact path (no low-resolution buffer,
 * no instancing), high = their defaults, balanced = every system on the
 * low-resolution buffer, low = half of the particles as well.
 * Effect shaders (fx_specialize on |
 * off, fx_quality exact | fast; both reload the scenes): balanced =
 * constants compiled in (within 1/255 of the plain shaders), low = fused
 * displacement runs and cheaper blur / light rays as well. */
static const WalpePresetExtra preset_extras[] = {
    { "particle_density", 'i', { "100", "100", "100", "50" },
      { NULL, NULL, NULL, "Only half of the particles are drawn." } },
    { "particle_lowres", 's', { "off", "auto", "all", "all" },
      { NULL, "Large soft particles may be drawn at reduced resolution.",
        "The particles, soft and sharp alike, are drawn at reduced resolution and look softer.",
        "The particles, soft and sharp alike, are drawn at reduced resolution and look softer." } },
    { "particle_instancing", 'b', { "0", "1", "1", "1" }, { NULL, NULL, NULL, NULL } },
    /* oversized layer pictures stored reduced to the size they are shown
     * at (config tex_downscale, "Downscale oversized textures"): off with
     * max only, which is the exact picture (two scenes of the test
     * library come out softer than version 0.12 with it on); read when a scene
     * loads, and max reloads the scenes for its texture slack anyway */
    { "tex_downscale", 'b', { "0", "1", "1", "1" }, { NULL, NULL, NULL, NULL } },
    { "fx_specialize", 'b', { "0", "0", "1", "1" },
      { NULL, NULL,
        "Effect shaders get their fixed settings compiled in: the same picture to within one "
        "colour step in 255.", NULL } },
    /* simulation effects (engine/fx_sim.h; Wallpaper Engine's Fluid
     * Simulation): the smoke's picture at half size from balanced on, and
     * half the step rate at low. The speed and the shape of the flow stay. */
    { "fluid_quality", 's', { "full", "full", "reduced", "low" },
      { NULL, NULL,
        "Simulated smoke (Fluid Simulation) is computed at half size and looks softer.",
        "Simulated smoke (Fluid Simulation) is computed at half size and in 30 instead of 60 "
        "steps a second: softer, and it moves in coarser steps." } },
    { "fx_quality", 's', { "exact", "exact", "exact", "fast" },
      { NULL, NULL, NULL,
        "Stacked wave, shake and ripple effects of a layer are drawn in one pass, blur and "
        "light rays read fewer texels: moving edges can sit up to a pixel elsewhere, blur and "
        "light rays are slightly less smooth." } },
};

/* which rows this build has: its config.json carries the key */
guint walpe_config_has_keys (struct WalpeConfig_ *cfg, const char *const *keys, guint n);

static guint extras_known (struct WalpeConfig_ *cfg)
{
    static guint mask;
    static gsize once;
    if (!cfg)
        return ~0u;
    if (g_once_init_enter (&once)) {
        const char *keys[G_N_ELEMENTS (preset_extras)];
        for (guint i = 0; i < G_N_ELEMENTS (preset_extras); i++)
            keys[i] = preset_extras[i].key;
        mask = walpe_config_has_keys (cfg, keys, G_N_ELEMENTS (preset_extras));
        g_once_init_leave (&once, 1);
    }
    return mask;
}

const WalpePresetExtra *walpe_preset_extras (guint *n)
{
    if (n)
        *n = G_N_ELEMENTS (preset_extras);
    return preset_extras;
}

void walpe_preset_extras_into (WalpePreset p, JsonObject *req)
{
    if (!req || p < WALPE_PRESET_MAX || p > WALPE_PRESET_LOW)
        return;
    for (guint i = 0; i < G_N_ELEMENTS (preset_extras); i++) {
        const WalpePresetExtra *x = &preset_extras[i];
        if (json_object_has_member (req, x->key))
            continue;   /* said explicitly in the same request */
        if (x->type == 'i')
            json_object_set_int_member (req, x->key, atoi (x->value[p]));
        else if (x->type == 'b')
            json_object_set_boolean_member (req, x->key, atoi (x->value[p]) != 0);
        else
            json_object_set_string_member (req, x->key, x->value[p]);
    }
}

const char *walpe_preset_name (WalpePreset p)
{
    return preset_names[p >= WALPE_PRESET_MAX && p <= WALPE_PRESET_CUSTOM ? p
                                                                          : WALPE_PRESET_CUSTOM];
}

const char *walpe_preset_label (WalpePreset p)
{
    return preset_labels[p >= WALPE_PRESET_MAX && p <= WALPE_PRESET_CUSTOM ? p
                                                                           : WALPE_PRESET_CUSTOM];
}

const char *walpe_preset_cost (WalpePreset p)
{
    return preset_costs[p >= WALPE_PRESET_MAX && p <= WALPE_PRESET_CUSTOM ? p
                                                                          : WALPE_PRESET_CUSTOM];
}

char *walpe_preset_cost_full (struct WalpeConfig_ *cfg, WalpePreset p)
{
    GString *s = g_string_new (walpe_preset_cost (p));
    guint known = extras_known (cfg);
    for (guint i = 0; i < G_N_ELEMENTS (preset_extras); i++)
        if (p >= WALPE_PRESET_MAX && p <= WALPE_PRESET_LOW && (known & (1u << i)) &&
            preset_extras[i].cost[p])
            g_string_append_printf (s, " %s", preset_extras[i].cost[p]);
    return g_string_free (s, FALSE);
}

const char *walpe_quality_picture_note (void)
{
    return "Can change the picture: effect update rate (held effects move in steps), effect "
           "resolution, post-processing, render scale, texture resolution (all softer) and "
           "texture slack (at most 2 % enlargement). Cannot change any single frame, only how "
           "many are drawn: the frame rate limits (fps, fps_idle, the focus and battery limits).";
}

gboolean walpe_preset_parse (const char *s, WalpePreset *p)
{
    for (int i = 0; s && i < (int) G_N_ELEMENTS (preset_names); i++)
        if (!g_ascii_strcasecmp (s, preset_names[i])) {
            *p = (WalpePreset) i;
            return TRUE;
        }
    /* Wallpaper Engine's names for the same idea */
    if (s && (!g_ascii_strcasecmp (s, "maximum") || !g_ascii_strcasecmp (s, "ultra") ||
              !g_ascii_strcasecmp (s, "exact"))) {
        *p = WALPE_PRESET_MAX;
        return TRUE;
    }
    if (s && !g_ascii_strcasecmp (s, "default")) {
        *p = WALPE_PRESET_HIGH;
        return TRUE;
    }
    if (s && !g_ascii_strcasecmp (s, "medium")) {
        *p = WALPE_PRESET_BALANCED;
        return TRUE;
    }
    return FALSE;
}

static int fps_fix (int v)
{
    return v <= 0 ? 0 : CLAMP (v, WALPE_QUALITY_FPS_MIN, WALPE_QUALITY_FPS_MAX);
}

void walpe_quality_sanitize (WalpeQuality *q)
{
    q->fx_rate = CLAMP (q->fx_rate, 1, 3);
    q->fx_rate_all = q->fx_rate_all ? 1 : 0;
    q->fps_idle = fps_fix (q->fps_idle);
    q->tex_slack = q->tex_slack > 0 ? WALPE_TEX_SLACK_DEFAULT : 0;
    q->post = CLAMP (q->post, WALPE_POST_FULL, WALPE_POST_OFF);
    q->render_scale = q->render_scale <= 0 ? 100
                                           : CLAMP (q->render_scale, WALPE_RENDER_SCALE_MIN, 100);
    q->tex_res = CLAMP (q->tex_res, WALPE_TEXRES_FULL, WALPE_TEXRES_QUARTER);
    q->fx_res = CLAMP (q->fx_res, WALPE_FXRES_FULL, WALPE_FXRES_HALF);
}

/* ---------------------------------------------------------------- keys */

typedef enum { QK_INT, QK_FPS, QK_ENUM, QK_SLACK } QKind;

typedef struct {
    const char *key;        /* config.json / IPC */
    const char *alias;      /* short name (walpe-ctl), or NULL */
    guint bit;
    QKind kind;
    int min, max;           /* QK_INT */
    const char *names[5];   /* QK_ENUM: value i is names[i] (NULL ends) */
    size_t off;
    const char *allowed;    /* for error texts */
} QKey;

static const QKey qkeys[] = {
    { "fx_rate", NULL, WALPE_Q_FX_RATE, QK_INT, 1, 3, { NULL },
      offsetof (WalpeQuality, fx_rate), "1 (every frame), 2 or 3" },
    { "fx_rate_mode", NULL, WALPE_Q_FX_RATE_ALL, QK_ENUM, 0, 1, { "slow", "all", NULL },
      offsetof (WalpeQuality, fx_rate_all), "slow or all" },
    { "fps_idle", NULL, WALPE_Q_FPS_IDLE, QK_FPS, 0, 0, { NULL },
      offsetof (WalpeQuality, fps_idle), "0 (no limit of its own) or 5..200" },
    { "post_processing", "post", WALPE_Q_POST, QK_ENUM, 0, 2, { "full", "reduced", "off", NULL },
      offsetof (WalpeQuality, post), "full, reduced or off" },
    { "render_scale", "scale", WALPE_Q_RENDER_SCALE, QK_INT, WALPE_RENDER_SCALE_MIN, 100, { NULL },
      offsetof (WalpeQuality, render_scale), "25..100 (percent)" },
    { "tex_res", "tex", WALPE_Q_TEX_RES, QK_ENUM, 0, 2, { "full", "half", "quarter", NULL },
      offsetof (WalpeQuality, tex_res), "full, half or quarter" },
    { "fx_res", NULL, WALPE_Q_FX_RES, QK_ENUM, 0, 3, { "full", "screen", "reduced", "half" },
      offsetof (WalpeQuality, fx_res), "full, screen, reduced or half" },
    { "texture_slack", "slack", WALPE_Q_TEX_SLACK, QK_SLACK, 0, WALPE_TEX_SLACK_DEFAULT, { NULL },
      offsetof (WalpeQuality, tex_slack), "0 (exact sizes) or 2 (percent, the default)" },
};

static int *qfield (WalpeQuality *q, const QKey *k)
{
    return (int *) ((char *) q + k->off);
}

static const QKey *qkey_find (const char *name)
{
    for (guint i = 0; name && i < G_N_ELEMENTS (qkeys); i++)
        if (!strcmp (name, qkeys[i].key) || (qkeys[i].alias && !strcmp (name, qkeys[i].alias)))
            return &qkeys[i];
    return NULL;
}

void walpe_quality_to_json (JsonBuilder *b, const WalpeQuality *q, guint mask, gboolean with_preset)
{
    if (with_preset) {
        json_builder_set_member_name (b, "quality_preset");
        json_builder_add_string_value (b, walpe_preset_name (walpe_quality_preset_of (q)));
    }
    for (guint i = 0; i < G_N_ELEMENTS (qkeys); i++) {
        const QKey *k = &qkeys[i];
        if (!(mask & k->bit))
            continue;
        int v = *qfield ((WalpeQuality *) q, k);
        json_builder_set_member_name (b, k->key);
        if (k->kind == QK_ENUM)
            json_builder_add_string_value (b, k->names[CLAMP (v, 0, k->max)]);
        else
            json_builder_add_int_value (b, v);
    }
}

/* a whole number from a JSON value or a string of digits; FALSE = neither */
static gboolean node_int (JsonNode *n, int *out)
{
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return FALSE;
    GType t = json_node_get_value_type (n);
    double d;
    if (t == G_TYPE_INT64) {
        d = (double) json_node_get_int (n);
    } else if (t == G_TYPE_DOUBLE) {
        d = json_node_get_double (n);
    } else if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        char *end = NULL;
        if (!s || !*s || strlen (s) > 12)
            return FALSE;
        d = g_ascii_strtod (s, &end);
        if (!end || *end)
            return FALSE;
        /* "50%" is not a number; "5e1", "inf", "0x10" are not wanted */
        if (strspn (s, "0123456789") != strlen (s))
            return FALSE;
    } else {
        return FALSE;
    }
    if (!(d >= -1e6 && d <= 1e6) || d != floor (d))
        return FALSE;
    *out = (int) d;
    return TRUE;
}

static gboolean node_is_default (JsonNode *n)
{
    if (!n || JSON_NODE_HOLDS_NULL (n))
        return TRUE;
    if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        return s && (!g_ascii_strcasecmp (s, "default") || !g_ascii_strcasecmp (s, "inherit"));
    }
    return FALSE;
}

/* one key's value from a JSON node; FALSE = not allowed */
static gboolean qkey_read (const QKey *k, JsonNode *n, int *out)
{
    int v = 0;
    switch (k->kind) {
    case QK_INT:
        if (!node_int (n, &v) || v < k->min || v > k->max)
            return FALSE;
        break;
    case QK_FPS:
        if (!node_int (n, &v) || (v != 0 && (v < WALPE_QUALITY_FPS_MIN || v > WALPE_QUALITY_FPS_MAX)))
            return FALSE;
        break;
    case QK_SLACK:
        /* 0 or 2, also on / off */
        if (n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING &&
            (!g_ascii_strcasecmp (json_node_get_string (n), "on") ||
             !g_ascii_strcasecmp (json_node_get_string (n), "off")))
            v = g_ascii_strcasecmp (json_node_get_string (n), "on") ? 0 : WALPE_TEX_SLACK_DEFAULT;
        else if (!node_int (n, &v) || (v != 0 && v != WALPE_TEX_SLACK_DEFAULT))
            return FALSE;
        break;
    case QK_ENUM: {
        if (!n || !JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING)
            return FALSE;
        const char *s = json_node_get_string (n);
        gboolean found = FALSE;
        for (int i = 0; s && i <= k->max && k->names[i]; i++)
            if (!g_ascii_strcasecmp (s, k->names[i])) {
                v = i;
                found = TRUE;
            }
        if (!found)
            return FALSE;
        break;
    }
    }
    *out = v;
    return TRUE;
}

static char *node_text (JsonNode *n)
{
    if (!n)
        return g_strdup ("nothing");
    JsonGenerator *g = json_generator_new ();
    json_generator_set_root (g, n);
    char *s = json_generator_to_data (g, NULL);
    g_object_unref (g);
    if (s && strlen (s) > 40) {
        /* cut between two characters: with the cut inside a multi-byte
         * one the error text, and with it the whole reply line, was not
         * UTF-8 and no client could read it */
        gsize n = 37;
        while (n > 0 && ((guchar) s[n] & 0xC0) == 0x80)
            n--;
        memcpy (s + n, "...", 4);
    }
    return s;
}

static gboolean quality_read (JsonObject *o, WalpeQuality *q, guint *set, guint *unset,
                              gboolean lenient, char **err)
{
    WalpeQuality w = *q;
    guint s = 0, u = 0;
    if (err)
        *err = NULL;
    if (!o) {
        if (set)
            *set = 0;
        if (unset)
            *unset = 0;
        return TRUE;
    }
    /* the preset first: single keys in the same object then adjust it */
    const char *pk = json_object_has_member (o, "quality_preset") ? "quality_preset"
                   : json_object_has_member (o, "preset") ? "preset" : NULL;
    if (pk) {
        JsonNode *pn = json_object_get_member (o, pk);
        WalpePreset p = WALPE_PRESET_CUSTOM;
        const char *ps = pn && JSON_NODE_HOLDS_VALUE (pn) &&
                         json_node_get_value_type (pn) == G_TYPE_STRING
                             ? json_node_get_string (pn) : NULL;
        if (unset && node_is_default (pn)) {
            u = WALPE_Q_ALL;
        } else if (ps && walpe_preset_parse (ps, &p)) {
            if (p != WALPE_PRESET_CUSTOM) {   /* "custom" = keep the knobs as they are */
                walpe_quality_preset (p, &w);
                s = WALPE_Q_ALL;
            }
        } else if (!lenient) {
            char *t = node_text (pn);
            if (err)
                *err = g_strdup_printf ("%s: max, high, balanced or low (not %s)", pk, t);
            g_free (t);
            return FALSE;
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS (qkeys); i++) {
        const QKey *k = &qkeys[i];
        const char *name = json_object_has_member (o, k->key) ? k->key
                         : k->alias && json_object_has_member (o, k->alias) ? k->alias : NULL;
        if (!name)
            continue;
        JsonNode *n = json_object_get_member (o, name);
        int v = 0;
        if (unset && node_is_default (n)) {
            u |= k->bit;
            s &= ~k->bit;
        } else if (qkey_read (k, n, &v)) {
            *qfield (&w, k) = v;
            s |= k->bit;
            u &= ~k->bit;
        } else if (!lenient) {
            char *t = node_text (n);
            if (err)
                *err = g_strdup_printf ("%s: %s (not %s)", name, k->allowed, t);
            g_free (t);
            return FALSE;
        }
    }
    *q = w;
    if (set)
        *set = s;
    if (unset)
        *unset = u;
    return TRUE;
}

gboolean walpe_quality_from_json (JsonObject *o, WalpeQuality *q, guint *set, guint *unset,
                                  char **err)
{
    return quality_read (o, q, set, unset, FALSE, err);
}

gboolean walpe_quality_kv_to_json (JsonBuilder *b, const char *key, const char *value,
                                   gboolean *taken, char **err)
{
    gboolean preset = !g_strcmp0 (key, "preset") || !g_strcmp0 (key, "quality_preset");
    const QKey *k = preset ? NULL : qkey_find (key);
    if (err)
        *err = NULL;
    if (taken)
        *taken = preset || k != NULL;
    if (!preset && !k)
        return TRUE;
    if (!value)
        value = "";
    /* check through the same reader the daemon uses */
    JsonObject *probe = json_object_new ();
    gboolean dflt = !g_ascii_strcasecmp (value, "default") || !g_ascii_strcasecmp (value, "inherit");
    const char *name = preset ? "quality_preset" : k->key;
    if (dflt)
        json_object_set_null_member (probe, name);
    else
        json_object_set_string_member (probe, name, value);
    WalpeQuality q;
    guint s = 0, u = 0;
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    gboolean ok = quality_read (probe, &q, &s, &u, FALSE, err);
    json_object_unref (probe);
    if (!ok)
        return FALSE;
    json_builder_set_member_name (b, name);
    if (dflt)
        json_builder_add_null_value (b);
    else if (preset || k->kind == QK_ENUM)
        json_builder_add_string_value (b, value);
    else
        json_builder_add_int_value (b, *qfield (&q, k));
    return TRUE;
}

/* config.json: values that are not allowed are skipped, the rest is used */
void walpe_quality_load_json (JsonObject *o, WalpeQuality *q, guint *set)
{
    guint s = 0;
    gboolean has_knob = FALSE;
    for (guint i = 0; o && i < G_N_ELEMENTS (qkeys); i++)
        has_knob |= json_object_has_member (o, qkeys[i].key);
    if (o && has_knob && json_object_has_member (o, "quality_preset")) {
        /* a saved file holds both: the knobs are what counts, the preset
         * name is only their label (a hand-written file with the name
         * alone gets the preset) */
        JsonObject *c = json_object_new ();
        for (guint i = 0; i < G_N_ELEMENTS (qkeys); i++)
            if (json_object_has_member (o, qkeys[i].key))
                json_object_set_member (c, qkeys[i].key,
                                        json_node_copy (json_object_get_member (o, qkeys[i].key)));
        quality_read (c, q, &s, NULL, TRUE, NULL);
        json_object_unref (c);
    } else {
        quality_read (o, q, &s, NULL, TRUE, NULL);
    }
    walpe_quality_sanitize (q);
    if (set)
        *set = s;
}

void walpe_quality_effective (const WalpeQuality *global, const WalpeQualityOv *ov,
                              WalpeQuality *out)
{
    *out = *global;
    for (guint i = 0; ov && i < G_N_ELEMENTS (qkeys); i++)
        if (ov->mask & qkeys[i].bit)
            *qfield (out, &qkeys[i]) = *qfield ((WalpeQuality *) &ov->q, &qkeys[i]);
    walpe_quality_sanitize (out);
}

/* ------------------------------------------------- frame rate by state */

int walpe_quality_fps (const WalpeQuality *q, const WalpeFpsState *st, int cap)
{
    int fps = MAX (cap, 1);
    /* every rule can only lower the rate: a focused application or battery
     * power never makes the wallpaper run faster than the idle desktop */
    if (q->fps_idle > 0)
        fps = MIN (fps, q->fps_idle);
    if (st && st->focus_app && st->focus_fps > 0)
        fps = MIN (fps, st->focus_fps);
    if (st && st->on_battery && st->battery_fps > 0)
        fps = MIN (fps, st->battery_fps);
    return MAX (fps, 1);
}

const char *walpe_fps_state_name (const WalpeFpsState *st)
{
    gboolean f = st && st->focus_app, b = st && st->on_battery;
    return f ? (b ? "focus+battery" : "focus") : (b ? "battery" : "idle");
}

/* ----------------------------------------------------------- estimates */

/* effect pass pixels with q: the effect resolution scales every pass,
 * post-processing takes its share of the rest, then the update divisor
 * holds what it may of what is left. In "slow" mode a set
 * that tolerates /3 is in both px_hold[0] and px_hold[1]; one that only
 * tolerates /2 is held at /2 when 3 is asked for. */
static double fx_after (double fx, const double hold[4], double post_red, double post_off,
                        double res_cut, const WalpeQuality *q)
{
    double post = q->post == WALPE_POST_OFF ? post_off : q->post == WALPE_POST_REDUCED ? post_red : 0;
    post = CLAMP (post, 0.0, fx);
    double left = fx - post;
    if (fx > 0)
        left *= 1.0 - CLAMP (res_cut, 0.0, fx) / fx;
    if (q->fx_rate >= 2 && fx > 0) {
        int base = q->fx_rate_all ? 2 : 0;
        double share = left / fx;   /* what post left of the held sets, pro rata */
        double h2 = MIN (hold[base], fx) * share, h3 = MIN (hold[base + 1], hold[base]) * share;
        if (q->fx_rate == 2)
            left -= h2 * 0.5;
        else
            left -= h3 * (2.0 / 3.0) + MAX (h2 - h3, 0.0) * 0.5;
    }
    return MAX (left, 0.0);
}

double walpe_quality_estimate_frame (const WalpeQualityStats *st, const WalpeQuality *q)
{
    double fx = fx_after (st->px_fx, st->px_hold, st->px_post_red, st->px_post_off,
                          q->fx_res > WALPE_FXRES_FULL && q->fx_res <= WALPE_FXRES_HALF
                              ? st->px_fxres[q->fx_res - 1] : 0.0, q);
    double s = CLAMP (q->render_scale, WALPE_RENDER_SCALE_MIN, 100) / 100.0;
    double px = (st->px_base + fx) * s * s;
    if (q->render_scale < 100)
        px += (double) st->w * st->h;   /* the scaled-up copy to the window */
    return px;
}

double walpe_quality_estimate_passes (const WalpeQualityStats *st, const WalpeQuality *q)
{
    /* "reduced" keeps every pass (smaller buffers); "off" drops passes */
    return fx_after (st->passes, st->passes_hold, 0.0, st->passes_post_off, 0.0, q);
}

double walpe_quality_estimate (const WalpeQualityStats *st, const WalpeQuality *q,
                               const WalpeFpsState *fs, int cap, double hz)
{
    int fps = walpe_paced_fps (walpe_quality_fps (q, fs, cap), hz);
    return walpe_quality_estimate_frame (st, q) * fps;
}

/* ------------------------------------------------------------ requests */
#include "common/config.h"

static guint quality_diff (const WalpeQuality *a, const WalpeQuality *b)
{
    guint m = 0;
    for (guint i = 0; i < G_N_ELEMENTS (qkeys); i++)
        if (*qfield ((WalpeQuality *) a, &qkeys[i]) != *qfield ((WalpeQuality *) b, &qkeys[i]))
            m |= qkeys[i].bit;
    return m;
}

gboolean walpe_quality_ipc_set (WalpeConfig *cfg, JsonObject *req, guint *given, guint *changed,
                                char **err)
{
    WalpeQuality q = cfg->quality;
    guint set = 0;
    if (given)
        *given = 0;
    if (changed)
        *changed = 0;
    if (!walpe_quality_from_json (req, &q, &set, NULL, err))
        return FALSE;
    walpe_quality_sanitize (&q);
    if (given)
        *given = set;
    if (changed)
        *changed = quality_diff (&cfg->quality, &q);
    cfg->quality = q;
    return TRUE;
}

gboolean walpe_quality_ipc_set_wallpaper (WalpeConfig *cfg, const char *path, JsonObject *values,
                                          gboolean reset, guint *changed, char **err)
{
    if (err)
        *err = NULL;
    if (changed)
        *changed = 0;
    if (!path || !*path) {
        if (err)
            *err = g_strdup ("no wallpaper");
        return FALSE;
    }
    WalpeQuality before, after;
    walpe_config_quality_for (cfg, path, &before);
    const WalpeQualityOv *cur = walpe_config_quality_ov (cfg, path);
    WalpeQualityOv ov = { 0 };
    if (cur && !reset)
        ov = *cur;
    else
        ov.q = cfg->quality;
    /* a preset given for one wallpaper starts from that preset, a single
     * key from what the wallpaper has now */
    guint set = 0, unset = 0;
    WalpeQuality w = ov.q;
    for (guint i = 0; i < G_N_ELEMENTS (qkeys); i++)   /* unset knobs: show the global value */
        if (!(ov.mask & qkeys[i].bit))
            *qfield (&w, &qkeys[i]) = *qfield (&cfg->quality, &qkeys[i]);
    if (!walpe_quality_from_json (values, &w, &set, &unset, err))
        return FALSE;
    ov.q = w;
    ov.mask = (ov.mask | set) & ~unset;
    walpe_config_set_quality_ov (cfg, path, &ov);
    walpe_config_quality_for (cfg, path, &after);
    if (changed)
        *changed = quality_diff (&before, &after);
    return TRUE;
}

void walpe_quality_ipc_describe (WalpeConfig *cfg, JsonBuilder *b)
{
    walpe_quality_to_json (b, &cfg->quality, WALPE_Q_ALL, TRUE);
    json_builder_set_member_name (b, "presets");
    json_builder_begin_object (b);
    for (int p = WALPE_PRESET_MAX; p <= WALPE_PRESET_LOW; p++) {
        WalpeQuality v;
        walpe_quality_preset ((WalpePreset) p, &v);
        json_builder_set_member_name (b, walpe_preset_name ((WalpePreset) p));
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "label");
        json_builder_add_string_value (b, walpe_preset_label ((WalpePreset) p));
        char *cost = walpe_preset_cost_full (cfg, (WalpePreset) p);
        json_builder_set_member_name (b, "cost");
        json_builder_add_string_value (b, cost);
        g_free (cost);
        /* a wallpaper's own preset: these knobs only */
        json_builder_set_member_name (b, "cost_own");
        json_builder_add_string_value (b, walpe_preset_cost ((WalpePreset) p));
        /* what the global preset sets outside these knobs, in this build */
        json_builder_set_member_name (b, "also");
        json_builder_begin_object (b);
        for (guint x = 0; x < G_N_ELEMENTS (preset_extras); x++) {
            if (!(extras_known (cfg) & (1u << x)))
                continue;
            json_builder_set_member_name (b, preset_extras[x].key);
            if (preset_extras[x].type == 'i')
                json_builder_add_int_value (b, atoi (preset_extras[x].value[p]));
            else if (preset_extras[x].type == 'b')
                json_builder_add_boolean_value (b, atoi (preset_extras[x].value[p]) != 0);
            else
                json_builder_add_string_value (b, preset_extras[x].value[p]);
        }
        json_builder_end_object (b);
        walpe_quality_to_json (b, &v, WALPE_Q_ALL, FALSE);
        json_builder_end_object (b);
    }
    json_builder_end_object (b);
    json_builder_set_member_name (b, "wallpapers");
    json_builder_begin_object (b);
    if (cfg->wallpaper_quality) {
        GList *ks = g_list_sort (g_hash_table_get_keys (cfg->wallpaper_quality),
                                 (GCompareFunc) strcmp);
        for (GList *k = ks; k; k = k->next) {
            const WalpeQualityOv *ov = g_hash_table_lookup (cfg->wallpaper_quality, k->data);
            if (!ov || !ov->mask)
                continue;
            json_builder_set_member_name (b, k->data);
            json_builder_begin_object (b);
            walpe_quality_to_json (b, &ov->q, ov->mask, FALSE);
            if (ov->mask == WALPE_Q_ALL) {
                json_builder_set_member_name (b, "quality_preset");
                json_builder_add_string_value (b, walpe_preset_name (walpe_quality_preset_of (&ov->q)));
            }
            json_builder_end_object (b);
        }
        g_list_free (ks);
    }
    json_builder_end_object (b);
}
