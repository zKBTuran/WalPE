/* scene_fit_test.c - layer textures as the input of effects, and layers
 * attached to a puppet (scenenode.c, scenenode_effects.c, puppet.c).
 *
 * Generated scenes (textures, shaders and a puppet model are written
 * here) on Mesa's surfaceless platform, a pbuffer as the window:
 *  1. A layer shown at half its texture's size has the texture stored at
 *     half size as its effects' input - but only where that gives the same
 *     picture. A first effect that displaces its lookups over the whole
 *     layer (or under a mask painted over half of it) must read the
 *     full-size texture: with a texture of alternating black and white
 *     columns and a lookup moved by half a texel the full texture gives
 *     white, the averaged half-size copy grey. Under a mask painted over
 *     5 % of the layer, and for an effect that reads the texture as it is,
 *     the texture is still stored at half size (the memory saving). So is
 *     a picture without fine detail under the same displaced read (a
 *     gradient): what a reduced texture would lose is measured first.
 *  2. With tex_downscale off (preset Maximum) the texture stays at file
 *     size, so its effects may not work below half of it: a layer shown at
 *     a quarter of its texture read it 4:1 with plain bilinear sampling
 *     and lost three of every four columns (black instead of the average).
 *  3. A child of a puppet layer that names one of the puppet's attachment
 *     points sits at that point, not at the puppet layer's centre; one
 *     that names a point the puppet does not have stays relative to the
 *     layer.
 *  4. Objects nested in another one's "children" are drawn after it (they
 *     were drawn before it, i.e. under it).
 *  5. A text layer that is hidden holds no texture; shown later (here by
 *     an "Adjust layers" override) it is rendered in that frame.
 *  6. An old-format scene's layer list is freed (seen by LeakSanitizer).
 *     Its layers are where their "translation" says, in scene pixels from
 *     the scene's top left corner: also the layers of a group, a layer
 *     turned about its pivot, one moved by parallax, on an output of
 *     another shape than the scene and under a camera zoom.
 *  7. What the effect code keeps per render thread is freed when the
 *     thread ends (seen by a build with LeakSanitizer: the checks run on a
 *     thread of their own).
 * Skipped (exit 0, "skip") without EGL surfaceless / GL 3.3.
 * `meson test -C build scene-fit` */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/config.h"
#include "common/layeradj.h"
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/scenenode_effects.h"

static int checks, fails;

static void result (gboolean ok, const char *what)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static void put (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

static void put_bytes (const char *dir, const char *rel, GByteArray *a)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, (const char *) a->data, a->len, NULL);
    g_free (d);
    g_free (p);
    g_byte_array_unref (a);
}

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}
static void put16 (GByteArray *a, guint16 v)
{
    guint8 b[2] = { (guint8) v, (guint8) (v >> 8) };
    g_byte_array_append (a, b, 2);
}
static void putf (GByteArray *a, float f)
{
    guint32 u;
    memcpy (&u, &f, 4);
    put32 (a, u);
}
static void puts0 (GByteArray *a, const char *s)
{
    g_byte_array_append (a, (const guint8 *) s, (guint) strlen (s) + 1);
}

/* a TEXB0003 container with one raw RGBA8 level; value (x, y) = the red
 * and blue byte, green is 40 (not grey: stored as RGBA8), alpha 255 */
typedef guint8 (*ValueFn) (int x, int y);

static void write_tex (const char *dir, const char *rel, int w, int h, ValueFn fn)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    put32 (a, 0);
    put32 (a, 2);
    put32 (a, (guint32) w);
    put32 (a, (guint32) h);
    put32 (a, (guint32) w);
    put32 (a, (guint32) h);
    put32 (a, 0);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1);
    put32 (a, 0xffffffffu);
    put32 (a, 1);
    put32 (a, (guint32) w);
    put32 (a, (guint32) h);
    put32 (a, 0);
    put32 (a, (guint32) (w * h * 4));
    put32 (a, (guint32) (w * h * 4));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guint8 v = fn (x, y);
            const guint8 px[4] = { v, 40, v, 255 };
            g_byte_array_append (a, px, 4);
        }
    put_bytes (dir, rel, a);
}

static guint8 v_columns (int x, int y) { (void) y; return (x & 1) ? 255 : 0; }
static guint8 v_every4th (int x, int y) { (void) y; return (x & 3) ? 0 : 255; }
static guint8 v_dark (int x, int y) { (void) x; (void) y; return 20; }
static guint8 v_smooth (int x, int y) { return (guint8) ((x + y) / 2); }   /* half a step per texel */
static guint8 v_mask_small (int x, int y) { return x < 14 && y < 14 ? 255 : 0; }   /* 4.8 % of 64 x 64 */
static guint8 v_mask_half (int x, int y) { (void) y; return x < 32 ? 255 : 0; }

