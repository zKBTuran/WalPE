#include "walpe-build-config.h" /* HAVE_WEB (factory gating only) */
#include "content.h"
#include "common/util.h"
#include "image.h"
#include "gif.h"
#include "scene.h"
#include "video.h"
#include "pkg.h"
#include "scenesound.h" /* is sound on for a wallpaper: its package's sound files */
#include "scenenode.h" /* walpe_we_assets_hint */
#include "common/workshop.h"
#include "common/config.h" /* walpe_default_dir */
#include "fit.h"
#include <math.h>
#include <string.h>

/* frame_align: see content.h */
gint walpe_frame_align_ = 0;
gint walpe_load_stop_ = 0;       /* content.h walpe_load_stopping */

void walpe_frame_align_set (gboolean on)
{
    g_atomic_int_set (&walpe_frame_align_, on ? 1 : 0);
}

gboolean walpe_frame_align_get (void)
{
    return g_atomic_int_get (&walpe_frame_align_) != 0;
}

#ifdef HAVE_WEB
#include "web.h"
#endif

const char *walpe_type_name (WalpeType t)
{
    switch (t) {
    case WALPE_TYPE_SCENE: return "scene";
    case WALPE_TYPE_VIDEO: return "video";
    case WALPE_TYPE_GIF: return "gif";
    case WALPE_TYPE_IMAGE: return "image";
    case WALPE_TYPE_WEB: return "web";
    case WALPE_TYPE_UNSUPPORTED: return "unsupported";
    default: return "none";
    }
}

static const char *video_exts[] = { "mp4", "mkv", "webm", "mov", "avi", "m4v", "ts", "flv", "wmv", "ogv", NULL };
static const char *image_exts[] = { "png", "jpg", "jpeg", "bmp", "tga", "webp", "avif", "jxl", "tex", NULL };

static gboolean has_ext (const char *file, const char **exts)
{
    const char *dot = strrchr (file, '.');
    if (!dot)
        return FALSE;
    dot++;
    for (int i = 0; exts[i]; i++)
        if (!g_ascii_strcasecmp (dot, exts[i]))
            return TRUE;
    return FALSE;
}

/* project.json of a dir as an object node (unref), or NULL */
static JsonNode *read_project (const char *dir)
{
    char *pj = g_build_filename (dir, "project.json", NULL);
    JsonNode *root = wpj_parse_file (pj);
    g_free (pj);
    if (root && json_node_get_node_type (root) != JSON_NODE_OBJECT) {
        json_node_unref (root);
        root = NULL;
    }
    return root;
}

/* "dependency" of a preset (no "file", no scene/pkg of its own) */
static char *preset_dependency (const char *dir)
{
    JsonNode *root = read_project (dir);
    if (!root)
        return NULL;
    JsonObject *o = json_node_get_object (root);
    char *dep = NULL;
    JsonNode *dn = json_object_get_member (o, "dependency");
    if (dn && json_node_get_node_type (dn) == JSON_NODE_VALUE && !wpj_str (o, "file", NULL)) {
        if (json_node_get_value_type (dn) == G_TYPE_STRING)
            dep = g_strdup (json_node_get_string (dn));
        else
            dep = g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (dn));
    }
    json_node_unref (root);
    /* workshop ids only: never a path */
    if (dep && (!*dep || strspn (dep, "0123456789") != strlen (dep))) {
        g_free (dep);
        dep = NULL;
    }
    return dep;
}

