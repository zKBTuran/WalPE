/* config_test - config.json (src/common/config.c)
 *
 * Private XDG_CONFIG_HOME, no daemon, no display:
 *  - the defaults of a new install are the ones README.md states;
 *  - every key survives save + load with a value that is not its default;
 *  - members this build does not know (a newer version's, hand-written
 *    ones) are still in the file after a save;
 *  - a config.json as version 0.12 wrote it: its values are kept, the former
 *    default frame rate 200 becomes 60 exactly once, a rate the user set
 *    afterwards stays;
 *  - a file that is not a JSON object (cut short, "[]", a number) is kept
 *    under another name and the defaults apply;
 *  - a save that cannot be written (the disk full in the middle of the
 *    file, no directory to write into) is reported, leaves the file on
 *    disk whole and no temporary file behind. */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include "common/config.h"
#include "common/quality.h"
#include "common/util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *cfg_dir, *cfg_path;

static void put_config (const char *json)
{
    g_assert (g_file_set_contents (cfg_path, json, -1, NULL));
}

static JsonNode *read_config (void)
{
    return wpj_parse_file (cfg_path);
}

/* files next to config.json whose name starts with "config.json.broken-" */
static int broken_copies (gboolean remove)
{
    int n = 0;
    GDir *d = g_dir_open (cfg_dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name (d)))
        if (g_str_has_prefix (name, "config.json.broken-")) {
            n++;
            if (remove) {
                char *p = g_build_filename (cfg_dir, name, NULL);
                g_unlink (p);
                g_free (p);
            }
        }
    if (d)
        g_dir_close (d);
    return n;
}

static void test_defaults (void)
{
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    /* README "Settings" */
    CHECK (c->fps == 60 && WALPE_FPS_DEFAULT == 60);
    CHECK (c->config_version == 4);
    /* a video inside a scene is played from a copy of the size shown */
    CHECK (c->video_scene_convert);
    CHECK (!c->paused && c->pause_fullscreen && c->pause_dpms && c->pause_covered);
    /* a fullscreen window pauses its own monitor only */
    CHECK (!c->pause_fullscreen_all);
    CHECK (c->cover_transparent && !g_strcmp0 (c->transparent_windows, "st"));
    CHECK (c->render_shared && c->fx_cache && c->tex_downscale && c->parallax);
    CHECK (!c->software_render && c->software_threads == 0);
    CHECK (c->render_scale == 100 && c->texture_slack == 2);
    CHECK (!c->web_accel && !c->video_hwdec);
    /* the web module leaves the daemon with its last web wallpaper */
    CHECK (c->web_unload);
    /* hardware decoding and its zero-copy path are opt-in */
    CHECK (!c->video_hwdec_api && !c->video_zerocopy && !strcmp (walpe_config_hwdec_word (c), "off"));
    /* wallpaper sound is on, not muted, at half volume until set */
    CHECK (c->scene_sound && !c->mute && c->volume == 0.5 && !c->sound_migrated);
    CHECK (c->preview_anim == WALPE_PREVIEW_ALWAYS);
    CHECK (c->focus_action == WALPE_FOCUS_RUN && !c->pause_battery && c->battery_fps == 0);
    CHECK (c->pause_idle == 0);     /* the idle rule is opt-in */
    CHECK (c->particle_lowres == 1 && c->particle_density == 100 && c->particle_instancing);
    CHECK (!c->fx_specialize && !g_strcmp0 (c->fx_quality, "exact"));
    CHECK (walpe_quality_preset_of (&c->quality) == WALPE_PRESET_HIGH);
    CHECK (!c->steam_user && !c->we_assets && !c->broken_copy && !c->fps_migrated && !c->extra);
    CHECK (!g_strcmp0 (c->current, ""));
    /* loading writes nothing */
    CHECK (!g_file_test (cfg_path, G_FILE_TEST_EXISTS));
    walpe_config_free (c);
}