/* a puppet: a 100 x 100 quad on two bones (root, arm) in their bind pose,
 * one animation that is never played, and the attachment point "hand" on
 * the arm, 60 to the right of and 40 above the layer's centre */
static void write_puppet (const char *dir, const char *rel)
{
    GByteArray *b = g_byte_array_new ();
    g_byte_array_append (b, (const guint8 *) "MDLV0019\0", 9);
    put32 (b, 0); put32 (b, 1); put32 (b, 1);
    puts0 (b, "materials/pup.json");
    put32 (b, 0);
    put32 (b, 0x0180000F);
    put32 (b, 4 * 80);
    static const float xs[4] = { -50, 50, 50, -50 }, ys[4] = { -50, -50, 50, 50 };
    static const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 1, 1, 0, 0 };
    for (int i = 0; i < 4; i++) {
        const float pnt[10] = { xs[i], ys[i], 0, 0, 0, 1, 1, 0, 0, 1 };
        for (int k = 0; k < 10; k++)
            putf (b, pnt[k]);
        put32 (b, 1); put32 (b, 0); put32 (b, 0); put32 (b, 0);
        putf (b, 1); putf (b, 0); putf (b, 0); putf (b, 0);
        putf (b, us[i]); putf (b, vs[i]);
    }
    put32 (b, 12);
    static const guint16 idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        put16 (b, idx[i]);
    gsize ms = b->len;
    GByteArray *body = g_byte_array_new ();
    put16 (body, 2); put16 (body, 0);
    for (int i = 0; i < 2; i++) {
        puts0 (body, i ? "arm" : "root");
        put32 (body, 0);
        put32 (body, (guint32) (i - 1));
        put32 (body, 64);
        for (int k = 0; k < 16; k++)
            putf (body, k % 5 == 0 ? 1.0f : 0.0f);
        puts0 (body, "{}");
    }
    g_byte_array_append (b, (const guint8 *) "MDLS0002\0", 9);
    put32 (b, (guint32) (ms + 9 + 4 + body->len));
    g_byte_array_append (b, body->data, body->len);
    g_byte_array_unref (body);
    /* attachment points */
    gsize md = b->len;
    body = g_byte_array_new ();
    put16 (body, 1);
    put16 (body, 1);   /* bone: arm */
    puts0 (body, "hand");
    for (int k = 0; k < 16; k++)
        putf (body, k % 5 == 0 ? 1.0f : k == 12 ? 60.0f : k == 13 ? 40.0f : 0.0f);
    g_byte_array_append (b, (const guint8 *) "MDAT0001\0", 9);
    put32 (b, (guint32) (md + 9 + 4 + body->len));
    g_byte_array_append (b, body->data, body->len);
    g_byte_array_unref (body);
    /* one animation: 2 frames, both at the bind pose */
    g_byte_array_append (b, (const guint8 *) "MDLA0003\0", 9);
    put32 (b, 0);
    put32 (b, 1);
    put32 (b, 1); put32 (b, 0);
    puts0 (b, "a");
    puts0 (b, "loop");
    putf (b, 30.0f);
    put32 (b, 1);
    put32 (b, 0);
    put32 (b, 2);
    for (int bone = 0; bone < 2; bone++) {
        put32 (b, 0);
        put32 (b, 2 * 36);
        for (int f = 0; f < 2; f++) {
            const float fr[9] = { 0, 0, 0, 0, 0, 0, 1, 1, 1 };
            for (int k = 0; k < 9; k++)
                putf (b, fr[k]);
        }
    }
    put_bytes (dir, rel, b);
}

static const char *VERT =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
    "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
    "void main() {\n\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "\tv_TexCoord = a_TexCoord;\n}\n";

static void put_effect (const char *dir, const char *name, const char *frag)
{
    char *rel = g_strdup_printf ("shaders/effects/%s.vert", name);
    put (dir, rel, VERT);
    g_free (rel);
    rel = g_strdup_printf ("shaders/effects/%s.frag", name);
    put (dir, rel, frag);
    g_free (rel);
    rel = g_strdup_printf ("materials/effects/%s.json", name);
    char *t = g_strdup_printf ("{\"passes\":[{\"shader\":\"effects/%s\",\"blending\":\"normal\","
                               "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\","
                               "\"cullmode\":\"nocull\"}]}", name);
    put (dir, rel, t);
    g_free (t);
    g_free (rel);
    rel = g_strdup_printf ("effects/%s/effect.json", name);
    t = g_strdup_printf ("{\"name\":\"%s\",\"group\":\"t\",\"passes\":[{\"material\":"
                         "\"materials/effects/%s.json\"}]}", name, name);
    put (dir, rel, t);
    g_free (t);
    g_free (rel);
}