char *walpe_preset_base_dir (const char *path, char **dep_id)
{
    if (dep_id)
        *dep_id = NULL;
    char *abs = walpe_expand_path (path);
    if (!abs || !g_file_test (abs, G_FILE_TEST_IS_DIR)) {
        g_free (abs);
        return NULL;
    }
    char *dep = preset_dependency (abs);
    if (!dep) {
        g_free (abs);
        return NULL;
    }
    GPtrArray *cands = g_ptr_array_new_with_free_func (g_free);
    char *parent = g_path_get_dirname (abs);
    g_ptr_array_add (cands, g_build_filename (parent, dep, NULL));
    g_free (parent);
    char **libs = walpe_workshop_steam_libraries ();
    for (int i = 0; libs[i]; i++)
        g_ptr_array_add (cands, g_build_filename (libs[i], dep, NULL));
    g_strfreev (libs);
    char *wl = walpe_expand_path (walpe_default_dir ());
    g_ptr_array_add (cands, g_build_filename (wl, dep, NULL));
    g_free (wl);
    char *base = NULL;
    for (guint i = 0; i < cands->len && !base; i++) {
        const char *c = g_ptr_array_index (cands, i);
        char *pj = g_build_filename (c, "project.json", NULL);
        if (g_file_test (pj, G_FILE_TEST_IS_REGULAR) && g_strcmp0 (c, abs))
            base = g_strdup (c);
        g_free (pj);
    }
    g_ptr_array_unref (cands);
    if (dep_id)
        *dep_id = dep;
    else
        g_free (dep);
    g_free (abs);
    return base;
}

/* presets chain to their base item. A base that is a preset itself is
 * followed this many times at most: workshop items that name each other as
 * "dependency" (or one item installed in two libraries that names itself)
 * recursed until the stack ran out, in the daemon and in the GUI. */
#define WALPE_PRESET_MAX_DEPTH 4

static WalpeType detect_type_at (const char *path, char **media_path, int depth)
{
    if (media_path)
        *media_path = NULL;
    {
        /* presets take the type of their base item */
        char *base = walpe_preset_base_dir (path, NULL);
        if (base) {
            WalpeType bt = WALPE_TYPE_NONE;
            if (depth < WALPE_PRESET_MAX_DEPTH)
                bt = detect_type_at (base, media_path, depth + 1);
            else
                g_debug ("content: preset chain at %s is too deep (a dependency cycle?)", walpe_log_str (path));
            g_free (base);
            return bt;
        }
    }
    char *abs = walpe_expand_path (path);
    if (!abs)
        return WALPE_TYPE_NONE;

    if (g_file_test (abs, G_FILE_TEST_IS_DIR)) {
        char *pj = g_build_filename (abs, "project.json", NULL);
        JsonNode *root = wpj_parse_file (pj);
        g_free (pj);
        WalpeType t = WALPE_TYPE_NONE;
        if (root) {
            JsonObject *o = json_node_get_object (root);
            const char *type = wpj_str (o, "type", "scene");
            const char *file = wpj_str (o, "file", NULL);
            /* copy before unref: these strings borrow from the node */
            char *type_c = g_strdup (type);
            char *file_c = g_strdup (file);
            json_node_unref (root);
            if (!g_ascii_strcasecmp (type_c, "scene"))
                t = WALPE_TYPE_SCENE;
            else if (!g_ascii_strcasecmp (type_c, "video"))
                t = WALPE_TYPE_VIDEO;
            else if (!g_ascii_strcasecmp (type_c, "web"))
                t = WALPE_TYPE_WEB; /* factory refuses it when built without web */
            else
                t = WALPE_TYPE_UNSUPPORTED;
            /* "file" comes from the item (a Workshop download): it names
             * something inside the item's folder. "../" used to be
             * followed, so a video item could show any video the user can
             * read. */
            if (file_c && (g_path_is_absolute (file_c) || !strcmp (file_c, "..") ||
                           g_str_has_prefix (file_c, "../") || strstr (file_c, "/../") ||
                           g_str_has_suffix (file_c, "/.."))) {
                g_warning ("%s: project.json names a file outside the wallpaper folder (%s); "
                           "not loaded", walpe_log_str (abs), walpe_log_str (file_c));
                t = WALPE_TYPE_UNSUPPORTED;
            } else if (media_path && file_c) {
                /* second check on the built path (by its text and, when it
                 * exists, with links resolved): "file" stays inside the
                 * wallpaper's own directory */
                char *mp = g_build_filename (abs, file_c, NULL);
                if (walpe_path_is_inside (mp, abs))
                    *media_path = mp;
                else {
                    g_debug ("content: %s: \"file\" %s points outside the wallpaper, ignored",
                             walpe_log_str (abs), walpe_log_str (file_c));
                    g_free (mp);
                }
            }
            g_free (type_c);
            g_free (file_c);
        }
        g_free (abs);
        return t;
    }

    if (g_file_test (abs, G_FILE_TEST_IS_REGULAR)) {
        const char *base = strrchr (abs, '/');
        base = base ? base + 1 : abs;
        if (!g_ascii_strcasecmp (base, "project.json") ||
            !g_ascii_strcasecmp (base, "scene.json")) {
            g_free (abs);
            return WALPE_TYPE_SCENE;
        }
        if (has_ext (base, video_exts)) {
            if (media_path)
                *media_path = g_strdup (abs);
            g_free (abs);
            return WALPE_TYPE_VIDEO;
        }
        if (has_ext (base, (const char *[]) { "gif", "webp", NULL })) {
            if (media_path)
                *media_path = g_strdup (abs);
            g_free (abs);
            return WALPE_TYPE_GIF;
        }
        if (has_ext (base, image_exts)) {
            if (media_path)
                *media_path = g_strdup (abs);
            g_free (abs);
            return WALPE_TYPE_IMAGE;
        }
    }
    g_free (abs);
    return WALPE_TYPE_NONE;
}

