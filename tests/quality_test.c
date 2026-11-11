/* quality_test.c - the quality-for-load knobs (common/quality.h,
 * engine/fxclass.h), no display and no daemon needed:
 * presets and their names, reading / writing the knobs as JSON, the
 * config.json round trip with per-wallpaper settings, the requests the
 * daemon answers (set / setquality / quality) over a real control socket,
 * the frame rate chosen for the desktop's state (the daemon's focus and
 * battery rules as inputs), the saving estimates, and the effect
 * classification (family by name, speed from shader source). */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include "common/config.h"
#include "common/ipc.h"
#include "common/library.h"
#include "common/quality.h"
#include "common/util.h"
#include "engine/fxclass.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *tmp, *cfg_path;

static JsonObject *obj (const char *json)
{
    JsonNode *n = wpj_parse_data (json, -1);
    g_assert (n && JSON_NODE_HOLDS_OBJECT (n));
    JsonObject *o = json_object_ref (json_node_get_object (n));
    json_node_unref (n);
    return o;
}

/* ------------------------------------------------------------- presets */

static void test_presets (void)
{
    WalpeQuality q, h, x;
    walpe_quality_preset (WALPE_PRESET_HIGH, &h);
    /* Max = every knob inert and exact texture sizes: what was drawn
     * before the knobs existed. High (the default) differs in the texture
     * slack only. */
    walpe_quality_preset (WALPE_PRESET_MAX, &x);
    CHECK (x.fx_rate == 1 && x.fps_idle == 0 && x.post == WALPE_POST_FULL && x.render_scale == 100 &&
           x.tex_res == WALPE_TEXRES_FULL && x.fx_res == WALPE_FXRES_FULL && x.tex_slack == 0);
    CHECK (walpe_quality_preset_of (&x) == WALPE_PRESET_MAX);
    q = x;
    q.tex_slack = 2;
    CHECK (walpe_quality_equal (&q, &h) && !walpe_quality_equal (&x, &h));
    CHECK (h.fx_rate == 1 && h.fps_idle == 0 && h.tex_slack == 2);
    CHECK (h.post == WALPE_POST_FULL && h.render_scale == 100 && h.tex_res == WALPE_TEXRES_FULL);
    CHECK (h.fx_res == WALPE_FXRES_FULL);
    CHECK (walpe_quality_preset_of (&h) == WALPE_PRESET_HIGH);
    walpe_quality_preset (WALPE_PRESET_BALANCED, &q);
    CHECK (q.fx_rate == 2 && !q.fx_rate_all && q.post == WALPE_POST_REDUCED);
    CHECK (q.fps_idle == 0 && q.tex_slack == 2);
    CHECK (q.render_scale == 100 && q.tex_res == WALPE_TEXRES_FULL && q.fx_res == WALPE_FXRES_SCREEN);
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_BALANCED);
    walpe_quality_preset (WALPE_PRESET_LOW, &q);
    CHECK (q.fx_rate == 2 && q.fx_rate_all && q.post == WALPE_POST_OFF);
    CHECK (q.fps_idle == 30 && q.tex_slack == 2);
    CHECK (q.render_scale == 75 && q.tex_res == WALPE_TEXRES_HALF && q.fx_res == WALPE_FXRES_SCREEN);
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_LOW);
    q.render_scale = 50;
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_CUSTOM);
    walpe_quality_preset (WALPE_PRESET_BALANCED, &q);
    q.fx_res = WALPE_FXRES_HALF;
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_CUSTOM);
    /* with the divisor off, its mode does not make High "custom" */
    q = h;
    q.fx_rate_all = 1;
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_HIGH);
    /* custom is not a preset: asking for it gives High's values */
    walpe_quality_preset (WALPE_PRESET_CUSTOM, &q);
    CHECK (walpe_quality_equal (&q, &h));

    WalpePreset p;
    CHECK (walpe_preset_parse ("balanced", &p) && p == WALPE_PRESET_BALANCED);
    CHECK (walpe_preset_parse ("LOW", &p) && p == WALPE_PRESET_LOW);
    CHECK (walpe_preset_parse ("max", &p) && p == WALPE_PRESET_MAX);
    CHECK (walpe_preset_parse ("High", &p) && p == WALPE_PRESET_HIGH);
    CHECK (walpe_preset_parse ("ultra", &p) && p == WALPE_PRESET_MAX);      /* WE's names */
    CHECK (walpe_preset_parse ("medium", &p) && p == WALPE_PRESET_BALANCED);
    CHECK (!walpe_preset_parse ("fast", &p) && !walpe_preset_parse (NULL, &p));
    CHECK (!strcmp (walpe_preset_name (WALPE_PRESET_CUSTOM), "custom"));
    CHECK (!strcmp (walpe_preset_label (WALPE_PRESET_HIGH), "High (default)"));
    CHECK (!strcmp (walpe_preset_name (WALPE_PRESET_MAX), "max"));
    /* every preset says in plain words what it costs; so does the knob list */
    for (int i = WALPE_PRESET_MAX; i <= WALPE_PRESET_LOW; i++)
        CHECK (strlen (walpe_preset_cost ((WalpePreset) i)) > 20);
    CHECK (strstr (walpe_preset_cost (WALPE_PRESET_BALANCED), "every second frame") &&
           strstr (walpe_preset_cost (WALPE_PRESET_LOW), "stutter"));
    {
        /* the global preset's sentence adds what its extras cost; a row
         * has a value for each preset and is named only where it costs */
        guint n = 0;
        const WalpePresetExtra *x = walpe_preset_extras (&n);
        CHECK (n >= 6);
        gboolean spec = FALSE, fast = FALSE, exact = FALSE;
        for (guint i = 0; i < n; i++) {
            for (int p = 0; p < 4; p++)
                CHECK (x[i].value[p] && *x[i].value[p]);
            CHECK (!x[i].cost[WALPE_PRESET_MAX]);
            if (!strcmp (x[i].key, "fx_specialize"))
                spec = !strcmp (x[i].value[WALPE_PRESET_MAX], "0") &&
                       !strcmp (x[i].value[WALPE_PRESET_HIGH], "0") &&
                       !strcmp (x[i].value[WALPE_PRESET_BALANCED], "1");
            /* max is the exact picture: oversized layer pictures are not
             * stored reduced; every other preset keeps the default */
            if (!strcmp (x[i].key, "tex_downscale"))
                exact = x[i].type == 'b' && !strcmp (x[i].value[WALPE_PRESET_MAX], "0") &&
                        !strcmp (x[i].value[WALPE_PRESET_HIGH], "1") &&
                        !strcmp (x[i].value[WALPE_PRESET_BALANCED], "1") &&
                        !strcmp (x[i].value[WALPE_PRESET_LOW], "1") &&
                        !x[i].cost[WALPE_PRESET_HIGH] && !x[i].cost[WALPE_PRESET_LOW];
            if (!strcmp (x[i].key, "fx_quality"))
                fast = !strcmp (x[i].value[WALPE_PRESET_HIGH], "exact") &&
                       !strcmp (x[i].value[WALPE_PRESET_BALANCED], "exact") &&
                       !strcmp (x[i].value[WALPE_PRESET_LOW], "fast");
        }
        CHECK (spec && fast && exact);
        {
            /* as members of a request: max switches the downscale off,
             * high switches it on again; a value said in the request stays */
            JsonObject *rq = json_object_new ();
            walpe_preset_extras_into (WALPE_PRESET_MAX, rq);
            CHECK (json_object_has_member (rq, "tex_downscale") &&
                   !json_object_get_boolean_member (rq, "tex_downscale"));
            json_object_unref (rq);
            rq = json_object_new ();
            walpe_preset_extras_into (WALPE_PRESET_HIGH, rq);
            CHECK (json_object_get_boolean_member (rq, "tex_downscale"));
            json_object_unref (rq);
            rq = json_object_new ();
            json_object_set_boolean_member (rq, "tex_downscale", TRUE);
            walpe_preset_extras_into (WALPE_PRESET_MAX, rq);
            CHECK (json_object_get_boolean_member (rq, "tex_downscale"));
            json_object_unref (rq);
        }
        char *mx = walpe_preset_cost_full (NULL, WALPE_PRESET_MAX);
        char *lo = walpe_preset_cost_full (NULL, WALPE_PRESET_LOW);
        char *ba = walpe_preset_cost_full (NULL, WALPE_PRESET_BALANCED);
        CHECK (!strcmp (mx, walpe_preset_cost (WALPE_PRESET_MAX)));
        CHECK (strstr (lo, "half of the particles") && strstr (lo, "30 frames per second") &&
               strstr (lo, "75 %"));
        CHECK (strstr (ba, "soft and sharp") && !strstr (ba, "half of the particles"));
        CHECK (!strstr (walpe_preset_cost (WALPE_PRESET_LOW), "particles"));
        g_free (mx);
        g_free (lo);
        g_free (ba);
    }
    CHECK (strstr (walpe_quality_picture_note (), "Can change the picture") &&
           strstr (walpe_quality_picture_note (), "Cannot change"));

    q = h;
    q.fx_rate = 9; q.fps_idle = 3; q.post = 7; q.tex_slack = 7;
    q.render_scale = 10; q.tex_res = -1; q.fx_res = 9;
    walpe_quality_sanitize (&q);
    CHECK (q.fx_rate == 3 && q.fps_idle == 5 && q.tex_slack == 2);
    q.fps_idle = 999; q.tex_slack = -1;
    walpe_quality_sanitize (&q);
    CHECK (q.fps_idle == 200 && q.tex_slack == 0);
    q.fps_idle = -4;
    walpe_quality_sanitize (&q);
    CHECK (q.fps_idle == 0);
    CHECK (q.post == WALPE_POST_OFF && q.render_scale == 25 && q.tex_res == 0);
    CHECK (q.fx_res == WALPE_FXRES_HALF);
}

