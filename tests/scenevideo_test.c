/* scenevideo_test - the converted copy of a video inside a scene
 * (src/common/scenevideo.c, and walpe_import_start_scene of import.c).
 *
 * Without ffmpeg:
 *  - the decision: which clips get a copy and how large it is - fitted into
 *    the box of "video_resolution", never enlarged, not for a saving below
 *    one and a half, larger when the scene draws the layer larger than the
 *    box, none without a box;
 *  - the copy's name: the same for the same bytes at the same place (also
 *    after the file was written again), another one for another clip,
 *    another range, another size; always inside the cache folder;
 *  - the argument vector of the conversion: the byte range through the
 *    subfile protocol with the file name behind "file:", only that protocol
 *    and only MP4 allowed, the exact size, the colour matrix; a file name
 *    made of options stays one argument.
 * With ffmpeg and ffprobe in PATH (else skipped), on a clip made here and
 * put into the middle of another file with a hostile name:
 *  - the copy has the size asked for, as many pictures as the clip, 8-bit
 *    4:2:0 H.264, the matrix tag, and no ".part" is left;
 *  - the child runs at the lowest priority (nice 19; SCHED_IDLE where the
 *    system has it);
 *  - a clip that is not 8-bit 4:2:0 in limited range is left as it is, with
 *    a reason and no output;
 *  - a copy larger than the clip is refused;
 *  - a run that is stopped leaves no file and no child. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include "common/scenevideo.h"
#include "common/import.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *tmp;
static GMainLoop *loop;

static char *P (const char *rel)
{
    return g_build_filename (tmp, rel, NULL);
}

/* ------------------------------------------------------------ decision */

static gboolean target (int sw, int sh, int bw, int bh, int vw, int vh, int *w, int *h)
{
    const WalpeVideoBox box = { bw, bh };
    return walpe_scene_video_target (sw, sh, &box, vw, vh, 2.0, w, h);
}