WalpeType walpe_detect_type (const char *path, char **media_path)
{
    return detect_type_at (path, media_path, 0);
}

/* ---- WalPE's built-in per-wallpaper options (fit.h) */

static const char BUILTIN_FLAT_PROPS[] =
    "{"
    "\"walpe_display\":{\"type\":\"group\",\"order\":900000,\"text\":\"Display\"},"
    "\"walpe_fit\":{\"type\":\"combo\",\"order\":900001,\"text\":\"Alignment\",\"value\":\"cover\","
    "\"options\":[{\"label\":\"Cover (fill the screen, crop)\",\"value\":\"cover\"},"
    "{\"label\":\"Fit (whole picture, black bars)\",\"value\":\"contain\"},"
    "{\"label\":\"Stretch\",\"value\":\"stretch\"},"
    "{\"label\":\"Center (original size)\",\"value\":\"center\"}]},"
    "\"walpe_pos_x\":{\"type\":\"slider\",\"order\":900002,\"text\":\"Horizontal position\","
    "\"condition\":\"walpe_fit.value != 'stretch'\","
    "\"min\":0,\"max\":100,\"step\":1,\"fraction\":false,\"value\":50},"
    "\"walpe_pos_y\":{\"type\":\"slider\",\"order\":900003,\"text\":\"Vertical position\","
    "\"condition\":\"walpe_fit.value != 'stretch'\","
    "\"min\":0,\"max\":100,\"step\":1,\"fraction\":false,\"value\":50},"
    "\"walpe_flip\":{\"type\":\"bool\",\"order\":900004,\"text\":\"Flip horizontally\",\"value\":false}"
    "}";

/* web wallpapers: network access of the page (engine/web.c, default on) */
static const char BUILTIN_WEB_PROPS[] =
    "{"
    "\"walpe_privacy\":{\"type\":\"group\",\"order\":900000,\"text\":\"Privacy\"},"
    "\"walpe_network\":{\"type\":\"bool\",\"order\":900001,"
    "\"text\":\"Allow network access\",\"value\":true}"
    "}";

JsonNode *walpe_content_builtin_props (const char *path)
{
    if (!path || !*path)
        return NULL;
    WalpeType t = walpe_detect_type (path, NULL);
    if (t != WALPE_TYPE_VIDEO && t != WALPE_TYPE_GIF && t != WALPE_TYPE_IMAGE && t != WALPE_TYPE_WEB)
        return NULL;
    JsonParser *p = json_parser_new ();
    JsonNode *n = NULL;
    if (json_parser_load_from_data (p, t == WALPE_TYPE_WEB ? BUILTIN_WEB_PROPS : BUILTIN_FLAT_PROPS,
                                    -1, NULL))
        n = json_parser_steal_root (p);
    g_object_unref (p);
    return n;
}