static void put_picture (const char *dir, const char *name, int w, int h, ValueFn fn)
{
    char *rel = g_strdup_printf ("materials/%s.tex", name);
    write_tex (dir, rel, w, h, fn);
    g_free (rel);
    rel = g_strdup_printf ("materials/%s.json", name);
    char *t = g_strdup_printf ("{\"passes\":[{\"blending\":\"translucent\",\"cullmode\":\"nocull\","
                               "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\","
                               "\"shader\":\"genericimage2\",\"textures\":[\"%s\"]}]}", name);
    put (dir, rel, t);
    g_free (t);
    g_free (rel);
}

/* a 256 x 256 scene whose `objects` text is given */
static char *make_scene (const char *objects)
{
    char *dir = g_dir_make_tmp ("walpe-scene-fit-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "models/pic.json", "{\"autosize\":true,\"material\":\"materials/pic.json\"}");
    put (dir, "models/pic4.json", "{\"autosize\":true,\"material\":\"materials/pic4.json\"}");
    put (dir, "models/pics.json", "{\"autosize\":true,\"material\":\"materials/pics.json\"}");
    put (dir, "models/pup.json",
         "{\"autosize\":true,\"material\":\"materials/pup.json\",\"puppet\":\"models/pup.mdl\"}");
    put_picture (dir, "pic", 256, 256, v_columns);
    put_picture (dir, "pic4", 256, 256, v_every4th);
    put_picture (dir, "pics", 256, 256, v_smooth);
    put_picture (dir, "pup", 16, 16, v_dark);
    write_tex (dir, "materials/mask_small.tex", 64, 64, v_mask_small);
    write_tex (dir, "materials/mask_half.tex", 64, 64, v_mask_half);
    write_puppet (dir, "models/pup.mdl");
    /* reads the layer's texture as it is */
    put_effect (dir, "plain",
                "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
                "\tgl_FragColor = c;\n}\n");
    /* reads it half a texel to the right, everywhere */
    put_effect (dir, "shift",
                "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                "void main() {\n"
                "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord + vec2(0.5 / 256.0, 0.0));\n}\n");
    /* ... where its painted mask says so */
    put_effect (dir, "mshift",
                "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                "uniform sampler2D g_Texture1; // {\"material\":\"mask\",\"label\":\"mask\","
                "\"mode\":\"opacitymask\",\"default\":\"util/white\"}\n"
                "void main() {\n\tfloat m = texSample2D(g_Texture1, v_TexCoord).r;\n"
                "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord + vec2(0.5 / 256.0, 0.0) * m);\n}\n");
    char *sc = g_strdup_printf (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":256,\"width\":256},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[%s]}", objects);
    put (dir, "scene.json", sc);
    g_free (sc);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"scene fit test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    return dir;
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

#define PB 256   /* the pbuffer; frames are drawn into its lower left w x h */

/* load `objects`, draw 3 frames at w x h, return the last one (RGBA, rows
 * bottom up; g_free) and the bytes of every GL texture alive afterwards */
static guint8 *frame_of (const char *objects, int w, int h, gboolean no_downscale, gint64 *tex_bytes)
{
    char *dir = make_scene (objects);
    if (!dir)
        return NULL;
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir,
                           .no_tex_downscale = no_downscale };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    guint8 *px = NULL;
    if (c) {
        c->request_redraw = redraw_cb;
        if (c->gl_init (c, &err)) {
            px = g_malloc0 ((gsize) w * h * 4);
            for (int f = 0; f < 3; f++) {
                glBindFramebuffer (GL_FRAMEBUFFER, 0);
                glViewport (0, 0, w, h);
                c->next_frame_us = 0;
                c->surface_lost = TRUE;
                c->gl_render (c, w, h, 0.0f, 0.0f);
                glFinish ();
            }
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            glPixelStorei (GL_PACK_ALIGNMENT, 4);
            glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (tex_bytes) {
                int n = 0;
                gint64 comp = 0;
                walpe_gl_tex_totals (&n, tex_bytes, &comp);
            }
        } else {
            printf ("gl_init failed: %s\n", err ? err->message : "?");
        }
        if (c->gl_free)
            c->gl_free (c);
        c->free (c);
    } else {
        printf ("load failed: %s\n", err ? err->message : "?");
    }
    g_clear_error (&err);
    rm_rf (dir);
    g_free (dir);
    return px;
}

/* pixels of the frame whose red byte lies in [lo, hi] */
static int count_red (const guint8 *px, int w, int h, int lo, int hi)
{
    int n = 0;
    for (int i = 0; px && i < w * h; i++)
        n += px[i * 4] >= lo && px[i * 4] <= hi;
    return n;
}

static char *picture_layer (const char *model, const char *effect, const char *mask)
{
    char *tx = mask ? g_strdup_printf (",\"textures\":[null,\"%s\"]", mask) : g_strdup ("");
    char *o = g_strdup_printf (
        "{\"id\":1,\"name\":\"picture\",\"image\":\"models/%s.json\",\"origin\":\"128 128 0\","
        "\"size\":\"256 256\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"visible\":true,"
        "\"effects\":[{\"file\":\"effects/%s/effect.json\",\"id\":9,\"name\":\"e\",\"visible\":true,"
        "\"passes\":[{\"id\":10%s}]}]}", model, effect, tx);
    g_free (tx);
    return o;
}

static void fit_checks (void)
{
    /* ---- 1. the input of effects, layer shown at half its texture's size */
    const int w = 128, h = 128, all = w * h;
    gint64 b_plain = 0, b_shift = 0, b_small = 0, b_half = 0;
    char *o = picture_layer ("pic", "plain", NULL);
    guint8 *plain = frame_of (o, w, h, FALSE, &b_plain);
    g_free (o);
    o = picture_layer ("pic", "shift", NULL);
    guint8 *shift = frame_of (o, w, h, FALSE, &b_shift);
    g_free (o);
    o = picture_layer ("pic", "mshift", "mask_small");
    guint8 *small = frame_of (o, w, h, FALSE, &b_small);
    g_free (o);
    o = picture_layer ("pic", "mshift", "mask_half");
    guint8 *half = frame_of (o, w, h, FALSE, &b_half);
    g_free (o);
    if (!plain || !shift || !small || !half) {
        result (FALSE, "the generated scenes load and draw");
        return;
    }
    printf ("   grey / white pixels of %d: plain %d / %d, shift %d / %d, small mask %d / %d, "
            "half mask %d / %d\n", all, count_red (plain, w, h, 118, 138), count_red (plain, w, h, 236, 255),
            count_red (shift, w, h, 118, 138), count_red (shift, w, h, 236, 255),
            count_red (small, w, h, 118, 138), count_red (small, w, h, 236, 255),
            count_red (half, w, h, 118, 138), count_red (half, w, h, 236, 255));
    printf ("   texture bytes: plain %" G_GINT64_FORMAT ", shift %" G_GINT64_FORMAT ", small mask %"
            G_GINT64_FORMAT ", half mask %" G_GINT64_FORMAT "\n", b_plain, b_shift, b_small, b_half);
    result (count_red (plain, w, h, 118, 138) == all,
            "an effect that reads the texture as it is: the average of the columns (grey)");
    result (count_red (shift, w, h, 236, 255) == all,
            "a first effect that displaces its lookups everywhere reads the full-size texture (white, not grey)");
    result (b_shift - b_plain >= 150000,
            "the texture read as it is is stored at half size, the displaced one at full size");
    result (count_red (small, w, h, 236, 255) == 0 && count_red (small, w, h, 118, 138) == all,
            "displaced under a mask over 5 % of the layer: still stored at half size");
    result (b_small < b_shift - 150000, "... and takes that much less memory");
    int hw = count_red (half, w, h, 236, 255);
    result (hw >= all * 4 / 10 && hw <= all * 6 / 10,
            "displaced under a mask over half of the layer: the full-size texture again");
    g_free (plain);
    g_free (shift);
    g_free (small);
    g_free (half);

    /* ... the same displaced read of a picture without fine detail: the
     * reduced texture gives the same values, so it is stored reduced */
    gint64 b_smooth = 0;
    o = picture_layer ("pics", "shift", NULL);
    guint8 *smooth = frame_of (o, w, h, FALSE, &b_smooth);
    guint8 *smooth_full = frame_of (o, w, h, TRUE, NULL);
    g_free (o);
    if (!smooth || !smooth_full) {
        result (FALSE, "the smooth scene loads and draws");
    } else {
        int worst = 0;
        for (int k = 0; k < all * 4; k++)
            worst = MAX (worst, abs ((int) smooth[k] - (int) smooth_full[k]));
        printf ("   smooth picture under the displaced read: texture bytes %" G_GINT64_FORMAT
                ", largest difference to the full-size texture %d / 255\n", b_smooth, worst);
        result (b_smooth < b_shift - 150000,
                "displaced everywhere, but a picture without fine detail: stored at half size");
        result (worst <= 3, "... and the frame is the full-size texture's (within 3 / 255)");
    }
    g_free (smooth);
    g_free (smooth_full);

    /* ---- 2. tex_downscale off, layer shown at a quarter of its texture */
    o = picture_layer ("pic4", "plain", NULL);
    guint8 *exact = frame_of (o, 64, 64, TRUE, NULL);
    guint8 *deflt = frame_of (o, 64, 64, FALSE, NULL);
    g_free (o);
    if (!exact || !deflt) {
        result (FALSE, "the quarter-size scenes load and draw");
        return;
    }
    printf ("   quarter size, pixels with red in 54..74 of 4096: tex_downscale off %d, on %d; red of "
            "the first pixel %d / %d\n", count_red (exact, 64, 64, 54, 74),
            count_red (deflt, 64, 64, 54, 74), exact[0], deflt[0]);
    result (count_red (exact, 64, 64, 54, 74) == 64 * 64,
            "tex_downscale off: effects read the full texture within 2:1 (every fourth column counts)");
    result (count_red (deflt, 64, 64, 54, 74) == 64 * 64, "default: the same average from the reduced texture");
    g_free (exact);
    g_free (deflt);

    /* ---- 3. children of a puppet's attachment point */
    guint8 *att = frame_of (
        "{\"id\":1,\"name\":\"puppet\",\"image\":\"models/pup.json\",\"origin\":\"100 100 0\","
        "\"size\":\"100 100\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"visible\":true},"
        "{\"id\":2,\"name\":\"dot\",\"image\":\"models/solid.json\",\"parent\":1,"
        "\"attachment\":\"hand\",\"origin\":\"0 0 0\",\"size\":\"20 20\",\"scale\":\"1 1 1\","
        "\"angles\":\"0 0 0\",\"color\":\"1 0 0\",\"visible\":true},"
        "{\"id\":3,\"name\":\"stray\",\"image\":\"models/solid.json\",\"parent\":1,"
        "\"attachment\":\"nose\",\"origin\":\"-40 -40 0\",\"size\":\"20 20\",\"scale\":\"1 1 1\","
        "\"angles\":\"0 0 0\",\"color\":\"0 1 0\",\"visible\":true}", PB, PB, FALSE, NULL);
    if (!att) {
        result (FALSE, "the puppet scene loads and draws");
        return;
    }
#define AT(x, y) (att + ((gsize) (y) * PB + (x)) * 4)
    printf ("   pixel at the attachment point (160, 140): %d %d %d; at the layer's centre "
            "(100, 100): %d %d %d; at (60, 60): %d %d %d\n", AT (160, 140)[0], AT (160, 140)[1],
            AT (160, 140)[2], AT (100, 100)[0], AT (100, 100)[1], AT (100, 100)[2], AT (60, 60)[0],
            AT (60, 60)[1], AT (60, 60)[2]);
    result (AT (160, 140)[0] > 200 && AT (160, 140)[1] < 60,
            "a child that names an attachment point sits at the point");
    result (AT (100, 100)[0] < 100, "... and not at the puppet layer's centre");
    result (AT (60, 60)[1] > 200 && AT (60, 60)[0] < 60,
            "a child that names a point the puppet does not have stays relative to the layer");
#undef AT
    g_free (att);

    /* ---- 4. nested "children": after their parent */
    guint8 *nest = frame_of (
        "{\"id\":1,\"name\":\"outer\",\"image\":\"models/solid.json\",\"origin\":\"128 128 0\","
        "\"size\":\"120 120\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 0 0\","
        "\"visible\":true,\"children\":["
        "{\"id\":2,\"name\":\"inner\",\"image\":\"models/solid.json\",\"origin\":\"128 128 0\","
        "\"size\":\"40 40\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0 1 0\","
        "\"visible\":true}]},"
        "{\"id\":3,\"name\":\"last\",\"image\":\"models/solid.json\",\"origin\":\"128 128 0\","
        "\"size\":\"10 10\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0 0 1\","
        "\"visible\":true}", PB, PB, FALSE, NULL);
    if (!nest) {
        result (FALSE, "the nested scene loads and draws");
        return;
    }
#define AT(x, y) (nest + ((gsize) (y) * PB + (x)) * 4)
    printf ("   nested: (80, 128) %d %d %d, (115, 128) %d %d %d, (128, 128) %d %d %d\n",
            AT (80, 128)[0], AT (80, 128)[1], AT (80, 128)[2], AT (115, 128)[0], AT (115, 128)[1],
            AT (115, 128)[2], AT (128, 128)[0], AT (128, 128)[1], AT (128, 128)[2]);
    result (AT (80, 128)[0] > 200 && AT (80, 128)[1] < 60, "the parent is drawn");
    result (AT (115, 128)[1] > 200 && AT (115, 128)[0] < 60,
            "a nested child is drawn after its parent (on top of it)");
    result (AT (128, 128)[2] > 200 && AT (128, 128)[1] < 60,
            "the object that follows in the file is drawn after both");
#undef AT
    g_free (nest);
}

/* one frame of a live content into the pbuffer; px = PB x PB x 4 */
static void live_frame (WalpeContent *c, guint8 *px)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, PB, PB);
    c->next_frame_us = 0;
    c->surface_lost = TRUE;
    c->gl_render (c, PB, PB, 0.0f, 0.0f);
    glFinish ();
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 4);
    glReadPixels (0, 0, PB, PB, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static gint64 tex_bytes_now (void)
{
    int n = 0;
    gint64 b = 0, comp = 0;
    walpe_gl_tex_totals (&n, &b, &comp);
    return b;
}

/* pixels of rows [y0, y1) that are not the clear colour (black) */
static int lit_rows (const guint8 *px, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < PB; x++) {
            const guint8 *p = px + ((gsize) y * PB + x) * 4;
            n += p[0] > 40 || p[1] > 40 || p[2] > 40;
        }
    return n;
}

