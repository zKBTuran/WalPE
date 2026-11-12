/* video.c - video content wrapper. The mpv backend was removed (resource
 * decision: libmpv cost ~150MB extra RSS with no visual benefit); video
 * playback is the in-house MP4 demuxer + libavcodec H.264 software decode
 * (video_custom.c). Playback is silent: there is no audio path. */
#include "walpe-build-config.h" /* HAVE_FFMPEG */
#include "video.h"
#include "video_hwsel.h"   /* the words of the "video_hwdec" setting */
#include "glutil.h"
#include "common/config.h" /* IWYU pragma: keep */
#include "common/import.h" /* walpe_video_cache_file */
#include <glib/gstdio.h>
#include <string.h>
#include <utime.h>

static gint video_hwdec;   /* atomic: config "video_hwdec", 0 = off */
static gint video_hwdec_serial;     /* atomic: counts the times it was set */
static GMutex video_hwdec_lock;
static char video_hwdec_api[24];    /* video_hwdec_lock: the one API asked for, "" = auto */

int walpe_video_hwdec (void)
{
    return g_atomic_int_get (&video_hwdec);
}

void walpe_video_set_hwdec (int on)
{
    g_mutex_lock (&video_hwdec_lock);
    video_hwdec_api[0] = 0;
    g_atomic_int_set (&video_hwdec, on ? 1 : 0);
    g_atomic_int_inc (&video_hwdec_serial);
    g_mutex_unlock (&video_hwdec_lock);
}

gboolean walpe_video_set_hwdec_name (const char *name)
{
    WalpeHwWant w;
    if (!walpe_hwsel_parse (name, &w))
        return FALSE;
    g_mutex_lock (&video_hwdec_lock);
    g_strlcpy (video_hwdec_api, w.api, sizeof (video_hwdec_api));
    g_atomic_int_set (&video_hwdec, w.on ? 1 : 0);
    g_atomic_int_inc (&video_hwdec_serial);
    g_mutex_unlock (&video_hwdec_lock);
    return TRUE;
}

guint walpe_video_hwdec_api (char *buf, gsize len)
{
    g_mutex_lock (&video_hwdec_lock);
    if (buf && len)
        g_strlcpy (buf, video_hwdec_api, len);
    guint serial = (guint) g_atomic_int_get (&video_hwdec_serial);
    g_mutex_unlock (&video_hwdec_lock);
    return serial;
}

char *walpe_video_hwdec_setting (void)
{
    WalpeHwWant w = { FALSE, "" };
    g_mutex_lock (&video_hwdec_lock);
    w.on = g_atomic_int_get (&video_hwdec) != 0;
    g_strlcpy (w.api, video_hwdec_api, sizeof (w.api));
    g_mutex_unlock (&video_hwdec_lock);
    return g_strdup (walpe_hwsel_want_name (&w));
}

static gint video_zerocopy;    /* atomic: config "video_zerocopy" */
int walpe_video_zerocopy (void)
{
    return g_atomic_int_get (&video_zerocopy);
}

void walpe_video_set_zerocopy (int on)
{
    g_atomic_int_set (&video_zerocopy, on ? 1 : 0);
}

static gint video_limit;   /* atomic: config "video_limit" */
int walpe_video_limit (void)
{
    return g_atomic_int_get (&video_limit);
}

void walpe_video_set_limit (int on)
{
    g_atomic_int_set (&video_limit, on ? 1 : 0);
}

/* the frame rate limit of the video wallpaper this render thread is
 * drawing (vc_gl_render), for the video module */
static __thread int video_frame_limit;
int walpe_video_frame_limit (void)
{
    return video_frame_limit;
}

static gint video_upload;  /* atomic: config "video_upload" as WALPE_VUP_* */
int walpe_video_upload_parse (const char *name)
{
    int mode = WALPE_VUP_LEGACY;
    if (!name)
        return -1;
    char **tok = g_strsplit_set (name, "+, ", -1);
    for (int i = 0; tok[i] && mode >= 0; i++) {
        if (!*tok[i] || !g_ascii_strcasecmp (tok[i], "legacy"))
            continue;
        if (!g_ascii_strcasecmp (tok[i], "pbo"))
            mode = (mode & ~WALPE_VUP_BASE) | WALPE_VUP_PBO;
        else if (!g_ascii_strcasecmp (tok[i], "pbodata"))
            mode = (mode & ~WALPE_VUP_BASE) | WALPE_VUP_PBODATA;
        else if (!g_ascii_strcasecmp (tok[i], "persist"))
            mode = (mode & ~WALPE_VUP_BASE) | WALPE_VUP_PERSIST;
        else if (!g_ascii_strcasecmp (tok[i], "direct"))
            mode = (mode & ~WALPE_VUP_BASE) | WALPE_VUP_DIRECT;
        else if (!g_ascii_strcasecmp (tok[i], "pingpong"))
            mode |= WALPE_VUP_PINGPONG;
        else if (!g_ascii_strcasecmp (tok[i], "hold"))
            mode |= WALPE_VUP_HOLD;
        else
            mode = -1;
    }
    g_strfreev (tok);
    return mode;
}

