/* modload.c - loader for the optional dlopen'd backends (modules.h) and the
 * core-side forwarders of their public functions (web.h, video.h,
 * videotex.h), so callers stay unchanged. */
#include "walpe-build-config.h"
#include "modules.h"
#include "web.h"
#include "glutil.h"
#include "mp4demux.h"
#include "yuvconv.h"
#include "common/audio.h"
#include "common/media.h"
#include "common/config.h"
#include "common/util.h"
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean walpe_module_file_trusted (const char *path, char **why)
{
    struct stat st, dst;
    char *dir = g_path_get_dirname (path);
    const char *bad = NULL;
    uid_t me = getuid ();
    if (stat (path, &st) != 0 || stat (dir, &dst) != 0)
        bad = "cannot be examined";
    else if (st.st_uid != me && st.st_uid != 0)
        bad = "belongs to another user";
    else if (st.st_mode & S_IWOTH)
        bad = "is writable by everybody";
    else if (dst.st_uid != me && dst.st_uid != 0)
        bad = "is in a directory of another user";
    else if ((dst.st_mode & S_IWOTH) && !(dst.st_mode & S_ISVTX))
        bad = "is in a directory everybody can write to";
    if (bad && why)
        *why = g_strdup_printf ("%s %s: not loaded", path, bad);
    g_free (dir);
    return bad == NULL;
}

#if defined(HAVE_WEB) || defined(HAVE_FFMPEG)
/* the daemon exports these for the modules (modules.dynlist); functions only
 * a module calls would otherwise never be linked in from the static core */
__attribute__ ((used)) static const void *const walpe_module_api[] = {
    (const void *) walpe_audio_get, (const void *) walpe_audio_ref,
    (const void *) walpe_audio_unref, (const void *) walpe_config_get_prop,
    (const void *) walpe_glutil_init, (const void *) walpe_glutil_draw_quad,
    (const void *) walpe_glutil_draw_yuv_range, (const void *) walpe_gl_is_software,
    (const void *) walpe_mat3_cover, (const void *) walpe_media_ref,
    (const void *) walpe_media_unref, (const void *) walpe_media_serial,
    (const void *) walpe_media_get, (const void *) walpe_media_now_clear,
    (const void *) mp4_demux_open_range, (const void *) mp4_demux_free,
    (const void *) mp4_demux_next, (const void *) walpe_yuv420_bgra_ex,
    (const void *) walpe_yuv420_bgra_half, (const void *) walpe_yuv_coeffs,
    (const void *) walpe_video_hwdec, (const void *) walpe_video_note_decoder,
    (const void *) walpe_video_note_frame,
    (const void *) walpe_video_limit, (const void *) walpe_video_frame_limit,
    (const void *) walpe_video_upload_mode, (const void *) walpe_video_note_upload,
    (const void *) walpe_video_hwdec_api, (const void *) walpe_video_zerocopy,
    (const void *) wpj_str, (const void *) wpj_obj,
    (const void *) wpj_num, (const void *) wpj_bool, (const void *) wpj_parse_file,
};
#endif

#if defined(HAVE_WEB) || defined(HAVE_FFMPEG)
/* where modules are looked for, in this order: $WALPE_MODULE_DIR, the
 * executable's directory, the install dir (each g_free, or NULL) */
static void mod_dirs (char *dirs[3], gboolean warn)
{
    const char *env = g_getenv ("WALPE_MODULE_DIR");
    dirs[0] = dirs[1] = NULL;
    /* an absolute directory: a relative one meant "whatever directory the
     * daemon happens to be started in", and the module is code */
    if (env && *env && g_path_is_absolute (env))
        dirs[0] = g_strdup (env);
    else if (env && *env && warn)
        g_warning ("module: WALPE_MODULE_DIR=%s is not an absolute path: ignored", env);
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    if (exe) {
        dirs[1] = g_path_get_dirname (exe);
        g_free (exe);
    }
    dirs[2] = g_strdup (WALPE_MODULEDIR);
}

/* dlopen <name>.so from $WALPE_MODULE_DIR, the executable's directory, then
 * the install dir; returns the entry symbol or NULL (*why = reason, g_free) */
