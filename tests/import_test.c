/* import_test - the video importer (src/common/import.c)
 *
 * Decisions without ffmpeg: the 1080p cap (never upscaled, even sizes,
 * rotation and pixel aspect applied), which files are copied as they are,
 * the argument vector. With ffmpeg and ffprobe in PATH (else that part is
 * skipped) real runs in a private temp dir, clips generated here:
 *  - a file that is already fine is copied (remux), a thumbnail is written;
 *  - a file in another codec and larger than 1080p is encoded to exactly
 *    1920x1080 H.264;
 *  - a file that is no video fails with a reason, nothing is left behind;
 *  - a file that qualifies for a copy but whose copy fails is encoded (an
 *    "ffmpeg" put in front of the real one refuses every stream copy);
 *  - a run that is cancelled leaves no output and no ".part" file, and its
 *    callback is not called;
 *  - the unfinished outputs of a killed daemon are removed
 *    (walpe_import_tidy_parts), and only those;
 *  - the ffmpeg of a conversion does not outlive the process that started
 *    it (this program, started again with --hold, is killed mid-way). */
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common/import.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *tmp;
static GMainLoop *loop;

static char *P (const char *rel)
{
    return g_build_filename (tmp, rel, NULL);
}

static gboolean have_arg (char **argv, const char *a, const char *next)
{
    for (int i = 0; argv[i]; i++)
        if (!strcmp (argv[i], a) && (!next || (argv[i + 1] && !strcmp (argv[i + 1], next))))
            return TRUE;
    return FALSE;
}

/* the box the importer had built in until the size became a setting
 * (video_resolution): what "1080p" gives. Every decision below is the one
 * of that fixed limit. */
static const WalpeVideoBox hd = { 1920, 1080 };

/* the box is the caller's: another one, and none */
static void test_boxes (void)
{
    WalpeMediaInfo mi = { .has_video = TRUE, .codec = (char *) "h264", .pix_fmt = (char *) "yuv420p",
                          .width = 3840, .height = 2160, .sar_num = 1, .sar_den = 1 };
    const WalpeVideoBox none = { 0, 0 }, uhd = { 3840, 2160 }, qhd = { 2560, 1440 }, hdp = { 1280, 720 };
    int w = 0, h = 0;
    /* no limit: a 4K H.264 file is copied as it is */
    CHECK (walpe_import_can_remux (&mi, &none) && walpe_import_can_remux (&mi, NULL));
    walpe_import_target_size (&mi, &none, &w, &h);
    CHECK (w == 3840 && h == 2160);
    walpe_import_target_size (&mi, NULL, &w, &h);
    CHECK (w == 3840 && h == 2160);
    /* a 4K box holds it; 1440p and 720p boxes scale it down */
    CHECK (walpe_import_can_remux (&mi, &uhd) && !walpe_import_can_remux (&mi, &qhd));
    walpe_import_target_size (&mi, &qhd, &w, &h);
    CHECK (w == 2560 && h == 1440);
    walpe_import_target_size (&mi, &hdp, &w, &h);
    CHECK (w == 1280 && h == 720);
    /* never enlarged, whatever the box */
    mi.width = 640; mi.height = 360;
    walpe_import_target_size (&mi, &uhd, &w, &h);
    CHECK (w == 640 && h == 360);
    /* a stream without a size: something even, not 0, also without a box */
    mi.width = mi.height = 0;
    walpe_import_target_size (&mi, &none, &w, &h);
    CHECK (w >= 2 && h >= 2 && !(w & 1) && !(h & 1));
    /* the scale filter carries the box's result */
    mi.width = 7680; mi.height = 4320; mi.codec = (char *) "hevc";
    char **av = walpe_import_build_argv ("/in/a.mkv", "/out/a.mp4.part", &mi, "libx264", FALSE, &uhd);
    CHECK (have_arg (av, "-vf", "scale=3840:2160:flags=bicubic,setsar=1,format=yuv420p"));
    g_strfreev (av);
    av = walpe_import_build_argv ("/in/a.mkv", "/out/a.mp4.part", &mi, "libx264", FALSE, &none);
    CHECK (have_arg (av, "-vf", "scale=7680:4320:flags=bicubic,setsar=1,format=yuv420p"));
    g_strfreev (av);

    /* the converted copy's name: one per box, and the 1920 x 1080 box has
     * the name copies had before there was a choice (no suffix) */
    char *dir = g_dir_make_tmp ("walpe-import-box-XXXXXX", NULL);
    char *src = g_build_filename (dir, "clip.webm", NULL);
    g_file_set_contents (src, "x", 1, NULL);
    char *a = walpe_video_cache_file (src, &hd), *b = walpe_video_cache_file (src, &uhd);
    char *c = walpe_video_cache_file (src, &none), *d = walpe_video_cache_file (src, NULL);
    CHECK (a && b && c && d && strcmp (a, b) && strcmp (a, c) && strcmp (b, c) && !strcmp (c, d));
    CHECK (a && g_str_has_suffix (a, ".mp4") && !strstr (a, "-full") && !strstr (a, "x1080"));
    CHECK (b && g_str_has_suffix (b, "-3840x2160.mp4") && c && g_str_has_suffix (c, "-full.mp4"));
    {
        /* same stem: only the box differs */
        char *ba = a ? g_path_get_basename (a) : NULL, *bb = b ? g_path_get_basename (b) : NULL;
        CHECK (ba && bb && strlen (ba) == 36 && !strncmp (ba, bb, 32));
        g_free (ba);
        g_free (bb);
    }
    g_free (a); g_free (b); g_free (c); g_free (d);
    g_remove (src);
    g_rmdir (dir);
    g_free (src);
    g_free (dir);
}