JsonNode *walpe_content_property_defs (const char *path, JsonNode **localization)
{
    if (localization)
        *localization = NULL;
    char *abs = walpe_expand_path (path);
    if (!abs)
        return NULL;
    JsonNode *own = g_file_test (abs, G_FILE_TEST_IS_DIR) ? read_project (abs) : NULL;
    JsonNode *src = own;
    JsonObject *preset = NULL;
    char *base = own ? walpe_preset_base_dir (abs, NULL) : NULL;
    JsonNode *basej = base ? read_project (base) : NULL;
    if (basej) {
        preset = wpj_obj (json_node_get_object (own), "preset");
        src = basej;
    }
    JsonObject *gen = src ? wpj_obj (json_node_get_object (src), "general") : NULL;
    JsonObject *props = wpj_obj (gen, "properties");
    JsonNode *out = NULL;
    if (props) {
        JsonNode *pn = json_node_new (JSON_NODE_OBJECT);
        json_node_set_object (pn, props);
        out = json_node_copy (pn);      /* deep copy */
        json_node_unref (pn);
        JsonObject *oo = json_node_get_object (out);
        /* "walpe_sound" is WalPE's own key: the wallpaper's sound switch
         * (engine/scenesound.h; `walpe-ctl setprop walpe_sound=on|off`). A
         * project that declares an option of that name got a row of its
         * own wording in the options panel ("Reduce flicker"), and a click
         * on it switched the wallpaper's sound on although the setting
         * scene_sound is off. Scenes have no built-in options that would
         * take the name (below), so the declaration is dropped here. */
        json_object_remove_member (oo, "walpe_sound");
        GList *ks = preset ? json_object_get_members (preset) : NULL;
        for (GList *k = ks; k; k = k->next) {
            JsonNode *pv = json_object_get_member (preset, k->data);
            JsonObject *d = wpj_obj (oo, k->data);
            if (d && pv && json_node_get_node_type (pv) == JSON_NODE_VALUE)
                json_object_set_member (d, "value", json_node_copy (pv));
        }
        g_list_free (ks);
    }
    if (localization && wpj_obj (gen, "localization")) {
        JsonNode *ln = json_node_new (JSON_NODE_OBJECT);
        json_node_set_object (ln, wpj_obj (gen, "localization"));
        *localization = json_node_copy (ln);
        json_node_unref (ln);
    }
    JsonNode *bi = walpe_content_builtin_props (abs);
    if (bi) {
        if (!out) {
            out = bi;
        } else {
            JsonObject *oo = json_node_get_object (out), *bo = json_node_get_object (bi);
            GList *ks = json_object_get_members (bo);
            /* WalPE's own options keep WalPE's definition. A project that
             * declares a property of the same name used to win: a web
             * wallpaper could give "walpe_network" - the switch that
             * takes its network access away - another label ("Reduce
             * motion"), another type, or a condition that hides the row
             * for good. */
            for (GList *k = ks; k; k = k->next)
                json_object_set_member (oo, k->data,
                                        json_node_copy (json_object_get_member (bo, k->data)));
            g_list_free (ks);
            json_node_unref (bi);
        }
    }
    if (basej)
        json_node_unref (basej);
    if (own)
        json_node_unref (own);
    g_free (base);
    g_free (abs);
    return out;
}

void walpe_content_fit (const char *origin, void *fitp)
{
    WalpeFit *f = fitp;
    *f = (WalpeFit) WALPE_FIT_DEFAULT;
    if (!origin)
        return;
    char *v = walpe_config_get_prop (origin, "walpe_fit");
    f->mode = walpe_fit_mode_from_string (v);
    g_free (v);
    /* "nan" parses as a number and passes CLAMP (every comparison with NaN
     * is false): the picture's quad became NaN. Not a number = default. */
    if ((v = walpe_config_get_prop (origin, "walpe_pos_x"))) {
        double d = g_ascii_strtod (v, NULL);
        if (isfinite (d))
            f->pos_x = (float) CLAMP (d / 100.0, 0.0, 1.0);
        g_free (v);
    }
    if ((v = walpe_config_get_prop (origin, "walpe_pos_y"))) {
        double d = g_ascii_strtod (v, NULL);
        if (isfinite (d))
            f->pos_y = (float) CLAMP (d / 100.0, 0.0, 1.0);
        g_free (v);
    }
    if ((v = walpe_config_get_prop (origin, "walpe_flip"))) {
        f->flip_h = !g_ascii_strcasecmp (v, "true") || !g_strcmp0 (v, "1");
        g_free (v);
    }
}