static void test_round_trip (void)
{
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    g_free (c->wallpapers_dir);
    c->wallpapers_dir = g_strdup ("/lib/walls");
    g_free (c->current);
    c->current = g_strdup ("/lib/walls/111");
    walpe_config_set_monitor_path (c, "DP-0", "/lib/walls/222");
    walpe_config_set_monitor_path (c, "HDMI-0", "/lib/walls/333");
    c->fps = 144;
    c->volume = 0.25;
    c->video_scene_convert = FALSE;
    c->paused = TRUE;
    c->pause_fullscreen = c->pause_dpms = c->pause_covered = FALSE;
    g_free (c->transparent_windows);
    c->transparent_windows = g_strdup ("st,foot");
    c->cover_transparent = c->render_shared = c->fx_cache = c->tex_downscale = c->parallax = FALSE;
    c->fx_specialize = TRUE;
    g_free (c->fx_quality);
    c->fx_quality = g_strdup ("fast");
    c->web_accel = c->video_hwdec = c->software_render = TRUE;
    c->web_unload = FALSE;
    CHECK (walpe_config_set_hwdec (c, "VAAPI") && c->video_hwdec);  /* one decoding API by name */
    c->video_zerocopy = TRUE;
    c->software_threads = 6;
    c->particle_lowres = 2;
    c->particle_density = 45;
    c->particle_instancing = FALSE;
    c->steam_user = g_strdup ("some_account");
    c->we_assets = g_strdup ("/opt/we/assets");
    c->focus_action = WALPE_FOCUS_LIMIT;
    c->focus_fps = 20;
    c->pause_battery = TRUE;
    c->battery_fps = 24;
    c->pause_idle = 300;
    c->preview_anim = WALPE_PREVIEW_HOVER;
    c->vsync = FALSE;
    c->scene_sound = c->mute = TRUE;
    walpe_quality_preset (WALPE_PRESET_LOW, &c->quality);
    c->quality.fps_idle = 40;       /* a custom mix */
    walpe_config_set_prop (c, "/lib/walls/111", "speed", "2");
    walpe_config_set_prop (c, "/lib/walls/111/", "title", "a \"b\" \\ c");     /* any spelling of the path */
    walpe_config_set_prop (c, "/lib/walls/222", "walpe_fit", "contain");
    {
        WalpeQualityOv ov = { 0 };
        walpe_quality_preset (WALPE_PRESET_HIGH, &ov.q);
        ov.q.tex_res = 1;
        ov.mask = WALPE_Q_TEX_RES;
        walpe_config_set_quality_ov (c, "/lib/walls/222", &ov);
    }
    walpe_config_save (c);
    int low_scale = c->quality.render_scale, low_slack = c->quality.tex_slack;
    walpe_config_free (c);

    c = walpe_config_load ();
    CHECK (!g_strcmp0 (c->wallpapers_dir, "/lib/walls") && !g_strcmp0 (c->current, "/lib/walls/111"));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (c, "DP-0"), "/lib/walls/222"));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (c, "HDMI-0"), "/lib/walls/333"));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (c, "eDP-1"), "/lib/walls/111"));
    CHECK (c->fps == 144 && !c->fps_migrated && c->volume == 0.25 && c->paused);
    CHECK (!c->video_scene_convert);
    CHECK (!c->pause_fullscreen && !c->pause_dpms && !c->pause_covered);
    CHECK (!g_strcmp0 (c->transparent_windows, "st,foot"));
    CHECK (!c->cover_transparent && !c->render_shared && !c->fx_cache && !c->tex_downscale && !c->parallax);
    CHECK (c->fx_specialize && !g_strcmp0 (c->fx_quality, "fast"));
    CHECK (c->web_accel && c->video_hwdec && c->software_render && c->software_threads == 6);
    CHECK (!c->web_unload);
    CHECK (!g_strcmp0 (c->video_hwdec_api, "vaapi") && c->video_zerocopy &&
           !strcmp (walpe_config_hwdec_word (c), "vaapi"));
    CHECK (c->particle_lowres == 2 && c->particle_density == 45 && !c->particle_instancing);
    CHECK (!g_strcmp0 (c->steam_user, "some_account") && !g_strcmp0 (c->we_assets, "/opt/we/assets"));
    CHECK (c->focus_action == WALPE_FOCUS_LIMIT && c->focus_fps == 20);
    CHECK (c->pause_battery && c->battery_fps == 24 && c->preview_anim == WALPE_PREVIEW_HOVER);
    CHECK (c->pause_idle == 300);
    CHECK (!c->vsync);
    CHECK (c->scene_sound && c->mute);
    CHECK (c->quality.fps_idle == 40 && c->quality.render_scale == low_scale &&
           c->quality.tex_slack == low_slack);
    CHECK (c->render_scale == low_scale && c->texture_slack == low_slack);     /* both sides agree */
    CHECK (walpe_quality_preset_of (&c->quality) == WALPE_PRESET_CUSTOM);
    char *v = walpe_config_get_prop ("/lib/walls/111", "speed");
    CHECK (!g_strcmp0 (v, "2"));
    g_free (v);
    v = walpe_config_get_prop ("/lib/walls/111", "title");
    CHECK (!g_strcmp0 (v, "a \"b\" \\ c"));
    g_free (v);
    v = walpe_config_get_prop ("/lib/walls/222", "walpe_fit");
    CHECK (!g_strcmp0 (v, "contain"));
    g_free (v);
    CHECK (walpe_config_get_prop ("/lib/walls/333", "speed") == NULL);
    const WalpeQualityOv *ov = walpe_config_quality_ov (c, "/lib/walls/222");
    CHECK (ov && ov->mask == WALPE_Q_TEX_RES && ov->q.tex_res == 1);
    CHECK (walpe_config_quality_ov (c, "/lib/walls/111") == NULL);
    CHECK (!c->extra);      /* nothing of its own counts as unknown */
    /* a second save of what was loaded writes the same file */
    char *a = NULL, *b = NULL;
    g_file_get_contents (cfg_path, &a, NULL, NULL);
    walpe_config_save (c);
    g_file_get_contents (cfg_path, &b, NULL, NULL);
    CHECK (a && b && !strcmp (a, b));
    g_free (a);
    g_free (b);
    /* clearing the two keys that are written only while set removes them */
    g_clear_pointer (&c->steam_user, g_free);
    g_clear_pointer (&c->we_assets, g_free);
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (!c->steam_user && !c->we_assets && !c->extra);
    walpe_config_free (c);
}