static void test_decisions (void)
{
    WalpeMediaInfo mi = { .has_video = TRUE, .codec = (char *) "h264", .pix_fmt = (char *) "yuv420p",
                          .width = 1920, .height = 1080, .sar_num = 1, .sar_den = 1 };
    int w = 0, h = 0;
    CHECK (walpe_import_can_remux (&mi, &hd));           /* already what the player reads */
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 1920 && h == 1080);
    mi.width = 640; mi.height = 360;                /* small: copied, never upscaled */
    CHECK (walpe_import_can_remux (&mi, &hd));
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 640 && h == 360);
    mi.width = 3840; mi.height = 2160;              /* 4K: encoded down to the cap */
    CHECK (!walpe_import_can_remux (&mi, &hd));
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 1920 && h == 1080);
    mi.width = 3440; mi.height = 1440;              /* ultrawide: width decides */
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 1920 && h == 802);
    mi.width = 1080; mi.height = 1920;              /* portrait: height decides */
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 606 && h == 1080);
    mi.width = 1920; mi.height = 1080; mi.rotation = 90;    /* stored sideways */
    CHECK (!walpe_import_can_remux (&mi, &hd));
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 606 && h == 1080);
    mi.rotation = 0; mi.sar_num = 4; mi.sar_den = 3; mi.width = 1440;   /* anamorphic 1440x1080 */
    CHECK (!walpe_import_can_remux (&mi, &hd));
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 1920 && h == 1080);
    mi.sar_num = mi.sar_den = 1; mi.width = 1279; mi.height = 719;      /* odd sizes */
    CHECK (!walpe_import_can_remux (&mi, &hd));
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w == 1278 && h == 718);
    mi.width = 1280; mi.height = 720;
    mi.pix_fmt = (char *) "yuv420p10le";
    CHECK (!walpe_import_can_remux (&mi, &hd));
    mi.pix_fmt = (char *) "yuvj420p";
    CHECK (walpe_import_can_remux (&mi, &hd));
    mi.codec = (char *) "hevc";
    CHECK (!walpe_import_can_remux (&mi, &hd));
    mi.width = mi.height = 0;                       /* unknown size: the cap, never 0 */
    walpe_import_target_size (&mi, &hd, &w, &h);
    CHECK (w >= 2 && h >= 2 && w <= 1920 && h <= 1080);

    /* the argument vector: names can never be options or protocols, the
     * source may only be a local container file, no audio, +faststart */
    mi.width = 3840; mi.height = 2160; mi.codec = (char *) "vp9"; mi.pix_fmt = (char *) "yuv420p";
    char **av = walpe_import_build_argv ("/in/-i evil.webm", "/out/x.mp4.part", &mi, "libx264", FALSE, &hd);
    CHECK (have_arg (av, "-i", "file:/in/-i evil.webm") && have_arg (av, "file:/out/x.mp4.part", NULL));
    CHECK (have_arg (av, "-protocol_whitelist", "file") && have_arg (av, "-an", NULL));
    CHECK (have_arg (av, "-c:v", "libx264") && have_arg (av, "-movflags", "+faststart"));
    CHECK (have_arg (av, "-vf", "scale=1920:1080:flags=bicubic,setsar=1,format=yuv420p"));
    CHECK (have_arg (av, "-f", "mp4") && have_arg (av, "-nostdin", NULL));
    g_strfreev (av);
    av = walpe_import_build_argv ("/in/a.mp4", "/out/a.mp4.part", &mi, "libx264", TRUE, &hd);
    CHECK (have_arg (av, "-c:v", "copy") && !have_arg (av, "-vf", NULL));
    g_strfreev (av);
    /* an import into the library keeps the first sound track, where the
     * video has one (as AAC, also beside a stream copy of the pictures);
     * everything else about the run is the same */
    for (int remux = 0; remux < 2; remux++) {
        av = walpe_import_library_argv ("/in/a.mkv", "/out/a.mp4.part", &mi, "libx264", remux, &hd);
        CHECK (!have_arg (av, "-an", NULL) && have_arg (av, "-map", "0:v:0") && have_arg (av, "-map", "0:a:0?") &&
               have_arg (av, "-c:a", "aac") && have_arg (av, "-ac", "2") && have_arg (av, "-sn", NULL) &&
               have_arg (av, "-protocol_whitelist", "file") && have_arg (av, "-f", "mp4") &&
               have_arg (av, "-c:v", remux ? "copy" : "libx264"));
        g_strfreev (av);
    }
    /* taking the sound track out of a video wallpaper: the same limits on
     * what is read, the sound only, lossless, bounded */
    av = walpe_video_sound_argv ("/in/-i evil.mp4", "/c/x-sound.flac.part");
    CHECK (have_arg (av, "-i", "file:/in/-i evil.mp4") && have_arg (av, "file:/c/x-sound.flac.part", NULL) &&
           have_arg (av, "-protocol_whitelist", "file") && have_arg (av, "-format_whitelist", NULL) &&
           have_arg (av, "-map", "0:a:0") && have_arg (av, "-vn", NULL) && have_arg (av, "-c:a", "flac") &&
           have_arg (av, "-ac", "2") && have_arg (av, "-t", "21600") && have_arg (av, "-fs", NULL) &&
           have_arg (av, "-f", "flac") && have_arg (av, "-nostdin", NULL));
    g_strfreev (av);

    /* ffprobe's answer */
    WalpeMediaInfo p;
    CHECK (walpe_media_info_parse ("{\"streams\":[{\"codec_type\":\"video\",\"codec_name\":\"h264\","
                                   "\"pix_fmt\":\"yuv420p\",\"width\":1920,\"height\":\"1080\","
                                   "\"sample_aspect_ratio\":\"1:1\",\"side_data_list\":[{\"rotation\":-90}]}],"
                                   "\"format\":{\"format_name\":\"mov,mp4\",\"duration\":\"12.5\"}}", &p));
    CHECK (p.has_video && p.width == 1920 && p.height == 1080 && p.rotation == 270 && p.duration == 12.5);
    walpe_media_info_clear (&p);
    /* numbers that are no int, angles that are no number: no size, no rotation
     * (the casts they went through are undefined for them) */
    CHECK (walpe_media_info_parse ("{\"streams\":[{\"codec_name\":\"hevc\",\"width\":1e30,\"height\":\"1e999\","
                                   "\"side_data_list\":[{\"rotation\":1e308}]}]}", &p));
    CHECK (p.has_video && p.width <= 0 && p.height <= 0 && p.rotation == 270);  /* 1e308 mod 360 = 296 */
    {
        int tw = 0, th = 0;
        WalpeVideoBox b4k = { 3840, 2160 };
        walpe_import_target_size (&p, &b4k, &tw, &th);      /* a stream that does not say its size */
        CHECK (tw >= 2 && th >= 2 && tw <= 3840 && th <= 3840);
        CHECK (!walpe_import_can_remux (&p, &b4k));
    }
    walpe_media_info_clear (&p);
    CHECK (walpe_media_info_parse ("{\"streams\":[{\"width\":-1e30,\"height\":4294967298,"
                                   "\"tags\":{\"rotate\":\"nan\"}}]}", &p));
    CHECK (p.width <= 0 && p.height <= 0 && p.rotation == 0);
    walpe_media_info_clear (&p);
    CHECK (walpe_media_info_parse ("{\"streams\":[{\"width\":8,\"height\":8,"
                                   "\"side_data_list\":[{\"rotation\":\"-inf\"}]}]}", &p));
    CHECK (p.width == 8 && p.rotation == 0);
    walpe_media_info_clear (&p);
    CHECK (walpe_media_info_parse ("{\"streams\":[{\"width\":8,\"height\":8,"
                                   "\"side_data_list\":[{\"rotation\":-450.4}]}]}", &p));
    CHECK (p.rotation == 270);                               /* as before: -450 = 270 */
    walpe_media_info_clear (&p);
    CHECK (!walpe_media_info_parse ("[1]", &p) && !walpe_media_info_parse ("{", &p));
    CHECK (walpe_media_info_parse ("{\"streams\":[]}", &p) && !p.has_video);
    walpe_media_info_clear (&p);
}

