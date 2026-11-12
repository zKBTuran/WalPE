/* scene_texlimit_still_test - texture_limit and the texture quality reach
 * a scene in which nothing moves.
 *
 * Both are applied in the scene's third frame (scenenode.c,
 * scenenode_texres_apply: by then the layer textures are stored at the
 * level their size on screen calls for). A scene without a frame clock
 * draws its first frames and then waits for a reason to draw, so its
 * textures stayed as large as without the setting - until a pause, an
 * exposure or anything else happened to bring a third frame.
 *
 * The test draws the way the daemon's render thread does: a frame, and
 * another one for as long as the wallpaper asks for it (next_frame_us).
 * One 1280x720 picture layer on a 640x360 output:
 *  1. no limit, texture quality full: the scene goes idle at once and asks
 *     for nothing (the reference size of the texture);
 *  2. texture_limit 256: idle after the frame that applies it, the texture
 *     within 256 pixels on its longer side, nothing asked for afterwards;
 *  3. texture quality "half": a quarter of the reference size;
 *  4. loaded while paused (under a fullscreen window): no frame is asked
 *     for while the pause lasts, and the limit is applied after the resume.
 * OpenGL is the stand-in of the fuzz harnesses (tests/fuzz/fakegl.c): no
 * display, nothing is drawn; the texture sizes are the scene's own
 * account (`walpe-ctl mem`: layer_textures). */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <epoxy/gl.h>
#include <string.h>
#include "common/quality.h"
#include "engine/content.h"
#include "engine/glutil.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SW 1280
#define SH 720
#define OW 640
#define OH 360

static void put (const char *dir, const char *rel, const void *data, gssize len)
{
    char *p = g_build_filename (dir, rel, NULL), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    if (!g_file_set_contents (p, data, len, NULL))
        g_error ("cannot write %s", p);
    g_free (d);
    g_free (p);
}

static void put32 (GByteArray *a, guint32 v)
{
    v = GUINT32_TO_LE (v);
    g_byte_array_append (a, (const guint8 *) &v, 4);
}

/* a still scene: one layer picture of SW x SH, raw RGBA, no smaller levels */
static void make_scene (const char *dir)
{
    GByteArray *t = g_byte_array_new ();
    g_byte_array_append (t, (const guint8 *) "TEXV0005\0TEXI0001", 18);
    const guint32 head[] = { 0, 0, SW, SH, SW, SH, 0 };
    for (guint i = 0; i < G_N_ELEMENTS (head); i++)
        put32 (t, head[i]);
    g_byte_array_append (t, (const guint8 *) "TEXB0003", 9);
    put32 (t, 1);
    put32 (t, 0xFFFFFFFF);
    put32 (t, 1);
    const guint32 lvl[] = { SW, SH, 0, SW * SH * 4, SW * SH * 4 };
    for (guint i = 0; i < G_N_ELEMENTS (lvl); i++)
        put32 (t, lvl[i]);
    guint at = t->len;
    g_byte_array_set_size (t, at + SW * SH * 4);
    for (guint i = 0; i < SW * SH; i++)
        memcpy (t->data + at + i * 4, "\x28\xa0\x3c\xff", 4);
    put (dir, "materials/big.tex", t->data, t->len);
    g_byte_array_free (t, TRUE);
    put (dir, "materials/big.json",
         "{\"passes\":[{\"blending\":\"translucent\",\"cullmode\":\"nocull\",\"depthtest\":\"disabled\","
         "\"depthwrite\":\"disabled\",\"shader\":\"genericimage2\",\"textures\":[\"big\"]}]}", -1);
    put (dir, "models/big.json", "{\"autosize\":true,\"material\":\"materials/big.json\"}", -1);
    put (dir, "scene.json",
         "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
         "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
         "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":720,\"width\":1280},"
         "\"skylightcolor\":\"0.3 0.3 0.3\"},"
         "\"objects\":[{\"id\":7,\"name\":\"big\",\"image\":\"models/big.json\",\"origin\":\"640 360 0\","
         "\"size\":\"1280 720\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"alpha\":1.0,"
         "\"visible\":true}]}", -1);
    put (dir, "project.json", "{\"file\":\"scene.json\",\"title\":\"big\",\"type\":\"scene\"}", -1);
}

static void rm_rf (const char *path)
{
    GDir *d = g_dir_open (path, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name (d))) {
            char *p = g_build_filename (path, n, NULL);
            if (g_file_test (p, G_FILE_TEST_IS_DIR) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK))
                rm_rf (p);
            else
                g_unlink (p);
            g_free (p);
        }
        g_dir_close (d);
    }
    g_rmdir (path);
}

static void redraw_cb (gpointer u) { (void) u; }

/* the bytes of the layers' file textures, as `walpe-ctl mem` reports them */
static gint64 layer_bytes (WalpeContent *c, gint64 *frames)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    c->mem_stats (c, b);
    json_builder_end_object (b);
    JsonNode *n = json_builder_get_root (b);
    JsonObject *o = json_node_get_object (n);
    gint64 v = -1;
    if (json_object_has_member (o, "layer_textures"))
        v = json_object_get_int_member (json_object_get_object_member (o, "layer_textures"), "bytes");
    if (frames)
        *frames = json_object_has_member (o, "frames_drawn") ? json_object_get_int_member (o, "frames_drawn") : -1;
    json_node_unref (n);
    g_object_unref (b);
    return v;
}

