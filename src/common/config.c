#include "config.h"
#include "glapi.h"
#include "util.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include "layeradj.h"
#include "gpusel.h"
#include "engine/video_hwsel.h"   /* the words of "video_hwdec" (a header without dependencies) */
#include "fluidsim.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static WalpeConfig *active_config;   /* daemon singleton, set on load */
/* guards cfg->props: render threads read overrides while IPC setprop
 * mutates the table on the main thread */
static GMutex props_lock;

const char *walpe_default_dir (void)
{
    return "~/.local/share/walpe/workshop";
}

/* $XDG_RUNTIME_DIR/walpe.sock (GLib: ~/.cache/walpe.sock without
 * XDG_RUNTIME_DIR). A unix socket address holds 107 bytes: with a long
 * home directory and no runtime dir the path does not fit, bind() used a
 * cut-off name that no unlink() ever matched, and every later start
 * failed. Too long -> <tmp>/walpe-<uid>/walpe.sock, in a directory that
 * is ours alone (0700, owned by us, not a symlink); daemon, GUI, tray and
 * walpe-ctl all get their path here, so they agree.
 *
 * The same directory of our own is used when the runtime directory is not
 * private (walpe_dir_is_private): XDG_RUNTIME_DIR=/tmp, a runtime
 * directory that a login script "made" after another account had taken
 * its name, one that is gone. The socket, its lock file ("walpe.sock.lock")
 * and the crash-loop marker ("walpe.sock.loading") are found by name:
 * where others can create files, another account could hold the lock or
 * put a FIFO in the marker's place (the daemon never starts), plant a
 * marker (wallpapers skipped), or serve a socket of its own that GUI,
 * tray and walpe-ctl then take for the daemon (they send it wallpaper
 * paths and believe its answers). When there is no private place at all
 * (the fallback's name is taken by someone else too) the answer is
 * WALPE_SOCKET_NONE, a path under which nothing can exist: no daemon
 * starts and no client connects, instead of doing either in a place
 * someone else controls. */
char *walpe_socket_path (void)
{
    const char *rt = g_get_user_runtime_dir ();
    char *p = g_build_filename (rt, "walpe.sock", NULL);
    gboolean ours = walpe_dir_is_private (rt);
    /* room for the ".loading" / ".lock" files next to it is not needed in
     * sun_path: only the socket itself is bound */
    if (ours && strlen (p) < sizeof (((struct sockaddr_un *) 0)->sun_path))
        return p;
    char *dir = g_strdup_printf ("%s/walpe-%lu", g_get_tmp_dir (), (unsigned long) getuid ());
    g_mkdir (dir, 0700);    /* exists already: checked below */
    GStatBuf st;
    char *alt = g_build_filename (dir, "walpe.sock", NULL);
    gboolean ok = g_lstat (dir, &st) == 0 && S_ISDIR (st.st_mode) && st.st_uid == getuid () &&
                  (st.st_mode & 077) == 0 &&
                  strlen (alt) < sizeof (((struct sockaddr_un *) 0)->sun_path);
    static gsize said;   /* once per process: the GUI asks for the path with every request */
    if (!ours && g_once_init_enter (&said)) {
        if (ok)
            g_warning ("the runtime directory %s is not a directory of your own that only you "
                       "can write to (XDG_RUNTIME_DIR; it should be yours, mode 0700): the "
                       "control socket is kept in %s instead", rt, dir);
        else
            g_warning ("the runtime directory %s is not a directory of your own that only you "
                       "can write to, and neither is %s: there is no safe place for the control "
                       "socket. Set XDG_RUNTIME_DIR to a directory of yours with mode 0700",
                       rt, dir);
        g_once_init_leave (&said, 1);
    }
    g_free (dir);
    if (ok) {
        g_free (p);
        return alt;
    }
    g_free (alt);
    if (!ours) {
        g_free (p);
        return g_strdup (WALPE_SOCKET_NONE);
    }
    return p;   /* too long: walpe_ipc_serve refuses it with a clear message */
}

/* render_scale and texture_slack exist twice: as fields of their own
 * (what the daemon reads at a load and per frame) and as quality knobs
 * (presets, a wallpaper's own settings). Whichever side changed since the
 * last call wins, then both agree. Called after a load, before a save and
 * after the daemon changed either. */
void walpe_config_quality_sync (WalpeConfig *c)
{
    int slack = g_atomic_int_get (&c->texture_slack) > 0 ? WALPE_TEX_SLACK_DEFAULT : 0;
    int scale = CLAMP (g_atomic_int_get (&c->render_scale), WALPE_RENDER_SCALE_MIN, 100);
    if (slack != c->qsync_slack)
        c->quality.tex_slack = slack;
    if (scale != c->qsync_scale)
        c->quality.render_scale = scale;
    walpe_quality_sanitize (&c->quality);
    g_atomic_int_set (&c->texture_slack, c->quality.tex_slack);
    g_atomic_int_set (&c->render_scale, c->quality.render_scale);
    c->qsync_slack = c->quality.tex_slack;
    c->qsync_scale = c->quality.render_scale;
}