static void text_checks (void)
{
    /* a shown line in the lower half, a hidden one with a large box in
     * the upper half (scene y is up, and so are the rows read back) */
    char *dir = make_scene (
        "{\"id\":1,\"name\":\"shown\",\"text\":{\"value\":\"WALPE 12:34\"},\"font\":\"systemfont_arial\","
        "\"pointsize\":8,\"origin\":\"128 64 0\",\"size\":\"240 60\",\"scale\":\"1 1 1\","
        "\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"horizontalalign\":\"center\","
        "\"verticalalign\":\"center\",\"visible\":true},"
        "{\"id\":2,\"name\":\"hidden\",\"text\":{\"value\":\"HIDDEN TEXT\"},\"font\":\"systemfont_arial\","
        "\"pointsize\":8,\"origin\":\"128 192 0\",\"size\":\"1500 800\",\"scale\":\"0.16 0.16 1\","
        "\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"horizontalalign\":\"center\","
        "\"verticalalign\":\"center\",\"visible\":false}");
    if (!dir)
        return;
    WalpeConfig *cfg = walpe_config_load ();
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c || (c->request_redraw = redraw_cb, !c->gl_init (c, &err))) {
        printf ("load failed: %s\n", err ? err->message : "?");
        result (FALSE, "the text scene loads");
        return;
    }
    guint8 *px = g_malloc0 ((gsize) PB * PB * 4);
    for (int f = 0; f < 3; f++)
        live_frame (c, px);
    const gint64 b0 = tex_bytes_now ();
    const int low0 = lit_rows (px, 0, PB / 2), up0 = lit_rows (px, PB / 2, PB);
    /* the hidden layer's box alone is 1500 x 800 x 4 = 4.8 MB */
    result (b0 < 2000000, "a hidden text layer holds no texture");
    result (up0 == 0, "... and draws nothing");
    const WalpeLayerAdj show = { .key = (char *) "2:hidden", .dx = 0, .dy = 0, .scale = 1,
                                 .vis = WALPE_LAYER_VIS_SHOWN };
    const char *why = NULL;
    if (!walpe_layers_put (cfg, dir, &show, NULL, &why))
        result (FALSE, why ? why : "walpe_layers_put");
    live_frame (c, px);
    live_frame (c, px);
    const gint64 b1 = tex_bytes_now ();
    const int up1 = lit_rows (px, PB / 2, PB);
    printf ("   text: texture bytes %" G_GINT64_FORMAT " hidden, %" G_GINT64_FORMAT " shown; lit "
            "pixels: shown line %d, hidden line %d -> %d\n", b0, b1, low0, up0, up1);
    result (b1 > b0 + 4000000, "shown later: its texture is made in that frame");
    if (low0 > 0)
        result (up1 > 0, "... and its text is on the picture");
    else
        printf ("skip  no glyphs were drawn for the shown line either (no usable font here)\n");
    walpe_layers_remove (cfg, dir, NULL);
    g_free (px);
    c->gl_free (c);
    c->free (c);
    walpe_config_free (cfg);
    rm_rf (dir);
    g_free (dir);
}

