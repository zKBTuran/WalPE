/* scene_feat_test.c - camera parallax and sprite-sheet layers on generated
 * scenes (EGL surfaceless; solid colour layers and a sprite sheet written
 * by the test itself, no wallpaper files).
 *
 * Camera parallax (320 x 180 scene shown 1:1):
 *   - pointer in the middle: a layer in the middle with a depth stands
 *     still; pointer at the right edge / the top: it moves left / down by
 *     depth x amount x influence x half the scene;
 *   - a layer without "parallaxDepth" does not move; a child of the moved
 *     layer moves with it, whatever depth it carries;
 *   - at rest (pointer in the middle) no layer is displaced: the frame is
 *     the one of the same scene without camera parallax, byte for byte;
 *     parallax_offset=on (off by default; live) adds the offset term,
 *     which displaces a layer off the centre while the pointer rests;
 *   - camera_parallax=off (read when the scene loads): nothing moves;
 *   - a scene without camera parallax draws the same frame wherever the
 *     pointer is, and is not drawn again for it.
 * The scene's zoom: general.zoom 1e39 (a number, but none a float holds)
 * is no zoom - the frame of the same scene without the key, not a black
 * one.
 * Sprite sheets (a 64 x 32 sheet of four 32 x 16 frames: red, green, blue,
 * white, 2 s each - long, so that a slow first frame cannot use one up):
 *   - the layer shows the first frame over all of its quad, not the sheet
 *     squeezed into it; with an effect on the layer the effect sees the
 *     frame (half the colour);
 *   - a frame that is not due is not drawn again; after the frame's time
 *     the picture moved on by itself, also behind the effect;
 *   - a script sets the frame and pauses (only with QuickJS);
 *   - a sheet whose third frame lies outside the picture is refused with a
 *     line in the log and shows its first frame as a still; one whose
 *     first frame lies outside shows the picture as a whole;
 *   - sprite_sheets=off: the sheet as a whole, as before;
 *   - eighteen layers on one sheet with the largest table (65536 frames):
 *     sixteen play, the scene's total is reached and the other two are
 *     stills, said once in the log.
 *
 * Skipped (exit 0, "skip") without EGL surfaceless / GL 3.3.
 * WALPE_FEATURES is cleared by meson.build. */
#include "walpe-build-config.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/content.h"
#include "engine/features.h"
#include "engine/glutil.h"
#include "engine/scenenode_effects.h"

static int checks, fails;
static GString *logged;

static void result (gboolean ok, const char *what)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static void log_cb (const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer u)
{
    (void) dom; (void) lvl; (void) u;
    g_string_append (logged, msg);
    g_string_append_c (logged, '\n');
}

static void put_bytes (const char *dir, const char *rel, const void *data, gssize len)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, data, len, NULL);
    g_free (d);
    g_free (p);
}

static void put (const char *dir, const char *rel, const char *text)
{
    put_bytes (dir, rel, text, -1);
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

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

static void putf (GByteArray *a, float f)
{
    guint32 u;
    memcpy (&u, &f, 4);
    put32 (a, u);
}

/* A sprite sheet container: 64 x 32 raw RGBA, four 32 x 16 quarters in red,
 * green, blue and white, and a frame table of four frames of 2 s.
 * bad < 0: the quarters in that order; else frame `bad` lies far outside
 * the picture (numbers whose sum wraps around as integers). */
static void write_sheet (const char *dir, const char *rel, int bad)
{
    static const guint32 col[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };   /* R G B A bytes */
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    const guint32 hdr[7] = { 0, 4, 64, 32, 64, 32, 0 };    /* RGBA8888, animated */
    for (int i = 0; i < 7; i++)
        put32 (a, hdr[i]);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1); put32 (a, 0); put32 (a, 1);              /* images, format, mips */
    put32 (a, 64); put32 (a, 32); put32 (a, 0); put32 (a, 64 * 32 * 4); put32 (a, 64 * 32 * 4);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++)
            put32 (a, col[(y / 16) * 2 + x / 32]);
    g_byte_array_append (a, (const guint8 *) "TEXS0003\0", 9);
    put32 (a, 4); put32 (a, 32); put32 (a, 16);
    for (int f = 0; f < 4; f++) {
        put32 (a, 0);                                       /* image */
        putf (a, 2.0f);                                     /* duration */
        if (f == bad) {
            putf (a, 2147483520.0f); putf (a, 0.0f); putf (a, 2147483520.0f);
            putf (a, 0.0f); putf (a, 0.0f); putf (a, 16.0f);
        } else {
            putf (a, (float) (f % 2 * 32)); putf (a, (float) (f / 2 * 16));   /* x, y */
            putf (a, 32.0f); putf (a, 0.0f); putf (a, 0.0f); putf (a, 16.0f); /* x axis, y axis */
        }
    }
    put_bytes (dir, rel, a->data, (gssize) a->len);
    g_byte_array_unref (a);
}

