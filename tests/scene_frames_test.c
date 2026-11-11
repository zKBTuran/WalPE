/* scene_frames_test.c - what the scene renderer draws per frame.
 *
 * The scene renderer leaves work out when it cannot change the picture: a
 * frame equal to the one on screen is not drawn at all (frame_unchanged),
 * the leading run of unchanged layers is put back from one copy instead of
 * being composited again (static bake), a layer that covers no pixel does
 * not run its effects. This test renders small generated scenes (solid
 * colour layers and one time-driven effect: no texture files, no fonts)
 * on Mesa's surfaceless platform with a pbuffer as the window and checks
 *  1. a scene without anything animated: the first frame is drawn, the
 *     next ones are reported unchanged and draw nothing; surface_lost
 *     forces a frame, with the same pixels;
 *  2. a scene with static layers under an animated one: the frames with
 *     the bake and the frame skip (default) are byte-identical to the
 *     frames with both switched off (WALPE_BAKE=0 WALPE_FRAME_SKIP=0),
 *     and the bake really replaces draw calls;
 *  3. the bake follows a resize (new size, same pixels as without it);
 *  4. render_scale 50: the frame still fills the window and is the
 *     scaled-up half-size picture (not identical, close).
 *  5. a layer with a blend mode that reads the frame under it: the copy
 *     of the frame it works on goes when the scene pauses and when it is
 *     freed, and the picture after a pause is the one before it.
 * Skipped (exit 0, "skip") without EGL surfaceless / GL 3.3.
 * `meson test -C build scene-frames` */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* a solid colour layer; fx = an "effects" member or "" */
static void layer (GString *s, int id, const char *origin, const char *size, const char *color,
                   double alpha, const char *angles, const char *fx)
{
    g_string_append_printf (s,
        "%s{\"id\":%d,\"name\":\"l%d\",\"image\":\"models/solid.json\",\"origin\":\"%s\","
        "\"size\":\"%s\",\"scale\":\"1 1 1\",\"angles\":\"%s\",\"color\":\"%s\",\"alpha\":%.3f,"
        "\"visible\":true%s}", id > 1 ? "," : "", id, id, origin, size, angles, color, alpha, fx);
}

static const char *WAVE_FX =
    ",\"effects\":[{\"file\":\"effects/tw/effect.json\",\"id\":9,\"name\":\"tw\",\"visible\":true,"
    "\"passes\":[{}]}]";

/* scene with `animated`: one layer with a g_Time effect above the static ones */
static char *make_scene (gboolean animated)
{
    char *dir = g_dir_make_tmp ("walpe-scene-frames-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "shaders/effects/tw.vert",
         "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
         "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
         "void main() {\n\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
         "\tv_TexCoord = a_TexCoord;\n}\n");
    put (dir, "shaders/effects/tw.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
         "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
         "\tc.rgb *= 0.6 + 0.4 * sin(v_TexCoord.x * 20.0 + g_Time);\n\tgl_FragColor = c;\n}\n");
    put (dir, "materials/effects/tw.json",
         "{\"passes\":[{\"shader\":\"effects/tw\",\"blending\":\"normal\",\"depthtest\":\"disabled\","
         "\"depthwrite\":\"disabled\",\"cullmode\":\"nocull\"}]}");
    put (dir, "effects/tw/effect.json",
         "{\"name\":\"tw\",\"group\":\"t\",\"passes\":[{\"material\":\"materials/effects/tw.json\"}]}");
    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0.10 0.20 0.30\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[");
    layer (s, 1, "160 90 0", "320 180", "0.2 0.5 0.3", 1.0, "0 0 0", "");
    layer (s, 2, "100 60 0", "120 80", "0.9 0.2 0.1", 0.6, "0 0 0", "");
    layer (s, 3, "200 110 0", "140 60", "0.1 0.3 0.9", 0.5, "0 0 0.5", "");   /* rotated */
    layer (s, 4, "150 130 0", "90 50", "1.0 0.9 0.2", 0.8, "0 0 0", "");
    layer (s, 5, "160 90 0", "60 60", "0.5 0.5 0.5", 1.0, "0 0 0", "");
    if (animated)
        layer (s, 6, "230 60 0", "120 70", "0.8 0.8 1.0", 0.9, "0 0 0", WAVE_FX);
    /* scaled to (next to) nothing, between two pixel centres: neither its
     * effect nor its draw may run */
    {
        char *zero = g_strdup_printf ("%s", animated ? WAVE_FX : "");
        g_string_append_printf (s,
            ",{\"id\":7,\"name\":\"zero\",\"image\":\"models/solid.json\",\"origin\":\"80 140 0\","
            "\"size\":\"100 40\",\"scale\":\"0.0001 0.0001 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\","
            "\"alpha\":1.0,\"visible\":true%s}", zero);
        g_free (zero);
    }
    layer (s, 8, "60 150 0", "70 30", "0.3 0.9 0.9", 0.7, "0 0 0", "");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"scene frames test\",\"type\":\"scene\","
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

/* draw calls of one gl_render: the epoxy pointer is wrapped once the
 * function is resolved (after the first frames) */
static void (*real_draw_arrays) (GLenum, GLint, GLsizei);
static int n_draws;
static void count_draw_arrays (GLenum mode, GLint first, GLsizei count)
{
    n_draws++;
    real_draw_arrays (mode, first, count);
}

static void redraw_cb (gpointer u) { (void) u; }

#define W 320
#define H 180

typedef struct {
    WalpeContent *c;
    guint8 *px;        /* what the window shows (front) */
    int w, h;
} Run;

static gboolean run_open (Run *r, const char *dir, int render_scale)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir,
                           .render_scale = render_scale };
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
    r->px = g_malloc0 ((gsize) W * H * 4);
    r->w = W;
    r->h = H;
    return TRUE;
}