static gpointer mod_load (const char *name, const char *entry, char **why)
{
    char *dirs[3];
    GString *err = g_string_new (NULL);
    mod_dirs (dirs, TRUE);
    gpointer sym = NULL;
    for (int i = 0; i < 3 && !sym; i++) {
        if (!dirs[i])
            continue;
        char *file = g_strconcat (name, ".so", NULL);
        char *path = g_build_filename (dirs[i], file, NULL);
        g_free (file);
        char *untrusted = NULL;
        if (g_file_test (path, G_FILE_TEST_IS_REGULAR) &&
            !walpe_module_file_trusted (path, &untrusted)) {
            g_warning ("module: %s", untrusted);   /* also when a later directory has one */
            g_string_append_printf (err, "%s%s", err->len ? "; " : "", untrusted);
            g_free (untrusted);
        } else if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
            void *h = dlopen (path, RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                g_string_append_printf (err, "%s%s", err->len ? "; " : "", dlerror ());
            } else if (!(sym = dlsym (h, entry))) {
                g_string_append_printf (err, "%s%s: no %s", err->len ? "; " : "", path, entry);
                dlclose (h);
            } else {
                g_message ("module: loaded %s", path);
            }
        }
        g_free (path);
    }
    if (!sym && !err->len)
        g_string_printf (err, "%s.so not found (WALPE_MODULE_DIR, next to the daemon, %s)",
                         name, WALPE_MODULEDIR);
    for (int i = 0; i < 3; i++)
        g_free (dirs[i]);
    *why = sym ? NULL : g_string_free (err, FALSE);
    if (sym)
        g_string_free (err, TRUE);
    return sym;
}
#endif

static G_GNUC_UNUSED GMutex mod_lock;   /* (unused without web and video) */

/* `walpe-ctl mem` (memreport.h): when a module was loaded first and what
 * asked for it, and how often its entries were used since. A module stays
 * loaded for good (modules.h), so "why is it there" is a question about
 * the past: the first caller says who it is in mod_why / mod_why_path
 * (this thread's, set around its walpe_*_module (TRUE) call). */
typedef struct {
    gint64 loaded_us;       /* mod_lock: monotonic time of the load, 0 = not loaded */
    char why[256];          /* mod_lock */
    gint uses;              /* atomic: calls that went into the module to make something */
} ModNote;
static G_GNUC_UNUSED ModNote web_note, video_note;
static G_GNUC_UNUSED __thread const char *mod_why, *mod_why_path;

static G_GNUC_UNUSED void mod_note_loaded (ModNote *n, const char *fallback)
{
    const char *p = mod_why_path;
    gsize pl = p ? strlen (p) : 0;
    n->loaded_us = g_get_monotonic_time ();
    g_snprintf (n->why, sizeof (n->why), "%s%s%s%s", mod_why ? mod_why : fallback, p ? ": " : "",
                pl > 180 ? "..." : "", p ? (pl > 180 ? p + pl - 180 : p) : "");
}

#ifdef HAVE_WEB
static const WalpeWebModule *web_mod;
static gboolean web_tried;
static char *web_err;
static gint web_sound_q16;      /* atomic: walpe_web_sound's master, 0..65536 */
static gint web_sound_on;       /* atomic: ... and its `enabled` */
static char *web_sink_note;     /* walpe_web_sound_sink_dir: what it found (said once, at the load) */
static char *web_registry_dir;  /* walpe_web_registry_dir: made at the load */

void walpe_web_registry_dir (const char *dir)
{
    g_mutex_lock (&mod_lock);
    g_free (web_registry_dir);
    web_registry_dir = g_strdup (dir);
    g_mutex_unlock (&mod_lock);
}

/* The directory itself, beyond its one file. GStreamer does not load a file
 * by name as the module loader does: it loads every plugin it finds in a
 * directory of its path, and looks into the directories below it. And the
 * path is a list: a directory with the list's separator in its name would
 * be read as two entries, the second of them relative to wherever the page
 * process happens to stand. So: no ':' in the name, and nothing in the
 * directory that fails the rule of a module (another account's file, one
 * that everybody can write to). FALSE: *why (g_free) says what. */