/* An old-format scene ("object" / "layers", src/engine/scene.c) with a
 * group, a text layer and a picture that does not exist: loaded and freed
 * once without being drawn and once drawn. What is checked here is that it
 * loads and draws; that nothing of the layer list stays behind afterwards
 * is seen by a build with LeakSanitizer. */
static void legacy_checks (void)
{
    char *dir = g_dir_make_tmp ("walpe-scene-fit-XXXXXX", NULL);
    if (!dir)
        return;
    put (dir, "scene.json",
         "{\"object\":{\"width\":256,\"height\":256,\"layers\":["
         "{\"file\":\"missing.png\",\"scale\":1},"
         "{\"compose\":{\"layers\":[{\"file\":\"missing2.png\"},"
         "{\"text\":{\"text\":\"OLD\",\"font\":\"Sans\",\"size\":20},\"translation\":[128,128]}]}},"
         "{\"text\":{\"text\":\"FORMAT\",\"font\":\"Sans\",\"size\":24},\"translation\":[128,64]}]}}");
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"old format\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    gboolean ok = TRUE;
    for (int pass = 0; pass < 2 && ok; pass++) {
        GError *err = NULL;
        WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
        WalpeContent *c = walpe_content_new (dir, &opts, &err);
        if (!c) {
            printf ("load failed: %s\n", err ? err->message : "?");
            ok = FALSE;
        } else {
            c->request_redraw = redraw_cb;
            if (pass == 1) {
                if (c->gl_init (c, &err)) {
                    glBindFramebuffer (GL_FRAMEBUFFER, 0);
                    glViewport (0, 0, PB, PB);
                    c->gl_render (c, PB, PB, 0.0f, 0.0f);
                    glFinish ();
                    c->gl_free (c);
                } else {
                    printf ("gl_init failed: %s\n", err ? err->message : "?");
                    ok = FALSE;
                }
            }
            c->free (c);
        }
        g_clear_error (&err);
    }
    result (ok, "an old-format scene loads, draws and is freed (with and without being drawn)");
    rm_rf (dir);
    g_free (dir);
}