/* "video_hwdec" is off | auto | <api>; the switch values keep their meaning */
static void test_hwdec_words (void)
{
    WalpeConfig *c = walpe_config_load ();
    CHECK (walpe_config_set_hwdec (c, "auto") && c->video_hwdec && !c->video_hwdec_api &&
           !strcmp (walpe_config_hwdec_word (c), "auto"));
    CHECK (walpe_config_set_hwdec (c, "cuda") && c->video_hwdec && !g_strcmp0 (c->video_hwdec_api, "cuda"));
    /* off keeps the name for the next "on" (the switch in the settings) */
    CHECK (walpe_config_set_hwdec (c, "off") && !c->video_hwdec && !g_strcmp0 (c->video_hwdec_api, "cuda") &&
           !strcmp (walpe_config_hwdec_word (c), "off"));
    CHECK (walpe_config_set_hwdec (c, "on") && c->video_hwdec && !strcmp (walpe_config_hwdec_word (c), "cuda"));
    CHECK (walpe_config_set_hwdec (c, "true") && !strcmp (walpe_config_hwdec_word (c), "cuda"));
    /* "auto" forgets it */
    CHECK (walpe_config_set_hwdec (c, " Auto ") && c->video_hwdec && !c->video_hwdec_api);
    /* not a word of the setting: nothing changes */
    CHECK (!walpe_config_set_hwdec (c, "va api") && !walpe_config_set_hwdec (c, "") &&
           !walpe_config_set_hwdec (c, NULL) && c->video_hwdec && !c->video_hwdec_api);
    walpe_config_free (c);
}