/* one frame tick as the daemon does it: render; a drawn frame goes to the
 * screen (px) and leaves an undefined back buffer; an unchanged one
 * leaves everything alone. Returns TRUE when a frame was drawn. */
static gboolean run_frame (Run *r, gboolean lost)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, r->w, r->h);
    r->c->next_frame_us = 0;
    r->c->surface_lost = lost;
    r->c->frame_unchanged = FALSE;
    n_draws = 0;
    r->c->gl_render (r->c, r->w, r->h, 0.0f, 0.0f);
    glFinish ();
    if (r->c->frame_unchanged)
        return FALSE;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glReadPixels (0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, r->px);
    glDisable (GL_SCISSOR_TEST);
    glClearColor (1.0f, 0.0f, 1.0f, 1.0f);   /* "after the swap": garbage */
    glClear (GL_COLOR_BUFFER_BIT);
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

static void wrap_draws (void)
{
    if (epoxy_glDrawArrays != count_draw_arrays) {
        real_draw_arrays = epoxy_glDrawArrays;
        epoxy_glDrawArrays = count_draw_arrays;
    }
}

static int max_diff (const guint8 *a, const guint8 *b, gsize n)
{
    int d = 0;
    for (gsize i = 0; i < n; i++)
        if ((i & 3) != 3)
            d = MAX (d, abs ((int) a[i] - (int) b[i]));
    return d;
}