static JsonNode *config_to_json (WalpeConfig *c)
{
    walpe_config_quality_sync (c);
    g_autoptr (JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "config_version");
    json_builder_add_int_value (b, WALPE_CONFIG_VERSION);
    json_builder_set_member_name (b, "wallpapers_dir");
    json_builder_add_string_value (b, c->wallpapers_dir);
    json_builder_set_member_name (b, "current");
    json_builder_add_string_value (b, c->current ? c->current : "");
    json_builder_set_member_name (b, "video_backend");
    json_builder_add_string_value (b, c->video_backend);
    json_builder_set_member_name (b, "fps");
    json_builder_add_int_value (b, c->fps);
    json_builder_set_member_name (b, "volume");
    json_builder_add_double_value (b, c->volume);
    json_builder_set_member_name (b, "scene_sound");
    json_builder_add_boolean_value (b, c->scene_sound);
    json_builder_set_member_name (b, "mute");
    json_builder_add_boolean_value (b, c->mute);
    json_builder_set_member_name (b, "paused");
    json_builder_add_boolean_value (b, c->paused);
    json_builder_set_member_name (b, "monitor_current");
    json_builder_begin_object (b);
    if (c->monitor_current) {
        GHashTableIter it;
        const char *k, *v;
        g_hash_table_iter_init (&it, c->monitor_current);
        while (g_hash_table_iter_next (&it, (gpointer *) &k, (gpointer *) &v)) {
            json_builder_set_member_name (b, k);
            json_builder_add_string_value (b, v);
        }
    }
    json_builder_end_object (b);
    json_builder_set_member_name (b, "pause_fullscreen");
    json_builder_add_boolean_value (b, c->pause_fullscreen);
    /* written only when set: an untouched config keeps its bytes */
    if (c->pause_fullscreen_all) {
        json_builder_set_member_name (b, "pause_fullscreen_all");
        json_builder_add_boolean_value (b, TRUE);
    }
    json_builder_set_member_name (b, "transparent_windows");
    json_builder_add_string_value (b, c->transparent_windows);
    json_builder_set_member_name (b, "pause_covered");
    json_builder_add_boolean_value (b, c->pause_covered);
    json_builder_set_member_name (b, "pause_dpms");
    json_builder_add_boolean_value (b, c->pause_dpms);
    json_builder_set_member_name (b, "cover_transparent");
    json_builder_add_boolean_value (b, c->cover_transparent);
    json_builder_set_member_name (b, "render_shared");
    json_builder_add_boolean_value (b, c->render_shared);
    /* written only when set: an untouched config keeps its bytes */
    if (c->pointer_share) {
        json_builder_set_member_name (b, "pointer_share");
        json_builder_add_boolean_value (b, TRUE);
    }
    if (c->skip_hidden != WALPE_SKIP_OFF) {
        json_builder_set_member_name (b, "skip_hidden");
        json_builder_add_string_value (b, walpe_skip_hidden_name (c->skip_hidden));
    }
    if (c->skip_hidden_margin != WALPE_SKIP_MARGIN_DEFAULT) {
        json_builder_set_member_name (b, "skip_hidden_margin");
        json_builder_add_int_value (b, c->skip_hidden_margin);
    }
    if (c->skip_hidden_inset) {
        json_builder_set_member_name (b, "skip_hidden_inset");
        json_builder_add_int_value (b, c->skip_hidden_inset);
    }
    /* the window system: written only when chosen, like the keys above */
    if (c->backend == WALPE_BACKEND_X11 || c->backend == WALPE_BACKEND_WAYLAND) {
        json_builder_set_member_name (b, "backend");
        json_builder_add_string_value (b, walpe_backend_name (c->backend));
    }
    if (c->wayland_pointer) {
        json_builder_set_member_name (b, "wayland_pointer");
        json_builder_add_string_value (b, "off");
    }
    json_builder_set_member_name (b, "fx_cache");
    json_builder_add_boolean_value (b, c->fx_cache);
    json_builder_set_member_name (b, "fx_specialize");
    json_builder_add_boolean_value (b, c->fx_specialize);
    json_builder_set_member_name (b, "fx_quality");
    json_builder_add_string_value (b, c->fx_quality ? c->fx_quality : "exact");
    json_builder_set_member_name (b, "fluid_sim");
    json_builder_add_boolean_value (b, c->fluid_sim);
    json_builder_set_member_name (b, "fluid_quality");
    json_builder_add_string_value (b, walpe_fluid_quality_name ((WalpeFluidQuality) c->fluid_quality));
    json_builder_set_member_name (b, "tex_downscale");
    json_builder_add_boolean_value (b, c->tex_downscale);
    /* "texture_slack" and "render_scale" are written with the quality
     * knobs below (walpe_config_quality_sync keeps both sides equal) */
    json_builder_set_member_name (b, "web_accel");
    json_builder_add_boolean_value (b, c->web_accel);
    /* off | auto stay the switch they were (a file written here is read by
     * an older build as before); one API by name is a word next to it */
    json_builder_set_member_name (b, "video_hwdec");
    json_builder_add_boolean_value (b, c->video_hwdec);
    if (c->video_hwdec_api && *c->video_hwdec_api) {
        json_builder_set_member_name (b, "video_hwdec_api");
        json_builder_add_string_value (b, c->video_hwdec_api);
    }
    json_builder_set_member_name (b, "video_zerocopy");
    json_builder_add_boolean_value (b, c->video_zerocopy);
    json_builder_set_member_name (b, "video_limit");
    json_builder_add_boolean_value (b, c->video_limit);
    json_builder_set_member_name (b, "video_upload");
    json_builder_add_string_value (b, c->video_upload ? c->video_upload : "legacy");
    json_builder_set_member_name (b, "particle_lowres");
    json_builder_add_string_value (b, walpe_particle_lowres_name (c->particle_lowres));
    json_builder_set_member_name (b, "particle_density");
    json_builder_add_int_value (b, c->particle_density);
    json_builder_set_member_name (b, "particle_instancing");
    json_builder_add_boolean_value (b, c->particle_instancing);
    json_builder_set_member_name (b, "parallax");
    json_builder_add_boolean_value (b, c->parallax);
    if (c->features && g_hash_table_size (c->features) > 0) {
        /* renderer feature switches that were set (engine/features.h); a
         * name that is absent keeps its default. Sorted: the file does not
         * change from save to save. */
        json_builder_set_member_name (b, "features");
        json_builder_begin_object (b);
        GList *names = g_list_sort (g_hash_table_get_keys (c->features), (GCompareFunc) strcmp);
        for (GList *l = names; l; l = l->next) {
            json_builder_set_member_name (b, l->data);
            json_builder_add_boolean_value (
                b, GPOINTER_TO_INT (g_hash_table_lookup (c->features, l->data)) == 1);
        }
        g_list_free (names);
        json_builder_end_object (b);
    }
    json_builder_set_member_name (b, "software_render");
    json_builder_add_boolean_value (b, c->software_render);
    json_builder_set_member_name (b, "software_threads");
    json_builder_add_int_value (b, c->software_threads);
    /* the GPU choice: written only when it is not the default, so a
     * config that never used it stays as it was */
    if (c->gpu && strcmp (c->gpu, "auto")) {
        json_builder_set_member_name (b, "gpu");
        json_builder_add_string_value (b, c->gpu);
    }
    if (c->preset_follow_gpu) {
        json_builder_set_member_name (b, "preset_follow_gpu");
        json_builder_add_boolean_value (b, TRUE);
    }
    if (c->preset_gpu_class) {
        json_builder_set_member_name (b, "preset_gpu_class");
        json_builder_add_string_value (b, c->preset_gpu_class);
    }
    if (c->gl_api != WALPE_GL_API_AUTO) {
        json_builder_set_member_name (b, "gl_api");
        json_builder_add_string_value (b, walpe_gl_api_name ((WalpeGlApiPref) c->gl_api));
    }
    if (c->steam_user) {
        json_builder_set_member_name (b, "steam_user");
        json_builder_add_string_value (b, c->steam_user);
    }
    if (c->we_assets) {
        json_builder_set_member_name (b, "we_assets");
        json_builder_add_string_value (b, c->we_assets);
    }
    json_builder_set_member_name (b, "focus_action");
    json_builder_add_string_value (b, walpe_focus_action_name (c->focus_action));
    json_builder_set_member_name (b, "focus_fps");
    json_builder_add_int_value (b, c->focus_fps);
    json_builder_set_member_name (b, "pause_battery");
    json_builder_add_boolean_value (b, c->pause_battery);
    json_builder_set_member_name (b, "battery_fps");
    json_builder_add_int_value (b, c->battery_fps);
    json_builder_set_member_name (b, "pause_idle");
    json_builder_add_int_value (b, c->pause_idle);
    json_builder_set_member_name (b, "preview_anim");
    json_builder_add_string_value (b, walpe_preview_anim_name (c->preview_anim));
    json_builder_set_member_name (b, "vsync");
    json_builder_add_boolean_value (b, c->vsync);
    json_builder_set_member_name (b, "audio");
    json_builder_add_boolean_value (b, c->audio);
    /* quality knobs (quality.h): flat keys + the name of the preset they
     * equal; then each wallpaper's own keys */
    walpe_quality_to_json (b, &c->quality, WALPE_Q_ALL, TRUE);
    json_builder_set_member_name (b, "wallpaper_quality");
    json_builder_begin_object (b);
    if (c->wallpaper_quality) {
        GList *ks = g_list_sort (g_hash_table_get_keys (c->wallpaper_quality),
                                 (GCompareFunc) strcmp);
        for (GList *k = ks; k; k = k->next) {
            const WalpeQualityOv *ov = g_hash_table_lookup (c->wallpaper_quality, k->data);
            if (!ov || !ov->mask)
                continue;
            json_builder_set_member_name (b, k->data);
            json_builder_begin_object (b);
            walpe_quality_to_json (b, &ov->q, ov->mask, FALSE);
            json_builder_end_object (b);
        }
        g_list_free (ks);
    }
    json_builder_end_object (b);
    /* property overrides as a flat map "<path>\x1f<propid>" -> value
     * (streaming builder can't nest without grouping first) */
    json_builder_set_member_name (b, "props");
    json_builder_begin_object (b);
    g_mutex_lock (&props_lock);
    if (c->props) {
        GHashTableIter it;
        g_hash_table_iter_init (&it, c->props);
        const char *k, *v;
        while (g_hash_table_iter_next (&it, (gpointer *) &k, (gpointer *) &v)) {
            json_builder_set_member_name (b, k);
            json_builder_add_string_value (b, v);
        }
    }
    g_mutex_unlock (&props_lock);
    json_builder_end_object (b);
    walpe_layers_config_write (c, b);   /* "layers": per-wallpaper layer adjustments */
    /* render resolution (resolution.h). Each member only while it says
     * something: a config that never touched them keeps its other bytes */
    if (c->monitors && g_hash_table_size (c->monitors)) {
        json_builder_set_member_name (b, "monitors");
        json_builder_begin_object (b);
        GList *ks = g_list_sort (g_hash_table_get_keys (c->monitors), (GCompareFunc) strcmp);
        for (GList *k = ks; k; k = k->next) {
            const WalpeMonitorConf *mc = g_hash_table_lookup (c->monitors, k->data);
            char *rs = walpe_res_format (&mc->res), *tl = walpe_texlimit_format (mc->tex_limit);
            json_builder_set_member_name (b, k->data);
            json_builder_begin_object (b);
            json_builder_set_member_name (b, "resolution");
            json_builder_add_string_value (b, rs);
            json_builder_set_member_name (b, "texture_limit");
            json_builder_add_string_value (b, tl);
            json_builder_set_member_name (b, "asked");
            json_builder_add_boolean_value (b, mc->asked);
            /* what a newer version keeps in the entry stays in it */
            JsonObject *more = c->monitors_extra
                                   ? g_hash_table_lookup (c->monitors_extra, k->data) : NULL;
            GList *ms = more ? json_object_get_members (more) : NULL;
            for (GList *m = ms; m; m = m->next) {
                json_builder_set_member_name (b, m->data);
                json_builder_add_value (b, json_node_copy (json_object_get_member (more, m->data)));
            }
            g_list_free (ms);
            json_builder_end_object (b);
            g_free (rs);
            g_free (tl);
        }
        g_list_free (ks);
        json_builder_end_object (b);
    }
    if (!c->resolution_prompt) {
        json_builder_set_member_name (b, "resolution_prompt");
        json_builder_add_boolean_value (b, FALSE);
    }
    if (c->resolution_migrate) {
        json_builder_set_member_name (b, "resolution_migrate");
        json_builder_add_boolean_value (b, TRUE);
    }
    if (c->scale_filter != WALPE_SCALE_BILINEAR) {
        json_builder_set_member_name (b, "scale_filter");
        json_builder_add_string_value (b, walpe_scale_filter_name (c->scale_filter));
    }
    {
        /* always written, "auto" too. It is also how a file tells that it
         * has been of format 4: a build of format <= 3 that saves it keeps
         * the members it does not know, this one among them, and stamps the
         * file with its own version. Read again here, the member says
         * "nothing to migrate" (walpe_config_load); without it that file
         * looked like an upgrade - outputs nobody was asked about became
         * "asked", and "auto" became "1080p". */
        char *vr = walpe_vidres_format (&c->video_res);
        json_builder_set_member_name (b, "video_resolution");
        json_builder_add_string_value (b, vr);
        g_free (vr);
    }
    json_builder_end_object (b);
    return json_builder_get_root (b);
}