static void test_unknown_keys (void)
{
    put_config ("{\"config_version\":3,\"fps\":50,\"future_switch\":true,"
                "\"future_table\":{\"a\":[1,2,{\"b\":null}],\"c\":\"d\"},\"note_by_hand\":\"keep me\","
                "\"steam_user\":\"\",\"layers\":{}}");
    WalpeConfig *c = walpe_config_load ();
    CHECK (c->fps == 50);
    CHECK (c->extra && json_object_get_size (c->extra) == 3);
    /* they are not settings of this build (the presets ask) */
    static const char *const keys[] = { "fps", "future_switch", "particle_density", "note_by_hand" };
    CHECK (walpe_config_has_keys (c, keys, G_N_ELEMENTS (keys)) == (1u | 4u));
    c->fps = 75;                /* what the daemon does on `walpe-ctl set fps=75` */
    walpe_config_save (c);
    walpe_config_free (c);

    JsonNode *n = read_config ();
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_num (o, "fps", 0) == 75);
    CHECK (o && wpj_bool (o, "future_switch", FALSE));
    CHECK (o && !g_strcmp0 (wpj_str (o, "note_by_hand", ""), "keep me"));
    JsonObject *ft = o ? wpj_obj (o, "future_table") : NULL;
    JsonArray *fa = ft ? wpj_arr (ft, "a") : NULL;
    CHECK (ft && !g_strcmp0 (wpj_str (ft, "c", ""), "d") && fa && json_array_get_length (fa) == 3 &&
           json_array_get_int_element (fa, 1) == 2 &&
           JSON_NODE_HOLDS_OBJECT (json_array_get_element (fa, 2)));
    CHECK (o && !json_object_has_member (o, "steam_user"));     /* empty: not a kept unknown */
    if (n)
        json_node_unref (n);

    /* and a second round (load, change, save) keeps them again */
    c = walpe_config_load ();
    CHECK (c->fps == 75 && c->extra && json_object_get_size (c->extra) == 3);
    c->steam_user = g_strdup ("me");
    walpe_config_save (c);
    walpe_config_free (c);
    n = read_config ();
    o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_bool (o, "future_switch", FALSE) && wpj_obj (o, "future_table") &&
           !g_strcmp0 (wpj_str (o, "steam_user", ""), "me"));
    if (n)
        json_node_unref (n);
}

/* config.json as version 0.12 wrote it (every key of its config_to_json) */
static const char r12_config[] =
    "{\"config_version\":2,\"wallpapers_dir\":\"/home/u/.local/share/walpe/workshop\","
    "\"current\":\"/lib/2901582518\",\"video_backend\":\"ffmpeg\",\"fps\":200,\"volume\":0.5,"
    "\"paused\":false,\"monitor_current\":{\"DP-2\":\"/lib/3292662040\"},\"pause_fullscreen\":false,"
    "\"transparent_windows\":\"st,kitty\",\"pause_covered\":false,\"pause_dpms\":true,"
    "\"cover_transparent\":false,\"render_shared\":false,\"fx_cache\":true,\"tex_downscale\":false,"
    "\"parallax\":false,\"steam_user\":\"acc\",\"we_assets\":\"/we/assets\",\"vsync\":true,\"audio\":false,"
    "\"props\":{\"/lib/2901582518\\u001fclock\":\"true\",\"/lib/3292662040/\\u001fwalpe_fit\":\"contain\"}}";