static void effect (const char *dir, const char *name, const char *body)
{
    char *rel = g_strdup_printf ("shaders/effects/%s.vert", name);
    put (dir, rel,
         "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
         "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
         "void main() {\n    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
         "\tv_TexCoord = a_TexCoord;\n}\n");
    g_free (rel);
    rel = g_strdup_printf ("shaders/effects/%s.frag", name);
    char *fs = g_strdup_printf (
        "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n%s\tgl_FragColor = c;\n}\n",
        body);
    put (dir, rel, fs);
    g_free (fs);
    g_free (rel);
    rel = g_strdup_printf ("materials/effects/%s.json", name);
    char *js = g_strdup_printf (
        "{\"passes\":[{\"shader\":\"effects/%s\",\"blending\":\"normal\",\"depthtest\":\"disabled\","
        "\"depthwrite\":\"disabled\",\"cullmode\":\"nocull\"}]}", name);
    put (dir, rel, js);
    g_free (js);
    g_free (rel);
    rel = g_strdup_printf ("effects/%s/effect.json", name);
    js = g_strdup_printf (
        "{\"name\":\"%s\",\"group\":\"t\",\"passes\":[{\"material\":\"materials/effects/%s.json\"}]}",
        name, name);
    put (dir, rel, js);
    g_free (js);
    g_free (rel);
}

#define SCENE_HEAD(general) \
    "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"}," \
    "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false," general \
    "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":180,\"width\":320}," \
    "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[" \
    "{\"id\":1,\"name\":\"bg\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\"," \
    "\"size\":\"320 180\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0.2 0.4 0.6\"," \
    "\"alpha\":1.0,\"visible\":true}"

#define PROJECT \
    "{\"file\":\"scene.json\",\"title\":\"feature test\",\"type\":\"scene\"," \
    "\"general\":{\"properties\":{}}}"

/* a 20 x 20 solid layer; extra = further members (",\"key\":value...") */
static void solid (GString *s, int id, const char *origin, const char *color, const char *extra)
{
    g_string_append_printf (s,
        ",{\"id\":%d,\"name\":\"l%d\",\"image\":\"models/solid.json\",\"origin\":\"%s\","
        "\"size\":\"20 20\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"%s\",\"alpha\":1.0,"
        "\"visible\":true%s}", id, id, origin, color, extra ? extra : "");
}

/* Parallax scene. Layers (colour, where, depth):
 *   2 red     160  90  depth 0.25 0.25
 *   3 green   160 150  no parallaxDepth
 *   4 blue    child of 2 at +0 -40 (160 50), depth 1 1 of its own
 *   5 white    60  30  depth 0.25 0 (off the centre: the offset term) */