/* ---------------------------------------------------------------- JSON */

static void test_json (void)
{
    WalpeQuality q;
    guint set = 0, unset = 0;
    char *err = NULL;
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    JsonObject *o = obj ("{\"fx_rate\":2,\"fx_rate_mode\":\"all\",\"fps_idle\":30,"
                         "\"post_processing\":\"reduced\",\"render_scale\":75,\"tex_res\":\"quarter\","
                         "\"fx_res\":\"reduced\",\"fps\":144,\"unrelated\":true}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && !err);
    CHECK (q.fx_rate == 2 && q.fx_rate_all == 1 && q.fps_idle == 30 && q.tex_slack == 2);
    CHECK (q.post == WALPE_POST_REDUCED && q.render_scale == 75 && q.tex_res == WALPE_TEXRES_QUARTER);
    CHECK (q.fx_res == WALPE_FXRES_REDUCED);
    CHECK (set == (WALPE_Q_FX_RATE | WALPE_Q_FX_RATE_ALL | WALPE_Q_FPS_IDLE | WALPE_Q_POST |
                   WALPE_Q_RENDER_SCALE | WALPE_Q_TEX_RES | WALPE_Q_FX_RES));
    json_object_unref (o);

    /* walpe-ctl sends words: numbers as strings, short names */
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    o = obj ("{\"fx_rate\":\"3\",\"post\":\"OFF\",\"scale\":\"50\",\"tex\":\"half\",\"fps_idle\":\"20\","
             "\"slack\":\"off\"}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err));
    CHECK (q.fx_rate == 3 && q.post == WALPE_POST_OFF && q.render_scale == 50 &&
           q.tex_res == WALPE_TEXRES_HALF && q.fps_idle == 20 && q.tex_slack == 0);
    json_object_unref (o);

    /* a preset, then one knob on top of it in the same request */
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    o = obj ("{\"quality_preset\":\"low\",\"render_scale\":100}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err));
    CHECK (set == WALPE_Q_ALL && q.fx_rate_all && q.post == WALPE_POST_OFF && q.render_scale == 100);
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_CUSTOM);
    json_object_unref (o);
    o = obj ("{\"preset\":\"custom\"}");   /* keeps the knobs */
    WalpeQuality before = q;
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && set == 0 &&
           walpe_quality_equal (&q, &before));
    json_object_unref (o);

    /* not allowed: nothing changes, the text says what is */
    static const char *const bad[] = {
        "{\"fx_rate\":4}", "{\"fx_rate\":0}", "{\"fx_rate\":1.5}", "{\"fx_rate\":\"two\"}",
        "{\"fx_rate\":true}", "{\"fx_rate\":[2]}", "{\"fx_rate\":1e300}", "{\"fx_rate_mode\":\"fast\"}",
        "{\"fx_rate_mode\":1}", "{\"fps_idle\":3}", "{\"fps_idle\":201}", "{\"fps_idle\":-1}",
        "{\"texture_slack\":1}", "{\"texture_slack\":10}", "{\"texture_slack\":\"some\"}",
        "{\"post_processing\":\"none\"}", "{\"render_scale\":24}", "{\"render_scale\":101}",
        "{\"render_scale\":\"75%\"}", "{\"tex_res\":\"eighth\"}", "{\"quality_preset\":\"turbo\"}",
        "{\"quality_preset\":3}", "{\"fx_rate\":2,\"tex_res\":\"x\"}", "{\"fx_rate\":null}",
        "{\"tex_res\":\"default\"}", "{\"fx_res\":\"quarter\"}", "{\"fx_res\":2}",
        "{\"fx_res\":null}",
    };
    for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
        walpe_quality_preset (WALPE_PRESET_BALANCED, &q);
        before = q;
        o = obj (bad[i]);
        err = NULL;
        gboolean ok = walpe_quality_from_json (o, &q, &set, NULL, &err);
        if (ok || !err || !walpe_quality_equal (&q, &before)) {
            fails++;
            g_printerr ("FAIL: %s was accepted (or changed something)\n", bad[i]);
        }
        g_free (err);
        json_object_unref (o);
    }
    /* 0 = no limit of its own; the limits of the range */
    walpe_quality_preset (WALPE_PRESET_LOW, &q);
    o = obj ("{\"fps_idle\":0,\"render_scale\":25,\"texture_slack\":0}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err));
    CHECK (q.fps_idle == 0 && q.render_scale == 25 && q.tex_slack == 0);
    json_object_unref (o);
    o = obj ("{\"fps_idle\":5,\"texture_slack\":\"on\"}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && q.fps_idle == 5 && q.tex_slack == 2);
    json_object_unref (o);
    o = obj ("{\"fps_idle\":200,\"texture_slack\":2}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && q.fps_idle == 200 &&
           set == (WALPE_Q_FPS_IDLE | WALPE_Q_TEX_SLACK));
    json_object_unref (o);
    /* the keys this file no longer owns (the daemon's focus / battery
     * rules) are not looked at */
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    o = obj ("{\"fps_focus\":30,\"fps_battery\":20,\"focus_fps\":15,\"battery_fps\":20}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && set == 0 &&
           walpe_quality_preset_of (&q) == WALPE_PRESET_HIGH);
    json_object_unref (o);
    /* the max preset: exact sizes; one key takes high there too */
    o = obj ("{\"quality_preset\":\"max\"}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && set == WALPE_Q_ALL &&
           q.tex_slack == 0 && walpe_quality_preset_of (&q) == WALPE_PRESET_MAX);
    json_object_unref (o);
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    o = obj ("{\"texture_slack\":0}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) &&
           walpe_quality_preset_of (&q) == WALPE_PRESET_MAX);
    json_object_unref (o);
    /* effect resolution "full" does not ask for exact sizes */
    walpe_quality_preset (WALPE_PRESET_BALANCED, &q);
    o = obj ("{\"fx_res\":\"full\"}");
    CHECK (walpe_quality_from_json (o, &q, &set, NULL, &err) && q.fx_res == WALPE_FXRES_FULL &&
           q.tex_slack == 2);
    json_object_unref (o);
    walpe_quality_preset (WALPE_PRESET_LOW, &q);

    /* per wallpaper: null / "default" = follow the global setting */
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    o = obj ("{\"fx_rate\":2,\"post_processing\":null,\"tex_res\":\"default\"}");
    CHECK (walpe_quality_from_json (o, &q, &set, &unset, &err));
    CHECK (set == WALPE_Q_FX_RATE && unset == (WALPE_Q_POST | WALPE_Q_TEX_RES));
    json_object_unref (o);

    /* writing: every knob by its key, the preset's name with it */
    walpe_quality_preset (WALPE_PRESET_LOW, &q);
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    walpe_quality_to_json (b, &q, WALPE_Q_ALL, TRUE);
    json_builder_end_object (b);
    JsonNode *root = json_builder_get_root (b);
    JsonObject *w = json_node_get_object (root);
    CHECK (!g_strcmp0 (wpj_str (w, "quality_preset", ""), "low"));
    CHECK (wpj_num (w, "fx_rate", 0) == 2 && !g_strcmp0 (wpj_str (w, "fx_rate_mode", ""), "all"));
    CHECK (wpj_num (w, "fps_idle", 0) == 30 && wpj_num (w, "texture_slack", -1) == 2 &&
           !json_object_has_member (w, "fps_focus") && !json_object_has_member (w, "fps_battery"));
    CHECK (!g_strcmp0 (wpj_str (w, "post_processing", ""), "off") &&
           wpj_num (w, "render_scale", 0) == 75 && !g_strcmp0 (wpj_str (w, "tex_res", ""), "half"));
    CHECK (!g_strcmp0 (wpj_str (w, "fx_res", ""), "screen"));
    /* ... and what was written reads back as the same knobs */
    WalpeQuality r;
    walpe_quality_preset (WALPE_PRESET_HIGH, &r);
    CHECK (walpe_quality_from_json (w, &r, &set, NULL, &err) && walpe_quality_equal (&r, &q));
    json_node_unref (root);
    g_object_unref (b);

    /* walpe-ctl words -> typed members */
    b = json_builder_new ();
    json_builder_begin_object (b);
    gboolean taken = FALSE;
    CHECK (walpe_quality_kv_to_json (b, "post", "off", &taken, &err) && taken);
    CHECK (walpe_quality_kv_to_json (b, "fx_rate", "2", &taken, &err) && taken);
    CHECK (walpe_quality_kv_to_json (b, "preset", "balanced", &taken, &err) && taken);
    CHECK (walpe_quality_kv_to_json (b, "fps_idle", "default", &taken, &err) && taken);
    CHECK (walpe_quality_kv_to_json (b, "texture_slack", "0", &taken, &err) && taken);
    CHECK (walpe_quality_kv_to_json (b, "fps_focus", "30", &taken, &err) && !taken);   /* the daemon's */
    CHECK (walpe_quality_kv_to_json (b, "fx_res", "half", &taken, &err) && taken);
    CHECK (walpe_quality_kv_to_json (b, "volume", "1", &taken, &err) && !taken);   /* not ours */
    CHECK (!walpe_quality_kv_to_json (b, "fx_rate", "7", &taken, &err) && taken && err);
    g_free (err);
    CHECK (!walpe_quality_kv_to_json (b, "tex_res", "", &taken, &err) && taken && err);
    g_free (err);
    json_builder_end_object (b);
    root = json_builder_get_root (b);
    w = json_node_get_object (root);
    CHECK (!g_strcmp0 (wpj_str (w, "post_processing", ""), "off"));
    CHECK (json_node_get_value_type (json_object_get_member (w, "fx_rate")) == G_TYPE_INT64);
    CHECK (JSON_NODE_HOLDS_NULL (json_object_get_member (w, "fps_idle")));
    CHECK (wpj_num (w, "texture_slack", -1) == 0);
    json_node_unref (root);
    g_object_unref (b);
}

/* -------------------------------------------------------------- config */

static void put_config (const char *json)
{
    g_assert (g_file_set_contents (cfg_path, json, -1, NULL));
}

static void test_config (void)
{
    /* a config from before the knobs: High, no per-wallpaper entries */
    put_config ("{\"config_version\":3,\"fps\":60,\"current\":\"/w/a\"}");
    WalpeConfig *c = walpe_config_load ();
    CHECK (walpe_quality_preset_of (&c->quality) == WALPE_PRESET_HIGH);
    CHECK (!c->wallpaper_quality || g_hash_table_size (c->wallpaper_quality) == 0);
    WalpeQuality q;
    walpe_config_quality_for (c, "/w/a", &q);
    CHECK (walpe_quality_preset_of (&q) == WALPE_PRESET_HIGH);

    /* global knobs + one wallpaper's own, saved and loaded */
    walpe_quality_preset (WALPE_PRESET_BALANCED, &c->quality);
    c->quality.render_scale = 80;
    WalpeQualityOv ov = { .mask = WALPE_Q_POST | WALPE_Q_FPS_IDLE };
    walpe_quality_preset (WALPE_PRESET_HIGH, &ov.q);
    ov.q.post = WALPE_POST_OFF;
    ov.q.fps_idle = 10;
    walpe_config_set_quality_ov (c, "/w/a", &ov);
    WalpeQualityOv all = { .mask = WALPE_Q_ALL };
    walpe_quality_preset (WALPE_PRESET_LOW, &all.q);
    walpe_config_set_quality_ov (c, "/w/with \"quotes\" and \\ back", &all);
    walpe_config_save (c);
    walpe_config_free (c);

    c = walpe_config_load ();
    CHECK (c->quality.fx_rate == 2 && !c->quality.fx_rate_all && c->quality.fps_idle == 0 &&
           c->quality.tex_slack == 2 && c->quality.post == WALPE_POST_REDUCED &&
           c->quality.render_scale == 80 && c->quality.tex_res == 0);
    CHECK (walpe_quality_preset_of (&c->quality) == WALPE_PRESET_CUSTOM);
    const WalpeQualityOv *g = walpe_config_quality_ov (c, "/w/a");
    CHECK (g && g->mask == (WALPE_Q_POST | WALPE_Q_FPS_IDLE) && g->q.post == WALPE_POST_OFF &&
           g->q.fps_idle == 10);
    walpe_config_quality_for (c, "/w/a", &q);   /* its own on top of the global ones */
    CHECK (q.post == WALPE_POST_OFF && q.fps_idle == 10 && q.fx_rate == 2 && q.render_scale == 80 &&
           q.tex_slack == 2);
    g = walpe_config_quality_ov (c, "/w/with \"quotes\" and \\ back");
    CHECK (g && g->mask == WALPE_Q_ALL && walpe_quality_preset_of (&g->q) == WALPE_PRESET_LOW);
    walpe_config_quality_for (c, "/w/other", &q);   /* no entry: the global setting */
    CHECK (walpe_quality_equal (&q, &c->quality));
    CHECK (walpe_config_quality_ov (c, "/w/other") == NULL && walpe_config_quality_ov (c, "") == NULL);
    /* the file carries the label too */
    char *txt = NULL;
    g_assert (g_file_get_contents (cfg_path, &txt, NULL, NULL));
    CHECK (strstr (txt, "\"quality_preset\" : \"custom\"") != NULL);
    CHECK (strstr (txt, "\"wallpaper_quality\"") != NULL);
    g_free (txt);

    /* a mask of 0 removes the entry; forgetting a deleted wallpaper too */
    WalpeQualityOv none = { 0 };
    walpe_config_set_quality_ov (c, "/w/a", &none);
    CHECK (walpe_config_quality_ov (c, "/w/a") == NULL);
    CHECK (walpe_config_forget_path (c, "/w/with \"quotes\" and \\ back") >= 1);
    CHECK (walpe_config_quality_ov (c, "/w/with \"quotes\" and \\ back") == NULL);
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (!c->wallpaper_quality || g_hash_table_size (c->wallpaper_quality) == 0);
    walpe_config_free (c);

    /* hand-edited values that are not allowed are skipped one by one */
    put_config ("{\"fx_rate\":9,\"fx_rate_mode\":\"all\",\"fps_idle\":\"x\",\"texture_slack\":0,"
                "\"post_processing\":\"off\",\"render_scale\":5,\"tex_res\":7,\"fx_res\":\"tiny\","
                "\"wallpaper_quality\":{\"/w/a\":{\"fx_rate\":3,\"tex_res\":\"nope\"},"
                "\"/w/b\":{\"junk\":1},\"/w/c\":7,\"\":{\"fx_rate\":2}}}");
    c = walpe_config_load ();
    CHECK (c->quality.fx_rate == 1 && c->quality.fx_rate_all == 1 && c->quality.fps_idle == 0 &&
           c->quality.tex_slack == 0 && c->quality.post == WALPE_POST_OFF &&
           c->quality.render_scale == 25 /* this key is clamped, as before the knobs */ &&
           c->quality.tex_res == 0 && c->quality.fx_res == WALPE_FXRES_FULL);
    /* the two keys with config fields of their own agree with the knobs */
    CHECK (c->render_scale == c->quality.render_scale && c->texture_slack == c->quality.tex_slack);
    g = walpe_config_quality_ov (c, "/w/a");
    CHECK (g && g->mask == WALPE_Q_FX_RATE && g->q.fx_rate == 3);
    CHECK (!walpe_config_quality_ov (c, "/w/b") && !walpe_config_quality_ov (c, "/w/c"));
    CHECK (g_hash_table_size (c->wallpaper_quality) == 1);
    walpe_config_free (c);

    /* a hand-written file with only the preset's name gets the preset; a
     * saved one (name + knobs) follows the knobs */
    put_config ("{\"quality_preset\":\"low\"}");
    c = walpe_config_load ();
    CHECK (walpe_quality_preset_of (&c->quality) == WALPE_PRESET_LOW);
    walpe_config_free (c);
    put_config ("{\"quality_preset\":\"low\",\"fx_rate\":3,\"fx_rate_mode\":\"slow\"}");
    c = walpe_config_load ();
    CHECK (c->quality.fx_rate == 3 && !c->quality.fx_rate_all && c->quality.post == WALPE_POST_FULL &&
           c->quality.render_scale == 100);
    walpe_config_free (c);
    put_config ("{\"quality_preset\":\"nonsense\",\"wallpaper_quality\":[1,2]}");
    c = walpe_config_load ();
    CHECK (walpe_quality_preset_of (&c->quality) == WALPE_PRESET_HIGH);
    walpe_config_free (c);
}

/* ------------------------------------------------- requests over a socket */

static GMainLoop *loop;
static char *sock;
static WalpeConfig *srv_cfg;
static guint last_changed;

static void jb_fail (JsonBuilder *b, const char *e)
{
    json_builder_set_member_name (b, "ok");
    json_builder_add_boolean_value (b, FALSE);
    json_builder_set_member_name (b, "error");
    json_builder_add_string_value (b, e ? e : "?");
}

/* what the daemon does with these three commands (src/daemon/main.c),
 * minus the monitors: the shared functions on a config, saved after a
 * change */
static char *handler (const char *req, gpointer user)
{
    (void) user;
    JsonNode *root = wpj_parse_data (req, -1);
    if (!root || !JSON_NODE_HOLDS_OBJECT (root))
        return g_strdup ("{\"ok\":false,\"error\":\"bad json\"}");
    JsonObject *o = json_node_get_object (root);
    const char *cmd = wpj_str (o, "cmd", "");
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    char *err = NULL;
    if (!strcmp (cmd, "set")) {
        guint given = 0;
        if (!walpe_quality_ipc_set (srv_cfg, o, &given, &last_changed, &err)) {
            jb_fail (b, err);
        } else if (!given) {
            jb_fail (b, "nothing to set");
        } else {
            walpe_config_save (srv_cfg);
            json_builder_set_member_name (b, "ok");
            json_builder_add_boolean_value (b, TRUE);
        }
    } else if (!strcmp (cmd, "setquality")) {
        if (!walpe_quality_ipc_set_wallpaper (srv_cfg, wpj_str (o, "path", NULL),
                                              wpj_obj (o, "values"), wpj_bool (o, "reset", FALSE),
                                              &last_changed, &err)) {
            jb_fail (b, err);
        } else {
            walpe_config_save (srv_cfg);
            json_builder_set_member_name (b, "ok");
            json_builder_add_boolean_value (b, TRUE);
        }
    } else if (!strcmp (cmd, "quality")) {
        json_builder_set_member_name (b, "ok");
        json_builder_add_boolean_value (b, TRUE);
        json_builder_set_member_name (b, "quality");
        json_builder_begin_object (b);
        walpe_quality_ipc_describe (srv_cfg, b);
        json_builder_end_object (b);
    } else {
        jb_fail (b, "unknown cmd");
    }
    g_free (err);
    json_builder_end_object (b);
    JsonNode *rn = json_builder_get_root (b);
    JsonGenerator *g = json_generator_new ();
    json_generator_set_root (g, rn);
    char *out = json_generator_to_data (g, NULL);
    g_object_unref (g);
    json_node_unref (rn);
    g_object_unref (b);
    json_node_unref (root);
    return out;
}

static JsonObject *ask (const char *req)
{
    char *r = walpe_ipc_request (sock, req, 3000);
    if (!r) {
        fails++;
        g_printerr ("FAIL: no reply to %s\n", req);
        return json_object_new ();
    }
    JsonObject *o = obj (r);
    g_free (r);
    return o;
}

static gpointer client_thread (gpointer u)
{
    (void) u;
    JsonObject *r, *q;

    /* a preset, read back */
    r = ask ("{\"cmd\":\"set\",\"quality_preset\":\"balanced\"}");
    CHECK (wpj_bool (r, "ok", FALSE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"quality\"}");
    q = wpj_obj (r, "quality");
    CHECK (q && !g_strcmp0 (wpj_str (q, "quality_preset", ""), "balanced"));
    CHECK (q && wpj_num (q, "fx_rate", 0) == 2 && !g_strcmp0 (wpj_str (q, "fx_rate_mode", ""), "slow") &&
           wpj_num (q, "fps_idle", -1) == 0 && wpj_num (q, "texture_slack", -1) == 2 &&
           !g_strcmp0 (wpj_str (q, "fx_res", ""), "screen") &&
           !g_strcmp0 (wpj_str (q, "post_processing", ""), "reduced"));
    /* the presets themselves are listed for the GUI */
    JsonObject *ps = q ? wpj_obj (q, "presets") : NULL;
    CHECK (ps && wpj_obj (ps, "max") && wpj_obj (ps, "high") && wpj_obj (ps, "balanced") &&
           wpj_obj (ps, "low") && json_object_get_size (ps) == 4);
    CHECK (ps && wpj_num (wpj_obj (ps, "max"), "texture_slack", -1) == 0 &&
           wpj_num (wpj_obj (ps, "high"), "texture_slack", -1) == 2 &&
           strlen (wpj_str (wpj_obj (ps, "low"), "cost", "")) > 20);
    CHECK (ps && wpj_num (wpj_obj (ps, "low"), "render_scale", 0) == 75 &&
           !g_strcmp0 (wpj_str (wpj_obj (ps, "high"), "label", ""), "High (default)"));
    json_object_unref (r);

    /* single knobs as walpe-ctl sends them; the label follows */
    r = ask ("{\"cmd\":\"set\",\"fx_rate\":\"3\",\"post\":\"off\",\"render_scale\":50}");
    CHECK (wpj_bool (r, "ok", FALSE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"quality\"}");
    q = wpj_obj (r, "quality");
    CHECK (q && !g_strcmp0 (wpj_str (q, "quality_preset", ""), "custom") &&
           wpj_num (q, "fx_rate", 0) == 3 && wpj_num (q, "render_scale", 0) == 50 &&
           !g_strcmp0 (wpj_str (q, "post_processing", ""), "off") &&
           !g_strcmp0 (wpj_str (q, "fx_res", ""), "screen"));
    json_object_unref (r);

    /* a value that is not allowed: refused with a reason, nothing changed */
    r = ask ("{\"cmd\":\"set\",\"render_scale\":75,\"tex_res\":\"tiny\"}");
    CHECK (!wpj_bool (r, "ok", TRUE) && strstr (wpj_str (r, "error", ""), "tex_res"));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"quality\"}");
    CHECK (wpj_num (wpj_obj (r, "quality"), "render_scale", 0) == 50);
    json_object_unref (r);
    r = ask ("{\"cmd\":\"set\",\"fps\":60}");   /* no quality key in it */
    CHECK (!wpj_bool (r, "ok", TRUE));
    json_object_unref (r);

    /* one wallpaper: a preset, then one knob back to "as global" */
    r = ask ("{\"cmd\":\"setquality\",\"path\":\"/lib/123\",\"values\":{\"preset\":\"low\"}}");
    CHECK (wpj_bool (r, "ok", FALSE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"setquality\",\"path\":\"/lib/123\",\"values\":{\"render_scale\":null,"
             "\"fps_idle\":\"default\",\"tex_res\":\"quarter\"}}");
    CHECK (wpj_bool (r, "ok", FALSE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"setquality\",\"path\":\"/lib/456\",\"values\":{\"post_processing\":\"full\"}}");
    CHECK (wpj_bool (r, "ok", FALSE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"quality\"}");
    JsonObject *ws = wpj_obj (wpj_obj (r, "quality"), "wallpapers");
    JsonObject *w1 = ws ? wpj_obj (ws, "/lib/123") : NULL, *w2 = ws ? wpj_obj (ws, "/lib/456") : NULL;
    CHECK (w1 && !json_object_has_member (w1, "render_scale") &&
           !json_object_has_member (w1, "fps_idle") && wpj_num (w1, "texture_slack", -1) == 2 &&
           !g_strcmp0 (wpj_str (w1, "tex_res", ""), "quarter") &&
           !g_strcmp0 (wpj_str (w1, "fx_rate_mode", ""), "all"));
    CHECK (w2 && json_object_get_size (w2) == 1 &&
           !g_strcmp0 (wpj_str (w2, "post_processing", ""), "full"));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"setquality\",\"path\":\"/lib/123\",\"values\":{\"fx_rate\":5}}");
    CHECK (!wpj_bool (r, "ok", TRUE) && strstr (wpj_str (r, "error", ""), "fx_rate"));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"setquality\",\"values\":{\"fx_rate\":2}}");   /* which wallpaper? */
    CHECK (!wpj_bool (r, "ok", TRUE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"setquality\",\"path\":\"/lib/456\",\"reset\":true}");
    CHECK (wpj_bool (r, "ok", FALSE));
    json_object_unref (r);
    r = ask ("{\"cmd\":\"quality\"}");
    ws = wpj_obj (wpj_obj (r, "quality"), "wallpapers");
    CHECK (ws && wpj_obj (ws, "/lib/123") && !json_object_has_member (ws, "/lib/456"));
    json_object_unref (r);

    g_main_loop_quit (loop);
    return NULL;
}

static void test_ipc (void)
{
    put_config ("{}");
    srv_cfg = walpe_config_load ();
    sock = g_build_filename (tmp, "q.sock", NULL);
    loop = g_main_loop_new (NULL, FALSE);
    g_assert (walpe_ipc_serve (sock, handler, NULL));
    GThread *t = g_thread_new ("client", client_thread, NULL);
    g_main_loop_run (loop);
    g_thread_join (t);
    walpe_ipc_release (sock);
    g_main_loop_unref (loop);

    /* what changed last: the reset of /lib/456 took its post "full" away,
     * the global "off" shows again */
    CHECK (last_changed == WALPE_Q_POST);
    /* everything the requests set is in the file a restarted daemon reads */
    walpe_config_free (srv_cfg);
    WalpeConfig *c = walpe_config_load ();
    CHECK (c->quality.fx_rate == 3 && c->quality.post == WALPE_POST_OFF &&
           c->quality.render_scale == 50 && c->quality.fps_idle == 0 &&
           c->quality.fx_res == WALPE_FXRES_SCREEN);
    WalpeQuality q;
    walpe_config_quality_for (c, "/lib/123", &q);
    CHECK (q.fx_rate == 2 && q.fx_rate_all && q.fps_idle == 0 /* global again */ &&
           q.tex_slack == 2 && q.post == WALPE_POST_OFF &&
           q.render_scale == 50 /* global again */ && q.tex_res == WALPE_TEXRES_QUARTER);
    CHECK (walpe_config_quality_ov (c, "/lib/456") == NULL);
    {
        /* which keys this build's config.json carries: the presets name
         * and describe only extras that exist */
        const char *keys[] = { "quality_preset", "no_such_key", "fps" };
        CHECK (walpe_config_has_keys (c, keys, 3) == 5u);
        g_autoptr (JsonBuilder) jb = json_builder_new ();
        json_builder_begin_object (jb);
        walpe_quality_ipc_describe (c, jb);
        json_builder_end_object (jb);
        JsonNode *root = json_builder_get_root (jb);
        JsonObject *lo = wpj_obj (wpj_obj (json_node_get_object (root), "presets"), "low");
        JsonObject *also = lo ? wpj_obj (lo, "also") : NULL;
        CHECK (lo && also);
        if (lo && also) {
            const char *ky[] = { "particle_density", "fx_quality", "tex_downscale" };
            guint have = walpe_config_has_keys (c, ky, 3);
            /* "sets also" of max names the downscale switch where the
             * build's config has the key */
            JsonObject *mx = wpj_obj (wpj_obj (json_node_get_object (root), "presets"), "max");
            JsonObject *mxa = mx ? wpj_obj (mx, "also") : NULL;
            CHECK (mxa && json_object_has_member (mxa, "tex_downscale") == ((have & 4u) != 0));
            if (mxa && (have & 4u))
                CHECK (!json_object_get_boolean_member (mxa, "tex_downscale"));
            CHECK (json_object_has_member (also, "no_such_key") == FALSE);
            CHECK (json_object_has_member (also, "particle_density") == ((have & 1u) != 0));
            CHECK (json_object_has_member (also, "fx_quality") == ((have & 2u) != 0));
            const char *cost = wpj_str (lo, "cost", ""), *own = wpj_str (lo, "cost_own", "");
            CHECK (!strcmp (own, walpe_preset_cost (WALPE_PRESET_LOW)));
            CHECK ((strstr (cost, "half of the particles") != NULL) == ((have & 1u) != 0));
            CHECK ((strstr (cost, "in one pass") != NULL) == ((have & 2u) != 0));
        }
        json_node_unref (root);
    }
    walpe_config_free (c);
}

/* ------------------------------------------------- frame rate by state */

static void test_fps (void)
{
    WalpeQuality x, h, b, l;
    walpe_quality_preset (WALPE_PRESET_MAX, &x);
    walpe_quality_preset (WALPE_PRESET_HIGH, &h);
    walpe_quality_preset (WALPE_PRESET_BALANCED, &b);
    walpe_quality_preset (WALPE_PRESET_LOW, &l);
    /* the daemon's signals and its own two rules, as it hands them over */
    const WalpeFpsState idle = { 0 };
    const WalpeFpsState focus = { .focus_app = TRUE, .focus_fps = 15 };
    const WalpeFpsState focus_run = { .focus_app = TRUE };              /* focus_action = run */
    const WalpeFpsState batt = { .on_battery = TRUE, .battery_fps = 20 };
    const WalpeFpsState batt_none = { .on_battery = TRUE };             /* battery_fps = 0 */
    const WalpeFpsState both = { .focus_app = TRUE, .focus_fps = 15, .on_battery = TRUE,
                                 .battery_fps = 20 };
    const WalpeFpsState mains = { .focus_fps = 15, .battery_fps = 20 }; /* rules set, none holds */
    /* max, high, balanced: no limit of their own - the cap, or the daemon's rules */
    CHECK (walpe_quality_fps (&h, NULL, 60) == 60 && walpe_quality_fps (&h, &idle, 144) == 144);
    CHECK (walpe_quality_fps (&x, &idle, 60) == 60 && walpe_quality_fps (&b, &idle, 60) == 60);
    CHECK (walpe_quality_fps (&h, &focus, 60) == 15 && walpe_quality_fps (&h, &focus_run, 60) == 60);
    CHECK (walpe_quality_fps (&h, &batt, 60) == 20 && walpe_quality_fps (&h, &batt_none, 60) == 60);
    CHECK (walpe_quality_fps (&h, &both, 60) == 15 && walpe_quality_fps (&h, &mains, 60) == 60);
    /* low: 30 while idle; a rule of the daemon only ever lowers it */
    CHECK (walpe_quality_fps (&l, NULL, 60) == 30 && walpe_quality_fps (&l, &idle, 200) == 30);
    CHECK (walpe_quality_fps (&l, &focus, 60) == 15 && walpe_quality_fps (&l, &batt, 60) == 20);
    /* a focused application never makes the wallpaper run faster than the idle desktop */
    CHECK (walpe_quality_fps (&l, &focus_run, 60) == 30);
    const WalpeFpsState focus_fast = { .focus_app = TRUE, .focus_fps = 50 };
    CHECK (walpe_quality_fps (&l, &focus_fast, 60) == 30);
    /* never above the cap, never below 1 */
    CHECK (walpe_quality_fps (&l, &idle, 10) == 10 && walpe_quality_fps (&h, &focus, 5) == 5);
    CHECK (walpe_quality_fps (&h, &idle, 0) == 1 && walpe_quality_fps (&h, NULL, -5) == 1);
    /* pure: the same inputs give the same answer */
    for (int i = 0; i < 3; i++)
        CHECK (walpe_quality_fps (&l, &both, 60) == 15);
    CHECK (!strcmp (walpe_fps_state_name (NULL), "idle") && !strcmp (walpe_fps_state_name (&idle), "idle") &&
           !strcmp (walpe_fps_state_name (&focus), "focus") &&
           !strcmp (walpe_fps_state_name (&batt), "battery") &&
           !strcmp (walpe_fps_state_name (&both), "focus+battery"));
}

static void rm_tree (const char *dir)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        if (g_file_test (p, G_FILE_TEST_IS_DIR))
            rm_tree (p);
        else
            g_remove (p);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (dir);
}

/* ------------------------------------------------------------ estimates */

static void test_estimate (void)
{
    WalpeQualityStats st = { .w = 1920, .h = 1080, .fps = 60, .measured = TRUE,
                             .px_base = 6e6, .px_fx = 24e6, .passes = 30,
                             .px_hold = { 8e6, 4e6, 20e6, 20e6 },
                             .passes_hold = { 9, 2, 17, 17 },
                             .px_fxres = { 6e6, 12e6, 18e6 },
                             .px_post_red = 0.6e6, .px_post_off = 2.6e6, .passes_post_off = 1 };
    WalpeQuality h, q;
    walpe_quality_preset (WALPE_PRESET_HIGH, &h);
    const double eps = 1.0;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &h) - 30e6) < eps);
    CHECK (fabs (walpe_quality_estimate_passes (&st, &h) - 30) < 1e-9);
    CHECK (fabs (walpe_quality_estimate (&st, &h, NULL, 60, 0) - 30e6 * 60) < 60 * eps);
    /* the monitor's refresh rate limits the frames too */
    CHECK (fabs (walpe_quality_estimate (&st, &h, NULL, 200, 144) - 30e6 * 144) < 144 * eps);
    /* every 2nd frame, slow sets: half of what they may hold */
    q = h;
    q.fx_rate = 2;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 4e6)) < eps);
    CHECK (fabs (walpe_quality_estimate_passes (&st, &q) - (30 - 4.5)) < 1e-9);
    /* every 3rd, slow: 2/3 of what tolerates 3, 1/2 of what only tolerates 2 */
    q.fx_rate = 3;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 4e6 * 2 / 3 - 4e6 / 2)) < eps);
    /* all */
    q.fx_rate = 2;
    q.fx_rate_all = 1;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 10e6)) < eps);
    q.fx_rate = 3;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 20e6 * 2 / 3)) < eps);
    /* post-processing */
    q = h;
    q.post = WALPE_POST_REDUCED;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 0.6e6)) < eps);
    CHECK (fabs (walpe_quality_estimate_passes (&st, &q) - 30) < 1e-9);   /* same passes, smaller */
    q.post = WALPE_POST_OFF;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 2.6e6)) < eps);
    CHECK (fabs (walpe_quality_estimate_passes (&st, &q) - 29) < 1e-9);
    /* effect resolution: the same passes on smaller buffers; an update
     * divisor then holds its share of what is left */
    q = h;
    q.fx_res = WALPE_FXRES_SCREEN;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 6e6)) < eps);
    CHECK (fabs (walpe_quality_estimate_passes (&st, &q) - 30) < 1e-9);
    q.fx_res = WALPE_FXRES_HALF;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 18e6)) < eps);
    q.fx_rate = 2;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 - 18e6 - 4e6 * 0.25)) < eps);
    /* render scale: the area, plus the copy to the window */
    q = h;
    q.render_scale = 50;
    CHECK (fabs (walpe_quality_estimate_frame (&st, &q) - (30e6 / 4 + 1920.0 * 1080)) < eps);
    /* the frame rate of the state multiplies (the daemon's rules: 30 while
     * an application is focused, 20 on battery) */
    walpe_quality_preset (WALPE_PRESET_BALANCED, &q);
    const WalpeFpsState states[4] = { { 0 }, { .focus_app = TRUE, .focus_fps = 30 },
                                      { .on_battery = TRUE, .battery_fps = 20 },
                                      { .focus_app = TRUE, .focus_fps = 30, .on_battery = TRUE,
                                        .battery_fps = 20 } };
    double f_idle = walpe_quality_estimate (&st, &q, &states[0], 60, 60);
    double f_focus = walpe_quality_estimate (&st, &q, &states[1], 60, 60);
    double f_batt = walpe_quality_estimate (&st, &q, &states[2], 60, 60);
    CHECK (fabs (f_focus / f_idle - 0.5) < 1e-9 && fabs (f_batt / f_idle - 1.0 / 3) < 1e-9);
    CHECK (fabs (walpe_quality_estimate (&st, &q, NULL, 60, 60) - f_idle) < 1e-6);
    /* max and high cost the same here (the texture slack is the texture
     * patch's saving, not estimated by this file); each preset below costs
     * less than the one above it, in every state */
    WalpeQuality l, x;
    walpe_quality_preset (WALPE_PRESET_LOW, &l);
    walpe_quality_preset (WALPE_PRESET_MAX, &x);
    for (guint s = 0; s < 4; s++) {
        double ex = walpe_quality_estimate (&st, &x, &states[s], 60, 60);
        double eh = walpe_quality_estimate (&st, &h, &states[s], 60, 60);
        double eb = walpe_quality_estimate (&st, &q, &states[s], 60, 60);
        double el = walpe_quality_estimate (&st, &l, &states[s], 60, 60);
        CHECK (ex == eh && eh > eb && eb > el && el > 0);
    }
    /* never negative, also with numbers that do not add up */
    WalpeQualityStats odd = { .w = 100, .h = 100, .px_base = 0, .px_fx = 1000,
                              .px_hold = { 5000, 5000, 5000, 5000 }, .px_post_off = 9000,
                              .px_post_red = 9000 };
    CHECK (walpe_quality_estimate_frame (&odd, &l) >= 0);
    WalpeQualityStats none = { .w = 100, .h = 100 };
    CHECK (walpe_quality_estimate_frame (&none, &l) >= 0 && walpe_quality_estimate_passes (&none, &l) == 0);
}