static void test_tidy (void)
{
    char *d = P ("tidy");
    g_mkdir_with_parents (d, 0700);
    char *keep1 = P ("tidy/a.mp4"), *keep2 = P ("tidy/a.jpg"), *keep3 = P ("tidy/notes.part");
    char *gone1 = P ("tidy/a.mp4.part"), *gone2 = P ("tidy/b c.mp4.part");
    char *dirp = P ("tidy/dir.mp4.part"), *link = P ("tidy/link.mp4.part");
    g_file_set_contents (keep1, "v", -1, NULL);
    g_file_set_contents (keep2, "j", -1, NULL);
    g_file_set_contents (keep3, "n", -1, NULL);
    g_file_set_contents (gone1, "p", -1, NULL);
    g_file_set_contents (gone2, "p", -1, NULL);
    g_mkdir (dirp, 0700);
    CHECK (symlink (keep1, link) == 0);
    CHECK (walpe_import_tidy_parts (d) == 2);
    CHECK (!g_file_test (gone1, G_FILE_TEST_EXISTS) && !g_file_test (gone2, G_FILE_TEST_EXISTS));
    CHECK (g_file_test (keep1, G_FILE_TEST_EXISTS) && g_file_test (keep2, G_FILE_TEST_EXISTS) &&
           g_file_test (keep3, G_FILE_TEST_EXISTS) && g_file_test (dirp, G_FILE_TEST_IS_DIR) &&
           g_file_test (link, G_FILE_TEST_IS_SYMLINK));
    CHECK (walpe_import_tidy_parts (d) == 0);
    CHECK (walpe_import_tidy_parts (NULL) == 0);
    char *none = P ("no-such-dir");
    CHECK (walpe_import_tidy_parts (none) == 0);
    g_unlink (link);
    g_rmdir (dirp);
    g_unlink (keep1);
    g_unlink (keep2);
    g_unlink (keep3);
    g_rmdir (d);
    g_free (none); g_free (d); g_free (keep1); g_free (keep2); g_free (keep3);
    g_free (gone1); g_free (gone2); g_free (dirp); g_free (link);
}