static void scene_checks (void)
{
    gsize npx = (gsize) W * H * 4;
    /* ---- 1. nothing animated */
    char *sdir = make_scene (FALSE);
    Run r = { 0 };
    g_unsetenv ("WALPE_BAKE");
    g_unsetenv ("WALPE_FRAME_SKIP");
    g_unsetenv ("WALPE_RENDER_SCALE");
    g_unsetenv ("WALPE_PROP_PROBE");
    if (!sdir || !run_open (&r, sdir, 0)) {
        result (FALSE, "static scene loads");
        return;
    }
    gboolean d1 = run_frame (&r, FALSE);
    guint8 *first = g_malloc (npx);
    memcpy (first, r.px, npx);
    result (d1, "static scene: the first frame is drawn");
    result (first[0] != 255 || first[1] != 0 || first[2] != 255, "static scene: the frame has a picture");
    wrap_draws ();
    gboolean d2 = run_frame (&r, FALSE);
    result (!d2 && n_draws == 0, "static scene: the second frame is unchanged and draws nothing");
    gboolean d3 = run_frame (&r, FALSE);
    result (!d3, "static scene: so is the third");
    gboolean d4 = run_frame (&r, TRUE);
    result (d4 && !memcmp (first, r.px, npx), "static scene: surface_lost draws the same frame again");
    /* a covered monitor: pause (targets go), resume; the picture stays */
    r.c->set_paused (r.c, TRUE);
    r.c->set_paused (r.c, FALSE);
    gboolean d5 = run_frame (&r, TRUE);
    result (d5 && !memcmp (first, r.px, npx), "static scene: same frame after pause / resume");
    run_close (&r);
    rm_rf (sdir);
    g_free (sdir);

    /* ---- 2. static layers under an animated one (fixed effect time: the
     * frames of two runs are comparable) */
    g_setenv ("WALPE_FX_TIME", "1.5", TRUE);
    char *adir = make_scene (TRUE);
    Run plain = { 0 }, baked = { 0 };
    g_setenv ("WALPE_BAKE", "0", TRUE);
    g_setenv ("WALPE_FRAME_SKIP", "0", TRUE);
    gboolean ok = adir && run_open (&plain, adir, 0);
    g_unsetenv ("WALPE_BAKE");
    g_unsetenv ("WALPE_FRAME_SKIP");
    ok = ok && run_open (&baked, adir, 0);
    if (!ok) {
        result (FALSE, "animated scene loads");
        return;
    }
    gboolean same = TRUE, drawn = TRUE;
    int draws_plain = 0, draws_baked = 0;
    for (int f = 1; f <= 8; f++) {
        drawn &= run_frame (&plain, FALSE);
        draws_plain = n_draws;
        drawn &= run_frame (&baked, FALSE);
        draws_baked = n_draws;
        if (memcmp (plain.px, baked.px, npx)) {
            same = FALSE;
            printf ("   frame %d differs (max %d)\n", f, max_diff (plain.px, baked.px, npx));
        }
    }
    result (drawn, "animated scene: every frame is drawn");
    result (same, "animated scene: bake + frame skip give the frames of plain rendering, byte for byte");
    printf ("   draw calls per frame: %d plain, %d with the static bake\n", draws_plain, draws_baked);
    /* 7 layers that draw + 1 effect pass, against the bake copy + the
     * animated layer (pass + draw) + the layer above it */
    result (draws_plain >= 8 && draws_baked <= draws_plain - 4,
            "animated scene: the bake replaces the static layers' draws");
    result (draws_plain <= 8, "a layer scaled to nothing runs no effect pass and no draw");

    /* ---- 3. resize */
    plain.w = baked.w = 240;
    plain.h = baked.h = 136;
    same = TRUE;
    for (int f = 0; f < 6; f++) {
        run_frame (&plain, f == 0);
        run_frame (&baked, f == 0);
        same &= !memcmp (plain.px, baked.px, (gsize) 240 * 136 * 4);
    }
    result (same, "animated scene: identical after a resize too");
    plain.w = baked.w = W;
    plain.h = baked.h = H;
    for (int f = 0; f < 4; f++) {
        run_frame (&plain, f == 0);
        run_frame (&baked, f == 0);
    }
    result (!memcmp (plain.px, baked.px, npx), "animated scene: and back at the first size");

    /* ---- 3b. an exposed surface while the bake is in use: the static
     * layers are drawn again and a new bake is taken (what the window held
     * while a part of it was covered is not to be trusted) */
    run_frame (&plain, FALSE);
    baked.c->surface_exposed = TRUE;
    run_frame (&baked, TRUE);
    int draws_lost = n_draws;
    gboolean same_lost = !memcmp (plain.px, baked.px, npx);
    run_frame (&plain, FALSE);
    run_frame (&baked, FALSE);
    result (same_lost && !memcmp (plain.px, baked.px, npx) && draws_lost >= draws_plain &&
            n_draws <= draws_plain - 4,
            "animated scene: an exposed surface draws the static layers again and takes a new bake");
    guint8 *full = g_malloc (npx);
    memcpy (full, plain.px, npx);
    run_close (&plain);
    run_close (&baked);

    /* ---- 4. render scale */
    Run half = { 0 };
    if (run_open (&half, adir, 50)) {
        for (int f = 0; f < 5; f++)
            run_frame (&half, FALSE);
        gsize magenta = 0, off = 0;
        for (gsize i = 0; i < npx; i += 4) {
            magenta += half.px[i] == 255 && half.px[i + 1] == 0 && half.px[i + 2] == 255;
            int d = MAX (abs ((int) half.px[i] - full[i]), MAX (abs ((int) half.px[i + 1] - full[i + 1]),
                                                               abs ((int) half.px[i + 2] - full[i + 2])));
            off += d > 48;
        }
        result (magenta == 0, "render_scale 50: the frame fills the window");
        result (memcmp (half.px, full, npx) != 0, "render_scale 50: it is not the full-size frame");
        printf ("   render_scale 50: %.2f %% of the pixels differ by more than 48/255 (edges)\n",
                100.0 * off / (npx / 4));
        result (off * 100 < 15 * (npx / 4), "render_scale 50: and close to it (edges aside)");
        half.c->set_render_scale (half.c, 100);
        run_frame (&half, FALSE);
        run_frame (&half, FALSE);
        result (!memcmp (half.px, full, npx), "render_scale back to 100 (live): the full-size frame again");
    } else {
        result (FALSE, "scene loads with render_scale 50");
    }
    run_close (&half);
    g_free (first);
    g_free (full);
    rm_rf (adir);
    g_free (adir);
}