/* ------------------------------------------------- effect classification */

static gboolean val (const char *id, double *m, gpointer user)
{
    /* "name=value,name=value" */
    const char *tab = user;
    gsize n = strlen (id);
    for (const char *p = tab; p && *p; p = strchr (p, ',') ? strchr (p, ',') + 1 : NULL)
        if (!strncmp (p, id, n) && p[n] == '=') {
            *m = fabs (g_ascii_strtod (p + n + 1, NULL));
            return TRUE;
        }
    return FALSE;
}

static WalpeFxTime scan (const char *src, const char *values)
{
    WalpeFxTime t = { 0 };
    walpe_fx_time_scan (src, val, (gpointer) values, &t);
    return t;
}

static void test_fxclass (void)
{
    /* family by Wallpaper Engine effect name */
    CHECK (walpe_fx_family ("effects/blurprecise/effect.json") == WALPE_FXF_BLUR);
    CHECK (walpe_fx_family ("effects/blur/effect.json") == WALPE_FXF_BLUR);
    CHECK (walpe_fx_family ("effects/workshop/2798319181/bokeh_blur/effect.json") == WALPE_FXF_BLUR);
    CHECK (walpe_fx_family ("effects/workshop/2822917890/bloom/effect.json") == WALPE_FXF_BLOOM);
    CHECK (walpe_fx_family ("effects/filmgrain/effect.json") == WALPE_FXF_GRAIN);
    CHECK (walpe_fx_family ("effects/workshop/1/Film_Grain/effect.json") == WALPE_FXF_GRAIN);
    CHECK (walpe_fx_family ("effects/workshop/1/vignette/effect.json") == WALPE_FXF_VIGNETTE);
    CHECK (walpe_fx_family ("effects/chromaticaberration/effect.json") == WALPE_FXF_CHROMA);
    CHECK (walpe_fx_family ("effects/workshop/1/chromatic_aberration/effect.json") == WALPE_FXF_CHROMA);
    /* effects that carry content are in no family, whatever else is in the name */
    CHECK (walpe_fx_family ("effects/waterripple/effect.json") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family ("effects/opacity/effect.json") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family ("effects/workshop/1/cutout_vignette/effect.json") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family ("effects/workshop/1/blur_mask/effect.json") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family ("effects/motionblur/effect.json") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family ("effects/workshop/1/reveal_bloom/effect.json") == WALPE_FXF_NONE);
    /* the directory is the name: a "blur" elsewhere in the path is not */
    CHECK (walpe_fx_family ("effects/workshop/blur/tint/effect.json") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family (NULL) == WALPE_FXF_NONE && walpe_fx_family ("") == WALPE_FXF_NONE);
    CHECK (walpe_fx_family_cosmetic (WALPE_FXF_BLOOM) && walpe_fx_family_cosmetic (WALPE_FXF_GRAIN) &&
           walpe_fx_family_cosmetic (WALPE_FXF_VIGNETTE) && walpe_fx_family_cosmetic (WALPE_FXF_CHROMA));
    CHECK (!walpe_fx_family_cosmetic (WALPE_FXF_BLUR) && !walpe_fx_family_cosmetic (WALPE_FXF_NONE));
    CHECK (walpe_fx_family_reducible (WALPE_FXF_BLUR) && walpe_fx_family_reducible (WALPE_FXF_BLOOM) &&
           !walpe_fx_family_reducible (WALPE_FXF_GRAIN) && !walpe_fx_family_reducible (WALPE_FXF_NONE));
    CHECK (walpe_fx_name_fast ("effects/filmgrain/effect.json") &&
           walpe_fx_name_fast ("effects/vhs/effect.json") &&
           walpe_fx_name_fast ("effects/workshop/9/distortion_glitch/effect.json"));
    CHECK (!walpe_fx_name_fast ("effects/waterwaves/effect.json") &&
           !walpe_fx_name_fast ("effects/shake/effect.json"));

    /* speed from shader source: the ways effect shaders use the time (this
     * test's own statements; the helper names are the dialect's) */
    WalpeFxTime t;
    t = scan ("uniform float g_Time;\nvoid main () {\n"
              " pulse = smoothstep (lo, hi, sin (g_Time * g_PulseSpeed + (g_PulsePhase - 1.57)) * 0.5 + 0.5);\n}",
              "g_PulseSpeed=3");
    CHECK (t.uses_time && t.derived && fabs (t.rate - 3) < 1e-9);
    t = scan ("vec4 swing = phase + frac (g_Speed * g_Time / M_PI_2 * vec4 (1, -0.2, 0.01, -0.0005)) "
              "* M_PI_2;\nfloat beat = g_Speed * g_Time + phase;",
              "g_Speed=10");
    CHECK (t.derived && fabs (t.rate - 10) < 1e-6);
    t = scan ("float distance = g_Time * g_Speed + dot (c, v_Direction) * g_Scale;\n"
              "float distance2 = (g_Time + g_Offset2) * g_Speed2 + dot (c, d2) * g_Scale2;",
              "g_Speed=2,g_Speed2=5,g_Offset2=4,g_Scale=200");
    CHECK (t.derived && fabs (t.rate - 5) < 1e-9);
    t = scan ("wave.xy = spun + g_Time * g_RippleSpeed * g_RippleSpeed + drift;\n"
              "vec2 drift = rotateVec2 (vec2 (0, 1), g_Angle) * g_DriftSpeed * g_DriftSpeed * g_Time;",
              "g_RippleSpeed=0.15,g_DriftSpeed=0,g_Angle=1");
    CHECK (t.derived && fabs (t.rate - 0.0225) < 1e-9);   /* drift speed 0: that term is still */
    t = scan ("v_N.wz = vec2 (a.y, -a.x) * 0.633 + vec2 (-g_Time, g_Time) * 0.5 * g_NoiseSpeed;",
              "g_NoiseSpeed=0.3");
    CHECK (t.derived && fabs (t.rate - 0.15) < 1e-9);
    t = scan ("float n = texSample2D (g_Texture2, vec2 (g_Time * 0.08333333, g_Time * 0.02777777) "
              "* g_NoiseSpeed).r;", "g_NoiseSpeed=0.5");
    CHECK (t.derived && fabs (t.rate - 0.08333333 * 0.5) < 1e-9);
    t = scan ("float sway = sin (g_Time);\nvec3 g = c * smoothstep (0, 2, 1 + 0.5 * sin (g_Time * "
              "vec3 (11, 7, 13) * 2));", "");
    CHECK (t.derived && fabs (t.rate - 26) < 1e-9);
    t = scan ("vec4 laps = vec4 (frac (g_Time * g_Drift),\n frac (0.5 + g_Time * g_Drift),\n"
              " frac (g_Time * g_Drift + 0.25),\n frac (0.25 + g_Time * g_Drift + 0.125));",
              "g_Drift=0.4");
    CHECK (t.derived && fabs (t.rate - 0.4) < 1e-9);
    t = scan ("at.xy += g_Time * g_Speed * vec2 (0.003, 0.0004);", "g_Speed=2");
    CHECK (t.derived && fabs (t.rate - 0.006) < 1e-9);
    t = scan ("float tick = (g_Time * g_Speed) + g_Lead;", "g_Speed=0.5");
    CHECK (t.derived && fabs (t.rate - 0.5) < 1e-9);
    t = scan ("glint.x += u_shift + u_speed * (g_Time + lag);", "u_speed=0.2");
    CHECK (t.derived && fabs (t.rate - 0.2) < 1e-9);
    /* what cannot be told: a local variable, a function of the shader's own */
    t = scan ("v_Slide = slide * g_Time;", "");
    CHECK (t.uses_time && !t.derived);
    t = scan ("at.xyz += swayOffset (at.xyz, rest, g_Angle, "
              "g_Time,\n g_Speed);", "g_Speed=1");
    CHECK (t.uses_time && !t.derived);
    t = scan ("float tick = g_Time * g_Speed * thickness;", "g_Speed=1");
    CHECK (t.uses_time && !t.derived);
    /* one term that cannot be told spoils the shader, whatever the others say */
    t = scan ("a = sin (g_Time * g_Speed);\nb = mystery * g_Time;", "g_Speed=1");
    CHECK (t.uses_time && !t.derived);
    /* not a use: the declaration, comments, other identifiers */
    t = scan ("uniform float g_Time; // g_Time * 100\n/* g_Time * 50 */\nfloat x = g_TimeOffset * 9;\n"
              "#if USE_g_Time\n#endif\n", "");
    CHECK (!t.uses_time);
    t = scan (NULL, "");
    CHECK (!t.uses_time);
    /* several shaders accumulate: the fastest term, derived only when all are */
    WalpeFxTime acc = { 0 };
    walpe_fx_time_scan ("x = sin (g_Time * a);", val, (gpointer) "a=2", &acc);
    walpe_fx_time_scan ("y = cos (g_Time * b);", val, (gpointer) "b=7", &acc);
    CHECK (acc.derived && fabs (acc.rate - 7) < 1e-9);
    walpe_fx_time_scan ("z = f * g_Time;", val, (gpointer) "", &acc);
    CHECK (!acc.derived);

    /* "bounds": a sine cut to a narrow band moves faster than its phase */
    CHECK (fabs (walpe_fx_bounds_factor (0, 1) - 1) < 1e-9);
    CHECK (fabs (walpe_fx_bounds_factor (0, 0.5) - 2) < 1e-9);
    CHECK (fabs (walpe_fx_bounds_factor (0.5, 0) - 2) < 1e-9);          /* order does not matter */
    double blink = walpe_fx_bounds_factor (0.99, 0.997);                 /* a blink */
    CHECK (blink > 30 && blink < 40);
    CHECK (walpe_fx_bounds_factor (0.5, 0.5) >= 1000);                   /* a step */
    CHECK (walpe_fx_bounds_factor (-3, 9) == walpe_fx_bounds_factor (0, 1));

    /* the divisor an effect tolerates: at most WALPE_FX_SLOW_STEP per redraw */
    WalpeFxTime s3 = { TRUE, TRUE, 3 }, s10 = { TRUE, TRUE, 10 }, s11 = { TRUE, TRUE, 11 },
                un = { TRUE, FALSE, 0.1 }, no = { FALSE, FALSE, 0 };
    CHECK (walpe_fx_slow_div (&s3, 1, 3, 60) == 3 && walpe_fx_slow_div (&s3, 1, 2, 60) == 2);
    CHECK (walpe_fx_slow_div (&s10, 1, 3, 60) == 2 && walpe_fx_slow_div (&s10, 1, 2, 60) == 2);
    CHECK (walpe_fx_slow_div (&s11, 1, 3, 60) == 1);
    CHECK (walpe_fx_slow_div (&s3, 1, 3, 30) == 3 && walpe_fx_slow_div (&s10, 1, 3, 30) == 1);
    CHECK (walpe_fx_slow_div (&s3, 1, 3, 15) == 1);          /* a low frame rate holds nothing */
    CHECK (walpe_fx_slow_div (&s3, blink, 3, 60) == 1);      /* the blink is fast at any speed */
    CHECK (walpe_fx_slow_div (&un, 1, 3, 60) == 1);          /* unknown speed: every frame */
    CHECK (walpe_fx_slow_div (&no, 1, 3, 60) == 3);          /* no time in it: whatever is asked */
    CHECK (walpe_fx_slow_div (&s3, 1, 1, 60) == 1 && walpe_fx_slow_div (&s3, 1, 9, 60) == 3);
    CHECK (walpe_fx_slow_div (&s3, 1, 3, 0) == 1);
}

int main (void)
{
    /* before any GLib call that caches the config dir */
    tmp = g_dir_make_tmp ("walpe-quality-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("HOME", tmp, TRUE);
    char *dir = g_build_filename (tmp, "walpe", NULL);
    g_mkdir_with_parents (dir, 0700);
    cfg_path = g_build_filename (dir, "config.json", NULL);

    test_presets ();
    test_json ();
    test_config ();
    test_ipc ();
    test_fps ();
    test_estimate ();
    test_fxclass ();

    rm_tree (tmp);
    if (fails) {
        g_printerr ("%d check(s) failed\n", fails);
        return 1;
    }
    g_print ("quality: presets, JSON, config round trip, requests, frame rate by state, "
             "estimates, effect classification OK\n");
    return 0;
}