static void test_decision (void)
{
    int w = -1, h = -1;
    /* a 4K clip on a 1080p monitor: the case it is for */
    CHECK (target (3840, 2160, 1920, 1080, 0, 0, &w, &h) && w == 1920 && h == 1080);
    CHECK (target (3840, 2160, 1920, 1080, 1920, 1080, &w, &h) && w == 1920 && h == 1080);
    /* drawn a little larger than the box, within the texture slack */
    CHECK (target (3840, 2160, 1920, 1080, 1950, 1097, &w, &h) && w == 1920 && h == 1080);
    /* 1440p and 720p monitors */
    CHECK (target (3840, 2160, 2560, 1440, 0, 0, &w, &h) && w == 2560 && h == 1440);
    CHECK (target (3840, 2160, 1280, 720, 0, 0, &w, &h) && w == 1280 && h == 720);
    CHECK (target (2560, 1440, 1920, 1080, 0, 0, &w, &h) && w == 1920 && h == 1080);
    /* nothing is enlarged, and a clip that fits has no copy */
    CHECK (!target (1920, 1080, 1920, 1080, 0, 0, &w, &h) && w == 0 && h == 0);
    CHECK (!target (1280, 720, 1920, 1080, 0, 0, &w, &h));
    CHECK (!target (1920, 1080, 3840, 2160, 0, 0, &w, &h));
    /* a saving below one and a half: not worth a second file (1080p on a
     * 1600 x 900 monitor is 1.44 times the pixels) */
    CHECK (!target (1920, 1080, 1600, 900, 0, 0, &w, &h));
    CHECK (target (1920, 1080, 1366, 768, 0, 0, &w, &h) && w == 1364 && h == 768);
    /* the clip's own shape is kept: a 4:3 clip in a 16:9 box, an upright
     * clip, a wide one */
    CHECK (target (2880, 2160, 1920, 1080, 0, 0, &w, &h) && w == 1440 && h == 1080);
    CHECK (target (2160, 3840, 1920, 1080, 0, 0, &w, &h) && w == 606 && h == 1080);
    CHECK (target (5120, 1440, 1920, 1080, 0, 0, &w, &h) && w == 1920 && h == 540);
    /* even sides for 4:2:0 */
    CHECK (target (3838, 2158, 1921, 1081, 0, 0, &w, &h) && !(w & 1) && !(h & 1) && w <= 1921 && h <= 1081);
    /* no box (video_resolution none, or no monitor): no copy */
    CHECK (!target (3840, 2160, 0, 0, 0, 0, &w, &h));
    CHECK (!walpe_scene_video_target (3840, 2160, NULL, 0, 0, 2.0, &w, &h));
    /* a size that is none */
    CHECK (!target (0, 0, 1920, 1080, 0, 0, &w, &h));
    CHECK (!target (-5, 2160, 1920, 1080, 0, 0, &w, &h));

    /* The layer is drawn larger than the box - zoomed, or a scene that
     * covers a monitor of another shape: the copy covers what is drawn, in
     * eighths of the clip, and is never shown enlarged beyond the slack. */
    CHECK (target (3840, 2160, 1920, 1080, 2304, 1296, &w, &h) && w == 2400 && h == 1350);
    CHECK (w * 1.02 >= 2304 && h * 1.02 >= 1296);
    /* a 16:9 scene covering an upright 1080 x 1920 monitor is drawn
     * 3414 x 1920: seven eighths of a 4K clip, too little to save */
    CHECK (!target (3840, 2160, 1080, 1920, 3414, 1920, &w, &h));
    /* ... and 3440 x 1440 (drawn 3440 x 1935): the same */
    CHECK (!target (3840, 2160, 3440, 1440, 3440, 1935, &w, &h));
    /* drawn at the clip's size or beyond: the clip itself */
    CHECK (!target (3840, 2160, 1920, 1080, 3840, 2160, &w, &h));
    CHECK (!target (3840, 2160, 1920, 1080, 9000, 5000, &w, &h));
    /* an 8K clip drawn at 2.1 K on a 1080p monitor: three eighths */
    CHECK (target (7680, 4320, 1920, 1080, 2100, 1181, &w, &h) && w == 2880 && h == 1620);
    /* drawn smaller than the box: the box decides (video_resolution is
     * the size videos are converted at) */
    CHECK (target (3840, 2160, 1920, 1080, 400, 225, &w, &h) && w == 1920 && h == 1080);
    /* without slack the copy covers every pixel that is drawn */
    {
        const WalpeVideoBox box = { 1920, 1080 };
        CHECK (walpe_scene_video_target (3840, 2160, &box, 1950, 1097, 0.0, &w, &h) && w == 2400 && h == 1350);
        CHECK (walpe_scene_video_target (3840, 2160, &box, 1920, 1080, 0.0, &w, &h) && w == 1920 && h == 1080);
        /* a slack that is no number is none */
        CHECK (walpe_scene_video_target (3840, 2160, &box, 1950, 1097, (double) NAN, &w, &h) && w == 2400);
    }
    /* growing never turns back: what a larger view asks for covers every smaller one */
    for (int v = 1000; v <= 3900; v += 37) {
        int w2 = 0, h2 = 0;
        gboolean a = target (3840, 2160, 1920, 1080, v, v * 9 / 16, &w, &h);
        gboolean b = target (3840, 2160, 1920, 1080, v + 37, (v + 37) * 9 / 16, &w2, &h2);
        CHECK (!a || !b || (w2 >= w && h2 >= h));
        CHECK (a || !b);    /* no copy for a smaller view: none for a larger one */
        if (a)      /* never shown enlarged beyond the slack, and worth it */
            CHECK (w * 1.02 + 1 >= v && (double) 3840 * 2160 >= 1.5 * w * h);
    }

    /* the setting: on by default, a switch, and a change is seen */
    g_unsetenv ("WALPE_VIDEO_SCENE_CONVERT");
    CHECK (walpe_scene_video_on ());
    guint g0 = walpe_scene_video_gen ();
    walpe_scene_video_set_on (FALSE);
    CHECK (!walpe_scene_video_on () && walpe_scene_video_gen () != g0);
    g0 = walpe_scene_video_gen ();
    walpe_scene_video_set_on (TRUE);
    CHECK (walpe_scene_video_on () && walpe_scene_video_gen () != g0);
    g0 = walpe_scene_video_gen ();
    walpe_scene_video_touch ();
    CHECK (walpe_scene_video_gen () != g0);
}

/* ------------------------------------------------------------ the name */

static void put (const char *path, const guint8 *d, gsize n)
{
    CHECK (g_file_set_contents (path, (const char *) d, (gssize) n, NULL));
}

static int hook_calls, hook_wanted;
static char *hook_dst;
static void hook (const WalpeSceneVideoWant *w, gboolean wanted)
{
    hook_calls++;
    hook_wanted += wanted ? 1 : -1;
    g_free (hook_dst);
    hook_dst = g_strdup (w->dst);
}