/* ---- real runs */

typedef struct {
    gboolean called, ok;
    char *dst, *error, *mode;
    int max_percent;
} Result;

static void done_cb (WalpeImport *imp, gboolean ok, const char *dst, const char *error, gpointer user)
{
    Result *r = user;
    r->called = TRUE;
    r->ok = ok;
    r->dst = g_strdup (dst);
    r->error = g_strdup (error);
    r->mode = g_strdup (walpe_import_mode (imp));
    g_main_loop_quit (loop);
}

static gboolean stop_loop (gpointer u)
{
    (void) u;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static gboolean run_tool (const char *const *argv)
{
    int status = -1;
    return g_spawn_sync (NULL, (char **) argv, NULL,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, NULL, &status, NULL) && status == 0;
}

/* "codec,width,height" of the first video stream, g_free */
static char *probe (const char *file)
{
    const char *argv[] = { "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                           "stream=codec_name,width,height", "-of", "csv=p=0", file, NULL };
    char *out = NULL;
    if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
                       NULL, &out, NULL, NULL, NULL))
        return g_strdup ("");
    return g_strstrip (out);
}

/* names in dir that end in suffix */
static int count_suffix (const char *dir, const char *suffix)
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

static Result run_import (const char *src, const char *dest, int secs)
{
    Result r = { 0 };
    GError *err = NULL;
    WalpeImport *imp = walpe_import_start (src, dest, "libx264", &hd, done_cb, &r, &err);
    if (!imp) {
        r.error = g_strdup (err ? err->message : "?");
        g_clear_error (&err);
        return r;
    }
    guint t = g_timeout_add_seconds ((guint) secs, stop_loop, NULL);
    g_main_loop_run (loop);
    if (r.called)
        g_source_remove (t);
    r.max_percent = walpe_import_percent (imp);
    walpe_import_free (imp);
    return r;
}