/* the members config_to_json writes only while they are set: a file that
 * has one is not carrying something this build does not know */
static const char *const optional_keys[] = {
    "steam_user", "we_assets", "layers", "monitors", "resolution_prompt", "resolution_migrate",
    "scale_filter",
    "features",     /* the renderer feature switches that were ever set */
};

static const char *const focus_names[] = { "run", "limit", "pause" };
static const char *const preview_names[] = { "always", "hover", "off" };

const char *walpe_skip_hidden_name (int v)
{
    return v == WALPE_SKIP_ALWAYS ? "always" : v == WALPE_SKIP_AUTO ? "auto" : "off";
}

int walpe_skip_hidden_parse (const char *s, int dflt)
{
    if (!s)
        return dflt;
    if (!g_ascii_strcasecmp (s, "off") || !strcmp (s, "0") || !g_ascii_strcasecmp (s, "false"))
        return WALPE_SKIP_OFF;
    if (!g_ascii_strcasecmp (s, "auto") || !strcmp (s, "1") || !g_ascii_strcasecmp (s, "true") ||
        !g_ascii_strcasecmp (s, "on"))
        return WALPE_SKIP_AUTO;
    if (!g_ascii_strcasecmp (s, "always") || !strcmp (s, "2"))
        return WALPE_SKIP_ALWAYS;
    return dflt;
}

const char *walpe_focus_action_name (int v)
{
    return focus_names[CLAMP (v, 0, 2)];
}

int walpe_focus_action_parse (const char *s, int dflt)
{
    for (int i = 0; s && i < 3; i++)
        if (!g_ascii_strcasecmp (s, focus_names[i]))
            return i;
    return dflt;
}

const char *walpe_preview_anim_name (int v)
{
    return preview_names[CLAMP (v, 0, 2)];
}

int walpe_preview_anim_parse (const char *s, int dflt)
{
    for (int i = 0; s && i < 3; i++)
        if (!g_ascii_strcasecmp (s, preview_names[i]))
            return i;
    return dflt;
}

static gint save_count;   /* atomic: the GUI and the tray also save from worker threads */
static char *save_error;    /* why the last save failed (NULL = it was written) */
static GMutex save_error_lock;   /* two saving threads must not both free the old text */

const char *walpe_config_save_error (void)
{
    g_mutex_lock (&save_error_lock);
    const char *e = save_error;
    g_mutex_unlock (&save_error_lock);
    return e;
}

guint walpe_config_save_count (void)
{
    return (guint) g_atomic_int_get (&save_count);
}

int walpe_config_migrate_fps (int version, int fps)
{
    if (version < 2 && fps == WALPE_FPS_LEGACY_DEFAULT)
        return WALPE_FPS_DEFAULT;
    if (version < 3 && fps == WALPE_FPS_UNCAPPED_DEFAULT)
        return WALPE_FPS_DEFAULT;
    return fps;
}

gboolean walpe_config_set_hwdec (WalpeConfig *c, const char *word)
{
    WalpeHwWant w;
    if (!c || !walpe_hwsel_parse (word, &w))
        return FALSE;
    char *low = g_ascii_strdown (word, -1);
    gboolean is_auto = !strcmp (g_strstrip (low), "auto");
    g_free (low);
    c->video_hwdec = w.on;
    if (w.api[0] || is_auto) {
        g_free (c->video_hwdec_api);
        c->video_hwdec_api = w.api[0] ? g_strdup (w.api) : NULL;
    }
    return TRUE;
}