static void test_name (void)
{
    const gsize n = 300 * 1024;
    guint8 *d = g_malloc (n);
    for (gsize i = 0; i < n; i++)
        d[i] = (guint8) (i * 2654435761u >> 13);
    char *a = P ("a.tex"), *b = P ("b.tex");
    put (a, d, n);
    char *id = walpe_scene_video_id (a, 87, n - 87);
    CHECK (id && strlen (id) == 32);
    /* the same again, and after the file was written again (a package
     * that was extracted anew: other times, same bytes) */
    char *id2 = walpe_scene_video_id (a, 87, n - 87);
    CHECK (!g_strcmp0 (id, id2));
    g_free (id2);
    g_usleep (20000);
    g_unlink (a);
    put (a, d, n);
    id2 = walpe_scene_video_id (a, 87, n - 87);
    CHECK (!g_strcmp0 (id, id2));
    g_free (id2);
    /* another range, another file with the same bytes */
    id2 = walpe_scene_video_id (a, 88, n - 88);
    CHECK (id2 && g_strcmp0 (id, id2));
    g_free (id2);
    id2 = walpe_scene_video_id (a, 87, n - 100);
    CHECK (id2 && g_strcmp0 (id, id2));
    g_free (id2);
    put (b, d, n);
    id2 = walpe_scene_video_id (b, 87, n - 87);
    CHECK (id2 && g_strcmp0 (id, id2));
    g_free (id2);
    /* length 0 and a length past the end mean "to the end of the file" */
    char *e1 = walpe_scene_video_id (a, 87, 0), *e2 = walpe_scene_video_id (a, 87, n * 4);
    CHECK (e1 && !g_strcmp0 (e1, e2) && !g_strcmp0 (e1, id));
    g_free (e1);
    g_free (e2);
    /* a clip that was replaced: one byte at its start, one at its end, and
     * another length with the same start */
    d[87 + 5] ^= 1;
    put (a, d, n);
    id2 = walpe_scene_video_id (a, 87, n - 87);
    CHECK (id2 && g_strcmp0 (id, id2));
    g_free (id2);
    d[87 + 5] ^= 1;
    d[n - 3] ^= 1;
    put (a, d, n);
    id2 = walpe_scene_video_id (a, 87, n - 87);
    CHECK (id2 && g_strcmp0 (id, id2));
    g_free (id2);
    d[n - 3] ^= 1;
    put (a, d, n - 1000);
    id2 = walpe_scene_video_id (a, 87, 0);
    CHECK (id2 && g_strcmp0 (id, id2));
    g_free (id2);
    /* a small clip (shorter than the two samples) */
    put (b, d, 500);
    id2 = walpe_scene_video_id (b, 100, 400);
    CHECK (id2 && strlen (id2) == 32);
    g_free (id2);
    /* what cannot be read: no name */
    char *missing = P ("missing.tex");
    CHECK (!walpe_scene_video_id (missing, 0, 10));
    g_free (missing);
    CHECK (!walpe_scene_video_id ("relative.tex", 0, 10));
    CHECK (!walpe_scene_video_id (NULL, 0, 10));
    CHECK (!walpe_scene_video_id (tmp, 0, 10));             /* a folder */
    CHECK (!walpe_scene_video_id (b, 500, 10));             /* a range behind the end */

    /* the copy's file: in the cache folder, named by the clip and the size */
    char *f = walpe_scene_video_cache_file (id, 1920, 1080);
    char *dir = g_build_filename (g_get_user_cache_dir (), "walpe", "video", NULL);
    char *want = g_strdup_printf ("%s/%s-scene-1920x1080.mp4", dir, id);
    CHECK (!g_strcmp0 (f, want));
    char *f2 = walpe_scene_video_cache_file (id, 2560, 1440);
    CHECK (f2 && g_strcmp0 (f, f2) && g_str_has_prefix (f2, dir) && g_str_has_suffix (f2, ".mp4"));
    /* a name that is not one of ours never becomes a path */
    CHECK (!walpe_scene_video_cache_file (NULL, 1920, 1080));
    CHECK (!walpe_scene_video_cache_file ("../../../../etc/passwd0000000000000", 1920, 1080));
    CHECK (!walpe_scene_video_cache_file ("0123456789abcdef0123456789abcde/", 1920, 1080));
    CHECK (!walpe_scene_video_cache_file ("0123", 1920, 1080));
    CHECK (!walpe_scene_video_cache_file (id, 0, 1080));
    CHECK (!walpe_scene_video_cache_file (id, 1920, -2));

    /* the request: nothing without a hook, the hook otherwise */
    const WalpeSceneVideoWant w = { .src = a, .off = 87, .len = n - 87, .dst = f, .w = 1920, .h = 1080, .matrix = 1 };
    walpe_scene_video_want (&w, TRUE);
    CHECK (hook_calls == 0);
    walpe_scene_video_set_hook (hook);
    walpe_scene_video_want (&w, TRUE);
    CHECK (hook_calls == 1 && hook_wanted == 1 && !g_strcmp0 (hook_dst, f));
    walpe_scene_video_want (&w, FALSE);
    CHECK (hook_calls == 2 && hook_wanted == 0);
    walpe_scene_video_want (NULL, TRUE);
    CHECK (hook_calls == 2);
    walpe_scene_video_set_hook (NULL);
    g_clear_pointer (&hook_dst, g_free);

    g_unlink (a);
    g_unlink (b);
    g_free (f); g_free (f2); g_free (want); g_free (dir); g_free (id);
    g_free (a); g_free (b); g_free (d);
}