static void result_clear (Result *r)
{
    g_free (r->dst);
    g_free (r->error);
    g_free (r->mode);
}

/* the first process whose parent is pid (0 = none), and whether a process
 * still runs (a zombie does not) */
static long stat_field (long pid, int which, char *state)
{
    char *path = g_strdup_printf ("/proc/%ld/stat", pid), *txt = NULL;
    long v = -1;
    if (g_file_get_contents (path, &txt, NULL, NULL)) {
        const char *p = strrchr (txt, ')');     /* (the name may hold spaces) */
        char st = 0;
        long ppid = 0;
        if (p && sscanf (p + 1, " %c %ld", &st, &ppid) == 2) {
            v = which ? ppid : 0;
            if (state)
                *state = st;
        }
    }
    g_free (txt);
    g_free (path);
    return v;
}

static long child_of (long pid)
{
    long found = 0;
    GDir *d = g_dir_open ("/proc", 0, NULL);
    const char *name;
    while (d && !found && (name = g_dir_read_name (d))) {
        long p = g_ascii_isdigit (*name) ? atol (name) : 0;
        if (p > 0 && stat_field (p, 1, NULL) == pid)
            found = p;
    }
    if (d)
        g_dir_close (d);
    return found;
}

static gboolean pid_runs (long pid)
{
    char st = 0;
    return stat_field (pid, 0, &st) == 0 && st != 'Z' && st != 'X';
}

/* import-test --hold <src> <dst>: a conversion into <dst>, and this
 * process stays until it is killed */
static int hold_main (const char *src, const char *dst)
{
    Result r = { 0 };
    loop = g_main_loop_new (NULL, FALSE);
    WalpeImport *imp = walpe_import_start_to (src, dst, "libx264", &hd, done_cb, &r, NULL);
    if (!imp)
        return 2;
    g_main_loop_run (loop);
    for (;;)
        pause ();
    return 0;
}

/* the question a child of an import asks before it starts ffmpeg: is the
 * process that made it still its parent? Asked here by a child of this
 * program about its real parent, and about a process that is not. */
static void test_child_of (void)
{
    gint64 me = (gint64) getpid ();
    CHECK (walpe_import_child_of ((gint64) getppid ()));
    CHECK (!walpe_import_child_of (me));
    pid_t c = fork ();
    if (c == 0)
        _exit ((walpe_import_child_of (me) ? 0 : 1) | (walpe_import_child_of ((gint64) getpid ()) ? 2 : 0) |
               (walpe_import_child_of (me + 1) ? 4 : 0));
    int st = -1;
    CHECK (c > 0 && waitpid (c, &st, 0) == c && WIFEXITED (st) && WEXITSTATUS (st) == 0);
}