const char *walpe_config_hwdec_word (const WalpeConfig *c)
{
    if (!c || !c->video_hwdec)
        return "off";
    return c->video_hwdec_api && *c->video_hwdec_api ? c->video_hwdec_api : "auto";
}

int walpe_particle_lowres_from_name (const char *name, int fallback)
{
    if (!name)
        return fallback;
    if (!g_ascii_strcasecmp (name, "off") || !g_strcmp0 (name, "0") || !g_ascii_strcasecmp (name, "false"))
        return 0;
    if (!g_ascii_strcasecmp (name, "auto") || !g_strcmp0 (name, "1") || !g_ascii_strcasecmp (name, "on") ||
        !g_ascii_strcasecmp (name, "true"))
        return 1;
    if (!g_ascii_strcasecmp (name, "all") || !g_strcmp0 (name, "2"))
        return 2;
    return fallback;
}

const char *walpe_particle_lowres_name (int mode)
{
    return mode <= 0 ? "off" : mode == 1 ? "auto" : "all";
}

/* names of feature switches: lower case letters, digits and '_', at most
 * 63 bytes - what a "features" member of a config file is held to */
static gboolean feature_name_ok (const char *name)
{
    gsize n = name ? strlen (name) : 0;
    if (n < 1 || n > 63)
        return FALSE;
    for (gsize i = 0; i < n; i++)
        if (!(g_ascii_islower (name[i]) || g_ascii_isdigit (name[i]) || name[i] == '_'))
            return FALSE;
    return TRUE;
}

void walpe_config_set_feature (WalpeConfig *cfg, const char *name, gboolean on)
{
    if (!cfg || !feature_name_ok (name))
        return;
    if (!cfg->features)
        cfg->features = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    if (g_hash_table_size (cfg->features) >= 256 && !g_hash_table_contains (cfg->features, name))
        return;   /* a file with thousands of made-up names */
    g_hash_table_replace (cfg->features, g_strdup (name), GINT_TO_POINTER (on ? 1 : 2));
}

int walpe_config_get_feature (const WalpeConfig *cfg, const char *name)
{
    int v = cfg && cfg->features && name ? GPOINTER_TO_INT (g_hash_table_lookup (cfg->features, name)) : 0;
    return v == 1 ? 1 : v == 2 ? 0 : -1;
}