/* ------------------------------------------------------------ the argv */

static int arg_at (char **argv, const char *a)
{
    for (int i = 0; argv[i]; i++)
        if (!strcmp (argv[i], a))
            return i;
    return -1;
}

static gboolean have_arg (char **argv, const char *a, const char *next)
{
    int i = arg_at (argv, a);
    return i >= 0 && (!next || (argv[i + 1] && !strcmp (argv[i + 1], next)));
}

static void test_argv (void)
{
    const char *hostile = "/w/-i evil.mp4 -f lavfi,,start,0,end,9,,:concat:a|b\n-y x.tex";
    const WalpeImportScene sc = { .off = 87, .len = 15071044, .w = 1920, .h = 1080, .matrix = 1 };
    char **v = walpe_import_scene_argv (hostile, &sc, "/c/out.mp4.part", "libx264");
    CHECK (v && !strcmp (v[0], "ffmpeg"));
    int in = arg_at (v, "-i");
    char *url = g_strconcat ("subfile,,start,87,end,15071131,,:file:", hostile, NULL);
    /* the file name is one argument, behind the protocol's options and "file:" */
    CHECK (in > 0 && v[in + 1] && !strcmp (v[in + 1], url));
    g_free (url);
    int n_i = 0;
    for (int i = 0; v[i]; i++)
        n_i += !strcmp (v[i], "-i");
    CHECK (n_i == 1);
    /* only the byte range of a file, read as MP4 with H.264, as stored */
    CHECK (have_arg (v, "-protocol_whitelist", "file,subfile"));
    CHECK (have_arg (v, "-format_whitelist", "mov"));
    CHECK (have_arg (v, "-max_pixels", NULL));
    CHECK (arg_at (v, "-f") > 0 && arg_at (v, "-f") < in && !strcmp (v[arg_at (v, "-f") + 1], "mov"));
    CHECK (arg_at (v, "-c:v") > 0 && arg_at (v, "-c:v") < in && !strcmp (v[arg_at (v, "-c:v") + 1], "h264"));
    CHECK (arg_at (v, "-noautorotate") > 0 && arg_at (v, "-noautorotate") < in);
    /* exactly the size asked for, the matrix said, the import's format */
    CHECK (have_arg (v, "-vf", "scale=1920:1080:flags=bicubic,setsar=1,format=yuv420p,"
                               "setparams=colorspace=bt709:range=tv"));
    CHECK (have_arg (v, "-colorspace", "bt709") && have_arg (v, "-color_range", "tv"));
    CHECK (have_arg (v, "-preset", "medium") && have_arg (v, "-crf", "19"));
    CHECK (have_arg (v, "-profile:v", "high") && have_arg (v, "-pix_fmt", "yuv420p"));
    CHECK (have_arg (v, "-an", NULL) && have_arg (v, "-map", "0:v:0") && have_arg (v, "-movflags", "+faststart"));
    CHECK (!have_arg (v, "-c:v", "copy"));
    int last = 0;
    while (v[last + 1])
        last++;
    CHECK (!strcmp (v[last], "file:/c/out.mp4.part") && !strcmp (v[last - 1], "mp4"));
    g_strfreev (v);
    /* the other matrices; a number that is none: no tag */
    WalpeImportScene s2 = sc;
    s2.matrix = 6;
    v = walpe_import_scene_argv ("/a.tex", &s2, "/c/o.part", "libx264");
    CHECK (have_arg (v, "-colorspace", "smpte170m"));
    g_strfreev (v);
    s2.matrix = 9;
    v = walpe_import_scene_argv ("/a.tex", &s2, "/c/o.part", "libx264");
    CHECK (have_arg (v, "-colorspace", "bt2020nc"));
    g_strfreev (v);
    s2.matrix = 0;
    v = walpe_import_scene_argv ("/a.tex", &s2, "/c/o.part", "libx264");
    CHECK (!have_arg (v, "-colorspace", NULL) &&
           have_arg (v, "-vf", "scale=1920:1080:flags=bicubic,setsar=1,format=yuv420p"));
    g_strfreev (v);
    /* the import's own vector is what it was */
    WalpeMediaInfo mi = { .has_video = TRUE, .codec = (char *) "vp9", .pix_fmt = (char *) "yuv420p",
                          .width = 3840, .height = 2160, .sar_num = 1, .sar_den = 1 };
    const WalpeVideoBox hd = { 1920, 1080 };
    v = walpe_import_build_argv ("/a b.webm", "/c/o.part", &mi, "libx264", FALSE, &hd);
    CHECK (have_arg (v, "-i", "file:/a b.webm") && have_arg (v, "-protocol_whitelist", "file"));
    CHECK (have_arg (v, "-vf", "scale=1920:1080:flags=bicubic,setsar=1,format=yuv420p"));
    CHECK (arg_at (v, "-noautorotate") < 0 && arg_at (v, "-colorspace") < 0 && arg_at (v, "-f") > arg_at (v, "-i"));
    g_strfreev (v);

    /* what is no clip inside a file, or no size a copy can have, is not started */
    GError *err = NULL;
    char *dst = P ("x.mp4");
    const WalpeImportScene bad[] = {
        { .off = 0, .len = 10, .w = 640, .h = 360 },                    /* too short to be an MP4 */
        { .off = 0, .len = 5000, .w = 641, .h = 360 },                  /* odd */
        { .off = 0, .len = 5000, .w = 640, .h = 0 },
        { .off = 0, .len = 5000, .w = 100000, .h = 360 },
        { .off = G_MAXUINT64 - 10, .len = 5000, .w = 640, .h = 360 },   /* the range wraps */
    };
    for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
        CHECK (!walpe_import_start_scene ("/nonexistent.tex", &bad[i], dst, NULL, NULL, &err) && err);
        g_clear_error (&err);
    }
    CHECK (!walpe_import_start_scene ("/nonexistent.tex", NULL, dst, NULL, NULL, &err) && err);
    g_clear_error (&err);
    CHECK (!walpe_import_start_scene ("/nonexistent.tex", &sc, "relative.mp4", NULL, NULL, &err) && err);
    g_clear_error (&err);
    CHECK (!walpe_import_start_scene ("/nonexistent.tex", &sc, dst, NULL, NULL, &err) && err);  /* no such file */
    g_clear_error (&err);
    g_free (dst);
}