static guint8 v_lit (int x, int y) { (void) x; (void) y; return 255; }

/* an old-format scene of 256 x 128 drawn at w x h with the pointer at
 * (ptx, 0): the frame (RGBA, rows bottom up, PB wide; g_free) or NULL */
static guint8 *legacy_frame (const char *object, int w, int h, float ptx)
{
    char *dir = g_dir_make_tmp ("walpe-scene-fit-XXXXXX", NULL);
    if (!dir)
        return NULL;
    char *text = g_strdup_printf ("{\"object\":{\"width\":256,\"height\":128,%s}}", object);
    put (dir, "scene.json", text);
    g_free (text);
    put (dir, "project.json", "{\"file\":\"scene.json\",\"title\":\"old format\",\"type\":\"scene\"}");
    write_tex (dir, "bg.tex", 256, 128, v_dark);
    write_tex (dir, "dot.tex", 16, 16, v_lit);
    write_tex (dir, "bar.tex", 32, 8, v_lit);
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    guint8 *px = NULL;
    if (c && c->gl_init (c, &err)) {
        px = g_malloc0 ((gsize) PB * PB * 4);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, PB, PB);
        glClearColor (0, 0, 0, 1);
        glClear (GL_COLOR_BUFFER_BIT);
        glViewport (0, 0, w, h);
        c->gl_render (c, w, h, ptx, 0.0f);
        glFinish ();
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glReadPixels (0, 0, PB, PB, GL_RGBA, GL_UNSIGNED_BYTE, px);
        c->gl_free (c);
    } else {
        printf ("old-format scene: %s\n", err ? err->message : "not loaded");
    }
    if (c)
        c->free (c);
    g_clear_error (&err);
    rm_rf (dir);
    g_free (dir);
    return px;
}

