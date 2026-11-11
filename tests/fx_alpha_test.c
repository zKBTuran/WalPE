/* fx_alpha_test.c - a layer's opacity and its effects: which comes first.
 *
 * Wallpaper Engine applies a layer's opacity where it applies its colour,
 * in the layer's own material draw (genericimage*.frag: texture *
 * g_Color4, flat.frag: vec4 (g_Color, g_Alpha), font.frag: coverage *
 * g_Color4.a); the effects then work on the already transparent layer and
 * their result is blended as it is. the earlier build ran the effects on the opaque
 * layer and multiplied the opacity into the finished result, which is the
 * same picture for effects that leave the alpha alone and a different one
 * for every effect that adds to it (shine, glow: the rays were dimmed
 * with the layer, in WE they fill it up).
 *
 * This test renders small generated scenes (solid colour layers, two
 * generated effects, no texture files, no fonts) on an EGL surfaceless
 * context and checks, in both orders (default and WALPE_FX_ALPHA=after):
 *   - an effect that adds 0.5 to the alpha, on a layer of opacity 0.5;
 *   - the same on a layer without a colour (only the opacity makes the
 *     layer's own pass run: what a text layer has);
 *   - a layer without effects: byte for byte the same in both orders;
 *   - an effect that leaves the alpha alone: the same within rounding;
 *   - opacity 1 everywhere: byte for byte the same in both orders;
 *   - an opacity a script changes: the picture follows in the frame of
 *     the change, which the renderer draws without being asked to (the
 *     kept result of a static effect set is redone); the same with the
 *     effect update rate halved (a held set is redrawn at once).
 *
 * Skipped (exit 0, "skip") without EGL surfaceless / GL 3.3. In a build
 * without QuickJS (-Dscenescript=disabled) no script runs: the checks of
 * the scripted opacity are left out and the others run.
 * `meson test -C build fx-alpha` */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "walpe-build-config.h"
#include "common/quality.h"
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
        "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
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

/* a solid colour layer, 60 x 60 scene units around (x, 90); alpha = a JSON
 * value; fx = an effect name or NULL */
static void layer (GString *s, int id, int x, const char *color, const char *alpha, const char *fx)
{
    g_string_append_printf (s,
        ",{\"id\":%d,\"name\":\"l%d\",\"image\":\"models/solid.json\",\"origin\":\"%d 90 0\","
        "\"size\":\"60 60\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"%s\",\"alpha\":%s,"
        "\"visible\":true", id, id, x, color, alpha);
    if (fx)
        g_string_append_printf (s,
            ",\"effects\":[{\"file\":\"effects/%s/effect.json\",\"id\":%d,\"name\":\"%s\","
            "\"visible\":true,\"passes\":[{}]}]", fx, 100 + id, fx);
    g_string_append (s, "}");
}

/* opacity 0.5 for the first frames, 0.2 from the sixth update on */
#define ALPHA_SCRIPT \
    "{\"script\":\"'use strict';\\nlet n = 0;\\nexport function update(value) {\\n" \
    "  n++;\\n  return n > 5 ? 0.2 : 0.5;\\n}\\n\",\"value\":0.5}"

/* four layers in a row over a background of 0.2 0.4 0.6:
 *   x  40: colour, opacity `a`, effect `fx` ("add": adds 0.5 to the alpha)
 *   x 120: colour, opacity 0.5 (or 1), no effect
 *   x 200: colour, opacity 0.5 (or 1), effect "gain" (rgb * 0.5)
 *   x 280: white,  opacity 0.5 (or 1), effect "add" */