/* two solid layers, the upper one with a blend mode no GL blend state
 * gives (it is drawn by a shader that reads a copy of the frame under it) */
static char *make_blend_scene (void)
{
    char *dir = g_dir_make_tmp ("walpe-scene-frames-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0.10 0.20 0.30\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[");
    layer (s, 1, "160 90 0", "320 180", "0.2 0.5 0.3", 1.0, "0 0 0", "");
    layer (s, 2, "160 90 0", "300 160", "0.9 0.6 0.1", 0.7, "0 0 0", ",\"colorBlendMode\":9");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"scene frames test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    return dir;
}

static void tex_now (int *n, gint64 *bytes)
{
    gint64 compressed;
    walpe_gl_tex_totals (n, bytes, &compressed);
}

/* ---- 5. The copy of the frame a blend-mode layer works on belongs to the
 * render thread, not to a scene. It has to go with the scene's own targets
 * (pause) and with the scene: left alone it is a frame of RGBA8 that every
 * wallpaper shown afterwards carries along. */
static void blend_copy_checks (void)
{
    gsize npx = (gsize) W * H * 4;
    const gint64 copy = 300 * 160 * 4;   /* at least: the layer's rectangle */
    char *dir = make_blend_scene ();
    Run r = { 0 };
    int n0 = 0, n1 = 0, n2 = 0, n3 = 0;
    gint64 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    tex_now (&n0, &b0);
    if (!dir || !run_open (&r, dir, 0)) {
        result (FALSE, "blend scene loads");
        return;
    }
    gboolean drawn = run_frame (&r, FALSE);
    guint8 *first = g_malloc (npx);
    memcpy (first, r.px, npx);
    tex_now (&n1, &b1);
    result (drawn && b1 >= b0 + copy, "blend layer: drawn from a copy of the frame under it");
    r.c->set_paused (r.c, TRUE);
    tex_now (&n2, &b2);
    printf ("   textures: %d / %" G_GINT64_FORMAT " bytes before the scene, %d / %" G_GINT64_FORMAT
            " drawing, %d / %" G_GINT64_FORMAT " paused\n", n0, b0, n1, b1, n2, b2);
    result (b2 < b0 + copy, "blend layer: a paused scene does not keep the copy");
    r.c->set_paused (r.c, FALSE);
    gboolean again = run_frame (&r, TRUE);
    result (again && !memcmp (first, r.px, npx), "blend layer: same frame after pause / resume");
    run_close (&r);
    tex_now (&n3, &b3);
    printf ("   textures: %d / %" G_GINT64_FORMAT " bytes after the scene\n", n3, b3);
    result (n3 == n0 && b3 == b0, "blend layer: a freed scene leaves no texture behind");
    g_free (first);
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
    scene_checks ();
    blend_copy_checks ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