WalpeConfig *walpe_config_load (void)
{
    WalpeConfig *c = g_new0 (WalpeConfig, 1);
    c->wallpapers_dir    = g_strdup (walpe_default_dir ());
    c->current           = g_strdup ("");
    c->video_backend     = g_strdup ("ffmpeg");
    c->fps               = WALPE_FPS_DEFAULT; /* cap; each monitor is paced at
                                               * min(cap, its refresh rate) */
    c->config_version    = WALPE_CONFIG_VERSION;
    c->volume            = 0.0;
    c->scene_sound       = FALSE;   /* wallpapers are silent until asked */
    c->mute              = FALSE;
    c->paused            = FALSE;
    c->pause_fullscreen  = TRUE;
    c->pause_fullscreen_all = FALSE;    /* only the monitor the window is on */
    c->pause_dpms        = TRUE;
    c->pause_covered     = TRUE;
    c->transparent_windows = g_strdup ("st");
    c->cover_transparent = TRUE;
    c->render_shared     = TRUE;
    c->pointer_share     = FALSE;
    c->skip_hidden       = WALPE_SKIP_OFF;
    c->skip_hidden_margin = WALPE_SKIP_MARGIN_DEFAULT;
    c->skip_hidden_inset = 0;
    c->backend           = WALPE_BACKEND_AUTO;
    c->wayland_pointer   = 0;
    c->fx_cache          = TRUE;
    c->fx_specialize     = FALSE;
    c->fx_quality        = g_strdup ("exact");
    c->fluid_sim         = TRUE;
    c->fluid_quality     = WALPE_FLUID_FULL;
    c->tex_downscale     = TRUE;
    c->texture_slack     = 2;
    c->render_scale      = 100;
    c->particle_lowres   = 1;      /* automatic */
    c->particle_density  = 100;
    c->particle_instancing = TRUE;
    c->parallax          = TRUE;
    c->software_render   = FALSE;
    c->software_threads  = 0;
    c->gpu               = g_strdup ("auto");
    c->preset_follow_gpu = FALSE;
    c->preset_gpu_class  = NULL;
    c->gl_api            = WALPE_GL_API_AUTO;
    c->vsync             = TRUE;
    c->audio             = FALSE;
    c->focus_action      = WALPE_FOCUS_RUN;
    c->focus_fps         = 15;
    c->pause_battery     = FALSE;
    c->battery_fps       = 0;
    c->pause_idle        = 0;
    c->preview_anim      = WALPE_PREVIEW_ALWAYS;
    walpe_quality_preset (WALPE_PRESET_HIGH, &c->quality);
    c->qsync_slack = c->texture_slack;
    c->qsync_scale = c->render_scale;
    c->resolution_prompt = TRUE;
    c->resolution_migrate = FALSE;
    c->scale_filter      = WALPE_SCALE_BILINEAR;
    c->video_res.kind    = WALPE_VIDRES_AUTO;   /* as large as the monitors show it */

    char *path = g_build_filename (g_get_user_config_dir (), "walpe", "config.json", NULL);
    {
        /* the file names the Steam account, every wallpaper path and what
         * the user typed into wallpaper options: for the owner alone (it
         * was written 0644). Before anything else, so that a damaged file
         * kept under another name below has the narrow mode too. */
        GStatBuf st;
        if (g_stat (path, &st) == 0 && S_ISREG (st.st_mode) && st.st_uid == getuid () &&
            (st.st_mode & 077))
            g_chmod (path, 0600);
    }
    JsonNode *root = wpj_parse_file (path);
    if (root && !JSON_NODE_HOLDS_OBJECT (root)) {
        /* valid JSON that is not an object ("[]", a number, a text): as
         * unusable as a damaged file, and kept like one (it was read as
         * an object: a string of criticals, defaults, and the next save
         * wrote over it) */
        json_node_unref (root);
        root = NULL;
    }
    if (!root) {
        /* The file is there but is not a JSON object (cut short by a full
         * disk, a typo from a hand edit, a sync tool): the defaults apply,
         * and the next save would write them over it - wallpaper choice,
         * per-monitor wallpapers and every saved wallpaper option gone for
         * good. Keep the damaged file under another name and say so. */
        char *txt = NULL;
        gsize len = 0;
        if (g_file_get_contents (path, &txt, &len, NULL) && len > 0 && *g_strstrip (txt)) {
            GDateTime *now = g_date_time_new_now_local ();
            char *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
            char *keep = g_strdup_printf ("%s.broken-%s", path, stamp);
            if (g_rename (path, keep) == 0) {
                g_warning ("config: %s is not valid JSON and was not used; it is kept as %s "
                           "(fix it and move it back, or start over: default settings apply)",
                           path, keep);
                c->broken_copy = keep;
            } else {
                g_warning ("config: %s is not valid JSON and was not used; default settings "
                           "apply and will replace it at the next change", path);
                g_free (keep);
            }
            g_free (stamp);
            g_date_time_unref (now);
        }
        g_free (txt);
    }
    if (root) {
        JsonObject *o = json_node_get_object (root);
        const char *d = wpj_str (o, "wallpapers_dir", NULL);
        /* "" would make the path NULL (walpe_expand_path), which the save
         * and the GUI's library code do not expect: keep the default */
        if (d && *d) { g_free (c->wallpapers_dir); c->wallpapers_dir = walpe_expand_path (d); }
        const char *cur = wpj_str (o, "current", NULL);
        if (cur) { g_free (c->current); c->current = walpe_expand_path (cur); }
        const char *vb = wpj_str (o, "video_backend", NULL);
        if (vb && (!g_strcmp0 (vb, "mpv") || !g_strcmp0 (vb, "ffmpeg"))) {
            g_free (c->video_backend);
            c->video_backend = g_strdup (vb);
        }
        /* clamp the double first: a huge value must not reach the cast */
        c->fps = (int) CLAMP (wpj_num (o, "fps", c->fps), 5.0, 200.0);
        {
            int ver = (int) CLAMP (wpj_num (o, "config_version", 1), 1.0, 1000.0);
            int fps = walpe_config_migrate_fps (ver, c->fps);
            c->fps_migrated = fps != c->fps;
            c->fps_migrated_from = c->fps;
            c->fps = fps;
        }
        c->volume = CLAMP (wpj_num (o, "volume", c->volume), 0.0, 1.0);
        c->scene_sound = wpj_bool (o, "scene_sound", c->scene_sound);
        c->mute = wpj_bool (o, "mute", c->mute);
        c->paused = wpj_bool (o, "paused", c->paused);
        { JsonObject *mc = wpj_obj (o, "monitor_current");
          GList *ks = mc ? json_object_get_members (mc) : NULL;
          for (GList *k = ks; k; k = k->next) {
              const char *v = wpj_str (mc, k->data, NULL);
              if (v && *v)
                  walpe_config_set_monitor_path (c, k->data, v);
          }
          g_list_free (ks); }
        c->pause_fullscreen = wpj_bool (o, "pause_fullscreen", c->pause_fullscreen);
        c->pause_fullscreen_all = wpj_bool (o, "pause_fullscreen_all", c->pause_fullscreen_all);
        c->pause_dpms = wpj_bool (o, "pause_dpms", c->pause_dpms);
        c->pause_covered = wpj_bool (o, "pause_covered", c->pause_covered);
        { const char *tw = wpj_str (o, "transparent_windows", NULL);
          if (tw) { g_free (c->transparent_windows); c->transparent_windows = g_strdup (tw); } }
        c->cover_transparent = wpj_bool (o, "cover_transparent", c->cover_transparent);
        c->render_shared = wpj_bool (o, "render_shared", c->render_shared);
        c->pointer_share = wpj_bool (o, "pointer_share", c->pointer_share);
        c->skip_hidden = walpe_skip_hidden_parse (wpj_str (o, "skip_hidden", NULL), c->skip_hidden);
        c->skip_hidden_margin = (int) CLAMP (wpj_num (o, "skip_hidden_margin",
                                                      c->skip_hidden_margin), 0.0, 512.0);
        c->skip_hidden_inset = (int) CLAMP (wpj_num (o, "skip_hidden_inset",
                                                     c->skip_hidden_inset), 0.0, 256.0);
        /* a value that is not a backend's name reads as auto */
        c->backend = walpe_backend_parse (wpj_str (o, "backend", NULL), c->backend);
        { const char *wp = wpj_str (o, "wayland_pointer", NULL);
          if (wp) c->wayland_pointer = !g_ascii_strcasecmp (wp, "off"); }
        c->fx_cache = wpj_bool (o, "fx_cache", c->fx_cache);
        c->fx_specialize = wpj_bool (o, "fx_specialize", c->fx_specialize);
        { const char *q = wpj_str (o, "fx_quality", NULL);
          if (q && (!strcmp (q, "exact") || !strcmp (q, "fast"))) {
              g_free (c->fx_quality); c->fx_quality = g_strdup (q); } }
        c->fluid_sim = wpj_bool (o, "fluid_sim", c->fluid_sim);
        { WalpeFluidQuality fq;   /* (a name that is no level keeps the default) */
          if (walpe_fluid_quality_parse (wpj_str (o, "fluid_quality", NULL), &fq))
              c->fluid_quality = (int) fq; }
        c->tex_downscale = wpj_bool (o, "tex_downscale", c->tex_downscale);
        c->texture_slack = wpj_num (o, "texture_slack", c->texture_slack) > 0 ? 2 : 0;
        c->render_scale = (int) CLAMP (wpj_num (o, "render_scale", c->render_scale), 25.0, 100.0);
        c->web_accel = wpj_bool (o, "web_accel", c->web_accel);
        c->video_hwdec = wpj_bool (o, "video_hwdec", c->video_hwdec);
        {
            /* "video_hwdec": a switch, or one of the words off | auto | <api>
             * (a file edited by hand); "video_hwdec_api": the API alone */
            const char *hw = wpj_str (o, "video_hwdec", NULL);
            const char *api = wpj_str (o, "video_hwdec_api", NULL);
            WalpeHwWant w;
            if (hw && *hw)
                walpe_config_set_hwdec (c, hw);
            if (api && walpe_hwsel_parse (api, &w) && w.api[0]) {
                g_free (c->video_hwdec_api);
                c->video_hwdec_api = g_strdup (w.api);
            }
        }
        c->video_zerocopy = wpj_bool (o, "video_zerocopy", c->video_zerocopy);
        c->video_limit = wpj_bool (o, "video_limit", c->video_limit);
        { const char *u = wpj_str (o, "video_upload", NULL);
          if (u && *u && strlen (u) < 40) {
              g_free (c->video_upload); c->video_upload = g_strdup (u); } }
        c->particle_lowres = walpe_particle_lowres_from_name (wpj_str (o, "particle_lowres", NULL),
                                                              c->particle_lowres);
        c->particle_density = (int) CLAMP (wpj_num (o, "particle_density", c->particle_density),
                                           5.0, 100.0);
        c->particle_instancing = wpj_bool (o, "particle_instancing", c->particle_instancing);
        c->parallax = wpj_bool (o, "parallax", c->parallax);
        { /* renderer feature switches: every switch value under a sane name
           * is kept, also one this build has no feature for */
          JsonObject *fo = wpj_obj (o, "features");
          GList *names = fo ? json_object_get_members (fo) : NULL;
          for (GList *l = names; l; l = l->next) {
              JsonNode *fn = json_object_get_member (fo, l->data);
              if (fn && JSON_NODE_HOLDS_VALUE (fn) && json_node_get_value_type (fn) == G_TYPE_BOOLEAN)
                  walpe_config_set_feature (c, l->data, json_node_get_boolean (fn));
          }
          g_list_free (names); }
        c->software_render = wpj_bool (o, "software_render", c->software_render);
        c->software_threads = (int) CLAMP (wpj_num (o, "software_threads", c->software_threads),
                                           0.0, 32.0);
        { char *g = walpe_gpu_value_normalize (wpj_str (o, "gpu", NULL));
          if (g) { g_free (c->gpu); c->gpu = g; } }
        c->preset_follow_gpu = wpj_bool (o, "preset_follow_gpu", c->preset_follow_gpu);
        { const char *pc = wpj_str (o, "preset_gpu_class", NULL);
          if (pc && *pc && strlen (pc) < 24) {
              g_free (c->preset_gpu_class); c->preset_gpu_class = g_strdup (pc); } }
        { WalpeGlApiPref ga;    /* anything but the three names: auto */
          if (walpe_gl_api_parse (wpj_str (o, "gl_api", NULL), &ga))
              c->gl_api = (int) ga; }
        { const char *su = wpj_str (o, "steam_user", NULL);
          if (su && *su) { g_free (c->steam_user); c->steam_user = g_strdup (su); } }
        { const char *wa = wpj_str (o, "we_assets", NULL);
          if (wa && *wa) c->we_assets = walpe_expand_path (wa); }
        c->vsync = wpj_bool (o, "vsync", c->vsync);
        c->audio = wpj_bool (o, "audio", c->audio);
        c->focus_action = walpe_focus_action_parse (wpj_str (o, "focus_action", NULL), c->focus_action);
        c->focus_fps = (int) CLAMP (wpj_num (o, "focus_fps", c->focus_fps), 1.0, 200.0);
        c->pause_battery = wpj_bool (o, "pause_battery", c->pause_battery);
        c->battery_fps = (int) CLAMP (wpj_num (o, "battery_fps", c->battery_fps), 0.0, 200.0);
        c->pause_idle = (int) CLAMP (wpj_num (o, "pause_idle", c->pause_idle), 0.0, 86400.0);
        c->preview_anim = walpe_preview_anim_parse (wpj_str (o, "preview_anim", NULL), c->preview_anim);
        walpe_quality_load_json (o, &c->quality, NULL);
        /* the two keys both sides read: a key in the file is taken as the
         * field read it (clamped); without it the knob's value counts (a
         * preset's name alone sets it) */
        if (json_object_has_member (o, "texture_slack"))
            c->quality.tex_slack = c->texture_slack;
        else
            c->texture_slack = c->quality.tex_slack;
        if (json_object_has_member (o, "render_scale"))
            c->quality.render_scale = c->render_scale;
        else
            c->render_scale = c->quality.render_scale;
        c->qsync_slack = c->texture_slack;
        c->qsync_scale = c->render_scale;
        { JsonObject *wq = wpj_obj (o, "wallpaper_quality");
          GList *ks = wq ? json_object_get_members (wq) : NULL;
          for (GList *k = ks; k; k = k->next) {
              WalpeQualityOv ov = { 0 };
              walpe_quality_preset (WALPE_PRESET_HIGH, &ov.q);
              walpe_quality_load_json (wpj_obj (wq, k->data), &ov.q, &ov.mask);
              if (ov.mask && *(const char *) k->data)
                  walpe_config_set_quality_ov (c, k->data, &ov);
          }
          g_list_free (ks); }
        { JsonObject *pr = wpj_obj (o, "props");
          if (pr) {
              c->props = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
              GList *ks = json_object_get_members (pr);
              for (GList *k = ks; k; k = k->next) {
                  const char *v = wpj_str (pr, k->data, NULL);
                  if (!v)
                      continue;
                  /* keys saved under another spelling of the path ("dir/")
                   * move to the canonical one */
                  const char *sep = strchr (k->data, '\x1f');
                  char *kp = sep ? g_strndup (k->data, (gsize) (sep - (const char *) k->data)) : NULL;
                  char *cp = kp ? walpe_expand_path (kp) : NULL;
                  if (cp && sep)
                      g_hash_table_insert (c->props, g_strconcat (cp, sep, NULL), g_strdup (v));
                  else
                      g_hash_table_insert (c->props, g_strdup (k->data), g_strdup (v));
                  g_free (cp);
                  g_free (kp);
              }
              g_list_free (ks);
          } }
        walpe_layers_config_read (c, o);
        {
            /* render resolution (resolution.h). A file of a version that
             * had none: what that version did is what it says now - the
             * outputs its user had run at their own mode without a
             * question (entered when the daemon knows them:
             * resolution_migrate), and videos are imported and converted
             * within 1920 x 1080 as they were.
             * Not a file that has "video_resolution": format 4 always
             * writes it, so this one was of format 4 before a build of an
             * older format saved it (and kept what it did not know). What
             * it says holds: no upgrade step, the outputs without an entry
             * are still to be asked about, auto is still auto. */
            int fver = (int) CLAMP (wpj_num (o, "config_version", 1), 1.0, 1000.0);
            if (fver < 4 && !json_object_has_member (o, "video_resolution")) {
                c->resolution_migrate = TRUE;
                walpe_vidres_parse ("1080p", &c->video_res, NULL);
            }
            c->resolution_migrate = wpj_bool (o, "resolution_migrate", c->resolution_migrate);
            c->resolution_prompt = wpj_bool (o, "resolution_prompt", c->resolution_prompt);
            { int f = walpe_scale_filter_parse (wpj_str (o, "scale_filter", NULL));
              if (f >= 0)
                  c->scale_filter = f; }
            { const char *vr = wpj_str (o, "video_resolution", NULL);
              if (vr)   /* (a value that is not one keeps what is set) */
                  walpe_vidres_parse (vr, &c->video_res, NULL); }
            JsonObject *mo = wpj_obj (o, "monitors");
            GList *ks = mo ? json_object_get_members (mo) : NULL;
            for (GList *k = ks; k; k = k->next) {
                JsonObject *e = wpj_obj (mo, k->data);
                if (!e || !walpe_monitor_name_ok (k->data))
                    continue;
                if (c->monitors && g_hash_table_size (c->monitors) >= WALPE_MONITORS_READ_MAX) {
                    /* (every entry is in each listing and each save) */
                    g_warning ("config: \"monitors\" has more than %d entries: the rest is not "
                               "used and will not be saved again", WALPE_MONITORS_READ_MAX);
                    break;
                }
                /* a value that is not one leaves that part at its default */
                WalpeMonitorConf mc = { { WALPE_RES_NATIVE, 0, 0, 0 }, WALPE_TEXLIMIT_AUTO, TRUE };
                walpe_res_parse (wpj_str (e, "resolution", "native"), &mc.res, NULL);
                JsonNode *tn = json_object_get_member (e, "texture_limit");
                if (tn && JSON_NODE_HOLDS_VALUE (tn)) {
                    if (json_node_get_value_type (tn) == G_TYPE_STRING) {
                        walpe_texlimit_parse (json_node_get_string (tn), &mc.tex_limit, NULL);
                    } else if (json_node_get_value_type (tn) == G_TYPE_INT64 ||
                               json_node_get_value_type (tn) == G_TYPE_DOUBLE) {
                        double px = json_node_get_double (tn);
                        if (px >= WALPE_TEXLIMIT_MIN && px <= WALPE_TEXLIMIT_MAX)
                            mc.tex_limit = (int) px;
                    }
                }
                mc.asked = wpj_bool (e, "asked", TRUE);
                walpe_config_set_monitor (c, k->data, &mc);
                /* the members of the entry this build does not read (a
                 * newer version's): kept with it, as `extra` keeps the
                 * unknown members of the file. A save wrote the three
                 * known ones only, so a start of this version between two
                 * of a newer one took them out of the file. */
                GList *ems = json_object_get_members (e);
                for (GList *m = ems; m; m = m->next) {
                    if (!strcmp (m->data, "resolution") || !strcmp (m->data, "texture_limit") ||
                        !strcmp (m->data, "asked"))
                        continue;
                    if (!c->monitors_extra)
                        c->monitors_extra = g_hash_table_new_full (
                            g_str_hash, g_str_equal, g_free, (GDestroyNotify) json_object_unref);
                    JsonObject *more = g_hash_table_lookup (c->monitors_extra, k->data);
                    if (!more) {
                        more = json_object_new ();
                        g_hash_table_insert (c->monitors_extra, g_strdup (k->data), more);
                    }
                    json_object_set_member (more, m->data,
                                            json_node_copy (json_object_get_member (e, m->data)));
                }
                g_list_free (ems);
            }
            g_list_free (ks);
        }
        {
            /* what this build does not read stays as it is (config.h
             * `extra`): every member of the file that a save of the
             * loaded settings would not write */
            JsonNode *mine = config_to_json (c);
            JsonObject *mo = json_node_get_object (mine);
            GList *ks = json_object_get_members (o);
            for (GList *k = ks; k; k = k->next) {
                const char *name = k->data;
                gboolean optional = FALSE;   /* written only while set: not unknown */
                for (guint q = 0; q < G_N_ELEMENTS (optional_keys); q++)
                    optional = optional || !strcmp (name, optional_keys[q]);
                if (json_object_has_member (mo, name) || optional)
                    continue;
                if (!c->extra)
                    c->extra = json_object_new ();
                json_object_set_member (c->extra, name,
                                        json_node_copy (json_object_get_member (o, name)));
            }
            g_list_free (ks);
            json_node_unref (mine);
        }
        json_node_unref (root);
    } else {
        walpe_layers_config_read (c, NULL);
    }
    g_free (path);
    /* (under the lock of its readers: the GUI and the tray load a config on
     * worker threads as well as on their main thread) */
    g_mutex_lock (&props_lock);
    active_config = c;
    g_mutex_unlock (&props_lock);
    return c;
}