/* the setting; "hold" is not a setting (it stops the picture) */
gboolean walpe_video_set_upload (const char *name)
{
    int mode = walpe_video_upload_parse (name);
    gboolean ok = mode >= 0 && !(mode & WALPE_VUP_HOLD);
    g_atomic_int_set (&video_upload, ok ? mode : WALPE_VUP_LEGACY);
    return ok;
}

int walpe_video_upload_mode (void)
{
    static gint env_mode = -2;      /* -2 = not looked at, -1 = not set */
    int e = g_atomic_int_get (&env_mode);
    if (e == -2) {
        const char *v = g_getenv ("WALPE_VIDEO_UPLOAD");
        e = v && *v ? walpe_video_upload_parse (v) : -1;
        if (v && *v && e < 0)
            g_warning ("WALPE_VIDEO_UPLOAD=%s: not a known upload path (legacy, pbo, pbodata, "
                       "persist, direct, each also with +pingpong; hold)", v);
        g_atomic_int_set (&env_mode, e);
    }
    return e >= 0 ? e : g_atomic_int_get (&video_upload);
}

static GMutex decoder_note_lock;
static char *decoder_note;

void walpe_video_note_decoder (const char *text)
{
    g_mutex_lock (&decoder_note_lock);
    g_free (decoder_note);
    /* (one entry per open video, each with the reasons of the hardware
     * decoders that were tried; cut at a character, not inside one) */
    decoder_note = g_strndup (text ? text : "", 2400);
    const char *bad = NULL;
    if (!g_utf8_validate (decoder_note, -1, &bad) && bad)
        decoder_note[bad - decoder_note] = 0;
    g_mutex_unlock (&decoder_note_lock);
}

static char *upload_note;       /* decoder_note_lock */
void walpe_video_note_upload (const char *text)
{
    g_mutex_lock (&decoder_note_lock);
    g_free (upload_note);
    upload_note = g_strndup (text ? text : "", 300);
    g_mutex_unlock (&decoder_note_lock);
}

char *walpe_video_upload_note (void)
{
    g_mutex_lock (&decoder_note_lock);
    char *c = g_strdup (upload_note);
    g_mutex_unlock (&decoder_note_lock);
    return c;
}

static __thread guint video_frames_noted;

void walpe_video_note_frame (void)
{
    video_frames_noted++;
}

guint walpe_video_frames_noted (void)
{
    return video_frames_noted;
}

char *walpe_video_decoder_note (void)
{
    g_mutex_lock (&decoder_note_lock);
    char *c = g_strdup (decoder_note);
    g_mutex_unlock (&decoder_note_lock);
    return c;
}

typedef struct {
    WalpeContent base;
    WalpeVideo *v;
    WalpeLoadOpts opts;
    char *media; /* actual media file */
    int fps;     /* the monitor's frame rate limit (load, set_fps); 0 = unknown */
} VideoContent;

static void hook_request_redraw (VideoContent *s)
{
    /* the daemon stores the callback on the outer WalpeContent; backends
     * signal through their own struct, so hand it down before gl_init */
    s->v->request_redraw = s->base.request_redraw;
    s->v->request_user = s->base.request_user;
}

static gboolean vc_gl_init (WalpeContent *c, GError **err)
{
    VideoContent *s = (VideoContent *) c;
    walpe_glutil_init ();
    hook_request_redraw (s);
    return s->v->gl_init (s->v, err);
}

static gboolean vc_gl_render (WalpeContent *c, int w, int h, float px, float py)
{
    (void) px; (void) py;
    VideoContent *s = (VideoContent *) c;
    g_autoptr (GError) err = NULL;
    s->v->next_frame_us = 0;
    if (s->v->fit.mode != WALPE_FIT_COVER && s->v->fit.mode != WALPE_FIT_STRETCH) {
        /* the frame may not cover the monitor: black bars */
        glClearColor (0, 0, 0, 1);
        glClear (GL_COLOR_BUFFER_BIT);
    }
    video_frame_limit = s->fps;     /* walpe_video_frame_limit (), "video_limit" */
    s->v->render_w = c->render_w;   /* the size chosen for this monitor's picture */
    s->v->render_h = c->render_h;
    gboolean ok = s->v->gl_render (s->v, w, h, &err);
    video_frame_limit = 0;
    c->next_frame_us = s->v->next_frame_us; /* presentation beat */
    c->first_frame_pending = s->v->no_frame;
    if (!ok) {
        g_warning ("video render: %s", err ? err->message : "?");
        return FALSE;
    }
    return TRUE;
}

static void vc_gl_free (WalpeContent *c)
{
    VideoContent *s = (VideoContent *) c;
    if (s->v && s->v->gl_free)
        s->v->gl_free (s->v);
}