static char *make_parallax_scene (gboolean on)
{
    char *dir = g_dir_make_tmp ("walpe-feat-par-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    GString *s = g_string_new (on
        ? SCENE_HEAD ("\"cameraparallax\":true,\"cameraparallaxamount\":1.0,"
                      "\"cameraparallaxdelay\":0.0,\"cameraparallaxmouseinfluence\":1.0,")
        : SCENE_HEAD ("\"cameraparallax\":false,\"cameraparallaxamount\":1.0,"
                      "\"cameraparallaxdelay\":0.0,\"cameraparallaxmouseinfluence\":1.0,"));
    solid (s, 2, "160 90 0", "1 0 0", ",\"parallaxDepth\":\"0.25 0.25\"");
    solid (s, 3, "160 150 0", "0 1 0", NULL);
    solid (s, 4, "0 -40 0", "0 0 1", ",\"parent\":2,\"parallaxDepth\":\"1 1\"");
    solid (s, 5, "60 30 0", "1 1 1", ",\"parallaxDepth\":\"0.25 0\"");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json", PROJECT);
    return dir;
}

/* Sprite scene: the sheet on a 64 x 32 layer at 80 90 (plain) and on one at
 * 240 90 with the effect "gain" (rgb * 0.5). script: the plain layer's
 * "alpha" carries a script that shows frame 3 and pauses. */
static char *make_sprite_scene (int bad, gboolean script)
{
    char *dir = g_dir_make_tmp ("walpe-feat-spr-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "models/anim.json", "{\"autosize\":true,\"material\":\"materials/anim.json\"}");
    put (dir, "materials/anim.json",
         "{\"passes\":[{\"blending\":\"translucent\",\"cullmode\":\"nocull\",\"depthtest\":\"disabled\","
         "\"depthwrite\":\"disabled\",\"shader\":\"genericimage2\",\"textures\":[\"sheet\"]}]}");
    write_sheet (dir, "materials/sheet.tex", bad);
    effect (dir, "gain", "\tc.rgb *= 0.5;\n");
    GString *s = g_string_new (SCENE_HEAD ("\"cameraparallax\":false,"));
    g_string_append_printf (s,
        ",{\"id\":2,\"name\":\"plain\",\"image\":\"models/anim.json\",\"origin\":\"80 90 0\","
        "\"size\":\"64 32\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"alpha\":%s,"
        "\"visible\":true}",
        script ? "{\"script\":\"'use strict';\\nexport function update(value) {\\n"
                 "  const a = thisLayer.getTextureAnimation();\\n  a.setFrame(3);\\n  a.pause();\\n"
                 "  return 1;\\n}\\n\",\"value\":1.0}"
               : "1.0");
    g_string_append (s,
        ",{\"id\":3,\"name\":\"fx\",\"image\":\"models/anim.json\",\"origin\":\"240 90 0\","
        "\"size\":\"64 32\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"alpha\":1.0,"
        "\"visible\":true,\"effects\":[{\"file\":\"effects/gain/effect.json\",\"id\":103,"
        "\"name\":\"gain\",\"visible\":true,\"passes\":[{}]}]}");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json", PROJECT);
    return dir;
}

/* A sheet with the largest frame table: 64 x 32 raw RGBA in one colour and
 * 65536 frames of 8 x 8 texels, 2 s each. */
static void write_full_table_sheet (const char *dir, const char *rel)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    const guint32 hdr[7] = { 0, 4, 64, 32, 64, 32, 0 };    /* RGBA8888, animated */
    for (int i = 0; i < 7; i++)
        put32 (a, hdr[i]);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1); put32 (a, 0); put32 (a, 1);              /* images, format, mips */
    put32 (a, 64); put32 (a, 32); put32 (a, 0); put32 (a, 64 * 32 * 4); put32 (a, 64 * 32 * 4);
    for (int i = 0; i < 64 * 32; i++)
        put32 (a, 0xff0000ffu);
    g_byte_array_append (a, (const guint8 *) "TEXS0003\0", 9);
    put32 (a, 65536); put32 (a, 8); put32 (a, 8);
    for (int f = 0; f < 65536; f++) {
        put32 (a, 0);                                       /* image */
        putf (a, 2.0f);                                     /* duration */
        putf (a, (float) (f % 8 * 8)); putf (a, (float) (f / 8 % 4 * 8));   /* x, y */
        putf (a, 8.0f); putf (a, 0.0f); putf (a, 0.0f); putf (a, 8.0f);     /* x axis, y axis */
    }
    put_bytes (dir, rel, a->data, (gssize) a->len);
    g_byte_array_unref (a);
}

