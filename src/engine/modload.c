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
/* dlopen <name>.so from $WALPE_MODULE_DIR, the executable's directory, then
 * the install dir; returns the entry symbol or NULL (*why = reason, g_free) */
static gpointer mod_load (const char *name, const char *entry, char **why)
{
    char *dirs[3] = { NULL, NULL, NULL };
    const char *env = g_getenv ("WALPE_MODULE_DIR");
    GString *err = g_string_new (NULL);
    /* an absolute directory: a relative one meant "whatever directory the
     * daemon happens to be started in", and the module is code */
    if (env && *env && g_path_is_absolute (env))
        dirs[0] = g_strdup (env);
    else if (env && *env)
        g_warning ("module: WALPE_MODULE_DIR=%s is not an absolute path: ignored", env);
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    if (exe) {
        dirs[1] = g_path_get_dirname (exe);
        g_free (exe);
    }
    dirs[2] = g_strdup (WALPE_MODULEDIR);
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

#ifdef HAVE_WEB
static const WalpeWebModule *web_mod;
static gboolean web_tried;
static char *web_err;

const WalpeWebModule *walpe_web_module (gboolean load)
{
    g_mutex_lock (&mod_lock);
    if (!web_mod && load && !web_tried) {
        web_tried = TRUE;
        const WalpeWebModule *(*entry) (void) =
            (const WalpeWebModule *(*) (void)) mod_load ("walpe-web", "walpe_web_module_entry",
                                                         &web_err);
        const WalpeWebModule *m = entry ? entry () : NULL;
        if (m && m->abi == WALPE_MODULE_ABI)
            web_mod = m;
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
    const WalpeWebModule *m = walpe_web_module (TRUE);
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "web backend: %s",
                     web_err ? web_err : "not loaded");
        return NULL;
    }
    return m->web_new (path, opts, err);
}

/* the rest only matter once a web wallpaper ran: never load for them */
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
        if (m && m->abi == WALPE_MODULE_ABI)
            video_mod = m;
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
    const WalpeVideoModule *m = walpe_video_module (TRUE);
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "video backend: %s",
                     video_err ? video_err : "not loaded");
        return NULL;
    }
    return m->custom_new (path, err);
}

WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err)
{
    const WalpeVideoModule *m = walpe_video_module (TRUE);
    if (!m) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "video backend: %s",
                     video_err ? video_err : "not loaded");
        return NULL;
    }
    return m->tex_new (path, off, len, err);
}

/* a WalpeVideoTex only exists once the module is loaded */
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