static gboolean sink_dir_trusted (const char *dir, char **why)
{
    if (strchr (dir, G_SEARCHPATH_SEPARATOR)) {
        *why = g_strdup_printf ("%s has a '%c' in its name, which separates the entries of GStreamer's plugin "
                                "path: not used", dir, G_SEARCHPATH_SEPARATOR);
        return FALSE;
    }
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *n;
    gboolean ok = d != NULL;
    while (ok && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        g_free (*why);
        *why = NULL;
        ok = walpe_module_file_trusted (p, why);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    else
        *why = g_strdup_printf ("%s cannot be read: not used", dir);
    return ok;
}

/* gst/walpesound.c. Called once, by main () before any thread exists. */
char *walpe_web_sound_sink_dir (void)
{
    char *dirs[3], *found = NULL;
    const char *off = g_getenv ("WALPE_WEB_SOUND_SINK");
    GString *err = g_string_new (NULL);
    mod_dirs (dirs, FALSE);
    for (int i = 0; i < 3; i++) {
        char *dir = dirs[i] ? g_build_filename (dirs[i], "gst", NULL) : NULL;
        char *path = dir ? g_build_filename (dir, WALPE_WEB_SOUND_SINK_FILE, NULL) : NULL;
        char *untrusted = NULL;
        if (found || !path || (off && !strcmp (off, "0")) || !g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
            /* not here */
        } else if (!walpe_module_file_trusted (path, &untrusted) || !sink_dir_trusted (dir, &untrusted)) {
            /* (GStreamer loads whatever is in the directory, into the page process) */
            g_string_append_printf (err, "%s%s", err->len ? "; " : "", untrusted);
        } else {
            found = g_steal_pointer (&dir);
        }
        g_free (untrusted);
        g_free (path);
        g_free (dir);
        g_free (dirs[i]);
    }
#ifdef HAVE_WEB_SOUND_SINK
    const char *none = "not found in a directory gst/ next to walpe-web.so";
#else
    const char *none = "this build has none: no GStreamer headers when it was built";
#endif
    g_free (web_sink_note);
    if (found)
        web_sink_note = g_strdup_printf ("web: the level and the mute of page sound are kept in the page "
                                         "process (%s in %s)", WALPE_WEB_SOUND_SINK_FILE, found);
    else
        web_sink_note = g_strdup_printf ("web: %s is not used (%s): the level and the mute of page sound "
                                         "are the sound server's stream volume and mute, see docs/SOUND.md",
                                         WALPE_WEB_SOUND_SINK_FILE,
                                         off && !strcmp (off, "0") ? "WALPE_WEB_SOUND_SINK=0" :
                                         err->len ? err->str : none);
    g_string_free (err, TRUE);
    return found;
}

const WalpeWebModule *walpe_web_module (gboolean load)
{
    g_mutex_lock (&mod_lock);
    if (!web_mod && load && !web_tried) {
        web_tried = TRUE;
        const WalpeWebModule *(*entry) (void) =
            (const WalpeWebModule *(*) (void)) mod_load ("walpe-web", "walpe_web_module_entry",
                                                         &web_err);
        const WalpeWebModule *m = entry ? entry () : NULL;
        if (m && m->abi == WALPE_MODULE_ABI) {
            web_mod = m;
            mod_note_loaded (&web_note, "the WebKit start-up test");
            /* GStreamer's plugin list of the page processes: its directory
             * has to be there before WebKit starts a process (modules.h
             * walpe_web_registry_dir), and nothing has been started yet */
            if (web_registry_dir && g_mkdir_with_parents (web_registry_dir, 0700) != 0)
                g_warning ("web: %s could not be made (%s): the page processes cannot keep GStreamer's list "
                           "of plugins, a page that plays media starts more slowly",
                           walpe_log_str (web_registry_dir), g_strerror (errno));
            if (web_sink_note)
                g_message ("%s", web_sink_note);
            /* before the module makes its first page (still under
             * mod_lock: a level set meanwhile is handed on after this one) */
            m->web_sound (g_atomic_int_get (&web_sound_q16) / 65536.0, g_atomic_int_get (&web_sound_on));
        }
        else if (m)
            web_err = g_strdup ("walpe-web.so: ABI mismatch (rebuild)");
        if (!web_mod)
            g_warning ("module: web backend unavailable: %s", web_err);
    }
    const WalpeWebModule *r = web_mod;
    g_mutex_unlock (&mod_lock);
    return r;
}

WalpeContent *walpe_web_new (const char *path, const WalpeLoadOpts *opts, GError **err)
{
    mod_why = "a web wallpaper was opened";
    mod_why_path = path;
    const WalpeWebModule *m = walpe_web_module (TRUE);
    mod_why = mod_why_path = NULL;
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "web backend: %s",
                     web_err ? web_err : "not loaded");
        return NULL;
    }
    g_atomic_int_inc (&web_note.uses);
    return m->web_new (path, opts, err);
}

/* the rest only matter once a web wallpaper ran: never load for them */
void walpe_web_sound (double master, gboolean enabled)
{
    if (!(master > 0.0))    /* (also not a number) */
        master = 0.0;
    g_atomic_int_set (&web_sound_q16, (gint) (MIN (master, 1.0) * 65536.0));
    g_atomic_int_set (&web_sound_on, enabled ? 1 : 0);
    /* (waits for a load that is going on: that one hands the module what
     * it found, this call the newer values after it) */
    const WalpeWebModule *m = walpe_web_module (FALSE);
    if (m)
        m->web_sound (g_atomic_int_get (&web_sound_q16) / 65536.0, g_atomic_int_get (&web_sound_on));
}