static char *make_scene (const char *a, const char *fx, gboolean opaque)
{
    char *dir = g_dir_make_tmp ("walpe-fx-alpha-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    effect (dir, "add", "\tc.a = min(1.0, c.a + 0.5);\n");
    effect (dir, "gain", "\tc.rgb *= 0.5;\n");
    /* follows the time (so the set is animated, not kept as static), by
     * less than a step of an 8-bit target */
    effect (dir, "addt", "\tc.a = min(1.0, c.a + 0.5 + 0.0001 * sin(g_Time));\n");
    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":["
        "{\"id\":1,\"name\":\"bg\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\","
        "\"size\":\"320 180\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0.2 0.4 0.6\","
        "\"alpha\":1.0,\"visible\":true}");
    const char *half = opaque ? "1.0" : "0.5";
    layer (s, 2, 40, "1 0.5 0.25", a, fx);
    layer (s, 3, 120, "1 0.5 0.25", half, NULL);
    layer (s, 4, 200, "1 0.5 0.25", half, "gain");
    layer (s, 5, 280, "1 1 1", half, "add");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"fx alpha test\",\"type\":\"scene\","
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

#define W 320
#define H 180

typedef struct {
    WalpeContent *c;
    guint8 *px;
} Run;

/* after = WALPE_FX_ALPHA=after while the scene is loaded */
static gboolean run_open (Run *r, const char *dir, gboolean after)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    if (after)
        g_setenv ("WALPE_FX_ALPHA", "after", TRUE);
    else
        g_unsetenv ("WALPE_FX_ALPHA");
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

/* one frame tick; force = draw it whatever changed. TRUE when a frame was
 * drawn (px then holds it; an unchanged frame leaves px as it was) */
static gboolean run_frame (Run *r, gboolean force)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, W, H);
    r->c->next_frame_us = 0;
    r->c->surface_lost = force;
    r->c->frame_unchanged = FALSE;
    r->c->gl_render (r->c, W, H, 0.0f, 0.0f);
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
    g_unsetenv ("WALPE_FX_ALPHA");
}

/* the pixel in the middle of the layer at x */
static const guint8 *at (const Run *r, int x)
{
    return r->px + ((gsize) 90 * W + x) * 4;
}

static gboolean near (const guint8 *p, int r, int g, int b, int tol)
{
    return abs (p[0] - r) <= tol && abs (p[1] - g) <= tol && abs (p[2] - b) <= tol;
}

/* columns x0 .. x1-1 of two frames, largest difference */
static int cols_diff (const Run *a, const Run *b, int x0, int x1)
{
    int d = 0;
    for (int y = 0; y < H; y++)
        for (int x = x0; x < x1; x++)
            for (int c = 0; c < 3; c++) {
                gsize o = ((gsize) y * W + x) * 4 + c;
                d = MAX (d, abs ((int) a->px[o] - (int) b->px[o]));
            }
    return d;
}

/* background 51 102 153, colour 255 128 64 */
#define COL        255, 128, 64
#define COL_HALF   153, 115, 108     /* 0.5 colour + 0.5 background */
#define COL_07     194, 120, 91      /* 0.7 colour + 0.3 background */
#define COL_02     92, 107, 135      /* 0.2 colour + 0.8 background */
#define GAIN_HALF  89, 83, 92        /* 0.5 * (0.5 colour) + 0.5 background */
#define WHITE      255, 255, 255
#define WHITE_HALF 153, 178, 204

static void static_checks (void)
{
    char *dir = make_scene ("0.5", "add", FALSE);
    Run we = { 0 }, old = { 0 };
    if (!dir || !run_open (&we, dir, FALSE) || !run_open (&old, dir, TRUE)) {
        result (FALSE, "scene loads");
        return;
    }
    run_frame (&we, TRUE);
    run_frame (&old, TRUE);
    printf ("   add 0.5 to the alpha, opacity 0.5: %d %d %d (after: %d %d %d)\n",
            at (&we, 40)[0], at (&we, 40)[1], at (&we, 40)[2],
            at (&old, 40)[0], at (&old, 40)[1], at (&old, 40)[2]);
    result (near (at (&we, 40), COL, 3),
            "an effect that adds to the alpha fills the half transparent layer up (WE's order)");
    result (near (at (&old, 40), COL_HALF, 3),
            "WALPE_FX_ALPHA=after: the opacity scales the finished result (the earlier build's order)");
    result (near (at (&we, 280), WHITE, 3) && near (at (&old, 280), WHITE_HALF, 3),
            "the same on a layer without a colour (the opacity alone runs the layer's own pass)");
    result (near (at (&we, 120), COL_HALF, 3) && cols_diff (&we, &old, 80, 160) == 0,
            "a layer without effects: byte for byte the same in both orders");
    int dg = cols_diff (&we, &old, 160, 240);
    printf ("   effect that leaves the alpha alone: %d %d %d, orders differ by %d / 255\n",
            at (&we, 200)[0], at (&we, 200)[1], at (&we, 200)[2], dg);
    result (near (at (&we, 200), GAIN_HALF, 3) && dg <= 1,
            "an effect that leaves the alpha alone: the same picture within rounding");
    /* a second frame comes from the kept result: the same picture */
    guint8 first[4];
    memcpy (first, at (&we, 40), 4);
    run_frame (&we, TRUE);
    result (!memcmp (first, at (&we, 40), 3), "the kept result of the static set shows the same");
    run_close (&we);
    run_close (&old);
    rm_rf (dir);
    g_free (dir);

    /* opacity 1 everywhere: nothing may move */
    dir = make_scene ("1.0", "add", TRUE);
    if (!dir || !run_open (&we, dir, FALSE) || !run_open (&old, dir, TRUE)) {
        result (FALSE, "opaque scene loads");
        return;
    }
    run_frame (&we, TRUE);
    run_frame (&old, TRUE);
    result (cols_diff (&we, &old, 0, W) == 0 && near (at (&we, 40), COL, 3),
            "opacity 1: byte for byte the same in both orders");
    run_close (&we);
    run_close (&old);
    rm_rf (dir);
    g_free (dir);
}