static void test_round12 (void)
{
    put_config (r12_config);
    WalpeConfig *c = walpe_config_load ();
    CHECK (c->fps_migrated && c->fps_migrated_from == 200 && c->fps == 60);
    CHECK (!g_strcmp0 (c->current, "/lib/2901582518"));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (c, "DP-2"), "/lib/3292662040"));
    CHECK (c->volume == 0.5 && !c->pause_fullscreen && !c->pause_covered && c->pause_dpms);
    CHECK (!c->cover_transparent && !c->render_shared && c->fx_cache && !c->tex_downscale && !c->parallax);
    CHECK (!g_strcmp0 (c->transparent_windows, "st,kitty") && !g_strcmp0 (c->steam_user, "acc") &&
           !g_strcmp0 (c->we_assets, "/we/assets"));
    /* what version 0.12 did not have: this version's defaults */
    CHECK (!c->software_render && !c->web_accel && !c->video_hwdec && c->particle_lowres == 1 &&
           c->particle_density == 100 && c->render_scale == 100 && c->texture_slack == 2 &&
           walpe_quality_preset_of (&c->quality) == WALPE_PRESET_HIGH && !c->extra);
    char *v = walpe_config_get_prop ("/lib/2901582518", "clock");
    CHECK (!g_strcmp0 (v, "true"));
    g_free (v);
    v = walpe_config_get_prop ("/lib/3292662040", "walpe_fit");     /* saved with a trailing slash */
    CHECK (!g_strcmp0 (v, "contain"));
    g_free (v);
    walpe_config_save (c);          /* the daemon saves a migrated config at once */
    walpe_config_free (c);

    c = walpe_config_load ();
    CHECK (!c->fps_migrated && c->fps == 60 && c->config_version == 4);
    c->fps = 200;                   /* the user's own choice from now on */
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (!c->fps_migrated && c->fps == 200);
    walpe_config_free (c);

    /* a rate the user had set under version 0.12 is not touched */
    put_config ("{\"config_version\":2,\"fps\":144,\"current\":\"/lib/1\"}");
    c = walpe_config_load ();
    CHECK (!c->fps_migrated && c->fps == 144);
    walpe_config_free (c);
}

static void test_broken (void)
{
    static const char *const bad[] = {
        "{\"fps\": 50, \"current\": \"/lib/1\",",      /* cut short */
        "[1, 2, 3]",                                      /* JSON, not an object */
        "50",
        "\"text\"",
        "not json at all",
    };
    for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
        broken_copies (TRUE);
        put_config (bad[i]);
        WalpeConfig *c = walpe_config_load ();
        CHECK (c->fps == 60 && !g_strcmp0 (c->current, ""));
        CHECK (c->broken_copy != NULL);
        if (c->broken_copy) {
            char *kept = NULL;
            CHECK (g_file_get_contents (c->broken_copy, &kept, NULL, NULL) && !strcmp (kept, bad[i]));
            g_free (kept);
        }
        CHECK (broken_copies (FALSE) == 1);
        CHECK (!g_file_test (cfg_path, G_FILE_TEST_EXISTS));
        walpe_config_save (c);      /* the defaults go into a new file, the copy stays */
        CHECK (broken_copies (FALSE) == 1);
        walpe_config_free (c);
        c = walpe_config_load ();
        CHECK (!c->broken_copy && c->fps == 60);
        walpe_config_free (c);
    }
    broken_copies (TRUE);
    /* an empty file is not "damaged": nothing to keep */
    put_config ("  \n");
    WalpeConfig *c = walpe_config_load ();
    CHECK (!c->broken_copy && broken_copies (FALSE) == 0 && c->fps == 60);
    walpe_config_free (c);
    /* members of the wrong type are skipped one by one, the rest is used */
    put_config ("{\"config_version\":3,\"fps\":\"fast\",\"volume\":[1],\"paused\":\"yes\",\"current\":7,"
                "\"monitor_current\":[],\"props\":5,\"wallpaper_quality\":\"x\",\"pause_covered\":false,"
                "\"particle_density\":1e300,\"render_scale\":-5,\"software_threads\":99,"
                "\"focus_action\":\"dance\",\"preview_anim\":3,\"fx_quality\":\"best\"}");
    c = walpe_config_load ();
    CHECK (!c->broken_copy && c->fps == 60 && c->volume == 0.5 && !c->paused);
    CHECK (!g_strcmp0 (c->current, "") && !c->pause_covered);
    CHECK (c->particle_density == 100 && c->render_scale == 25 && c->software_threads == 32);
    CHECK (c->focus_action == WALPE_FOCUS_RUN && c->preview_anim == WALPE_PREVIEW_ALWAYS &&
           !g_strcmp0 (c->fx_quality, "exact"));
    walpe_config_free (c);
}

/* "pause_fullscreen_all" (a focused fullscreen window pauses every monitor,
 * as up to 0.12): off by default and not written then (an untouched config
 * keeps its bytes), written and read back when set, gone from the file
 * again when switched off, and a value that is no switch reads as off. */