/* `layers` image layers that all show the sheet with the largest table */
static char *make_full_table_scene (int layers)
{
    char *dir = g_dir_make_tmp ("walpe-feat-tab-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "models/anim.json", "{\"autosize\":true,\"material\":\"materials/anim.json\"}");
    put (dir, "materials/anim.json",
         "{\"passes\":[{\"blending\":\"translucent\",\"cullmode\":\"nocull\",\"depthtest\":\"disabled\","
         "\"depthwrite\":\"disabled\",\"shader\":\"genericimage2\",\"textures\":[\"sheet\"]}]}");
    write_full_table_sheet (dir, "materials/sheet.tex");
    GString *s = g_string_new (SCENE_HEAD ("\"cameraparallax\":false,"));
    for (int i = 0; i < layers; i++)
        g_string_append_printf (s,
            ",{\"id\":%d,\"name\":\"t%d\",\"image\":\"models/anim.json\",\"origin\":\"%d 90 0\","
            "\"size\":\"8 8\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"alpha\":1.0,"
            "\"visible\":true}", 2 + i, i, 10 + i * 16);
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json", PROJECT);
    return dir;
}

static int count_in_log (const char *what)
{
    int n = 0;
    for (const char *p = logged->str; (p = strstr (p, what)) != NULL; p++)
        n++;
    return n;
}

static void redraw_cb (gpointer u) { (void) u; }

#define W 320
#define H 180

typedef struct {
    WalpeContent *c;
    guint8 *px;
} Run;

static gboolean run_open (Run *r, const char *dir)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    r->px = g_malloc0 ((gsize) W * H * 4);
    r->c = walpe_content_new (dir, &opts, &err);
    if (!r->c) {
        printf ("load failed: %s\n", err ? err->message : "?");
        return FALSE;
    }
    r->c->request_redraw = redraw_cb;
    if (!r->c->gl_init (r->c, &err)) {
        printf ("gl_init failed: %s\n", err ? err->message : "?");
        return FALSE;
    }
    return TRUE;
}

/* one frame tick with the pointer at px, py (-1..1, y down); force = draw
 * it whatever changed. TRUE when a frame was drawn (px then holds it) */
static gboolean run_frame (Run *r, float px, float py, gboolean force)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, W, H);
    r->c->next_frame_us = 0;
    r->c->surface_lost = force;
    r->c->frame_unchanged = FALSE;
    r->c->gl_render (r->c, W, H, px, py);
    glFinish ();
    if (r->c->frame_unchanged)
        return FALSE;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glReadPixels (0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, r->px);
    return TRUE;
}

static void run_close (Run *r)
{
    if (r->c) {
        if (r->c->gl_free)
            r->c->gl_free (r->c);
        r->c->free (r->c);
    }
    g_free (r->px);
    memset (r, 0, sizeof *r);
}

/* the pixel at scene point x, y (the scene is shown 1:1, y up; the rows of
 * a read-back start at the bottom) */
static const guint8 *at (const Run *r, int x, int y)
{
    return r->px + ((gsize) y * W + x) * 4;
}

static gboolean is (const guint8 *p, int r, int g, int b)
{
    return abs (p[0] - r) <= 3 && abs (p[1] - g) <= 3 && abs (p[2] - b) <= 3;
}