/* first *.pkg in a project dir (WE packaged wallpapers). Like every file a
 * wallpaper brings it is a regular file of that folder (walpe_asset_path):
 * the first entry of that name was taken whatever it was, so a FIFO called
 * scene.pkg blocked the load in open () for good and a symbolic link was
 * followed to a package anywhere on the disk. */
static char *find_pkg (const char *dir)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d)
        return NULL;
    const char *n;
    char *out = NULL;
    while (!out && (n = g_dir_read_name (d))) {
        gsize l = strlen (n);
        if (l > 4 && !g_ascii_strcasecmp (n + l - 4, ".pkg"))
            out = walpe_asset_path (dir, n);
    }
    g_dir_close (d);
    return out;
}

/* A scene loaded from an extracted scene.pkg keeps reading from its cache
 * dir (textures loaded again at another size, fonts, video textures):
 * the dir stays held (pkg.c never evicts a held one) until the content is
 * freed. The content's own free is wrapped for that. */
typedef struct {
    void (*free) (WalpeContent *c);
    char *dir;
} PkgHold;

static GMutex pkg_hold_lock;
static GHashTable *pkg_holds;   /* WalpeContent * -> PkgHold * */

static void pkg_hold_free (WalpeContent *c)
{
    g_mutex_lock (&pkg_hold_lock);
    PkgHold *h = g_hash_table_lookup (pkg_holds, c);
    g_hash_table_remove (pkg_holds, c);
    g_mutex_unlock (&pkg_hold_lock);
    h->free (c);
    walpe_pkg_cache_release (h->dir);
    g_free (h->dir);
    g_free (h);
}

static void pkg_hold_attach (WalpeContent *c, const char *dir)
{
    PkgHold *h = g_new0 (PkgHold, 1);
    h->free = c->free;
    h->dir = g_strdup (dir);
    c->free = pkg_hold_free;
    g_mutex_lock (&pkg_hold_lock);
    if (!pkg_holds)
        pkg_holds = g_hash_table_new (g_direct_hash, g_direct_equal);
    g_hash_table_insert (pkg_holds, c, h);
    g_mutex_unlock (&pkg_hold_lock);
}

/* Is sound switched on for the wallpaper opened as `origin`, in a program
 * that plays sound at all? What scenenode_sound.h's group is made with:
 * the wallpaper's own switch (its option "walpe_sound"), else the setting. */
static gboolean scene_sound_wanted (const char *origin)
{
    if (!walpe_sound_allowed ())
        return FALSE;
    char *sw = walpe_config_get_prop (origin, "walpe_sound");
    int v = walpe_sound_switch_parse (sw);
    g_free (sw);
    return v > 0 || (v < 0 && walpe_sound_enabled ());
}

/* how many presets the load in progress on this thread has followed */
static _Thread_local int preset_depth;