static void vc_set_paused (WalpeContent *c, gboolean p)
{
    VideoContent *s = (VideoContent *) c;
    if (s->v->set_paused)
        s->v->set_paused (s->v, p);
}

/* render thread: the monitor's frame rate limit changed. A video keeps
 * its own frame rate unless "video_limit" is on (video.h); the module asks
 * for the limit while it renders. */
static void vc_set_fps (WalpeContent *c, int fps)
{
    ((VideoContent *) c)->fps = fps;
}

static void vc_set_volume (WalpeContent *c, double v)
{
    VideoContent *s = (VideoContent *) c;
    if (s->v->set_volume)
        s->v->set_volume (s->v, v);
}

static void vc_free (WalpeContent *c)
{
    VideoContent *s = (VideoContent *) c;
    if (s->v) {
        /* release GL resources with the context current, then the backend */
        if (s->v->gl_free)
            s->v->gl_free (s->v);
        if (s->v->free)
            s->v->free (s->v);
    }
    g_free (s->media);
    g_free (c->path);
    g_free (c->title);
    g_free (c);
}

WalpeContent *walpe_video_content_new (const char *path, const WalpeLoadOpts *opts, GError **err)
{
    VideoContent *s = g_new0 (VideoContent, 1);
    s->base.type = WALPE_TYPE_VIDEO;
    s->base.path = g_strdup (path);
    s->base.title = g_path_get_basename (path);
    s->base.gl_init = vc_gl_init;
    s->base.gl_render = vc_gl_render;
    s->base.gl_free = vc_gl_free;
    s->base.set_paused = vc_set_paused;
    s->base.set_volume = vc_set_volume;
    s->base.set_fps = vc_set_fps;
    s->base.free = vc_free;
    s->opts = *opts;
    s->fps = opts->fps;
    s->media = g_strdup (path);

#ifdef HAVE_FFMPEG
    {
        /* not H.264 in MP4 / MOV (WebM, HEVC, AV1, MKV, ...), or stored
         * rotated / with non-square pixels: the daemon converts such a
         * wallpaper once into its cache (keyed by path + size + mtime, so
         * a changed file has no copy). When that copy exists it is the one
         * to play. The wallpaper keeps its own path everywhere else
         * (config, options, status). */
        /* (the copy made for the box this wallpaper is shown with: a copy
         * for another limit has another name) */
        const WalpeVideoBox box = { opts ? opts->video_box_w : 0, opts ? opts->video_box_h : 0 };
        char *cf = walpe_video_cache_file (s->media, &box);
        if (cf && g_file_test (cf, G_FILE_TEST_IS_REGULAR)) {
            s->v = walpe_video_custom_new (cf, NULL);
            if (s->v) {
                g_message ("video: %s is played from its converted copy %s", s->media, cf);
                g_utime (cf, NULL);     /* in use: the daemon removes copies unused for months */
            }
        }
        g_free (cf);
    }
    if (!s->v)
        s->v = walpe_video_custom_new (s->media, err);
    if (!s->v) {
        vc_free (&s->base);
        return NULL;
    }
    walpe_content_fit (opts ? opts->origin_dir : path, &s->v->fit);
    return &s->base;
#else
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "video playback requires the FFmpeg backend (not compiled in)");
    g_free (s->media);
    g_free (s->base.path);
    g_free (s->base.title);
    g_free (s);
    return NULL;
#endif
}

#ifndef HAVE_FFMPEG
/* no libavcodec: video textures stay unsupported (scenes skip the layer) */
#include <gio/gio.h>
#include "videotex.h"
WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err)
{
    (void) off; (void) len;
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "%s: video textures need WalPE built with libavcodec", path);
    return NULL;
}
GLuint walpe_video_tex_update (WalpeVideoTex *v, int *w, int *h) { (void) v; (void) w; (void) h; return 0; }
gint64 walpe_video_tex_interval_us (WalpeVideoTex *v) { (void) v; return 0; }
void walpe_video_tex_set_view (WalpeVideoTex *v, int w, int h) { (void) v; (void) w; (void) h; }
void walpe_video_tex_set_paused (WalpeVideoTex *v, gboolean p) { (void) v; (void) p; }
void walpe_video_tex_free (WalpeVideoTex *v) { (void) v; }
WalpeVideoTex *walpe_video_tex_new_copy (const char *path, guint64 off, guint64 len,
                                         const WalpeVideoTexCopy *copy, GError **err)
{
    (void) copy;
    return walpe_video_tex_new (path, off, len, err);
}
void walpe_video_tex_set_copy (WalpeVideoTex *v, const WalpeVideoTexCopy *copy) { (void) v; (void) copy; }
void walpe_video_tex_info (WalpeVideoTex *v, WalpeVideoTexInfo *info)
{
    (void) v;
    memset (info, 0, sizeof *info);
}
char *walpe_video_scene_note (WalpeVideoJobText job, gpointer user) { (void) job; (void) user; return NULL; }
#endif