#define BG    51, 102, 153
#define RED   255, 0, 0
#define GREEN 0, 255, 0
#define BLUE  0, 0, 255
#define WHITE 255, 255, 255

static void parallax_checks (void)
{
    char *dir = make_parallax_scene (TRUE);
    Run r = { 0 };
    if (!dir || !run_open (&r, dir)) {
        result (FALSE, "parallax scene loads");
        return;
    }
    result (r.c->uses_pointer_pos, "a scene with camera parallax asks for the pointer position");
    /* pointer in the middle, the switches as they come: nothing is
     * displaced, also not layer 5, which stands 100 left and 60 below the
     * centre with depth 0.25 0 (the offset term is off by default) */
    run_frame (&r, 0.0f, 0.0f, TRUE);
    result (is (at (&r, 160, 90), RED) && is (at (&r, 160, 150), GREEN) && is (at (&r, 160, 50), BLUE),
            "pointer in the middle: the layers in the middle column stand where the file puts them");
    result (is (at (&r, 60, 30), WHITE) && is (at (&r, 35, 30), BG),
            "... and so does a layer off the centre: at rest nothing is displaced");
    guint8 *rest = g_memdup2 (r.px, (gsize) W * H * 4);   /* compared with the plain scene below */
    result (r.c->next_frame_us == 0, "at rest the scene asks for no frame");
    result (!run_frame (&r, 0.0f, 0.0f, FALSE), "... and the same pointer draws no frame");
    /* pointer at the right edge: against it, 0.25 * 1 * 1 * 160 = 40 left */
    gboolean drawn = run_frame (&r, 1.0f, 0.0f, FALSE);
    result (drawn && is (at (&r, 120, 90), RED) && is (at (&r, 160, 90), BG),
            "pointer at the right edge: the layer moved left by its depth's share");
    result (is (at (&r, 160, 150), GREEN), "a layer without parallaxDepth does not move");
    result (is (at (&r, 120, 50), BLUE) && is (at (&r, 160, 50), BG),
            "a child moves with its parent, not by the depth it carries itself");
    result (is (at (&r, 20, 30), WHITE) && is (at (&r, 60, 30), BG),
            "a layer off the centre moves by its depth's share too (60 - 40 = 20)");
    /* pointer at the top (y = -1): 0.25 * 90 = 22.5 down; x back */
    run_frame (&r, 0.0f, -1.0f, FALSE);
    result (is (at (&r, 160, 68), RED) && is (at (&r, 160, 60), RED) && is (at (&r, 160, 80), BG),
            "pointer at the top: the layer moved down");
    result (is (at (&r, 60, 30), WHITE), "a layer of depth 0 on an axis keeps that axis");
    /* the switch of the offset term, while the scene runs: layer 5, 100
     * left of the centre with depth 0.25, is put 25 further left while
     * the pointer rests in the middle; a layer in the middle is not moved */
    walpe_feature_set ("parallax_offset", TRUE);
    drawn = run_frame (&r, 0.0f, 0.0f, FALSE);
    result (drawn && is (at (&r, 35, 30), WHITE) && is (at (&r, 60, 30), BG) && is (at (&r, 160, 90), RED),
            "parallax_offset=on: the offset term displaces a layer off the centre while the pointer "
            "rests, the frame is drawn unasked");
    walpe_feature_set ("parallax_offset", FALSE);
    drawn = run_frame (&r, 0.0f, 0.0f, FALSE);
    result (drawn && !memcmp (rest, r.px, (gsize) W * H * 4),
            "... and off again the frame at rest is the first one, byte for byte");
    run_close (&r);

    /* camera_parallax=off is read when a scene loads */
    walpe_feature_set ("camera_parallax", FALSE);
    if (run_open (&r, dir)) {
        run_frame (&r, 1.0f, -1.0f, TRUE);
        result (!r.c->uses_pointer_pos && is (at (&r, 160, 90), RED) && is (at (&r, 60, 30), WHITE) &&
                is (at (&r, 160, 50), BLUE),
                "camera_parallax=off: nothing moves and the pointer is not asked for");
    } else {
        result (FALSE, "parallax scene loads with the feature off");
    }
    run_close (&r);
    walpe_feature_set ("camera_parallax", TRUE);
    rm_rf (dir);
    g_free (dir);

    /* the same scene without camera parallax: the pointer changes nothing */
    dir = make_parallax_scene (FALSE);
    if (dir && run_open (&r, dir)) {
        run_frame (&r, 0.0f, 0.0f, TRUE);
        guint8 *first = g_memdup2 (r.px, (gsize) W * H * 4);
        result (!r.c->uses_pointer_pos && !run_frame (&r, 1.0f, -1.0f, FALSE),
                "a scene without camera parallax is not drawn again for the pointer");
        run_frame (&r, 1.0f, -1.0f, TRUE);
        result (!memcmp (first, r.px, (gsize) W * H * 4) && is (at (&r, 60, 30), WHITE),
                "... and draws the same frame, byte for byte, wherever the pointer is");
        result (rest && !memcmp (first, rest, (gsize) W * H * 4),
                "the scene with camera parallax at rest drew that same frame (nothing displaced)");
        g_free (first);
    } else {
        result (FALSE, "scene without camera parallax loads");
    }
    run_close (&r);
    if (dir)
        rm_rf (dir);
    g_free (dir);
    g_free (rest);
}