gboolean walpe_config_save (WalpeConfig *cfg)
{
    g_atomic_int_inc (&save_count);
    char *dir = g_build_filename (g_get_user_config_dir (), "walpe", NULL);
    g_mkdir_with_parents (dir, 0700);   /* created private; an existing one keeps its mode */
    char *path = g_build_filename (dir, "config.json", NULL);
    g_free (dir);

    JsonNode *root = config_to_json (cfg);
    if (cfg->extra) {
        /* members this build does not know go back into the file */
        JsonObject *ro = json_node_get_object (root);
        GList *ks = json_object_get_members (cfg->extra);
        for (GList *k = ks; k; k = k->next)
            if (!json_object_has_member (ro, k->data))
                json_object_set_member (ro, k->data,
                                        json_node_copy (json_object_get_member (cfg->extra, k->data)));
        g_list_free (ks);
    }
    g_autoptr (JsonGenerator) gen = json_generator_new ();
    json_generator_set_root (gen, root);
    json_generator_set_pretty (gen, TRUE);
    char *data = json_generator_to_data (gen, NULL);
    GError *err = NULL;
    /* 0600 (see walpe_config_load), written next to the file and renamed */
    gboolean ok = walpe_write_private (path, data, -1, &err);
    if (!ok)
        g_warning ("config save failed: %s", err ? err->message : "?");
    /* kept for whoever answers the request that made the change: the
     * warning alone left `walpe-ctl set` and the settings window saying
     * "ok" for a change that is gone at the next start */
    char *why = ok ? NULL : g_strdup (err ? err->message : "config.json could not be written");
    g_mutex_lock (&save_error_lock);
    g_free (save_error);
    save_error = why;
    g_mutex_unlock (&save_error_lock);
    g_clear_error (&err);
    g_free (data);
    json_node_unref (root);
    g_free (path);
    return ok;
}