guint walpe_web_sound_wallpaper (const char *key, int wallpaper_switch, double volume)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    return m && key ? m->web_sound_page (key, wallpaper_switch, volume) : 0;
}

void walpe_web_input (gpointer viewer, const WalpeWebInput *in)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    if (m)
        m->web_input (viewer, in);
}

char *walpe_web_input_status (void)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    return m ? m->web_input_status () : g_strdup ("");
}

char *walpe_web_info (void)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    return m ? m->web_info () : g_strdup ("");
}

char *walpe_web_host (void)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    return m ? m->web_host () : g_strdup ("");
}

void walpe_web_stats (guint *snapshots)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    if (m)
        m->web_stats (snapshots);
    else if (snapshots)
        *snapshots = 0;
}

char *walpe_web_media_missing (void)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    return m ? m->web_media_missing () : g_strdup ("");
}

char *walpe_web_error (void)
{
    const WalpeWebModule *m = walpe_web_module (FALSE);
    return m ? m->web_error () : g_strdup ("");
}

int walpe_web_probe_run (void)
{
    const WalpeWebModule *m = walpe_web_module (TRUE);
    return m ? m->web_probe_run () : 2;
}
#else
const WalpeWebModule *walpe_web_module (gboolean load)
{
    (void) load;
    return NULL;
}

void walpe_web_sound (double master, gboolean enabled)
{
    (void) master; (void) enabled;
}

guint walpe_web_sound_wallpaper (const char *key, int wallpaper_switch, double volume)
{
    (void) key; (void) wallpaper_switch; (void) volume;
    return 0;
}
#endif

#ifdef HAVE_FFMPEG
static const WalpeVideoModule *video_mod;
static gboolean video_tried;
static char *video_err;

const WalpeVideoModule *walpe_video_module (gboolean load)
{
    g_mutex_lock (&mod_lock);
    if (!video_mod && load && !video_tried) {
        video_tried = TRUE;
        const WalpeVideoModule *(*entry) (void) =
            (const WalpeVideoModule *(*) (void)) mod_load ("walpe-video",
                                                           "walpe_video_module_entry",
                                                           &video_err);
        const WalpeVideoModule *m = entry ? entry () : NULL;
        if (m && m->abi == WALPE_MODULE_ABI) {
            video_mod = m;
            mod_note_loaded (&video_note, "a scene's compressed sound file (its decoder)");
        }
        else if (m)
            video_err = g_strdup ("walpe-video.so: ABI mismatch (rebuild)");
        if (!video_mod)
            g_warning ("module: video backend unavailable: %s", video_err);
    }
    const WalpeVideoModule *r = video_mod;
    g_mutex_unlock (&mod_lock);
    return r;
}

WalpeVideo *walpe_video_custom_new (const char *path, GError **err)
{
    mod_why = "a video wallpaper was opened";
    mod_why_path = path;
    const WalpeVideoModule *m = walpe_video_module (TRUE);
    mod_why = mod_why_path = NULL;
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "video backend: %s",
                     video_err ? video_err : "not loaded");
        return NULL;
    }
    g_atomic_int_inc (&video_note.uses);
    return m->custom_new (path, err);
}

WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err)
{
    mod_why = "a scene with a video texture was opened";
    mod_why_path = path;
    const WalpeVideoModule *m = walpe_video_module (TRUE);
    mod_why = mod_why_path = NULL;
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "video backend: %s",
                     video_err ? video_err : "not loaded");
        return NULL;
    }
    g_atomic_int_inc (&video_note.uses);
    return m->tex_new (path, off, len, err);
}

WalpeVideoTex *walpe_video_tex_new_copy (const char *path, guint64 off, guint64 len,
                                         const WalpeVideoTexCopy *copy, GError **err)
{
    /* (the scene loader's entry: without the two notes `walpe-ctl mem` said
     * of every scene video "a scene's compressed sound file (its decoder)",
     * used 0 times) */
    mod_why = "a scene with a video texture was opened";
    mod_why_path = path;
    const WalpeVideoModule *m = walpe_video_module (TRUE);
    mod_why = mod_why_path = NULL;
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "video backend: %s",
                     video_err ? video_err : "not loaded");
        return NULL;
    }
    g_atomic_int_inc (&video_note.uses);
    return m->tex_new_copy (path, off, len, copy, err);
}

/* (status asks; a daemon that never showed a video has no module) */
char *walpe_video_scene_note (WalpeVideoJobText job, gpointer user)
{
    const WalpeVideoModule *m = walpe_video_module (FALSE);
    return m ? m->scene_note (job, user) : NULL;
}