/* frames until one is not drawn any more (textures settle in the first) */
static void settle (Run *r)
{
    run_frame (r, 0.0f, 0.0f, TRUE);
    for (int i = 0; i < 8 && run_frame (r, 0.0f, 0.0f, FALSE); i++)
        ;
}

/* a red 20 x 20 layer in the middle of the canvas; zoom: general.zoom as it
 * stands in the file */
static char *make_zoom_scene (gboolean zoom)
{
    char *dir = g_dir_make_tmp ("walpe-feat-zoom-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    GString *s = g_string_new (zoom ? SCENE_HEAD ("\"cameraparallax\":false,\"zoom\":1e39,")
                                    : SCENE_HEAD ("\"cameraparallax\":false,"));
    solid (s, 2, "160 90 0", "1 0 0", NULL);
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json", PROJECT);
    return dir;
}

static void zoom_checks (void)
{
    Run a = { 0 }, b = { 0 };
    char *plain = make_zoom_scene (FALSE), *huge = make_zoom_scene (TRUE);
    if (plain && huge && run_open (&a, plain) && run_open (&b, huge)) {
        run_frame (&a, 0.0f, 0.0f, TRUE);
        run_frame (&b, 0.0f, 0.0f, TRUE);
        result (is (at (&a, 160, 90), 255, 0, 0) && is (at (&a, 40, 40), 51, 102, 153),
                "a scene without general.zoom shows its layers");
        result (!memcmp (a.px, b.px, (gsize) W * H * 4),
                "general.zoom 1e39 (no float holds it) is no zoom: the same frame, not a black one");
    } else {
        result (FALSE, "scenes with and without general.zoom load");
    }
    run_close (&a);
    run_close (&b);
    if (plain)
        rm_rf (plain);
    if (huge)
        rm_rf (huge);
    g_free (plain);
    g_free (huge);
}