void walpe_config_free (WalpeConfig *cfg)
{
    if (!cfg) return;
    /* no reader may find it any more before anything of it is freed
     * (walpe_config_get_prop looks at active_config under this lock) */
    g_mutex_lock (&props_lock);
    if (active_config == cfg)
        active_config = NULL;
    g_mutex_unlock (&props_lock);
    g_free (cfg->gpu);
    g_free (cfg->preset_gpu_class);
    g_free (cfg->wallpapers_dir);
    g_free (cfg->current);
    if (cfg->monitor_current)
        g_hash_table_unref (cfg->monitor_current);
    g_free (cfg->video_backend);
    g_free (cfg->transparent_windows);
    g_free (cfg->steam_user);
    g_free (cfg->we_assets);
    g_free (cfg->fx_quality);
    g_free (cfg->video_upload);
    g_free (cfg->video_hwdec_api);
    g_free (cfg->broken_copy);
    if (cfg->props)
        g_hash_table_unref (cfg->props);
    if (cfg->features)
        g_hash_table_unref (cfg->features);
    walpe_layers_config_free (cfg);
    if (cfg->wallpaper_quality)
        g_hash_table_unref (cfg->wallpaper_quality);
    if (cfg->monitors)
        g_hash_table_unref (cfg->monitors);
    if (cfg->monitors_extra)
        g_hash_table_unref (cfg->monitors_extra);
    g_strfreev (cfg->outputs);
    if (cfg->extra)
        json_object_unref (cfg->extra);
    g_free (cfg);
}

/* ------------------------------------------------- property overrides */

char *walpe_config_get_prop (const char *path, const char *key)
{
    if (!path || !key)
        return NULL;
    /* Render threads ask here while the main thread may be storing the
     * first option anyone ever set: walpe_config_set_prop makes the table
     * under props_lock. Whether there is a table is therefore looked at
     * under the lock as well (it used to be tested before it). Asked while
     * a wallpaper loads and when one of its options changes, not per
     * frame: two uncontended locks instead of one. */
    g_mutex_lock (&props_lock);
    gboolean any = active_config && active_config->props;
    g_mutex_unlock (&props_lock);
    if (!any)
        return NULL;
    char *cp = walpe_expand_path (path);   /* any spelling of the path finds its options */
    char *k = g_strconcat (cp ? cp : path, "\x1f", key, NULL);
    g_free (cp);
    g_mutex_lock (&props_lock);
    const char *v = active_config && active_config->props
                        ? g_hash_table_lookup (active_config->props, k) : NULL;
    char *out = v ? g_strdup (v) : NULL;
    g_mutex_unlock (&props_lock);
    g_free (k);
    return out;
}

void walpe_config_set_prop (WalpeConfig *cfg, const char *path,
                            const char *key, const char *value)
{
    if (!cfg || !path || !key)
        return;
    g_mutex_lock (&props_lock);
    if (!cfg->props)
        cfg->props = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    char *cp = walpe_expand_path (path);
    char *k = g_strconcat (cp ? cp : path, "\x1f", key, NULL);
    g_free (cp);
    if (value)
        g_hash_table_insert (cfg->props, k, g_strdup (value));
    else {
        g_hash_table_remove (cfg->props, k);
        g_free (k);
    }
    g_mutex_unlock (&props_lock);
}

