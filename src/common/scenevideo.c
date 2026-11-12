/* scenevideo.c - the converted copy of a video inside a scene: the setting,
 * the decision, the cache name, the request (see scenevideo.h). */
#include "scenevideo.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

static gint sv_on = 1;      /* atomic: config "video_scene_convert" */
static gint sv_gen = 1;     /* atomic */

void walpe_scene_video_touch (void)
{
    g_atomic_int_inc (&sv_gen);
}

guint walpe_scene_video_gen (void)
{
    return (guint) g_atomic_int_get (&sv_gen);
}

void walpe_scene_video_set_on (gboolean on)
{
    g_atomic_int_set (&sv_on, on ? 1 : 0);
    walpe_scene_video_touch ();
}

gboolean walpe_scene_video_on (void)
{
    static gint env = -2;   /* atomic: -2 = not looked at, -1 = not set */
    int e = g_atomic_int_get (&env);
    if (e == -2) {
        const char *v = g_getenv ("WALPE_VIDEO_SCENE_CONVERT");
        e = v && *v ? (*v != '0') : -1;
        g_atomic_int_set (&env, e);
    }
    return e >= 0 ? e : g_atomic_int_get (&sv_on);
}

gboolean walpe_scene_video_target (int src_w, int src_h, const WalpeVideoBox *box,
                                   int view_w, int view_h, double slack_pct,
                                   int *out_w, int *out_h)
{
    *out_w = *out_h = 0;
    if (src_w < 4 || src_h < 4 || !box || box->w <= 0 || box->h <= 0)
        return FALSE;
    int tw = 0, th = 0;
    walpe_video_box_fit (box, src_w, src_h, &tw, &th);
    /* (a slack that is no number counts as none) */
    const double k = 1.0 + (slack_pct > 0 ? MIN (slack_pct, 25.0) : 0.0) / 100.0;
    if (view_w > 0 && view_h > 0 && (view_w > tw * k || view_h > th * k)) {
        /* drawn larger than the box: the copy covers what is drawn */
        double f = MAX (view_w / (k * src_w), view_h / (k * src_h));
        f = ceil (f * 8.0 - 1e-9) / 8.0;
        if (!(f < 1.0))
            return FALSE;
        tw = (int) floor (src_w * f + 1e-6) & ~1;
        th = (int) floor (src_h * f + 1e-6) & ~1;
    }
    tw = MAX (tw, 2);
    th = MAX (th, 2);
    if (tw > src_w || th > src_h ||
        (double) src_w * src_h < WALPE_SCENE_VIDEO_MIN_GAIN * (double) tw * th)
        return FALSE;
    *out_w = tw;
    *out_h = th;
    return TRUE;
}

#define ID_SAMPLE (64 * 1024)

static gboolean id_feed (GChecksum *cs, int fd, guint64 at, gsize n)
{
    guint8 *buf = g_malloc (ID_SAMPLE);
    gboolean ok = TRUE;
    while (n > 0 && ok) {
        ssize_t got;
        do
            got = pread (fd, buf, MIN (n, (gsize) ID_SAMPLE), (off_t) at);
        while (got < 0 && errno == EINTR);
        if (got <= 0) {
            ok = FALSE;
        } else {
            g_checksum_update (cs, buf, got);
            at += (guint64) got;
            n -= (gsize) got;
        }
    }
    g_free (buf);
    return ok;
}

char *walpe_scene_video_id (const char *path, guint64 off, guint64 len)
{
    if (!path || !g_path_is_absolute (path))
        return NULL;
    int fd = open (path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return NULL;
    struct stat st;
    char *id = NULL;
    if (fstat (fd, &st) == 0 && S_ISREG (st.st_mode) && st.st_size > 0 &&
        off < (guint64) st.st_size) {
        const guint64 size = (guint64) st.st_size;
        if (len == 0 || len > size - off)
            len = size - off;
        GChecksum *cs = g_checksum_new (G_CHECKSUM_SHA256);
        char *head = g_strdup_printf ("walpe scene video 1\n%s\n%" G_GUINT64_FORMAT "\n%"
                                      G_GUINT64_FORMAT "\n%" G_GUINT64_FORMAT "\n",
                                      path, off, len, size);
        g_checksum_update (cs, (const guchar *) head, -1);
        g_free (head);
        const gsize n = (gsize) MIN (len, (guint64) ID_SAMPLE);
        if (id_feed (cs, fd, off, n) && id_feed (cs, fd, off + len - n, n))
            id = g_strndup (g_checksum_get_string (cs), 32);
        g_checksum_free (cs);
    }
    close (fd);
    return id;
}

gboolean walpe_scene_video_fast (void)
{
    static gint env = -1;   /* atomic: -1 = not looked at */
    int e = g_atomic_int_get (&env);
    if (e < 0) {
        const char *v = g_getenv ("WALPE_VIDEO_COPY_FAST");
        e = v && *v && *v != '0';
        g_atomic_int_set (&env, e);
    }
    return e;
}

#define SV_FAST_END "-fast.mp4"

gboolean walpe_scene_video_copy_is_fast (const char *file)
{
    return file && g_str_has_suffix (file, SV_FAST_END);
}

char *walpe_scene_video_cache_file_as (const char *id, int w, int h, gboolean fast)
{
    if (!id || strlen (id) != 32 || w < 2 || h < 2)
        return NULL;
    for (const char *p = id; *p; p++)
        if (!g_ascii_isxdigit (*p))
            return NULL;
    char *name = g_strdup_printf ("%s-scene-%dx%d%s", id, w, h, fast ? SV_FAST_END : ".mp4");
    char *p = g_build_filename (g_get_user_cache_dir (), "walpe", "video", name, NULL);
    g_free (name);
    return p;
}

char *walpe_scene_video_cache_file (const char *id, int w, int h)
{
    return walpe_scene_video_cache_file_as (id, w, h, walpe_scene_video_fast ());
}

/* set once, by the daemon, before its render threads exist */
static WalpeSceneVideoHook sv_hook;

void walpe_scene_video_set_hook (WalpeSceneVideoHook hook)
{
    sv_hook = hook;
}

void walpe_scene_video_want (const WalpeSceneVideoWant *want, gboolean wanted)
{
    if (sv_hook && want && want->dst)
        sv_hook (want, wanted);
}