WalpeContent *walpe_content_new (const char *path, const WalpeLoadOpts *opts, GError **err)
{
    char *media = NULL;
    WalpeType t = walpe_detect_type (path, &media);
    WalpeContent *c = NULL;
    char *abs = walpe_expand_path (path);
    if (t == WALPE_TYPE_SCENE && abs && g_file_test (abs, G_FILE_TEST_IS_REGULAR)) {
        /* a scene opened by its project.json or scene.json (a file manager's
         * "open with", a dropped file): the wallpaper is the folder. The file
         * itself was taken for the project directory and nothing loaded. */
        char *dir = g_path_get_dirname (abs);
        char *pj = g_build_filename (dir, "project.json", NULL);
        gboolean has_pj = g_file_test (pj, G_FILE_TEST_IS_REGULAR);
        g_free (pj);
        if (has_pj) {
            c = walpe_content_new (dir, opts, err);
            g_free (dir);
            g_free (media);
            g_free (abs);
            return c;
        }
        /* a scene.json on its own: the scene of its folder */
        g_free (media);
        media = abs;
        abs = dir;
    }
    /* the wallpaper as opened keys user-property overrides and hints the
     * WE install location (pkg scenes load from a cache dir) */
    WalpeLoadOpts o2 = opts ? *opts : (WalpeLoadOpts) { 0 };
    if (!o2.origin_dir)
        o2.origin_dir = abs;
    opts = &o2;
    walpe_we_assets_hint (o2.origin_dir);

    if (!o2.preset) {
        char *dep = NULL;
        char *base = walpe_preset_base_dir (abs, &dep);
        if (dep) {
            if (!base) {
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "preset %s needs workshop item %s, which is not installed "
                             "(not next to it, in a Steam library or the WalPE library; "
                             "walpe-ctl install %s)", path, dep, dep);
                g_free (dep);
                g_free (media);
                g_free (abs);
                return NULL;
            }
            if (preset_depth >= WALPE_PRESET_MAX_DEPTH) {
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS,
                             "preset %s: its chain of base items is too deep "
                             "(a dependency cycle?)", path);
                g_free (base);
                g_free (dep);
                g_free (media);
                g_free (abs);
                return NULL;
            }
            /* load the base item with the preset's values */
            JsonNode *pr = read_project (abs);
            JsonObject *pv = pr ? wpj_obj (json_node_get_object (pr), "preset") : NULL;
            o2.preset = pv ? pv : NULL;
            o2.preset_dir = abs;
            g_message ("content: preset %s -> base %s (%u values)", walpe_log_str (abs), walpe_log_str (base),
                       pv ? json_object_get_size (pv) : 0);
            preset_depth++;
            c = walpe_content_new (base, &o2, err);
            preset_depth--;
            if (pr)
                json_node_unref (pr);
            g_free (base);
            g_free (dep);
            g_free (media);
            g_free (abs);
            return c;
        }
    }

    switch (t) {
    case WALPE_TYPE_SCENE: {
        /* packaged wallpaper: the scene json lives inside scene.pkg —
         * extract to the cache and load from there */
        const char *sj = media ? media : abs;
        char *pkg = !g_file_test (sj, G_FILE_TEST_IS_REGULAR) ? find_pkg (abs) : NULL;
        if (pkg) {
            GError *pe = NULL;
            /* the package's sound files only for a wallpaper that may be
             * heard right now (pkg.h; the scene's sound layers fetch them
             * when sound is switched on later) */
            /* (WALPE_PKG_WHOLE=1: every package is extracted whole at its
             * load, as before copies could be made without sound files) */
            const char *whole = g_getenv ("WALPE_PKG_WHOLE");
            char *cache = walpe_pkg_extract_cached_opts (pkg, abs, (whole && *whole && strcmp (whole, "0")) ||
                                                                   scene_sound_wanted (o2.origin_dir), &pe);
            g_free (pkg);
            if (!cache) {
                g_propagate_error (err, pe);
                break;
            }
            g_message ("content: pkg extracted -> %s", walpe_log_str (cache));
            char *sfile = media ? g_path_get_basename (media) : NULL;
            c = walpe_scene_new (cache, sfile, opts, err);
            if (c)
                pkg_hold_attach (c, cache);
            else
                walpe_pkg_cache_release (cache);
            g_free (sfile);
            g_free (cache);
        } else {
            c = walpe_scene_new (abs, media, opts, err);
        }
        break;
    }
    case WALPE_TYPE_VIDEO:
        c = walpe_video_content_new (media ? media : abs, opts, err);
        break;
    case WALPE_TYPE_GIF:
        c = walpe_gif_new (media ? media : abs, err);
        break;
    case WALPE_TYPE_IMAGE:
        c = walpe_image_new (media ? media : abs, err);
        break;
    case WALPE_TYPE_WEB:
#ifdef HAVE_WEB
        c = walpe_web_new (abs, opts, err);
#else
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "web wallpapers need WalPE built with webkitgtk-6.0 (-Dweb=enabled)");
#endif
        break;
    case WALPE_TYPE_UNSUPPORTED:
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "this wallpaper type ('application' or unknown) cannot run on Linux");
        break;
    default:
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "no wallpaper content found at %s", path);
        break;
    }
    g_free (media);
    g_free (abs);
    return c;
}
