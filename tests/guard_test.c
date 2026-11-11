/* guard_test - input checks added by the 0.12 audit, no display and no
 * daemon needed:
 *  - project.json "type" is read whatever its case ("Video", "WEB");
 *  - project.json "file" must stay inside the wallpaper folder;
 *  - web wallpapers get the built-in "Allow network access" option,
 *    flat ones (video / GIF / image) keep their alignment options;
 *  - a fit position that is not a finite number keeps the default;
 *  - a picture with a huge canvas is refused before it is decoded;
 *  - the MP4 demuxer refuses a FIFO instead of blocking in open().
 * Works in a private temp dir that is also HOME and the XDG dirs. */
#include <glib.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/config.h"
#include "engine/content.h"
#include "engine/fit.h"
#include "engine/image.h"
#include "engine/mp4demux.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char *root;

/* <root>/<name>/project.json with the given type and "file" */
static char *item (const char *name, const char *type, const char *file)
{
    char *dir = g_build_filename (root, name, NULL);
    g_mkdir_with_parents (dir, 0700);
    char *pj = g_build_filename (dir, "project.json", NULL);
    char *ef = g_strescape (file, NULL);
    char *js = g_strdup_printf ("{ \"type\": \"%s\", \"title\": \"%s\", \"file\": \"%s\" }", type, name, ef);
    g_file_set_contents (pj, js, -1, NULL);
    g_free (js);
    g_free (ef);
    g_free (pj);
    return dir;
}

static WalpeType type_of (const char *name, const char *type, const char *file, char **media)
{
    char *dir = item (name, type, file);
    WalpeType t = walpe_detect_type (dir, media);
    g_free (dir);
    return t;
}

/* a GIF whose logical screen is w x h, with one 1x1 frame (43 bytes) */
static char *gif_canvas (const char *name, guint w, guint h)
{
    guint8 g[] = { 'G', 'I', 'F', '8', '9', 'a', (guint8) w, (guint8) (w >> 8), (guint8) h, (guint8) (h >> 8),
                   0x80, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff,
                   0x21, 0xf9, 4, 1, 0, 0, 0, 0,
                   0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, 0x3b };
    char *p = g_build_filename (root, name, NULL);
    g_file_set_contents (p, (const char *) g, sizeof g, NULL);
    return p;
}