/* Where the layers of an old-format scene are: the picture's top left
 * corner at "translation", in scene pixels from the scene's top left
 * corner, whatever the output. (They lay around the middle of the output
 * with the translation counted in halves of it: 32 px to the right was 32
 * half outputs to the right.) x, y below: scene pixels of a frame of h
 * rows, y down. */
static void legacy_place_checks (void)
{
#define LIT(px, x, y, h) ((px)[((gsize) ((h) - 1 - (y)) * PB + (x)) * 4] > 200)
#define BG(px, x, y, h) ((px)[((gsize) ((h) - 1 - (y)) * PB + (x)) * 4] < 60 && \
                         (px)[((gsize) ((h) - 1 - (y)) * PB + (x)) * 4 + 1] > 20)
    /* layers are listed top first: a dot at 32 16, a dot in a group at
     * 160 64 + 8 0, a bar of 32 x 8 at 100 90 turned by 90 degrees about
     * its middle, a dot at 200 16 that moves with the pointer, and a
     * background of the scene's size */
    const char *layers =
        "\"layers\":["
        "{\"file\":\"dot.tex\",\"translation\":[32,16]},"
        "{\"compose\":{\"layers\":[{\"file\":\"dot.tex\",\"translation\":[8,0]}]},\"translation\":[160,64]},"
        "{\"file\":\"bar.tex\",\"pivot\":[0.5,0.5],\"angle\":90,\"translation\":[100,90]},"
        "{\"file\":\"dot.tex\",\"translation\":[200,16],\"parallax\":{\"amount\":[1,1],\"depth\":100}},"
        "{\"file\":\"bg.tex\"}]";
    guint8 *px = legacy_frame (layers, 256, 128, 0.0f);
    result (px && LIT (px, 34, 18, 128) && LIT (px, 45, 29, 128) && BG (px, 29, 24, 128) && BG (px, 50, 24, 128) &&
            BG (px, 40, 13, 128) && BG (px, 40, 34, 128),
            "old format: a layer's translation is in scene pixels, from the top left corner");
    result (px && BG (px, 2, 2, 128) && BG (px, 253, 125, 128) && BG (px, 128, 64, 128),
            "old format: a layer without a translation starts at the scene's corner (the background fills it)");
    result (px && LIT (px, 170, 66, 128) && LIT (px, 181, 77, 128) && BG (px, 165, 72, 128) && BG (px, 186, 72, 128),
            "old format: the layers of a group are placed in the group");
    result (px && LIT (px, 116, 80, 128) && LIT (px, 116, 107, 128) && BG (px, 104, 94, 128) && BG (px, 128, 94, 128),
            "old format: a layer turns about its pivot and keeps its shape");
    result (px && LIT (px, 208, 24, 128) && BG (px, 238, 24, 128),
            "old format: a layer with parallax is at its place while the pointer is in the middle");
    g_free (px);
    px = legacy_frame (layers, 256, 128, 0.5f);
    result (px && LIT (px, 238, 24, 128) && BG (px, 204, 24, 128) && LIT (px, 40, 24, 128),
            "old format: parallax moves a layer by scene pixels (30 of at most 60 for half the way)");
    g_free (px);
    /* an output of another shape shows the middle of the scene (its x 64 .. 191) */
    px = legacy_frame (layers, 128, 128, 0.0f);
    result (px && LIT (px, 176 - 64, 72, 128) && BG (px, 2, 2, 128) && BG (px, 125, 125, 128) &&
            LIT (px, 116 - 64, 94, 128) && BG (px, 40, 24, 128),
            "old format: an output of another shape than the scene shows the scene's middle (it was black)");
    g_free (px);
    /* a camera that zooms by 2 about the scene's middle: a dot there is twice as large and stays there */
    px = legacy_frame ("\"camera\":{\"zoom\":2},\"layers\":[{\"file\":\"dot.tex\",\"translation\":[120,56]},"
                       "{\"file\":\"bg.tex\"}]", 256, 128, 0.0f);
    result (px && LIT (px, 114, 50, 128) && LIT (px, 141, 77, 128) && BG (px, 108, 64, 128) && BG (px, 148, 64, 128) &&
            BG (px, 2, 2, 128),
            "old format: the camera's zoom enlarges the scene about its middle (it was black)");
    g_free (px);
