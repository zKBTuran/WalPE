/* setkeys.h - the members a {"cmd":"set"} request may carry, and their types
 *
 * Earlier builds of the daemon took what they knew from a "set" request and
 * dropped the rest without a word: `walpe-ctl set coverd=off fps=30`
 * answered ok (the typo lost), and a value of the wrong type for a known
 * key (`covered=yes`, a text where a switch is expected) was "applied"
 * without changing anything, also with ok. walpe_set_check () is asked
 * before anything is changed: one unknown member or one value of the wrong
 * type and the whole request is refused with a sentence that names it.
 *
 * Header only (the daemon and tests/ipc_test.c include it); the test also
 * reads src/daemon/main.c and fails when ipc_set () asks for a member that
 * is missing here. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "common/config.h"      /* walpe_skip_hidden_parse */

typedef enum {
    WALPE_SET_BOOL,     /* a switch: JSON true / false (a number counts as one, as before) */
    WALPE_SET_NUM,      /* a number */
    WALPE_SET_STR,      /* text */
    WALPE_SET_ANY,      /* its branch reads several spellings and checks the value itself */
} WalpeSetKind;

typedef struct {
    const char *key;
    WalpeSetKind kind;
} WalpeSetKey;

static const WalpeSetKey walpe_set_keys[] = {
    { "cmd", WALPE_SET_STR },
    { "present", WALPE_SET_STR },
    { "software_render", WALPE_SET_BOOL },
    { "software_threads", WALPE_SET_ANY },      /* a number or "auto" */
    { "gpu", WALPE_SET_STR },                   /* auto | integrated | discrete | a device */
    { "preset_follow_gpu", WALPE_SET_BOOL },
    { "gl_api", WALPE_SET_STR },                /* auto, gl or gles */
    { "fps", WALPE_SET_NUM },
    { "volume", WALPE_SET_NUM },
    { "pause_fullscreen", WALPE_SET_BOOL },
    { "pause_fullscreen_all", WALPE_SET_BOOL },
    { "pause_dpms", WALPE_SET_BOOL },
    { "pause_covered", WALPE_SET_BOOL },
    { "cover_transparent", WALPE_SET_BOOL },
    { "focus_action", WALPE_SET_STR },
    { "focus_fps", WALPE_SET_NUM },
    { "pause_battery", WALPE_SET_BOOL },
    { "battery_fps", WALPE_SET_NUM },
    { "pause_idle", WALPE_SET_NUM },            /* seconds without input, 0 = off */
    { "preview_anim", WALPE_SET_ANY },          /* a name, or false = off */
    { "fx_cache", WALPE_SET_BOOL },
    { "web_accel", WALPE_SET_BOOL },
    { "web_unload", WALPE_SET_BOOL },
    { "particle_lowres", WALPE_SET_ANY },       /* a name, or a switch */
    { "particle_density", WALPE_SET_ANY },      /* a number, also as text */
    { "particle_instancing", WALPE_SET_BOOL },
    { "tex_downscale", WALPE_SET_BOOL },
    { "video_hwdec", WALPE_SET_ANY },            /* a switch, or off / auto / the API's name */
    { "fx_specialize", WALPE_SET_BOOL },
    { "fx_quality", WALPE_SET_STR },
    { "fx_fast", WALPE_SET_BOOL },
    { "render_shared", WALPE_SET_BOOL },
    { "transparent_windows", WALPE_SET_STR },
    { "steam_user", WALPE_SET_STR },
    { "we_assets", WALPE_SET_STR },
    { "parallax", WALPE_SET_BOOL },
    { "video_backend", WALPE_SET_STR },
    /* the quality knobs and their short names (quality.c reads and checks them) */
    { "quality_preset", WALPE_SET_ANY }, { "preset", WALPE_SET_ANY },
    { "fx_rate", WALPE_SET_ANY }, { "fx_rate_mode", WALPE_SET_ANY },
    { "fps_idle", WALPE_SET_ANY },
    { "post_processing", WALPE_SET_ANY }, { "post", WALPE_SET_ANY },
    { "render_scale", WALPE_SET_ANY }, { "scale", WALPE_SET_ANY },
    { "tex_res", WALPE_SET_ANY }, { "tex", WALPE_SET_ANY },
    { "fx_res", WALPE_SET_ANY },
    { "texture_slack", WALPE_SET_ANY }, { "slack", WALPE_SET_ANY },
    /* keys of the opt-in parts:
     * frame clocks on one grid, one rendering for same-size monitors that
     * follow the pointer, "Skip hidden areas", the video frame limit and
     * the way video planes are uploaded. All off / legacy by default. */
    { "frame_align", WALPE_SET_BOOL },
    { "pointer_share", WALPE_SET_BOOL },
    { "skip_hidden", WALPE_SET_ANY },           /* off / auto / always, a switch, 0..2 */
    { "skip_hidden_margin", WALPE_SET_NUM },
    { "skip_hidden_inset", WALPE_SET_NUM },
    { "video_limit", WALPE_SET_BOOL },
    { "video_upload", WALPE_SET_STR },
    /* the window system the daemon draws on (auto / x11 / wayland; read
     * at its next start) and, on Wayland, whether the wallpaper surface
     * may take pointer input (auto / off) */
    { "backend", WALPE_SET_STR },
    { "wayland_pointer", WALPE_SET_STR },
    /* hardware-decoded pictures stay in GPU memory (off by default) */
    { "video_zerocopy", WALPE_SET_BOOL },
    /* render resolution (common/resolution.h, which reads and checks the
     * values): a value for every connected monitor, or NAME:VALUE pairs */
    { "resolution", WALPE_SET_STR },            /* native, 720p .. 4k, WxH */
    { "texture_limit", WALPE_SET_ANY },         /* auto, none (also off), pixels */
    { "scale_filter", WALPE_SET_STR },          /* bilinear, nearest */
    { "video_resolution", WALPE_SET_STR },      /* auto, none, a resolution */
    { "resolution_prompt", WALPE_SET_BOOL },    /* may the GUI ask about new monitors */
    /* renderer features: switches of engine/features.h. A feature of that
     * table becomes a setting by its row here (the daemon takes every
     * feature that is listed, without a branch of its own). */
    { "camera_parallax", WALPE_SET_BOOL },
    { "parallax_offset", WALPE_SET_BOOL },
    { "script_camera", WALPE_SET_BOOL },
    { "sprite_sheets", WALPE_SET_BOOL },
    { "script_texanim", WALPE_SET_BOOL },
    { "bloom", WALPE_SET_BOOL },
    { "camera_shake", WALPE_SET_BOOL },
    /* simulation effects (engine/fx_sim.h) */
    { "fluid_sim", WALPE_SET_BOOL },
    { "fluid_quality", WALPE_SET_STR },
    /* wallpaper sound (engine/scenesound.h): scenes play their sound layers
     * (off by default); silence that keeps the volume setting */
    { "scene_sound", WALPE_SET_BOOL },
    { "mute", WALPE_SET_BOOL },
};