int main (void)
{
    root = g_dir_make_tmp ("walpe-guard-XXXXXX", NULL);
    g_setenv ("HOME", root, TRUE);
    g_setenv ("XDG_CONFIG_HOME", root, TRUE);
    g_setenv ("XDG_CACHE_HOME", root, TRUE);
    g_setenv ("XDG_DATA_HOME", root, TRUE);

    /* --- "type" in any case (25 of 137 items of the test library capitalise it) */
    char *m = NULL;
    CHECK (type_of ("t1", "video", "clip.mp4", &m) == WALPE_TYPE_VIDEO);
    CHECK (m && g_str_has_suffix (m, "/t1/clip.mp4"));
    g_clear_pointer (&m, g_free);
    CHECK (type_of ("t2", "Video", "clip.mp4", NULL) == WALPE_TYPE_VIDEO);
    CHECK (type_of ("t3", "WEB", "index.html", NULL) == WALPE_TYPE_WEB);
    CHECK (type_of ("t4", "Scene", "scene.json", NULL) == WALPE_TYPE_SCENE);
    CHECK (type_of ("t5", "application", "x.exe", NULL) == WALPE_TYPE_UNSUPPORTED);

    /* --- "file" stays inside the folder */
    CHECK (type_of ("f1", "video", "../t1/clip.mp4", &m) == WALPE_TYPE_UNSUPPORTED);
    CHECK (m == NULL);
    CHECK (type_of ("f2", "video", "/etc/passwd", &m) == WALPE_TYPE_UNSUPPORTED);
    CHECK (m == NULL);
    CHECK (type_of ("f3", "video", "sub/../../t1/clip.mp4", NULL) == WALPE_TYPE_UNSUPPORTED);
    CHECK (type_of ("f4", "web", "..", NULL) == WALPE_TYPE_UNSUPPORTED);
    CHECK (type_of ("f5", "video", "sub/..", NULL) == WALPE_TYPE_UNSUPPORTED);
    CHECK (type_of ("f6", "video", "sub/clip.mp4", &m) == WALPE_TYPE_VIDEO);
    CHECK (m && g_str_has_suffix (m, "/f6/sub/clip.mp4"));
    g_clear_pointer (&m, g_free);
    CHECK (type_of ("f7", "video", "a..b.mp4", NULL) == WALPE_TYPE_VIDEO);   /* dots in a name are fine */
    CHECK (type_of ("f8", "video", "..clip.mp4", NULL) == WALPE_TYPE_VIDEO);

    /* --- built-in options by type */
    char *web = item ("b1", "Web", "index.html"), *vid = item ("b2", "video", "clip.mp4");
    JsonNode *bw = walpe_content_builtin_props (web), *bv = walpe_content_builtin_props (vid);
    CHECK (bw && json_object_has_member (json_node_get_object (bw), "walpe_network"));
    CHECK (bw && !json_object_has_member (json_node_get_object (bw), "walpe_fit"));
    CHECK (bv && json_object_has_member (json_node_get_object (bv), "walpe_fit"));
    CHECK (bv && !json_object_has_member (json_node_get_object (bv), "walpe_network"));
    if (bw) {
        JsonObject *n = json_object_get_object_member (json_node_get_object (bw), "walpe_network");
        CHECK (n && json_object_get_boolean_member (n, "value") == TRUE);   /* network stays on by default */
        json_node_unref (bw);
    }
    if (bv)
        json_node_unref (bv);
    char *sc = item ("b3", "scene", "scene.json");
    CHECK (walpe_content_builtin_props (sc) == NULL);
    g_free (sc);

    /* --- fit position: "nan" / "inf" / text keep the default, numbers are clamped */
    WalpeConfig *cfg = walpe_config_load ();
    WalpeFit f;
    walpe_config_set_prop (cfg, vid, "walpe_pos_x", "nan");
    walpe_config_set_prop (cfg, vid, "walpe_pos_y", "-inf");
    walpe_content_fit (vid, &f);
    CHECK (f.pos_x == 0.5f && f.pos_y == 0.5f);
    walpe_config_set_prop (cfg, vid, "walpe_pos_x", "25");
    walpe_config_set_prop (cfg, vid, "walpe_pos_y", "400");
    walpe_content_fit (vid, &f);
    CHECK (f.pos_x == 0.25f && f.pos_y == 1.0f);
    walpe_config_free (cfg);
    g_free (web);
    g_free (vid);

    /* --- picture size limit (before decoding) */
    GError *err = NULL;
    char *big = gif_canvas ("big.gif", 12000, 12000), *small = gif_canvas ("small.gif", 320, 200);
    int gw = 0, gh = 0;
    if (gdk_pixbuf_get_file_info (big, &gw, &gh)) {     /* needs the GIF loader */
        CHECK (gw == 12000 && gh == 12000);
        CHECK (!walpe_picture_size_ok (big, &err));
        CHECK (err && strstr (err->message, "12000x12000"));
        g_clear_error (&err);
        CHECK (walpe_picture_size_ok (small, &err) && !err);
    } else {
        g_print ("guard-test: no GIF loader, picture size check skipped\n");
    }
    CHECK (walpe_picture_size_ok ("/nonexistent/x.png", NULL));     /* not this check's business */
    g_free (big);
    g_free (small);

    /* --- a FIFO is not a video: refused at once (open() used to block for good) */
    char *fifo = g_build_filename (root, "fifo.mp4", NULL);
    if (mkfifo (fifo, 0600) == 0) {
        gint64 t0 = g_get_monotonic_time ();
        Mp4Demux *d = mp4_demux_open (fifo, &err);
        CHECK (d == NULL);
        CHECK (err && strstr (err->message, "not a regular file"));
        CHECK (g_get_monotonic_time () - t0 < 2 * G_USEC_PER_SEC);
        g_clear_error (&err);
        if (d)
            mp4_demux_free (d);
    }
    g_free (fifo);
    /* a file that is no MP4: an error, not a crash */
    char *junk = g_build_filename (root, "junk.mp4", NULL);
    char buf[4096];
    memset (buf, 0x41, sizeof buf);
    g_file_set_contents (junk, buf, sizeof buf, NULL);
    CHECK (mp4_demux_open (junk, &err) == NULL && err);
    g_clear_error (&err);
    g_free (junk);

    /* tidy: remove the temp tree */
    char *argv[] = { "rm", "-rf", "--", root, NULL };
    g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (root);
    if (fails) {
        g_printerr ("guard-test: %d check(s) failed\n", fails);
        return 1;
    }
    g_print ("guard-test: all checks passed\n");
    return 0;
}