#undef LIT
#undef BG
}

/* Everything runs on a thread of its own, as a monitor's render thread
 * does in the daemon: what the effect code keeps per render thread (its
 * target pool and two small tables) has to be gone when the thread ends -
 * a build with LeakSanitizer reports it otherwise, when the test exits. */
static gpointer render_thread (gpointer unused)
{
    (void) unused;
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skip (no EGL surfaceless platform)\n");
        return NULL;
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
        return NULL;
    }
    cfg = cfgs[0];
    for (int i = 0; i < nc; i++) {
        EGLint r = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &r);
        if (r == 8) {
            cfg = cfgs[i];
            break;
        }
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, PB, EGL_HEIGHT, PB, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        printf ("skip (no GL 3.3 context with a pbuffer)\n");
        return NULL;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    /* the switches that would change what is tested */
    static const char *const envs[] = { "WALPE_TEX_FX_SOFT", "WALPE_TEX_DOWNSCALE", "WALPE_TEX_SLACK",
                                        "WALPE_FX_MIN_SCALE", "WALPE_FX_FULLRES", "WALPE_NOFX",
                                        "WALPE_RENDER_SCALE", "WALPE_PROP_PROBE", "WALPE_SKIP_IDS" };
    for (guint i = 0; i < G_N_ELEMENTS (envs); i++)
        g_unsetenv (envs[i]);
    walpe_glutil_init ();
    fx_set_cache_enabled (TRUE);
    fit_checks ();
    text_checks ();
    legacy_checks ();
    legacy_place_checks ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return NULL;
}

int main (void)
{
    /* a configuration of its own ("Adjust layers" writes one) */
    char *cfgdir = g_dir_make_tmp ("walpe-scene-fit-cfg-XXXXXX", NULL);
    if (cfgdir) {
        g_setenv ("HOME", cfgdir, TRUE);
        g_setenv ("XDG_CONFIG_HOME", cfgdir, TRUE);
        g_setenv ("XDG_CACHE_HOME", cfgdir, TRUE);
    }
    g_thread_join (g_thread_new ("render", render_thread, NULL));
    if (cfgdir)
        rm_rf (cfgdir);
    g_free (cfgdir);
    if (!checks)
        return 0;   /* skipped */
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