/* ------------------------------------------------------------ real runs */

typedef struct {
    gboolean called, ok;
    char *dst, *error;
} Result;

static void done_cb (WalpeImport *imp, gboolean ok, const char *dst, const char *error, gpointer user)
{
    (void) imp;
    Result *r = user;
    r->called = TRUE;
    r->ok = ok;
    r->dst = g_strdup (dst);
    r->error = g_strdup (error);
    g_main_loop_quit (loop);
}

static gboolean stop_loop (gpointer user)
{
    (void) user;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static gboolean run_tool (const char **argv)
{
    int status = 1;
    return g_spawn_sync (NULL, (char **) argv, NULL,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, NULL, &status, NULL) && status == 0;
}

/* "codec,width,height,pix_fmt,color_range,color_space,nb_frames"-like line, g_free */
static char *probe (const char *file)
{
    const char *argv[] = { "ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
                           "-show_entries",
                           "stream=codec_name,width,height,pix_fmt,color_range,color_space,nb_read_frames",
                           "-of", "csv=p=0", file, NULL };
    char *out = NULL;
    if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
                       NULL, &out, NULL, NULL, NULL))
        return g_strdup ("");
    return g_strstrip (out);
}

/* clip (a file) put into the middle of another file: [off, off + len) */
static gboolean embed (const char *clip, const char *out, guint64 *off, guint64 *len)
{
    char *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents (clip, &d, &n, NULL))
        return FALSE;
    GByteArray *a = g_byte_array_new ();
    for (int i = 0; i < 87; i++)
        g_byte_array_append (a, (const guint8 *) "T", 1);
    g_byte_array_append (a, (const guint8 *) d, (guint) n);
    for (int i = 0; i < 4000; i++)
        g_byte_array_append (a, (const guint8 *) "junk after the clip ", 20);
    gboolean ok = g_file_set_contents (out, (const char *) a->data, a->len, NULL);
    *off = 87;
    *len = n;
    g_byte_array_unref (a);
    g_free (d);
    return ok;
}