static void test_fullscreen_all (void)
{
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    CHECK (!c->pause_fullscreen_all && c->pause_fullscreen);
    walpe_config_save (c);
    walpe_config_free (c);
    JsonNode *n = read_config ();
    JsonObject *o = n ? json_node_get_object (n) : NULL;
    CHECK (o && json_object_has_member (o, "pause_fullscreen") &&
           !json_object_has_member (o, "pause_fullscreen_all"));
    if (n)
        json_node_unref (n);

    c = walpe_config_load ();
    c->pause_fullscreen_all = TRUE;
    walpe_config_save (c);
    walpe_config_free (c);
    n = read_config ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && json_object_has_member (o, "pause_fullscreen_all") &&
           wpj_bool (o, "pause_fullscreen_all", FALSE));
    if (n)
        json_node_unref (n);
    c = walpe_config_load ();
    CHECK (c->pause_fullscreen_all && c->pause_fullscreen);     /* the rule's own switch is another key */
    c->pause_fullscreen_all = FALSE;
    walpe_config_save (c);
    walpe_config_free (c);
    n = read_config ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !json_object_has_member (o, "pause_fullscreen_all"));
    if (n)
        json_node_unref (n);

    /* written by hand */
    put_config ("{\"config_version\":4,\"pause_fullscreen\":false,\"pause_fullscreen_all\":true}");
    c = walpe_config_load ();
    CHECK (!c->broken_copy && c->pause_fullscreen_all && !c->pause_fullscreen);
    walpe_config_free (c);
    put_config ("{\"config_version\":4,\"pause_fullscreen_all\":\"every\"}");
    c = walpe_config_load ();
    CHECK (!c->broken_copy && !c->pause_fullscreen_all);
    walpe_config_free (c);
    g_unlink (cfg_path);
}

/* the window system keys: "backend" (auto / x11 / wayland) and
 * "wayland_pointer" (auto / off). Not written at their defaults (an
 * untouched config keeps its bytes), read back when set, anything that is
 * not one of the names reads as the default, and a default written out by
 * hand survives a save. */
static void test_backend_keys (void)
{
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    CHECK (c->backend == WALPE_BACKEND_AUTO && c->wayland_pointer == 0);
    walpe_config_save (c);
    walpe_config_free (c);
    JsonNode *n = read_config ();
    JsonObject *o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !json_object_has_member (o, "backend") && !json_object_has_member (o, "wayland_pointer"));
    if (n)
        json_node_unref (n);

    c = walpe_config_load ();
    c->backend = WALPE_BACKEND_WAYLAND;
    c->wayland_pointer = 1;
    walpe_config_save (c);
    walpe_config_free (c);
    n = read_config ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "backend", ""), "wayland"));
    CHECK (o && !g_strcmp0 (wpj_str (o, "wayland_pointer", ""), "off"));
    if (n)
        json_node_unref (n);
    c = walpe_config_load ();
    CHECK (c->backend == WALPE_BACKEND_WAYLAND && c->wayland_pointer == 1);
    c->backend = WALPE_BACKEND_X11;
    c->wayland_pointer = 0;
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (c->backend == WALPE_BACKEND_X11 && c->wayland_pointer == 0);
    walpe_config_free (c);

    /* by hand: any case; names that are no backend, and other types, read
     * as auto */
    put_config ("{\"config_version\":3,\"backend\":\"X11\",\"wayland_pointer\":\"OFF\"}");
    c = walpe_config_load ();
    CHECK (!c->broken_copy && c->backend == WALPE_BACKEND_X11 && c->wayland_pointer == 1);
    walpe_config_free (c);
    put_config ("{\"config_version\":3,\"backend\":\"mir\",\"wayland_pointer\":\"maybe\"}");
    c = walpe_config_load ();
    CHECK (!c->broken_copy && c->backend == WALPE_BACKEND_AUTO && c->wayland_pointer == 0);
    walpe_config_free (c);
    put_config ("{\"config_version\":3,\"backend\":2,\"wayland_pointer\":true}");
    c = walpe_config_load ();
    CHECK (!c->broken_copy && c->backend == WALPE_BACKEND_AUTO && c->wayland_pointer == 0);
    walpe_config_free (c);
    /* "auto" spelled out stays in the file through a save */
    put_config ("{\"config_version\":3,\"backend\":\"auto\"}");
    c = walpe_config_load ();
    CHECK (c->backend == WALPE_BACKEND_AUTO);
    walpe_config_save (c);
    walpe_config_free (c);
    n = read_config ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "backend", ""), "auto"));
    if (n)
        json_node_unref (n);
    g_unlink (cfg_path);
}

