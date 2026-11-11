/* importsec_test.c - the video importer / converter against a hostile
 * source file. ffmpeg and ffprobe are
 * replaced by shell scripts this test writes into a private directory at
 * the front of PATH, so it needs neither of them and no display.
 *
 *  1. the command line: the source is one argument behind "file:" (a name
 *     that starts with "-" or holds shell characters cannot become an
 *     option or a command), protocols and demuxers are restricted, the
 *     decoders get a picture size limit - all before "-i";
 *  2. an ffmpeg that prints a line that is not UTF-8 (file contents quoted
 *     in a message) and then keeps writing: the job used to stop reading
 *     at that line, ffmpeg blocked on the full pipe and the import never
 *     ended (nor could another one start);
 *  3. a conversion on open (walpe_import_start_to) whose output grows
 *     without end is stopped at WALPE_CONVERT_MAX_MB and its partial
 *     output removed; the same job through walpe_import_start (a file the
 *     user imports by hand) is not limited.
 *
 *  4. (not the importer, but it needs no test program of its own) a web
 *     wallpaper cannot redefine WalPE's "Allow network access" option: a
 *     project.json property named walpe_network used to replace the
 *     built-in row in the options panel (another label, hidden for good).
 *
 * Run: meson test -C build importsec -v */
#include "common/import.h"
#include "common/util.h"
#include "engine/content.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

static GMainLoop *loop;
static struct { gboolean called, ok; char *error; } R;

static void on_done (WalpeImport *job, gboolean ok, const char *dst, const char *error, gpointer user)
{
    (void) job; (void) dst; (void) user;
    R.called = TRUE;
    R.ok = ok;
    g_free (R.error);
    R.error = g_strdup (error ? error : "");
    g_main_loop_quit (loop);
}