/* children of this process named ffmpeg: how many; *nice / *policy of the
 * first (from /proc/<pid>/stat) */
static int ffmpeg_children (int *nice_out, int *policy_out)
{
    int n = 0;
    GDir *d = g_dir_open ("/proc", 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name (d))) {
        if (!g_ascii_isdigit (*name))
            continue;
        char *p = g_strdup_printf ("/proc/%s/stat", name), *s = NULL;
        if (g_file_get_contents (p, &s, NULL, NULL)) {
            /* pid (comm) state ppid ... */
            char *rp = strrchr (s, ')');
            char **f = rp ? g_strsplit (rp + 2, " ", 0) : NULL;
            if (f && g_strv_length (f) > 38 && strstr (s, "(ffmpeg)") &&
                atoi (f[1]) == (int) getpid () && f[0][0] != 'Z') {
                if (!n && nice_out)
                    *nice_out = atoi (f[16]);
                if (!n && policy_out)
                    *policy_out = atoi (f[38]);
                n++;
            }
            g_strfreev (f);
        }
        g_free (s);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    return n;
}

static int count_names (const char *dir, const char *suffix)
{
    int n = 0;
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name (d)))
        n += g_str_has_suffix (name, suffix);
    if (d)
        g_dir_close (d);
    return n;
}

static Result run_scene (const char *src, const WalpeImportScene *sc, const char *dst, int secs)
{
    Result r = { 0 };
    GError *err = NULL;
    WalpeImport *imp = walpe_import_start_scene (src, sc, dst, done_cb, &r, &err);
    if (!imp) {
        r.error = g_strdup (err ? err->message : "?");
        g_clear_error (&err);
        return r;
    }
    guint t = g_timeout_add_seconds ((guint) secs, stop_loop, NULL);
    g_main_loop_run (loop);
    if (r.called)
        g_source_remove (t);
    walpe_import_free (imp);
    return r;
}