static void sprite_checks (void)
{
    char *dir = make_sprite_scene (-1, FALSE);
    Run r = { 0 };
    if (!dir || !run_open (&r, dir)) {
        result (FALSE, "sprite scene loads");
        return;
    }
    settle (&r);
    printf ("   plain layer: %d %d %d / %d %d %d, with effect: %d %d %d\n",
            at (&r, 64, 98)[0], at (&r, 64, 98)[1], at (&r, 64, 98)[2],
            at (&r, 96, 82)[0], at (&r, 96, 82)[1], at (&r, 96, 82)[2],
            at (&r, 240, 90)[0], at (&r, 240, 90)[1], at (&r, 240, 90)[2]);
    result (is (at (&r, 64, 98), RED) && is (at (&r, 96, 98), RED) && is (at (&r, 64, 82), RED) &&
            is (at (&r, 96, 82), RED),
            "the layer shows its first frame over all of its quad");
    result (is (at (&r, 224, 98), 128, 0, 0) && is (at (&r, 256, 82), 128, 0, 0),
            "an effect on the layer works on the frame, not on the sheet");
    result (strstr (logged->str, "plays a sprite sheet: 4 frames of 32x16") != NULL,
            "the log says what plays");
    result (r.c->next_frame_us > 0, "the scene asks for the frame in which the picture changes");
    result (!run_frame (&r, 0.0f, 0.0f, FALSE), "a frame that is not due is not drawn again");
    g_usleep (2300000);   /* the first frame's 2 s are over: frame 1, or 2 on a slow machine */
    gboolean drawn = run_frame (&r, 0.0f, 0.0f, FALSE);
    gboolean g1 = is (at (&r, 80, 90), GREEN), b1 = is (at (&r, 80, 90), BLUE);
    result (drawn && (g1 || b1), "after the frame's time the next frame is on, drawn unasked");
    result (g1 ? is (at (&r, 240, 90), 0, 128, 0) : is (at (&r, 240, 90), 0, 0, 128),
            "... also behind the effect (its kept result was redone)");
    run_close (&r);

    /* sprite_sheets=off (read when a scene loads): the sheet as a whole */
    walpe_feature_set ("sprite_sheets", FALSE);
    if (run_open (&r, dir)) {
        settle (&r);
        result (is (at (&r, 64, 98), RED) && is (at (&r, 96, 98), GREEN) && is (at (&r, 64, 82), BLUE) &&
                is (at (&r, 96, 82), WHITE),
                "sprite_sheets=off: the whole sheet in the layer, as before");
        result (r.c->next_frame_us == 0, "... and no frame is asked for");
    } else {
        result (FALSE, "sprite scene loads with the feature off");
    }
    run_close (&r);
    walpe_feature_set ("sprite_sheets", TRUE);
    rm_rf (dir);
    g_free (dir);

#ifdef HAVE_QUICKJS
    /* a script shows frame 3 and pauses */
    dir = make_sprite_scene (-1, TRUE);
    if (dir && run_open (&r, dir)) {
        for (int i = 0; i < 4; i++)
            run_frame (&r, 0.0f, 0.0f, TRUE);
        result (is (at (&r, 64, 98), WHITE) && is (at (&r, 96, 82), WHITE),
                "a script sets the frame of the layer's animation");
        g_usleep (2300000);
        run_frame (&r, 0.0f, 0.0f, TRUE);
        result (is (at (&r, 80, 90), WHITE), "... and a paused animation stays on it");
        /* the other layer's animation is its own: it went on */
        result (!is (at (&r, 240, 90), 128, 0, 0), "the layer the script did not name plays on");
    } else {
        result (FALSE, "sprite scene with a script loads");
    }
    run_close (&r);
    if (dir)
        rm_rf (dir);
    g_free (dir);
    /* script_texanim=off: the script's calls do nothing, the animation runs */
    walpe_feature_set ("script_texanim", FALSE);
    dir = make_sprite_scene (-1, TRUE);
    if (dir && run_open (&r, dir)) {
        for (int i = 0; i < 4; i++)
            run_frame (&r, 0.0f, 0.0f, TRUE);
        result (is (at (&r, 64, 98), RED), "script_texanim=off: the script does not reach the animation");
    } else {
        result (FALSE, "sprite scene with a script loads (script_texanim off)");
    }
    run_close (&r);
    walpe_feature_set ("script_texanim", TRUE);
    if (dir)
        rm_rf (dir);
    g_free (dir);
#else
    printf ("note: built without QuickJS, script control not tested\n");
#endif

    /* a table whose third frame lies outside the picture: refused, the
     * first frame as a still */
    dir = make_sprite_scene (2, FALSE);
    g_string_truncate (logged, 0);
    if (dir && run_open (&r, dir)) {
        settle (&r);
        result (strstr (logged->str, "refused") != NULL && strstr (logged->str, "frame 2") != NULL,
                "a frame outside the picture: the table is refused with a line in the log");
        result (is (at (&r, 64, 98), RED) && is (at (&r, 96, 82), RED) && is (at (&r, 240, 90), 128, 0, 0),
                "... and the layer shows its first frame");
        g_usleep (100000);
        result (!run_frame (&r, 0.0f, 0.0f, FALSE) && r.c->next_frame_us == 0,
                "... as a still: no frame is drawn or asked for later");
    } else {
        result (FALSE, "scene with a malformed sheet loads");
    }
    run_close (&r);
    if (dir)
        rm_rf (dir);
    g_free (dir);

    /* the first frame outside: nothing of the table is used */
    dir = make_sprite_scene (0, FALSE);
    g_string_truncate (logged, 0);
    if (dir && run_open (&r, dir)) {
        settle (&r);
        result (strstr (logged->str, "refused") != NULL && strstr (logged->str, "as a whole") != NULL,
                "the first frame outside the picture: refused, said in the log");
        result (is (at (&r, 64, 98), RED) && is (at (&r, 96, 98), GREEN) && is (at (&r, 96, 82), WHITE),
                "... and the picture is shown as a whole");
    } else {
        result (FALSE, "scene with a sheet whose first frame is malformed loads");
    }
    run_close (&r);
    if (dir)
        rm_rf (dir);
    g_free (dir);

    /* the frames of all animations of a scene together: every layer holds
     * the table it plays from, sixteen of the largest size are taken */
    dir = make_full_table_scene (18);
    g_string_truncate (logged, 0);
    if (dir && run_open (&r, dir)) {
        result (count_in_log ("plays a sprite sheet: 65536 frames") == 16,
                "18 layers on a sheet of 65536 frames: 16 play (the scene's total of frames)");
        result (count_in_log ("frames together") == 1 && strstr (logged->str, "as a still") != NULL,
                "... the others show their first frame as a still, said once in the log");
        result (run_frame (&r, 0.0f, 0.0f, TRUE) && is (at (&r, 10, 90), RED) &&
                is (at (&r, 10 + 17 * 16, 90), RED),
                "... and both kinds are drawn");
    } else {
        result (FALSE, "scene with 18 layers on a sheet of 65536 frames loads");
    }
    run_close (&r);
    if (dir)
        rm_rf (dir);
    g_free (dir);
}

int main (void)
{
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skip (no EGL surfaceless platform)\n");
        return 0;
    }
    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[128], cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, cfgs, 128, &nc) || nc < 1) {
        printf ("skip (no usable GL config)\n");
        return 0;
    }
    cfg = cfgs[0];
    for (int i = 0; i < nc; i++) {   /* an 8-bit window, like a desktop's */
        EGLint rs = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &rs);
        if (rs == 8) {
            cfg = cfgs[i];
            break;
        }
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        printf ("skip (no GL 3.3 context with a pbuffer)\n");
        return 0;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    logged = g_string_new (NULL);
    g_log_set_default_handler (log_cb, NULL);
    walpe_glutil_init ();
    fx_set_cache_enabled (TRUE);
    g_unsetenv ("WALPE_BAKE");
    g_unsetenv ("WALPE_FRAME_SKIP");
    g_unsetenv ("WALPE_RENDER_SCALE");
    g_unsetenv ("WALPE_PROP_PROBE");
    g_unsetenv ("WALPE_FX_TIME");
    g_unsetenv ("WALPE_NOFX");
    parallax_checks ();
    zoom_checks ();
    sprite_checks ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