static gboolean on_timeout (gpointer user)
{
    *(gboolean *) user = TRUE;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

/* run the main loop until the job reports or `seconds` pass; TRUE = it reported */
static gboolean wait_done (int seconds)
{
    gboolean timed_out = FALSE;
    R.called = FALSE;
    guint id = g_timeout_add_seconds ((guint) seconds, on_timeout, &timed_out);
    g_main_loop_run (loop);
    if (!timed_out)
        g_source_remove (id);
    return R.called;
}

static void write_tool (const char *dir, const char *name, const char *script)
{
    char *p = g_build_filename (dir, name, NULL);
    g_file_set_contents (p, script, -1, NULL);
    g_chmod (p, 0755);
    g_free (p);
}

static int argv_index (char **argv, const char *s)
{
    for (int i = 0; argv[i]; i++)
        if (!strcmp (argv[i], s))
            return i;
    return -1;
}

static void test_argv (void)
{
    WalpeMediaInfo mi = { .has_video = TRUE, .width = 640, .height = 360, .sar_num = 1, .sar_den = 1 };
    mi.codec = g_strdup ("vp9");
    mi.pix_fmt = g_strdup ("yuv420p");
    const char *src = "/tmp/-y evil; $(touch x) `id` |& .mkv";
    char **argv = walpe_import_build_argv (src, "/tmp/out.mp4.part", &mi, "libx264", FALSE, NULL);
    int in = argv_index (argv, "-i");
    CHECK (in > 0 && argv[in + 1] != NULL);
    if (in > 0 && argv[in + 1]) {
        char *want = g_strconcat ("file:", src, NULL);
        CHECK (!strcmp (argv[in + 1], want));       /* one argument, behind the file: protocol */
        g_free (want);
    }
    int pw = argv_index (argv, "-protocol_whitelist"), fw = argv_index (argv, "-format_whitelist"),
        mp = argv_index (argv, "-max_pixels");
    CHECK (pw > 0 && pw < in && !g_strcmp0 (argv[pw + 1], "file"));
    CHECK (fw > 0 && fw < in);
    CHECK (mp > 0 && mp < in);
    if (mp > 0) {
        gint64 px = g_ascii_strtoll (argv[mp + 1], NULL, 10);
        CHECK (px >= 7680 * 4320 && px <= 64 * 1000 * 1000);    /* 8K passes, 16K x 16K does not */
    }
    if (fw > 0) {       /* no demuxer that opens further files or URLs */
        char *f = g_strdup_printf (",%s,", argv[fw + 1]);
        static const char *const bad[] = { ",hls,", ",concat,", ",dash,", ",image2,", ",lavfi,",
                                           ",applehttp,", ",tedcaptions,", ",subfile," };
        for (guint i = 0; i < G_N_ELEMENTS (bad); i++)
            CHECK (!strstr (f, bad[i]));
        g_free (f);
    }
    CHECK (argv_index (argv, "-an") > in);
    int last = (int) g_strv_length (argv) - 1;
    CHECK (g_str_has_prefix (argv[last], "file:/tmp/out.mp4.part"));
    g_strfreev (argv);
    walpe_media_info_clear (&mi);
}

static void test_network_option (const char *dir)
{
    char *wp = g_build_filename (dir, "webitem", NULL);
    g_mkdir (wp, 0700);
    char *pj = g_build_filename (wp, "project.json", NULL), *html = g_build_filename (wp, "index.html", NULL);
    g_file_set_contents (html, "<html></html>", -1, NULL);
    g_file_set_contents (pj,
        "{ \"type\": \"web\", \"file\": \"index.html\", \"general\": { \"properties\": {"
        " \"speed\": { \"type\": \"slider\", \"text\": \"Speed\", \"value\": 1, \"min\": 0, \"max\": 2 },"
        " \"walpe_network\": { \"type\": \"text\", \"text\": \"Reduce motion\", \"value\": \"x\","
        " \"condition\": \"false\" },"
        " \"walpe_privacy\": { \"type\": \"bool\", \"text\": \"Looks\", \"value\": true } } } }", -1, NULL);
    JsonNode *defs = walpe_content_property_defs (wp, NULL);
    CHECK (defs && JSON_NODE_HOLDS_OBJECT (defs));
    if (defs && JSON_NODE_HOLDS_OBJECT (defs)) {
        JsonObject *o = json_node_get_object (defs);
        JsonObject *n = wpj_obj (o, "walpe_network");
        CHECK (n != NULL);
        CHECK (!g_strcmp0 (wpj_str (n, "type", ""), "bool"));
        CHECK (!g_strcmp0 (wpj_str (n, "text", ""), "Allow network access"));
        CHECK (n && !json_object_has_member (n, "condition"));
        CHECK (!g_strcmp0 (wpj_str (wpj_obj (o, "walpe_privacy"), "type", ""), "group"));
        CHECK (!g_strcmp0 (wpj_str (wpj_obj (o, "speed"), "text", ""), "Speed"));  /* its own stay */
        json_node_unref (defs);
    }
    g_remove (pj);
    g_remove (html);
    g_rmdir (wp);
    g_free (pj);
    g_free (html);
    g_free (wp);
}

int main (void)
{
    char *dir = g_dir_make_tmp ("walpe-importsec-XXXXXX", NULL);
    char *bin = g_build_filename (dir, "bin", NULL), *lib = g_build_filename (dir, "lib", NULL);
    char *cache = g_build_filename (dir, "cache", NULL);
    g_mkdir (bin, 0700);
    char *path = g_strdup_printf ("%s:%s", bin, g_getenv ("PATH") ? g_getenv ("PATH") : "/usr/bin:/bin");
    g_setenv ("PATH", path, TRUE);
    g_setenv ("WALPE_CONVERT_MAX_MB", "2", TRUE);
    g_unsetenv ("WALPE_CONVERT_MAX_MIN");
    g_unsetenv ("WALPE_IMPORT_ENCODER");
    loop = g_main_loop_new (NULL, FALSE);

    test_argv ();
    test_network_option (dir);

    /* the stand-ins. ffprobe: one VP9 stream. ffmpeg: "-encoders" lists
     * libx264; otherwise its mode is read from $FAKE_MODE:
     *   chatty: a line that is not UTF-8, 4000 more lines (several pipe
     *           buffers), then the output file and success
     *   grow:   an output file that grows by 1 MB every 0.2 s, for ever */
    write_tool (bin, "ffprobe",
                "#!/bin/sh\n"
                "printf '%s\\n' '{\"streams\":[{\"codec_type\":\"video\",\"codec_name\":\"vp9\","
                "\"pix_fmt\":\"yuv420p\",\"width\":640,\"height\":360,\"duration\":\"2.0\"}],"
                "\"format\":{\"format_name\":\"matroska,webm\",\"duration\":\"2.0\"}}'\n");
    write_tool (bin, "ffmpeg",
                "#!/bin/sh\n"
                "for a in \"$@\"; do last=$a; [ \"$a\" = -encoders ] && { echo ' V....D libx264              H.264'; exit 0; }; done\n"
                "out=${last#file:}\n"
                "case \"$out\" in *.jpg) : > \"$out\"; exit 0;; esac\n"
                "if [ \"$FAKE_MODE\" = chatty ]; then\n"
                "  printf '[matroska] unknown tag \\377\\376\\200 in stream\\n' >&2\n"
                "  i=0; while [ $i -lt 4000 ]; do echo \"frame=$i\"; echo \"out_time_us=${i}000\"; echo \"[warn] line $i of padding padding padding padding\" >&2; i=$((i+1)); done\n"
                "  echo data > \"$out\"; echo progress=end; exit 0\n"
                "fi\n"
                "if [ \"$FAKE_MODE\" = grow ]; then\n"
                "  : > \"$out\"\n"
                "  while :; do dd if=/dev/zero bs=1024 count=1024 2>/dev/null >> \"$out\"; sleep 0.2; done\n"
                "fi\n"
                "echo data > \"$out\"; exit 0\n");
    char *src = g_build_filename (dir, "clip \xe2\x98\x83.webm", NULL);
    g_file_set_contents (src, "not really a video", -1, NULL);

    /* 2. output that is not UTF-8 */
    g_setenv ("FAKE_MODE", "chatty", TRUE);
    GError *err = NULL;
    WalpeImport *job = walpe_import_start (src, lib, NULL, NULL, on_done, NULL, &err);
    CHECK (job != NULL);
    if (job) {
        gboolean done = wait_done (30);
        if (!done)
            g_printerr ("FAIL: the import did not end within 30 s (ffmpeg blocked on its "
                        "output pipe after a line that is not UTF-8)\n");
        CHECK (done);
        CHECK (!done || R.ok);
        walpe_import_free (job);
    }
    g_clear_error (&err);

    /* 3a. a conversion whose output grows for ever: stopped at 2 MB */
    g_setenv ("FAKE_MODE", "grow", TRUE);
    char *dst = g_build_filename (cache, "abc.mp4", NULL), *part = g_strconcat (dst, ".part", NULL);
    job = walpe_import_start_to (src, dst, NULL, NULL, on_done, NULL, &err);
    CHECK (job != NULL);
    if (job) {
        gboolean done = wait_done (30);
        if (!done)
            g_printerr ("FAIL: the conversion was still writing after 30 s (limit: 2 MB)\n");
        CHECK (done);
        CHECK (!done || !R.ok);
        CHECK (!done || strstr (R.error, "WALPE_CONVERT_MAX_MB") != NULL);
        GStatBuf st;
        if (!done && g_stat (part, &st) == 0)
            g_printerr ("  partial output so far: %.1f MB\n", (double) st.st_size / 1048576.0);
        walpe_import_free (job);        /* (stops the stand-in when the limit did not) */
        CHECK (!g_file_test (part, G_FILE_TEST_EXISTS));
        CHECK (!g_file_test (dst, G_FILE_TEST_EXISTS));
    }
    g_clear_error (&err);

    /* 3b. the same file imported by hand is not stopped by that limit */
    job = walpe_import_start (src, lib, NULL, NULL, on_done, NULL, &err);
    CHECK (job != NULL);
    if (job) {
        gboolean done = wait_done (4);  /* 4 s = ~20 MB written, limit 2 MB */
        CHECK (!done);
        walpe_import_free (job);
    }
    g_clear_error (&err);
    g_usleep (300000);                  /* let the stand-in go */

    /* tidy up: everything is below the directory this test made */
    {
        char *argv[] = { (char *) "rm", (char *) "-rf", (char *) "--", dir, NULL };
        if (g_str_has_prefix (dir, g_get_tmp_dir ()) && strstr (dir, "walpe-importsec-"))
            g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    }
    g_free (part);
    g_free (dst);
    g_free (src);
    g_free (path);
    g_free (cache);
    g_free (lib);
    g_free (bin);
    g_free (dir);
    g_free (R.error);
    if (failures)
        g_printerr ("importsec: %d check(s) failed\n", failures);
    else
        g_print ("importsec: ok\n");
    return failures ? 1 : 0;
}