static void test_runs (void)
{
    char *ff = g_find_program_in_path ("ffmpeg"), *fp = g_find_program_in_path ("ffprobe");
    if (!ff || !fp) {
        g_print ("scenevideo-test: ffmpeg / ffprobe not in PATH, real runs skipped\n");
        g_free (ff);
        g_free (fp);
        return;
    }
    g_free (ff);
    g_free (fp);
    char *in = P ("in"), *cache = P ("cache dir");
    g_mkdir_with_parents (in, 0700);
    char *clip = P ("in/clip.mp4"), *full = P ("in/full.mp4"), *c444 = P ("in/c444.mp4"), *lng = P ("in/long.mp4");
    /* a file name every part of which could be taken for something else */
    char *tex = P ("in/-i x.mp4 ,,start,0,,:concat:%d 'a' \"b\".tex");
    char *tex_full = P ("in/full.tex"), *tex_444 = P ("in/444.tex"), *tex_long = P ("in/long.tex");
    const char *mk[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                         "-t", "1", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", clip, NULL };
    const char *mk_full[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                              "-t", "0.5", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuvj420p",
                              "-color_range", "pc", full, NULL };
    const char *mk_444[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                             "-t", "0.5", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv444p",
                             c444, NULL };
    const char *mk_long[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                              "-t", "30", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                              lng, NULL };
    /* the conversions whose results are looked at run at this process's
     * priority: at the lowest one they would not end in time on a machine
     * that is busy with something else (the priority is checked below) */
    g_setenv ("WALPE_CONVERT_BACKGROUND", "0", TRUE);
    guint64 off = 0, len = 0, off2 = 0, len2 = 0, off3 = 0, len3 = 0, off4 = 0, len4 = 0;
    if (!run_tool (mk) || !run_tool (mk_full) || !run_tool (mk_444) || !run_tool (mk_long) ||
        !embed (clip, tex, &off, &len) || !embed (full, tex_full, &off2, &len2) ||
        !embed (c444, tex_444, &off3, &len3) || !embed (lng, tex_long, &off4, &len4)) {
        g_print ("scenevideo-test: this ffmpeg cannot make the test clips (libx264), real runs skipped\n");
        goto out;
    }

    /* the copy: the size asked for, every picture, the tag, nothing else left */
    {
        char *dst = g_build_filename (cache, "0123456789abcdef0123456789abcdef-scene-640x360.mp4", NULL);
        const WalpeImportScene sc = { .off = off, .len = len, .w = 640, .h = 360, .matrix = 1 };
        Result r = run_scene (tex, &sc, dst, 240);
        CHECK (r.called && r.ok && !g_strcmp0 (r.dst, dst) && g_file_test (dst, G_FILE_TEST_IS_REGULAR));
        if (!r.ok)
            g_printerr ("  error: %s\n", r.error ? r.error : "(none)");
        char *got = probe (dst);
        CHECK (!g_strcmp0 (got, "h264,640,360,yuv420p,tv,bt709,30"));
        if (g_strcmp0 (got, "h264,640,360,yuv420p,tv,bt709,30"))
            g_printerr ("  the copy is: %s\n", got);
        g_free (got);
        CHECK (count_names (cache, ".part") == 0 && count_names (cache, ".mp4") == 1);
        GStatBuf st;
        CHECK (g_stat (cache, &st) == 0 && (st.st_mode & 077) == 0);   /* the folder it made is private */
        g_free (r.dst);
        g_free (r.error);
        /* another matrix: said in the copy; the samples are only scaled */
        char *dst6 = g_build_filename (cache, "m6.mp4", NULL);
        const WalpeImportScene sc6 = { .off = off, .len = len, .w = 320, .h = 180, .matrix = 6 };
        r = run_scene (tex, &sc6, dst6, 240);
        CHECK (r.called && r.ok);
        got = probe (dst6);
        CHECK (!g_strcmp0 (got, "h264,320,180,yuv420p,tv,smpte170m,30"));
        g_free (got);
        g_free (r.dst);
        g_free (r.error);
        g_unlink (dst6);
        g_free (dst6);
        /* a copy larger than the clip is not made */
        char *dstb = g_build_filename (cache, "big.mp4", NULL);
        const WalpeImportScene big = { .off = off, .len = len, .w = 1920, .h = 1080, .matrix = 1 };
        r = run_scene (tex, &big, dstb, 120);
        CHECK (r.called && !r.ok && r.error && strstr (r.error, "left as it is"));
        CHECK (!g_file_test (dstb, G_FILE_TEST_EXISTS) && count_names (cache, ".part") == 0);
        g_free (r.dst);
        g_free (r.error);
        g_free (dstb);
        /* a range that is not the clip: no video, a reason, nothing left */
        char *dstr = g_build_filename (cache, "range.mp4", NULL);
        const WalpeImportScene wrong = { .off = off + 1000, .len = len - 1000, .w = 640, .h = 360, .matrix = 1 };
        r = run_scene (tex, &wrong, dstr, 120);
        CHECK (r.called && !r.ok && r.error && *r.error);
        CHECK (!g_file_test (dstr, G_FILE_TEST_EXISTS) && count_names (cache, ".part") == 0);
        g_free (r.dst);
        g_free (r.error);
        g_free (dstr);
        g_unlink (dst);
        g_free (dst);
    }

    /* full range, and 4:4:4: left as they are */
    {
        char *dst = g_build_filename (cache, "no.mp4", NULL);
        const WalpeImportScene sc = { .off = off2, .len = len2, .w = 640, .h = 360, .matrix = 1 };
        Result r = run_scene (tex_full, &sc, dst, 120);
        CHECK (r.called && !r.ok && r.error && strstr (r.error, "left as it is"));
        if (r.ok || !r.error || !strstr (r.error, "left as it is"))
            g_printerr ("  full range: ok %d, %s\n", r.ok, r.error ? r.error : "(no error)");
        CHECK (!g_file_test (dst, G_FILE_TEST_EXISTS));
        g_free (r.dst);
        g_free (r.error);
        const WalpeImportScene sc3 = { .off = off3, .len = len3, .w = 640, .h = 360, .matrix = 1 };
        r = run_scene (tex_444, &sc3, dst, 120);
        CHECK (r.called && !r.ok && r.error && strstr (r.error, "left as it is"));
        CHECK (!g_file_test (dst, G_FILE_TEST_EXISTS) && count_names (cache, ".part") == 0);
        g_free (r.dst);
        g_free (r.error);
        g_free (dst);
    }

    /* a long clip: the child runs at the lowest priority; stopped in the
     * middle it leaves no file and no child, and does not report */
    {
        g_unsetenv ("WALPE_CONVERT_BACKGROUND");
        Result c = { 0 };
        char *dst = g_build_filename (cache, "long.mp4", NULL), *part = g_strconcat (dst, ".part", NULL);
        const WalpeImportScene sc = { .off = off4, .len = len4, .w = 640, .h = 360, .matrix = 1 };
        WalpeImport *imp = walpe_import_start_scene (tex_long, &sc, dst, done_cb, &c, NULL);
        CHECK (imp != NULL);
        for (int i = 0; imp && i < 1200 && !c.called && !g_file_test (part, G_FILE_TEST_EXISTS); i++) {
            g_timeout_add (100, stop_loop, NULL);
            g_main_loop_run (loop);
        }
        if (imp && !c.called) {
            int ni = -100, pol = -1;
            CHECK (ffmpeg_children (&ni, &pol) == 1);
            CHECK (ni == 19);
            if (pol != 5)   /* SCHED_IDLE */
                g_print ("scenevideo-test: the conversion's scheduling policy is %d, not SCHED_IDLE (5): "
                         "not offered here\n", pol);
            CHECK (g_file_test (part, G_FILE_TEST_EXISTS));
            walpe_import_cancel (imp);
            for (int i = 0; i < 100 && ffmpeg_children (NULL, NULL) > 0; i++) {
                g_timeout_add (100, stop_loop, NULL);
                g_main_loop_run (loop);
            }
            g_timeout_add (300, stop_loop, NULL);
            g_main_loop_run (loop);
            CHECK (!c.called);
            CHECK (ffmpeg_children (NULL, NULL) == 0);
            CHECK (!g_file_test (part, G_FILE_TEST_EXISTS) && !g_file_test (dst, G_FILE_TEST_EXISTS));
            CHECK (count_names (cache, ".part") == 0);
        } else {
            g_print ("scenevideo-test: the long conversion ended before it could be stopped\n");
        }
        walpe_import_free (imp);
        g_unlink (dst);
        g_free (c.dst);
        g_free (c.error);
        g_free (dst);
        g_free (part);
    }
out:
    {
        const char *dirs[] = { cache, in };
        for (guint i = 0; i < G_N_ELEMENTS (dirs); i++) {
            GDir *d = g_dir_open (dirs[i], 0, NULL);
            const char *name;
            while (d && (name = g_dir_read_name (d))) {
                char *p = g_build_filename (dirs[i], name, NULL);
                g_unlink (p);
                g_free (p);
            }
            if (d)
                g_dir_close (d);
            g_rmdir (dirs[i]);
        }
    }
    g_free (clip); g_free (full); g_free (c444); g_free (lng);
    g_free (tex); g_free (tex_full); g_free (tex_444); g_free (tex_long);
    g_free (in); g_free (cache);
}

int main (void)
{
    tmp = g_dir_make_tmp ("walpe-scenevideo-test-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_IMPORT_ENCODER");
    g_unsetenv ("WALPE_CONVERT_BACKGROUND");
    loop = g_main_loop_new (NULL, FALSE);
    test_decision ();
    test_name ();
    test_argv ();
    test_runs ();
    {
        /* the cache folder the names were asked for (nothing was written there) */
        char *v = g_build_filename (tmp, "walpe", "video", NULL), *w = g_build_filename (tmp, "walpe", NULL);
        g_rmdir (v);
        g_rmdir (w);
        g_free (v);
        g_free (w);
    }
    g_rmdir (tmp);
    if (fails)
        g_printerr ("scenevideo-test: %d check(s) failed\n", fails);
    else
        g_print ("scenevideo-test: ok\n");
    return fails ? 1 : 0;
}