/* files next to config.json that a save left behind ("config.json.XXXXXX") */
static int temp_files (void)
{
    int n = 0;
    GDir *d = g_dir_open (cfg_dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name (d)))
        if (g_str_has_prefix (name, "config.json.") && !g_str_has_prefix (name, "config.json.broken-"))
            n++;
    if (d)
        g_dir_close (d);
    return n;
}

/* A save that cannot be written: the caller is told (it was a warning in
 * the log and nothing else, so `walpe-ctl set` answered ok for a change that
 * was gone at the next start), the file on disk stays whole and no
 * temporary file is left. */
static void test_save_failure (void)
{
    put_config ("{\"config_version\":3,\"fps\":50,\"current\":\"/lib/1\",\"note_by_hand\":\"keep\"}");
    WalpeConfig *c = walpe_config_load ();
    CHECK (c->fps == 50);
    CHECK (walpe_config_save (c) && walpe_config_save_error () == NULL);
    guint count = walpe_config_save_count ();

    /* 1. the disk is full in the middle of the file: no file may grow past
     * 64 bytes for the time of the save */
    struct rlimit old, lim;
    CHECK (getrlimit (RLIMIT_FSIZE, &old) == 0);
    lim = old;
    lim.rlim_cur = 64;
    void (*was) (int) = signal (SIGXFSZ, SIG_IGN);
    CHECK (setrlimit (RLIMIT_FSIZE, &lim) == 0);
    c->fps = 77;
    gboolean ok = walpe_config_save (c);
    CHECK (setrlimit (RLIMIT_FSIZE, &old) == 0);
    signal (SIGXFSZ, was);
    CHECK (!ok);
    CHECK (walpe_config_save_error () != NULL && strstr (walpe_config_save_error (), "config.json"));
    CHECK (walpe_config_save_count () == count + 1);    /* it still counts as a save that was tried */
    CHECK (temp_files () == 0);
    JsonNode *n = read_config ();
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_num (o, "fps", 0) == 50 && !g_strcmp0 (wpj_str (o, "current", ""), "/lib/1") &&
           !g_strcmp0 (wpj_str (o, "note_by_hand", ""), "keep"));
    if (n)
        json_node_unref (n);
    CHECK (c->fps == 77);       /* what runs is what was set */

    /* 2. the next save works: the value is written and the reason is gone */
    CHECK (walpe_config_save (c) && walpe_config_save_error () == NULL);
    n = read_config ();
    o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_num (o, "fps", 0) == 77);
    if (n)
        json_node_unref (n);

    /* 3. the directory is not there to write into (a file has its name) */
    char *aside = g_strconcat (cfg_dir, ".aside", NULL);
    CHECK (g_rename (cfg_dir, aside) == 0);
    CHECK (g_file_set_contents (cfg_dir, "in the way", -1, NULL));
    c->fps = 88;
    CHECK (!walpe_config_save (c) && walpe_config_save_error () != NULL);
    CHECK (g_unlink (cfg_dir) == 0 && g_rename (aside, cfg_dir) == 0);
    g_free (aside);
    n = read_config ();
    o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_num (o, "fps", 0) == 77);
    if (n)
        json_node_unref (n);
    CHECK (walpe_config_save (c) && walpe_config_save_error () == NULL && temp_files () == 0);
    walpe_config_free (c);
}