guint walpe_config_clear_props (WalpeConfig *cfg, const char *path)
{
    if (!cfg || !path || !cfg->props)
        return 0;
    char *cp = walpe_expand_path (path);
    char *prefix = g_strconcat (cp ? cp : path, "\x1f", NULL);
    g_free (cp);
    gsize pl = strlen (prefix);
    guint n = 0;
    g_mutex_lock (&props_lock);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init (&it, cfg->props);
    while (g_hash_table_iter_next (&it, &k, NULL))
        if (!strncmp (k, prefix, pl)) {
            g_hash_table_iter_remove (&it);
            n++;
        }
    g_mutex_unlock (&props_lock);
    g_free (prefix);
    return n;
}

void walpe_config_set_outputs (WalpeConfig *cfg, const char *const *names, guint n,
                               gboolean other_spellings)
{
    if (!cfg)
        return;
    g_strfreev (cfg->outputs);
    cfg->outputs = g_new0 (char *, (gsize) n + 1);
    for (guint i = 0, k = 0; names && i < n; i++)
        if (names[i])
            cfg->outputs[k++] = g_strdup (names[i]);
    cfg->other_spellings = other_spellings;
}

/* the key of `t` (output name -> ...) that holds for output `name`: the
 * name itself, or the entry under another spelling of its connector
 * (walpe_config_set_outputs) */
static const char *output_key (const WalpeConfig *cfg, GHashTable *t, const char *name)
{
    if (!t || !name || !cfg->other_spellings || g_hash_table_contains (t, name))
        return name;
    const char *found = NULL;
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init (&it, t);
    while (g_hash_table_iter_next (&it, &k, NULL)) {
        if (!walpe_output_name_same (k, name))
            continue;
        /* the entry of an output that is connected itself is that output's */
        gboolean theirs = FALSE;
        for (guint i = 0; cfg->outputs && cfg->outputs[i]; i++)
            theirs = theirs || !strcmp (cfg->outputs[i], k);
        if (theirs)
            continue;
        if (found)
            return name;    /* two of them: nobody can tell which is meant */
        found = k;
    }
    return found ? found : name;
}

/* drop what holds for `name`: its own entry and one under another spelling
 * (which would hold as soon as its own is gone) */
static void output_key_remove (const WalpeConfig *cfg, GHashTable *t, const char *name)
{
    for (int i = 0; t && i < 2; i++)
        if (!g_hash_table_remove (t, output_key (cfg, t, name)))
            break;
}

const char *walpe_config_monitor_path (WalpeConfig *cfg, const char *name)
{
    const char *p = name && cfg->monitor_current
                        ? g_hash_table_lookup (cfg->monitor_current,
                                               output_key (cfg, cfg->monitor_current, name))
                        : NULL;
    return p ? p : (cfg->current ? cfg->current : "");
}

void walpe_config_set_monitor_path (WalpeConfig *cfg, const char *name, const char *path)
{
    if (!name || !*name)
        return;
    if (!path) {
        output_key_remove (cfg, cfg->monitor_current, name);
        return;
    }
    if (!cfg->monitor_current)
        cfg->monitor_current = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_replace (cfg->monitor_current, g_strdup (name), walpe_expand_path (path));
}

/* ------------------------------------------ per-output render resolution */

const WalpeMonitorConf *walpe_config_monitor (WalpeConfig *cfg, const char *name)
{
    return cfg && cfg->monitors && name
               ? g_hash_table_lookup (cfg->monitors, output_key (cfg, cfg->monitors, name)) : NULL;
}

void walpe_config_monitor_get (WalpeConfig *cfg, const char *name, WalpeMonitorConf *out)
{
    const WalpeMonitorConf *mc = walpe_config_monitor (cfg, name);
    if (mc) {
        *out = *mc;
    } else {
        /* no entry: the output's own mode, textures as the render size asks,
         * and nobody has been asked about it */
        memset (out, 0, sizeof (*out));
        out->res.kind = WALPE_RES_NATIVE;
        out->tex_limit = WALPE_TEXLIMIT_AUTO;
        out->asked = FALSE;
    }
}

void walpe_config_set_monitor (WalpeConfig *cfg, const char *name, const WalpeMonitorConf *mc)
{
    if (!cfg || !walpe_monitor_name_ok (name))
        return;
    if (!mc) {
        /* (the whole entry goes, also what this build did not know of it) */
        for (int i = 0; cfg->monitors && i < 2; i++) {
            char *k = g_strdup (output_key (cfg, cfg->monitors, name));
            gboolean was = g_hash_table_remove (cfg->monitors, k);
            if (cfg->monitors_extra)
                g_hash_table_remove (cfg->monitors_extra, k);
            g_free (k);
            if (!was)
                break;
        }
        return;
    }
    if (!cfg->monitors)
        cfg->monitors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    WalpeMonitorConf *copy = g_memdup2 (mc, sizeof (*mc));
    copy->asked = copy->asked != FALSE;
    g_hash_table_replace (cfg->monitors, g_strdup (name), copy);
}

gboolean walpe_config_monitors_migrate (WalpeConfig *cfg, const char *const *names, guint n)
{
    if (!cfg || !cfg->resolution_migrate)
        return FALSE;
    for (guint i = 0; names && i < n; i++) {
        if (!walpe_monitor_name_ok (names[i]) || walpe_config_monitor (cfg, names[i]))
            continue;
        WalpeMonitorConf mc = { { WALPE_RES_NATIVE, 0, 0, 0 }, WALPE_TEXLIMIT_AUTO, TRUE };
        walpe_config_set_monitor (cfg, names[i], &mc);
    }
    cfg->resolution_migrate = FALSE;
    return TRUE;
}

/* --------------------------------------------- per-wallpaper quality */

const WalpeQualityOv *walpe_config_quality_ov (WalpeConfig *cfg, const char *path)
{
    return cfg && cfg->wallpaper_quality && path && *path
               ? g_hash_table_lookup (cfg->wallpaper_quality, path) : NULL;
}

void walpe_config_set_quality_ov (WalpeConfig *cfg, const char *path, const WalpeQualityOv *ov)
{
    if (!cfg || !path || !*path)
        return;
    if (!ov || !ov->mask) {
        if (cfg->wallpaper_quality)
            g_hash_table_remove (cfg->wallpaper_quality, path);
        return;
    }
    if (!cfg->wallpaper_quality)
        cfg->wallpaper_quality = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    WalpeQualityOv *copy = g_memdup2 (ov, sizeof (*ov));
    copy->mask &= WALPE_Q_ALL;
    walpe_quality_sanitize (&copy->q);
    g_hash_table_replace (cfg->wallpaper_quality, g_strdup (path), copy);
}

void walpe_config_quality_for (WalpeConfig *cfg, const char *path, WalpeQuality *out)
{
    walpe_quality_effective (&cfg->quality, walpe_config_quality_ov (cfg, path), out);
}

guint walpe_config_has_keys (WalpeConfig *cfg, const char *const *keys, guint n)
{
    guint mask = 0;
    JsonNode *root = config_to_json (cfg);
    JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    for (guint i = 0; o && i < n && i < 32; i++)
        if (json_object_has_member (o, keys[i]))
            mask |= 1u << i;
    if (root)
        json_node_unref (root);
    return mask;
}

guint walpe_config_quality_forget (WalpeConfig *cfg, const char *path)
{
    return cfg && cfg->wallpaper_quality && path && g_hash_table_remove (cfg->wallpaper_quality, path)
               ? 1 : 0;
}