/* one turn of the render thread: the frame, with the fields the daemon
 * sets before each one. TRUE = the wallpaper asks for another frame */
static gboolean frame (WalpeContent *c, int limit)
{
    c->next_frame_us = 0;
    c->render_w = c->render_h = 0;
    c->tex_limit = limit;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, OW, OH);
    c->gl_render (c, OW, OH, 0.0f, 0.0f);
    return c->next_frame_us != 0;
}

/* frames for as long as the wallpaper asks (at most max): how many */
static int until_idle (WalpeContent *c, int limit, int max)
{
    int n = 1;
    while (frame (c, limit) && n < max)
        n++;
    return n;
}

static WalpeContent *load (const char *dir, const WalpeQuality *q)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir, .quality = q };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c) {
        g_printerr ("load: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return NULL;
    }
    c->request_redraw = redraw_cb;
    if (c->gl_init && !c->gl_init (c, &err)) {
        g_printerr ("gl_init: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        c->free (c);
        return NULL;
    }
    return c;
}

static void unload (WalpeContent *c)
{
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
}

int main (void)
{
    g_setenv ("WALPE_SOUND_NULL", "1", TRUE);
    g_unsetenv ("WALPE_WE_ASSETS");
    g_unsetenv ("WALPE_TEX_DOWNSCALE");
    char *tmp = g_dir_make_tmp ("walpe-texlimit-XXXXXX", NULL);
    if (!tmp)
        return 1;
    /* the package cache and friends stay inside the test's directory */
    char *cache = g_build_filename (tmp, "cache", NULL);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_setenv ("XDG_CONFIG_HOME", cache, TRUE);
    char *dir = g_build_filename (tmp, "big", NULL);
    make_scene (dir);
    walpe_glutil_init ();

    /* 1. nothing set: idle at once, the reference size */
    gint64 ref = 0, fr = 0;
    WalpeContent *c = load (dir, NULL);
    CHECK (c != NULL);
    if (!c)
        goto out;
    CHECK (c->mem_stats != NULL);
    int n = until_idle (c, 0, 20);
    ref = layer_bytes (c, &fr);
    printf ("no limit: %d frame(s) asked for, layer textures %" G_GINT64_FORMAT " bytes\n", n, ref);
    CHECK (n < 3);                          /* no frame clock, and none made up */
    CHECK (ref >= (gint64) OW * OH * 4);    /* at least what the output shows */
    CHECK (!frame (c, 0));                  /* an exposure: one frame, nothing more */
    CHECK (layer_bytes (c, NULL) == ref);
    unload (c);

    /* 2. texture_limit 256 */
    c = load (dir, NULL);
    CHECK (c != NULL);
    if (!c)
        goto out;
    n = until_idle (c, 256, 20);
    gint64 lim = layer_bytes (c, &fr);
    printf ("limit 256: %d frame(s) asked for (%" G_GINT64_FORMAT " drawn), layer textures %" G_GINT64_FORMAT
            " bytes\n", n, fr, lim);
    CHECK (n < 20);                         /* it ends */
    CHECK (lim > 0 && lim <= 256 * 144 * 4);   /* 1280x720 within 256: 160x90 */
    CHECK (lim < ref);
    CHECK (!frame (c, 256));                /* applied: nothing more is asked for */
    CHECK (!frame (c, 256));
    CHECK (layer_bytes (c, NULL) == lim);
    unload (c);

    /* 3. texture quality "half" */
    WalpeQuality q;
    walpe_quality_preset (WALPE_PRESET_HIGH, &q);
    q.tex_res = WALPE_TEXRES_HALF;
    c = load (dir, &q);
    CHECK (c != NULL);
    if (!c)
        goto out;
    n = until_idle (c, 0, 20);
    gint64 half = layer_bytes (c, NULL);
    printf ("quality half: %d frame(s) asked for, layer textures %" G_GINT64_FORMAT " bytes\n", n, half);
    CHECK (n < 20);
    CHECK (half == ref / 4);
    CHECK (!frame (c, 0));
    unload (c);

    /* 4. loaded under a pause: no frame clock while it lasts */
    c = load (dir, NULL);
    CHECK (c != NULL);
    if (!c)
        goto out;
    if (c->set_paused)
        c->set_paused (c, TRUE);
    CHECK (!frame (c, 256));                /* the first frame of a load is drawn all the same */
    CHECK (!frame (c, 256));                /* (an exposure) */
    if (c->set_paused)
        c->set_paused (c, FALSE);
    n = until_idle (c, 256, 20);
    CHECK (n < 20);
    CHECK (layer_bytes (c, NULL) == lim);
    CHECK (!frame (c, 256));
    unload (c);

    if (!fails)
        printf ("scene texture limit, still scene: OK\n");
out:
    rm_rf (tmp);
    g_free (dir);
    g_free (cache);
    g_free (tmp);
    return fails ? 1 : 0;
}