static void test_runs (void)
{
    char *ff = g_find_program_in_path ("ffmpeg"), *fp = g_find_program_in_path ("ffprobe");
    if (!ff || !fp) {
        g_print ("import-test: ffmpeg / ffprobe not in PATH, real runs skipped\n");
        g_free (ff);
        g_free (fp);
        return;
    }
    g_free (ff);
    g_free (fp);
    char *in = P ("in"), *lib = P ("lib");
    g_mkdir_with_parents (in, 0700);
    char *fine = P ("in/fine.mp4"), *big = P ("in/big clip.avi"), *junk = P ("in/junk.mp4"),
         *lng = P ("in/long.avi");
    const char *mk_fine[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                              "testsrc2=size=640x360:rate=30", "-t", "1", "-c:v", "libx264", "-preset",
                              "ultrafast", "-pix_fmt", "yuv420p", fine, NULL };
    const char *mk_big[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                             "testsrc2=size=2560x1440:rate=10", "-t", "0.5", "-c:v", "mpeg4", "-q:v", "8",
                             big, NULL };
    const char *mk_long[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                              "testsrc2=size=1280x720:rate=30", "-t", "40", "-c:v", "mpeg4", "-q:v", "12",
                              lng, NULL };
    if (!run_tool (mk_fine) || !run_tool (mk_big) || !run_tool (mk_long)) {
        g_print ("import-test: this ffmpeg cannot make the test clips (libx264 / mpeg4), real runs skipped\n");
        goto out;
    }
    g_file_set_contents (junk, "this is not a video\n", -1, NULL);

    /* the sound track of a video wallpaper (walpe_video_sound_extract) */
    {
        char *snd = P ("in/sound.mp4"), *dir = P ("in/x");
        const char *mk_snd[] = { "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                                 "testsrc2=size=320x180:rate=30", "-f", "lavfi", "-i",
                                 "sine=frequency=440:sample_rate=44100", "-t", "1", "-c:v", "libx264",
                                 "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", snd, NULL };
        if (run_tool (mk_snd)) {
            char *f1 = walpe_video_sound_file (snd), *f2 = walpe_video_sound_file (fine),
                 *f3 = walpe_video_sound_file (junk), *err = NULL, *head = NULL;
            gsize len = 0;
            CHECK (f1 && f2 && f3 && strcmp (f1, f2) && g_str_has_suffix (f1, "-sound.flac") &&
                   g_str_has_prefix (f1, tmp) && !walpe_video_sound_file (dir) && !walpe_video_sound_file (in));
            CHECK (walpe_video_sound_extract (snd, f1, &err) == 1 && !err &&
                   g_file_get_contents (f1, &head, &len, NULL) && len > 1000 && !memcmp (head, "fLaC", 4));
            g_free (head);
            g_free (err);
            err = NULL;
            /* no track: the empty file that says so, and no second run */
            GStatBuf st;
            CHECK (walpe_video_sound_extract (fine, f2, &err) == 0 && !err && g_stat (f2, &st) == 0 && st.st_size == 0);
            CHECK (walpe_video_sound_extract (fine, f2, &err) == 0 && walpe_video_sound_extract (snd, f1, &err) == 1);
            g_free (err);
            err = NULL;
            /* not a video: an error that says what ffmpeg said, nothing left behind */
            char *part = g_strconcat (f3, ".part", NULL);
            CHECK (walpe_video_sound_extract (junk, f3, &err) == -1 && err && *err &&
                   !g_file_test (f3, G_FILE_TEST_EXISTS) && !g_file_test (part, G_FILE_TEST_EXISTS));
            g_free (err);
            g_free (part);
            g_remove (f1);
            g_remove (f2);
            char *cdir = g_path_get_dirname (f1), *cup = g_path_get_dirname (cdir);
            g_rmdir (cdir);
            g_rmdir (cup);
            g_free (cdir);
            g_free (cup);
            g_free (f1);
            g_free (f2);
            g_free (f3);
        } else {
            g_print ("import-test: this ffmpeg cannot make a clip with sound (aac), that part skipped\n");
        }
        g_remove (snd);
        g_free (snd);
        g_free (dir);
    }

    /* already fine: copied, thumbnail next to it */
    Result r = run_import (fine, lib, 120);
    CHECK (r.called && r.ok && r.dst && g_file_test (r.dst, G_FILE_TEST_IS_REGULAR));
    CHECK (!g_strcmp0 (r.mode, "remux") && r.max_percent == 100);
    if (r.dst) {
        char *got = probe (r.dst);
        CHECK (!g_strcmp0 (got, "h264,640,360"));
        g_free (got);
        char *want = g_build_filename (lib, "fine.mp4", NULL), *th = g_build_filename (lib, "fine.jpg", NULL);
        CHECK (!g_strcmp0 (r.dst, want) && g_file_test (th, G_FILE_TEST_IS_REGULAR));
        g_free (want);
        g_free (th);
    }
    CHECK (count_suffix (lib, ".part") == 0);
    result_clear (&r);

    /* another codec, larger than the cap: encoded to exactly 1920x1080 */
    r = run_import (big, lib, 240);
    CHECK (r.called && r.ok && r.dst && r.mode && g_str_has_prefix (r.mode, "transcode:"));
    if (r.dst) {
        char *got = probe (r.dst);
        CHECK (!g_strcmp0 (got, "h264,1920,1080"));
        g_free (got);
    }
    CHECK (count_suffix (lib, ".part") == 0 && count_suffix (lib, ".mp4") == 2);
    result_clear (&r);

    /* importing the same source again replaces its output, another file
     * of the same name gets a new one */
    r = run_import (fine, lib, 120);
    CHECK (r.called && r.ok && count_suffix (lib, ".mp4") == 3);    /* fine-2.mp4: fine.mp4 is another file */
    result_clear (&r);

    /* not a video: a reason, nothing left behind */
    r = run_import (junk, lib, 120);
    CHECK (r.called && !r.ok && !r.dst && r.error && *r.error);
    CHECK (count_suffix (lib, ".part") == 0 && count_suffix (lib, ".mp4") == 3);
    result_clear (&r);
    char *missing = P ("in/missing.mp4");
    r = run_import (missing, lib, 10);
    CHECK (!r.called && r.error && strstr (r.error, "no such file"));
    result_clear (&r);
    g_free (missing);

    /* a name with "%03d" in it: the thumbnail has the video's name, and no
     * file of the name a frame number would give is written (or replaced) */
    {
        char *pat = P ("in/take %03d.mp4"), *data = NULL;
        char *th = g_build_filename (lib, "take %03d.jpg", NULL), *other = g_build_filename (lib, "take 001.jpg", NULL);
        gsize len = 0;
        CHECK (g_file_get_contents (fine, &data, &len, NULL) && g_file_set_contents (pat, data, (gssize) len, NULL));
        g_file_set_contents (other, "a picture of that name", -1, NULL);
        r = run_import (pat, lib, 120);
        CHECK (r.called && r.ok && r.dst && g_str_has_suffix (r.dst, "/take %03d.mp4"));
        CHECK (g_file_test (th, G_FILE_TEST_IS_REGULAR));
        g_free (data);
        data = NULL;
        CHECK (g_file_get_contents (other, &data, &len, NULL) && !g_strcmp0 (data, "a picture of that name"));
        CHECK (count_suffix (lib, ".part") == 0 && count_suffix (lib, ".mp4") == 4);
        result_clear (&r);
        g_free (pat); g_free (data); g_free (th); g_free (other);
    }

    /* a file that qualifies for a copy, but the copy fails: encoded instead
     * of "ffmpeg failed". An "ffmpeg" in front of the real one in PATH
     * refuses every run that asks for a stream copy and notes that it was
     * asked. */
    {
        char *real = g_find_program_in_path ("ffmpeg"), *fake = P ("in/ffmpeg"), *mark = P ("in/copy-asked"),
             *again = P ("in/again.mp4"), *data = NULL;
        gsize len = 0;
        char *q_real = g_shell_quote (real), *q_mark = g_shell_quote (mark);
        char *sh = g_strdup_printf ("#!/bin/sh\nfor a in \"$@\"; do\n  if [ \"$a\" = copy ]; then : > %s; exit 1; fi\n"
                                    "done\nexec %s \"$@\"\n", q_mark, q_real);
        char *old_path = g_strdup (g_getenv ("PATH")), *path = g_strconcat (in, ":", old_path, NULL);
        if (g_file_get_contents (fine, &data, &len, NULL) && g_file_set_contents (again, data, (gssize) len, NULL) &&
            g_file_set_contents (fake, sh, -1, NULL) && !g_chmod (fake, 0700)) {
            g_setenv ("PATH", path, TRUE);
            r = run_import (again, lib, 240);
            g_setenv ("PATH", old_path, TRUE);
            CHECK (g_file_test (mark, G_FILE_TEST_EXISTS));     /* the copy was tried first */
            CHECK (r.called && r.ok && r.dst && r.mode && g_str_has_prefix (r.mode, "transcode:"));
            if (r.dst) {
                char *got = probe (r.dst);
                CHECK (!g_strcmp0 (got, "h264,640,360"));
                g_free (got);
            }
            CHECK (count_suffix (lib, ".part") == 0 && count_suffix (lib, ".mp4") == 5);
            result_clear (&r);
        } else {
            CHECK (!"the stand-in ffmpeg could not be written");
        }
        g_free (real); g_free (fake); g_free (mark); g_free (again); g_free (data);
        g_free (q_real); g_free (q_mark); g_free (sh); g_free (old_path); g_free (path);
    }

    /* cancelled in the middle of the encode: no output, no ".part", no callback */
    {
        Result c = { 0 };
        WalpeImport *imp = walpe_import_start (lng, lib, "libx264", &hd, done_cb, &c, NULL);
        CHECK (imp != NULL);
        char *part = g_build_filename (lib, "long.mp4.part", NULL), *dst = g_build_filename (lib, "long.mp4", NULL);
        /* wait until ffmpeg writes (or the run ended, on a very fast machine) */
        for (int i = 0; imp && i < 600 && !c.called && !g_file_test (part, G_FILE_TEST_EXISTS); i++) {
            g_timeout_add (100, stop_loop, NULL);
            g_main_loop_run (loop);
        }
        if (!c.called) {
            CHECK (g_file_test (part, G_FILE_TEST_EXISTS));
            walpe_import_cancel (imp);
            g_timeout_add (1500, stop_loop, NULL);      /* let the child die and the callbacks run */
            g_main_loop_run (loop);
            CHECK (!c.called);
            CHECK (!g_file_test (part, G_FILE_TEST_EXISTS) && !g_file_test (dst, G_FILE_TEST_EXISTS));
            CHECK (count_suffix (lib, ".part") == 0);
            CHECK (walpe_import_percent (imp) < 100);
        } else {
            g_print ("import-test: the long encode finished before it could be cancelled\n");
        }
        walpe_import_free (imp);
        result_clear (&c);
        g_free (part);
        g_free (dst);
    }
    /* the daemon is killed in the middle of a conversion: its ffmpeg ends
     * with it (only the daemon names the output and watches its limits).
     * The "daemon" is this program started again as a helper. */
    {
        char *self = g_file_read_link ("/proc/self/exe", NULL);
        char *dst = g_build_filename (lib, "held.mp4", NULL), *part = g_strconcat (dst, ".part", NULL);
        char *hargv[] = { self, (char *) "--hold", lng, dst, NULL };
        GPid helper = 0;
        gboolean up = self && g_spawn_async (NULL, hargv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                             &helper, NULL);
        CHECK (up);
        long child = 0;
        for (int i = 0; up && i < 600 && !(child && g_file_test (part, G_FILE_TEST_EXISTS)); i++) {
            g_usleep (100000);
            child = child_of ((long) helper);
        }
        if (up) {
            CHECK (child > 0 && g_file_test (part, G_FILE_TEST_EXISTS));
            kill (helper, SIGKILL);
            waitpid (helper, NULL, 0);
            gboolean gone = child <= 0;
            for (int i = 0; child > 0 && i < 100 && !(gone = !pid_runs (child)); i++)
                g_usleep (100000);
            CHECK (gone);
            if (child > 0 && !gone)
                kill ((pid_t) child, SIGKILL);     /* not left behind by a failing run */
            g_usleep (200000);
        }
        g_unlink (part);
        g_free (self); g_free (dst); g_free (part);
    }
out:
    {
        /* remove what the runs made */
        const char *dirs[] = { lib, in };
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
    g_free (fine); g_free (big); g_free (junk); g_free (lng); g_free (in); g_free (lib);
}

int main (int argc, char **argv)
{
    if (argc == 4 && !strcmp (argv[1], "--hold"))
        return hold_main (argv[2], argv[3]);
    tmp = g_dir_make_tmp ("walpe-import-test-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_IMPORT_ENCODER");
    loop = g_main_loop_new (NULL, FALSE);
    test_decisions ();
    test_boxes ();
    test_tidy ();
    test_child_of ();
    test_runs ();
    g_rmdir (tmp);
    if (fails)
        g_printerr ("import-test: %d check(s) failed\n", fails);
    else
        g_print ("import-test: ok\n");
    return fails ? 1 : 0;
}