static inline const WalpeSetKey *walpe_set_key_find (const char *name)
{
    for (guint i = 0; name && i < G_N_ELEMENTS (walpe_set_keys); i++)
        if (!strcmp (name, walpe_set_keys[i].key))
            return &walpe_set_keys[i];
    return NULL;
}

/* NULL = every member of the request is a setting of this build with a
 * value of its type; else the sentence for the reply (g_free) */
static inline char *walpe_set_check (JsonObject *req)
{
    char *err = NULL;
    GList *ks = req ? json_object_get_members (req) : NULL;
    for (GList *l = ks; l && !err; l = l->next) {
        const char *name = l->data;
        const WalpeSetKey *k = walpe_set_key_find (name);
        if (!k) {
            err = g_strdup_printf ("unknown setting \"%s\": nothing was changed (walpe-ctl without "
                                   "arguments lists the settings)", name);
            break;
        }
        JsonNode *n = json_object_get_member (req, name);
        GType t = n && JSON_NODE_HOLDS_VALUE (n) ? json_node_get_value_type (n) : G_TYPE_INVALID;
        gboolean num = t == G_TYPE_INT64 || t == G_TYPE_DOUBLE;
        if (k->kind == WALPE_SET_ANY) {
            if (n && !JSON_NODE_HOLDS_VALUE (n) && !JSON_NODE_HOLDS_NULL (n))
                err = g_strdup_printf ("%s: a single value is needed; nothing was changed", name);
        } else if (k->kind == WALPE_SET_BOOL && t != G_TYPE_BOOLEAN && !num) {
            err = g_strdup_printf ("%s is a switch: on or off (true / false); nothing was changed",
                                   name);
        } else if (k->kind == WALPE_SET_NUM && !num) {
            err = g_strdup_printf ("%s needs a number; nothing was changed", name);
        } else if (k->kind == WALPE_SET_STR && t != G_TYPE_STRING) {
            err = g_strdup_printf ("%s needs a name or text, not a switch or a number; nothing was "
                                   "changed", name);
        }
        /* Values of the right type that ipc_set () read as "leave it as it
         * is" and answered with ok: a mistyped mode of "Skip hidden areas",
         * a negative width, a video backend that does not exist (dropped
         * without a word when the request had another key in it). */
        if (err || !n || !JSON_NODE_HOLDS_VALUE (n))
            continue;
        if (!strcmp (name, "skip_hidden") && t == G_TYPE_STRING &&
            walpe_skip_hidden_parse (json_node_get_string (n), -1) < 0) {
            err = g_strdup ("skip_hidden: off, auto or always; nothing was changed");
        } else if ((!strcmp (name, "skip_hidden_margin") || !strcmp (name, "skip_hidden_inset")) &&
                   num && json_node_get_double (n) < 0) {
            err = g_strdup_printf ("%s: a number of pixels from 0 on; nothing was changed", name);
        } else if (!strcmp (name, "video_backend") && t == G_TYPE_STRING &&
                   strcmp (json_node_get_string (n), "ffmpeg") && strcmp (json_node_get_string (n), "mpv")) {
            err = g_strdup ("video_backend: ffmpeg (mpv is still taken and means the same: FFmpeg is "
                            "the one video backend); nothing was changed");
        }
    }
    g_list_free (ks);
    return err;
}