/* a WalpeVideoTex only exists once the module is loaded */
void walpe_video_tex_set_copy (WalpeVideoTex *v, const WalpeVideoTexCopy *copy)
{
    video_mod->tex_set_copy (v, copy);
}

void walpe_video_tex_info (WalpeVideoTex *v, WalpeVideoTexInfo *info)
{
    video_mod->tex_info (v, info);
}

GLuint walpe_video_tex_update (WalpeVideoTex *v, int *w, int *h)
{
    return video_mod->tex_update (v, w, h);
}

gint64 walpe_video_tex_interval_us (WalpeVideoTex *v)
{
    return video_mod->tex_interval_us (v);
}

void walpe_video_tex_set_view (WalpeVideoTex *v, int w, int h)
{
    video_mod->tex_set_view (v, w, h);
}

void walpe_video_tex_set_paused (WalpeVideoTex *v, gboolean paused)
{
    video_mod->tex_set_paused (v, paused);
}

void walpe_video_tex_free (WalpeVideoTex *v)
{
    if (v)
        video_mod->tex_free (v);
}
#else
const WalpeVideoModule *walpe_video_module (gboolean load)
{
    (void) load;
    return NULL;
}
#endif

gboolean walpe_video_mem_stats (WalpeVideoMemStats *out)
{
    memset (out, 0, sizeof (*out));
#ifdef HAVE_FFMPEG
    const WalpeVideoModule *m = walpe_video_module (FALSE);
    if (m && m->mem_stats) {
        m->mem_stats (out);
        return TRUE;
    }
#endif
    return FALSE;
}

#if defined(HAVE_WEB) || defined(HAVE_FFMPEG)
static void mod_note_json (JsonBuilder *b, const char *name, ModNote *n, gint64 start_us)
{
    g_mutex_lock (&mod_lock);
    gint64 at = n->loaded_us;
    char *why = g_strdup (n->why);
    g_mutex_unlock (&mod_lock);
    json_builder_set_member_name (b, name);
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "loaded");
    json_builder_add_boolean_value (b, at != 0);
    if (at) {
        json_builder_set_member_name (b, "loaded_after_s");
        json_builder_add_double_value (b, (double) (at - start_us) / 1e6);
        json_builder_set_member_name (b, "loaded_for");
        json_builder_add_string_value (b, why);
        json_builder_set_member_name (b, "uses");
        json_builder_add_int_value (b, g_atomic_int_get (&n->uses));
    }
    json_builder_end_object (b);
    g_free (why);
}
#endif

void walpe_modules_mem_json (JsonBuilder *b, gint64 start_us)
{
    (void) start_us;
    json_builder_set_member_name (b, "modules");
    json_builder_begin_object (b);
#ifdef HAVE_WEB
    mod_note_json (b, "web", &web_note, start_us);
#endif
#ifdef HAVE_FFMPEG
    mod_note_json (b, "video", &video_note, start_us);
#endif
    json_builder_end_object (b);
    WalpeVideoMemStats v;
    if (walpe_video_mem_stats (&v)) {
        json_builder_set_member_name (b, "video_module");
        json_builder_begin_object (b);
#define VI(k, x) json_builder_set_member_name (b, k); json_builder_add_int_value (b, (gint64) (x))
        VI ("clips", v.sources);
        VI ("viewers", v.viewers);
        VI ("decoders_open", v.decoders);
        VI ("parked", v.parked);
        VI ("paused", v.paused);
        VI ("hardware_decoders", v.hardware);
        VI ("decode_threads", v.threads);
        VI ("frames_queued", v.queued);
        VI ("queue_slots", v.queue_slots);
        VI ("frame_bytes", v.frame_bytes);
        VI ("scratch_bytes", v.scratch_bytes);
        VI ("upload_buffers", v.stage_buffers);
        VI ("upload_buffer_bytes", v.stage_bytes);
        VI ("frames_decoded", v.decoded);
        VI ("decoders_opened", v.opened);
        VI ("largest_width", v.width);
        VI ("largest_height", v.height);
        VI ("decoder_pool_estimate_bytes", v.pool_estimate_bytes);
#undef VI
        json_builder_end_object (b);
    }
}

char *walpe_modules_status (void)
{
    return g_strdup_printf ("web=%s video=%s",
#ifdef HAVE_WEB
                            walpe_web_module (FALSE) ? "loaded" : "not loaded",
#else
                            "not built",
#endif
#ifdef HAVE_FFMPEG
                            walpe_video_module (FALSE) ? "loaded" : "not loaded"
#else
                            "not built"
#endif
    );
}