/* a script takes the opacity from 0.5 to 0.2; fx: "add" (static set, its
 * result is kept) or "addt" (animated); rate 2 holds an animated set.
 * (Not called in a build without QuickJS: the opacity never changes there.) */
G_GNUC_UNUSED static void animated_checks (const char *fx, int rate, gboolean after, const char *what)
{
    char *dir = make_scene (ALPHA_SCRIPT, fx, FALSE);
    Run r = { 0 };
    if (!dir || !run_open (&r, dir, after)) {
        result (FALSE, "scripted scene loads");
        return;
    }
    if (rate > 1 && r.c->set_quality) {
        WalpeQuality q;
        walpe_quality_preset (WALPE_PRESET_HIGH, &q);
        q.fx_rate = rate;
        q.fx_rate_all = 1;
        r.c->set_quality (r.c, &q);
    }
    int n_first = 0, n_second = 0, n_other = 0, change = -1, n_drawn = 0;
    gboolean change_drawn = FALSE;
    for (int f = 1; f <= 40; f++) {
        /* only the first frame is asked for; the others are the renderer's
         * own decision (an unchanged frame is not drawn) */
        gboolean drawn = run_frame (&r, f == 1);
        n_drawn += drawn;
        const guint8 *p = at (&r, 40);
        gboolean first = after ? near (p, COL_HALF, 3) : near (p, COL, 3);
        gboolean second = after ? near (p, COL_02, 3) : near (p, COL_07, 3);
        if (second && change < 0) {
            change = f;
            change_drawn = drawn;
        }
        if (change < 0 && first)
            n_first++;
        else if (change >= 0 && second)
            n_second++;
        else {
            n_other++;
            printf ("   frame %d: %d %d %d\n", f, p[0], p[1], p[2]);
        }
        g_usleep (2000);
    }
    char *msg = g_strdup_printf ("%s: the picture follows the opacity in the frame it changes "
                                 "(frame %d, %d + %d frames, %d in between)",
                                 what, change, n_first, n_second, n_other);
    result (change > 1 && n_first >= 1 && n_second >= 1 && n_other == 0, msg);
    g_free (msg);
    msg = g_strdup_printf ("%s: the frame of the change is drawn unasked (%d of 40 frames drawn)",
                           what, n_drawn);
    result (change > 1 && change_drawn, msg);
    g_free (msg);
    run_close (&r);
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
        EGLint r = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &r);
        if (r == 8) {
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
    walpe_glutil_init ();
    fx_set_cache_enabled (TRUE);
    g_unsetenv ("WALPE_BAKE");
    g_unsetenv ("WALPE_FRAME_SKIP");
    g_unsetenv ("WALPE_RENDER_SCALE");
    g_unsetenv ("WALPE_PROP_PROBE");
    g_unsetenv ("WALPE_FX_TIME");
    static_checks ();
#ifdef HAVE_QUICKJS
    animated_checks ("add", 1, FALSE, "scripted opacity, static effect");
    animated_checks ("addt", 1, FALSE, "scripted opacity, animated effect");
    animated_checks ("addt", 2, FALSE, "scripted opacity, effect at half rate");
    animated_checks ("add", 1, TRUE, "scripted opacity, WALPE_FX_ALPHA=after");
#else
    printf ("skip  scripted opacity (4 cases, 8 checks): built without QuickJS, no script runs\n");
#endif
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