/* Wallpaper sound is on by default, at half volume; up to 0.14.0 it was
 * off at volume 0, and those versions wrote both values into every
 * config.json. A file of theirs that still holds the old defaults takes the
 * new ones once; every pair that was somebody's choice is kept. */
static void test_sound_defaults (void)
{
    static const struct { const char *json; gboolean on; double vol; gboolean migrated; const char *what; } k[] = {
        { "{\"config_version\":4,\"volume\":0.0,\"scene_sound\":false,\"mute\":false}", TRUE, 0.5, TRUE,
          "0.14.0, nothing set: this version's defaults" },
        { "{\"config_version\":2,\"volume\":0.0}", TRUE, 0.5, TRUE,
          "0.12 (no switch yet), volume 0 as it wrote it: this version's defaults" },
        { "{\"config_version\":2,\"volume\":0.3}", TRUE, 0.3, FALSE,
          "0.12 with a volume: kept, and sound is on" },
        { "{\"config_version\":4,\"volume\":0.0,\"scene_sound\":true}", TRUE, 0.0, FALSE,
          "0.14.0, switched on, volume 0: kept (on, silent)" },
        { "{\"config_version\":4,\"volume\":0.8,\"scene_sound\":false}", FALSE, 0.8, FALSE,
          "0.14.0, off with a volume: stays off" },
        { "{\"config_version\":4,\"volume\":0.0,\"scene_sound\":false,\"sound_defaults\":2}", FALSE, 0.0, FALSE,
          "this version, switched off and turned down: a choice, kept" },
        { "{\"config_version\":4}", TRUE, 0.5, FALSE, "neither key: the defaults, nothing migrated" },
    };
    for (guint i = 0; i < G_N_ELEMENTS (k); i++) {
        put_config (k[i].json);
        WalpeConfig *c = walpe_config_load ();
        if (!(c->scene_sound == k[i].on && c->volume == k[i].vol && c->sound_migrated == k[i].migrated && !c->mute)) {
            printf ("FAIL sound defaults, %s: scene_sound %d volume %g migrated %d\n", k[i].what, c->scene_sound,
                    c->volume, c->sound_migrated);
            fails++;
        }
        walpe_config_free (c);
    }
    /* once: the daemon saves a migrated configuration, and what the user
     * sets afterwards - also back to "off, 0" - stays */
    put_config (k[0].json);
    WalpeConfig *c = walpe_config_load ();
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (c->scene_sound && c->volume == 0.5 && !c->sound_migrated);
    c->scene_sound = FALSE;
    c->volume = 0.0;
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (!c->scene_sound && c->volume == 0.0 && !c->sound_migrated);
    walpe_config_free (c);
    CHECK (walpe_config_migrate_sound (1, TRUE, FALSE, 0.0) && walpe_config_migrate_sound (1, FALSE, TRUE, 0.0));
    CHECK (!walpe_config_migrate_sound (2, TRUE, FALSE, 0.0) && !walpe_config_migrate_sound (1, TRUE, TRUE, 0.0) &&
           !walpe_config_migrate_sound (1, TRUE, FALSE, 0.5));
}

int main (void)
{
    /* before any GLib call that caches the config dir */
    char *tmp = g_dir_make_tmp ("walpe-config-test-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("HOME", tmp, TRUE);
    cfg_dir = g_build_filename (tmp, "walpe", NULL);
    g_mkdir_with_parents (cfg_dir, 0700);
    cfg_path = g_build_filename (cfg_dir, "config.json", NULL);
    /* the "kept as ..." warnings of the damaged files are expected */
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR);

    test_defaults ();
    test_round_trip ();
    test_hwdec_words ();
    test_unknown_keys ();
    test_round12 ();
    test_sound_defaults ();
    test_broken ();
    test_backend_keys ();
    test_fullscreen_all ();
    test_save_failure ();

    broken_copies (TRUE);
    g_unlink (cfg_path);
    g_rmdir (cfg_dir);
    g_rmdir (tmp);
    g_free (tmp);
    if (fails)
        g_printerr ("config-test: %d check(s) failed\n", fails);
    else
        g_print ("config-test: ok\n");
    return fails ? 1 : 0;
}
