/* postfx_gl_test.c - scene bloom and camera shake as the scene renderer
 * draws them.
 *
 * Generated scenes (solid colour layers: no texture files, no fonts) are
 * rendered on Mesa's surfaceless platform with pbuffers as the window.
 *
 *  1. A bloom that is not drawn leaves the frame alone, byte for byte:
 *     the switch off, post-processing off, strength 0, threshold 1, a black
 *     tint, and a scene whose brightest colour is below the threshold all
 *     give the picture of the same scene without bloom - and make no copy
 *     of the frame.
 *  2. One bright square on black: it glows into its surroundings and not
 *     beyond the reach of the blur; the glow falls off with the distance,
 *     is the same to the left and right, above and below; strength and
 *     tint do what their names say; the frame in the window is copied once,
 *     at half size, and four draws are added.
 *  3. A still scene with bloom is as idle as without (nothing is drawn
 *     while nothing changes); flipping the switch or the post-processing
 *     level draws exactly one frame, and flipping back gives the earlier
 *     picture byte for byte.
 *  4. Render sizes: 720p, 1440p, 4K and odd sizes draw without a GL error;
 *     the glow reaches the same number of pixels at each (the standard
 *     chain's rule); at render scale 50 it keeps its reach on the monitor
 *     and the frame, already a texture, is not copied; a frame too small
 *     for bloom is left alone.
 *  5. "Skip hidden areas": inside the visible boxes a frame with bloom has
 *     the pixels of the unclipped frame (the glow of a square that itself
 *     is hidden still reaches in).
 *  6. Camera shake: at a fixed instant of the scene's clock the picture is
 *     moved by the offset camshake.c gives, the same on every load; with
 *     the clock running every tick draws and no copy of static layers is
 *     made per frame; the switch off gives the picture of the scene
 *     without shake and an idle scene.
 *
 * The whole test runs a second time with WALPE_BLOOM_COPY=full (the 1:1
 * copy a multisampled window needs): every statement about the picture
 * holds for both ways of reading the frame.
 *
 * Skipped (exit status 77, with a line that says why) without EGL
 * surfaceless / GL 3.3.
 * `meson test -C build postfx-gl postfx-gl-fullcopy` */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/quality.h"
#include "common/region.h"
#include "engine/camshake.h"
#include "engine/clip.h"
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/features.h"

static int checks, fails;

static void result (gboolean ok, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static void result (gboolean ok, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    char *what = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_free (what);
}

/* ---------------------------------------------------------- GL plumbing */

static EGLDisplay dpy;
static EGLConfig ecfg;
static EGLContext ctx;

static EGLSurface surface_new (int w, int h)
{
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface (dpy, ecfg, pa);
    if (s == EGL_NO_SURFACE)
        return s;
    EGLint gw = 0, gh = 0;
    eglQuerySurface (dpy, s, EGL_WIDTH, &gw);
    eglQuerySurface (dpy, s, EGL_HEIGHT, &gh);
    if (gw != w || gh != h) {
        eglDestroySurface (dpy, s);
        return EGL_NO_SURFACE;
    }
    return s;
}

/* blits and draws of one gl_render (the epoxy pointers are wrapped once
 * they are resolved) */
static void (*real_draw_arrays) (GLenum, GLint, GLsizei);
static void (*real_blit) (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static int n_draws, n_blits;
static struct { int sw, sh, dw, dh; } last_blit;

static void count_draw_arrays (GLenum mode, GLint first, GLsizei count)
{
    n_draws++;
    real_draw_arrays (mode, first, count);
}

static void count_blit (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,
                        GLint dy1, GLbitfield mask, GLenum filter)
{
    n_blits++;
    last_blit.sw = abs (sx1 - sx0);
    last_blit.sh = abs (sy1 - sy0);
    last_blit.dw = abs (dx1 - dx0);
    last_blit.dh = abs (dy1 - dy0);
    real_blit (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

static void wrap_counters (void)
{
    /* one call of each resolves its pointer (the calls themselves do
     * nothing or fail harmlessly) */
    glDrawArrays (GL_TRIANGLES, 0, 0);
    glBlitFramebuffer (0, 0, 1, 1, 0, 0, 1, 1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    while (glGetError () != GL_NO_ERROR)
        ;
    if (epoxy_glDrawArrays != count_draw_arrays) {
        real_draw_arrays = epoxy_glDrawArrays;
        epoxy_glDrawArrays = count_draw_arrays;
    }
    if (epoxy_glBlitFramebuffer != count_blit) {
        real_blit = epoxy_glBlitFramebuffer;
        epoxy_glBlitFramebuffer = count_blit;
    }
}

static int gl_errors;
static gboolean full_copy;   /* WALPE_BLOOM_COPY=full: the window is copied 1:1 */
#define COPY_SIDE(n) (full_copy ? (n) : ((n) + 1) / 2)

static void drain_gl_errors (void)
{
    for (int i = 0; i < 64 && glGetError () != GL_NO_ERROR; i++)
        gl_errors++;
}

/* ---------------------------------------------------------------- scenes */

static void put (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
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

static GPtrArray *scene_dirs;   /* removed at the end */

/* A canvas of cw x ch scene units, cleared to black, with `backdrops`
 * black layers over all of it and one square of side sq and colour
 * sq_color at its centre. general = more members of "general" (each with
 * a leading comma) or "". */
static char *make_scene (int cw, int ch, int sq, const char *sq_color, int backdrops,
                         const char *general)
{
    char *dir = g_dir_make_tmp ("walpe-postfx-gl-XXXXXX", NULL);
    if (!dir)
        return NULL;
    g_ptr_array_add (scene_dirs, dir);
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    GString *s = g_string_new (NULL);
    g_string_append_printf (s,
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"cameraparallax\":false,"
        "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":%d,\"width\":%d},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"%s},\"objects\":[", ch, cw, general);
    int id = 1;
    for (int i = 0; i < backdrops; i++, id++)
        g_string_append_printf (s,
            "{\"id\":%d,\"name\":\"back%d\",\"image\":\"models/solid.json\",\"origin\":\"%d %d 0\","
            "\"size\":\"%d %d\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0 0 0\","
            "\"alpha\":1.0,\"visible\":true},", id, id, cw / 2, ch / 2, cw, ch);
    g_string_append_printf (s,
        "{\"id\":%d,\"name\":\"square\",\"image\":\"models/solid.json\",\"origin\":\"%d %d 0\","
        "\"size\":\"%d %d\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"%s\","
        "\"alpha\":1.0,\"visible\":true}]}", id, cw / 2, ch / 2, sq, sq, sq_color);
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"postfx test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    return dir;
}

#define BLOOM_ON ",\"bloom\":true,\"bloomstrength\":2.0,\"bloomthreshold\":0.65"

/* ------------------------------------------------------------------ runs */

static void redraw_cb (gpointer u) { (void) u; }

typedef struct {
    WalpeContent *c;
    guint8 *px;        /* what the window shows: w x h RGBA, row 0 = bottom */
    int w, h;
    int draws, blits;  /* of the last drawn frame */
    int blit_dw, blit_dh;
} Run;

static gboolean run_open (Run *r, const char *dir, int w, int h, const WalpeQuality *q)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir, .quality = q };
    memset (r, 0, sizeof (*r));
    r->c = dir ? walpe_content_new (dir, &opts, &err) : NULL;
    if (!r->c) {
        printf ("load failed: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return FALSE;
    }
    r->c->request_redraw = redraw_cb;
    if (!r->c->gl_init (r->c, &err)) {
        printf ("gl_init failed: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return FALSE;
    }
    r->w = w;
    r->h = h;
    r->px = g_malloc0 ((gsize) w * h * 4);
    return TRUE;
}

/* one tick as the daemon does it: a drawn frame goes to the screen (px)
 * and leaves an undefined back buffer, an unchanged one leaves everything
 * alone. TRUE = a frame was drawn. */
static gboolean run_frame (Run *r, gboolean lost)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, r->w, r->h);
    r->c->next_frame_us = 0;
    r->c->surface_lost = lost;
    r->c->frame_unchanged = FALSE;
    n_draws = n_blits = 0;
    memset (&last_blit, 0, sizeof (last_blit));
    r->c->gl_render (r->c, r->w, r->h, 0.0f, 0.0f);
    glFinish ();
    drain_gl_errors ();
    if (r->c->frame_unchanged)
        return FALSE;
    r->draws = n_draws;
    r->blits = n_blits;
    r->blit_dw = last_blit.dw;
    r->blit_dh = last_blit.dh;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, r->px);
    glDisable (GL_SCISSOR_TEST);
    glClearColor (1.0f, 0.0f, 1.0f, 1.0f);   /* "after the swap": garbage */
    glClear (GL_COLOR_BUFFER_BIT);
    drain_gl_errors ();
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
    memset (r, 0, sizeof (*r));
}

static guint8 *keep (const Run *r)
{
    return g_memdup2 (r->px, (gsize) r->w * r->h * 4);
}

/* the first frame of a scene: its pixels (g_free) or NULL; *out = the run's
 * counters when asked for */
static guint8 *shot (const char *dir, int w, int h, const WalpeQuality *q, Run *out)
{
    Run r;
    guint8 *px = NULL;
    if (run_open (&r, dir, w, h, q) && run_frame (&r, FALSE))
        px = keep (&r);
    if (out) {
        *out = r;
        out->c = NULL;
        out->px = NULL;
    }
    run_close (&r);
    return px;
}

static inline const guint8 *at (const guint8 *px, int w, int x, int y)
{
    return px + ((gsize) y * w + x) * 4;
}

/* colour channels equal everywhere (the window's alpha is not the scene's) */
static gboolean same_rgb (const guint8 *a, const guint8 *b, int w, int h)
{
    if (!a || !b)
        return FALSE;
    for (gsize i = 0; i < (gsize) w * h; i++)
        if (a[i * 4] != b[i * 4] || a[i * 4 + 1] != b[i * 4 + 1] || a[i * 4 + 2] != b[i * 4 + 2])
            return FALSE;
    return TRUE;
}

/* the white square on the row y of a picture without bloom: first and
 * last column of it; FALSE = none */
static gboolean square_span (const guint8 *px, int w, int y, int *x0, int *x1)
{
    *x0 = *x1 = -1;
    for (int x = 0; x < w; x++)
        if (at (px, w, x, y)[0] >= 250) {
            if (*x0 < 0)
                *x0 = x;
            *x1 = x;
        }
    return *x0 >= 0;
}

/* how far to the right of column x1 the picture `on` is brighter than
 * `off` on row y, in pixels without a gap */
static int reach_right (const guint8 *on, const guint8 *off, int w, int y, int x1)
{
    int n = 0;
    for (int x = x1 + 1; x < w; x++, n++)
        if (at (on, w, x, y)[0] <= at (off, w, x, y)[0])
            break;
    return n;
}

/* ---------------------------------------------------------------- 1.-3. */

#define W 320
#define H 192
#define SQ 32   /* the square: columns 144..175, rows 80..111 */

static void test_not_drawn (const guint8 *ref)
{
    WalpeQuality off;
    walpe_quality_preset (WALPE_PRESET_HIGH, &off);
    off.post = WALPE_POST_OFF;
    static const struct { const char *what, *general; gboolean sw_off, post_off; } v[] = {
        { "the switch is off", BLOOM_ON, TRUE, FALSE },
        { "post-processing is off", BLOOM_ON, FALSE, TRUE },
        { "strength 0", ",\"bloom\":true,\"bloomstrength\":0,\"bloomthreshold\":0.65", FALSE, FALSE },
        { "threshold 1", ",\"bloom\":true,\"bloomstrength\":2,\"bloomthreshold\":1.0", FALSE, FALSE },
        { "a black tint", BLOOM_ON ",\"bloomtint\":\"0 0 0\"", FALSE, FALSE },
        { "bloom bound to nothing and false", ",\"bloom\":false,\"bloomstrength\":4", FALSE, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (v); i++) {
        char *dir = make_scene (W, H, SQ, "1 1 1", 0, v[i].general);
        if (v[i].sw_off)
            walpe_feature_set ("bloom", FALSE);
        Run r;
        guint8 *px = shot (dir, W, H, v[i].post_off ? &off : NULL, &r);
        walpe_feature_set ("bloom", TRUE);
        result (same_rgb (px, ref, W, H), "%s: the frame is the frame without bloom, byte for byte",
                v[i].what);
        result (px && r.blits == 0, "%s: no copy of the frame is made (%d blits)", v[i].what, r.blits);
        g_free (px);
    }
    /* a scene in which nothing is brighter than the threshold */
    char *plain = make_scene (W, H, SQ, "0.6 0.6 0.6", 0, "");
    char *bloom = make_scene (W, H, SQ, "0.6 0.6 0.6", 0, BLOOM_ON);
    guint8 *a = shot (plain, W, H, NULL, NULL), *b = shot (bloom, W, H, NULL, NULL);
    result (a && at (a, W, W / 2, H / 2)[0] > 140 && at (a, W, W / 2, H / 2)[0] < 166,
            "the grey square is drawn (%d)", a ? at (a, W, W / 2, H / 2)[0] : -1);
    result (same_rgb (a, b, W, H),
            "nothing above the threshold: bloom adds nothing, the frame is the same byte for byte");
    g_free (a);
    g_free (b);
}

static void test_square (const guint8 *ref, const Run *ref_run)
{
    char *dir = make_scene (W, H, SQ, "1 1 1", 0, BLOOM_ON);
    Run r;
    guint8 *on = shot (dir, W, H, NULL, &r);
    if (!on) {
        result (FALSE, "the scene with bloom renders");
        return;
    }
    const int x0 = W / 2 - SQ / 2, x1 = W / 2 + SQ / 2 - 1, y0 = H / 2 - SQ / 2, y1 = H / 2 + SQ / 2 - 1;
    const int cy = H / 2;
    result (!same_rgb (on, ref, W, H), "bloom changes the picture of a scene with a white square");
    result (at (ref, W, x0, cy)[0] == 255 && at (ref, W, x0 - 1, cy)[0] == 0 &&
            at (ref, W, x1, cy)[0] == 255 && at (ref, W, x1 + 1, cy)[0] == 0,
            "without bloom: the square is columns %d..%d, black around it", x0, x1);
    /* the square itself stays white */
    gboolean white = TRUE;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            white &= at (on, W, x, y)[0] == 255 && at (on, W, x, y)[1] == 255 && at (on, W, x, y)[2] == 255;
    result (white, "the square stays white");
    /* it glows: right next to it, in every channel alike (a white source) */
    const guint8 *g = at (on, W, x1 + 3, cy);
    result (g[0] >= 8 && abs (g[0] - g[1]) <= 1 && abs (g[0] - g[2]) <= 1,
            "3 pixels beside the square the black is lit, grey: %d %d %d", g[0], g[1], g[2]);
    /* ... less with every pixel of distance */
    gboolean falls = TRUE;
    for (int x = x1 + 2; x < W; x++)
        falls &= at (on, W, x, cy)[0] <= at (on, W, x - 1, cy)[0];
    result (falls, "the glow falls off with the distance (row %d, to the right)", cy);
    falls = TRUE;
    for (int y = y1 + 2; y < H; y++)
        falls &= at (on, W, W / 2, y)[0] <= at (on, W, W / 2, y - 1)[0];
    result (falls, "the glow falls off with the distance (column %d, upwards)", W / 2);
    /* ... and not at all beyond the reach of the blur: 6 taps of 8 pixels,
     * the 4 x 4 average before and the linear enlargement after it */
    gboolean far_same = TRUE;
    int reach = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0;
            const int dy = y < y0 ? y0 - y : y > y1 ? y - y1 : 0;
            const gboolean lit = memcmp (at (on, W, x, y), at (ref, W, x, y), 3) != 0;
            if (lit)
                reach = MAX (reach, MAX (dx, dy));
            if (MAX (dx, dy) > 64)
                far_same &= !lit;
        }
    result (far_same, "more than 64 pixels from the square nothing changes");
    result (reach >= 16 && reach <= 64, "the glow reaches %d pixels", reach);
    /* the same on both sides and above and below (the square sits on the
     * grid of the smallest target, so the chain has nothing to favour) */
    int asym = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            asym = MAX (asym, abs (at (on, W, x, y)[0] - at (on, W, W - 1 - x, y)[0]));
            asym = MAX (asym, abs (at (on, W, x, y)[0] - at (on, W, x, H - 1 - y)[0]));
        }
    result (asym <= 1, "the glow is the same left / right and above / below (largest difference %d)", asym);
    /* what it costs here: one copy of the frame, at half size, and four draws */
    result (r.blits == 1 && r.blit_dw == COPY_SIDE (W) && r.blit_dh == COPY_SIDE (H),
            "the frame in the window is copied once, at %s size (%d blit(s), %dx%d)",
            full_copy ? "full" : "half", r.blits, r.blit_dw, r.blit_dh);
    result (r.draws == ref_run->draws + 4, "four draws are added (%d against %d without bloom)",
            r.draws, ref_run->draws);

    /* strength 1 gives half the glow of strength 2 */
    char *half = make_scene (W, H, SQ, "1 1 1", 0, ",\"bloom\":true,\"bloomstrength\":1.0,\"bloomthreshold\":0.65");
    guint8 *h1 = shot (half, W, H, NULL, NULL);
    result (h1 && abs (2 * at (h1, W, x1 + 3, cy)[0] - g[0]) <= 3,
            "strength 1 gives half the glow of strength 2 (%d against %d)",
            h1 ? at (h1, W, x1 + 3, cy)[0] : -1, g[0]);
    g_free (h1);
    /* a red tint: only red is added */
    char *red = make_scene (W, H, SQ, "1 1 1", 0, BLOOM_ON ",\"bloomtint\":\"1 0 0\"");
    guint8 *t = shot (red, W, H, NULL, NULL);
    gboolean only_red = t != NULL, some_red = FALSE;
    for (int y = 0; t && y < H; y++)
        for (int x = 0; x < W; x++) {
            only_red &= at (t, W, x, y)[1] == at (ref, W, x, y)[1] && at (t, W, x, y)[2] == at (ref, W, x, y)[2];
            some_red |= at (t, W, x, y)[0] > at (ref, W, x, y)[0];
        }
    result (only_red && some_red, "a red tint adds red and nothing else");
    result (t && abs (at (t, W, x1 + 3, cy)[0] - g[0]) <= 1, "... as much red as the white glow has");
    g_free (t);
    /* a square just above the threshold glows a little; the amount follows
     * how far above it is: 0.8 - 0.65 against 1 - 0.65, times the colour */
    char *dim = make_scene (W, H, SQ, "0.8 0.8 0.8", 0, BLOOM_ON);
    guint8 *d = shot (dim, W, H, NULL, NULL);
    const int dv = d ? at (d, W, x1 + 3, cy)[0] : -1;
    const double want = g[0] * (0.8 * 0.15) / 0.35;
    result (d && dv >= 1 && dv < g[0] && fabs (dv - want) <= 3.0,
            "a square of 0.8 glows by the share above the threshold (%d, expected about %.1f)", dv, want);
    g_free (d);
    g_free (on);
}

static void test_idle (const guint8 *ref)
{
    char *dir = make_scene (W, H, SQ, "1 1 1", 0, BLOOM_ON);
    Run r;
    if (!run_open (&r, dir, W, H, NULL)) {
        result (FALSE, "the scene with bloom loads");
        run_close (&r);
        return;
    }
    result (run_frame (&r, FALSE), "the first frame is drawn");
    guint8 *first = keep (&r);
    gboolean a = run_frame (&r, FALSE), b = run_frame (&r, FALSE);
    result (!a && !b, "a still scene with bloom is idle: the next two ticks draw nothing");
    result (run_frame (&r, TRUE) && same_rgb (r.px, first, W, H),
            "a lost surface draws the frame again, with the same pixels");

    walpe_feature_set ("bloom", FALSE);
    result (run_frame (&r, FALSE), "bloom switched off: a frame is drawn on the next tick");
    result (same_rgb (r.px, ref, W, H), "... the picture of the scene without bloom, byte for byte");
    result (!run_frame (&r, FALSE), "... and the scene is idle again");
    walpe_feature_set ("bloom", TRUE);
    result (run_frame (&r, FALSE) && same_rgb (r.px, first, W, H),
            "bloom switched on again: the earlier picture is back, byte for byte");
    result (!run_frame (&r, FALSE), "... and the scene is idle again");

    if (r.c->set_quality) {
        WalpeQuality q;
        walpe_quality_preset (WALPE_PRESET_HIGH, &q);
        q.post = WALPE_POST_REDUCED;
        r.c->set_quality (r.c, &q);
        result (run_frame (&r, FALSE), "post-processing reduced: a frame is drawn");
        const int x1 = W / 2 + SQ / 2 - 1, cy = H / 2;
        int diff = 0;
        for (gsize i = 0; i < (gsize) W * H * 4; i++)
            if ((i & 3) != 3)
                diff = MAX (diff, abs ((int) r.px[i] - (int) first[i]));
        result (at (r.px, W, x1 + 3, cy)[0] >= 4 && !same_rgb (r.px, ref, W, H),
                "reduced: the square still glows (%d beside it)", at (r.px, W, x1 + 3, cy)[0]);
        result (diff > 0 && diff <= 48, "reduced: close to the full chain, not the same (largest difference %d)",
                diff);
        result (r.blits == 1 && r.blit_dw == COPY_SIDE (W), "reduced: still one copy of the frame");
        q.post = WALPE_POST_OFF;
        r.c->set_quality (r.c, &q);
        result (run_frame (&r, FALSE) && same_rgb (r.px, ref, W, H),
                "post-processing off: the picture without bloom, byte for byte");
        result (r.blits == 0, "post-processing off: no copy of the frame");
        walpe_quality_preset (WALPE_PRESET_HIGH, &q);
        r.c->set_quality (r.c, &q);
        result (run_frame (&r, FALSE) && same_rgb (r.px, first, W, H),
                "back to the default level: the first picture, byte for byte");
        result (!run_frame (&r, FALSE), "... and idle");
    }
    g_free (first);
    run_close (&r);
}

/* -------------------------------------------------------------------- 4. */

/* on / off pictures of the bloom scene on a canvas of 320 x 180 at window
 * size w x h; *reach = how far right of the square the glow goes on the
 * row through its centre. FALSE = could not be rendered. */
static gboolean size_pair (int w, int h, int render_scale, int *reach, Run *on_run)
{
    char *dir = make_scene (320, 180, 32, "1 1 1", 0, BLOOM_ON);
    Run r;
    gboolean ok = FALSE;
    *reach = -1;
    if (run_open (&r, dir, w, h, NULL)) {
        if (render_scale && r.c->set_render_scale)
            r.c->set_render_scale (r.c, render_scale);
        if (run_frame (&r, FALSE)) {
            guint8 *on = keep (&r);
            if (on_run) {
                *on_run = r;
                on_run->c = NULL;
                on_run->px = NULL;
            }
            walpe_feature_set ("bloom", FALSE);
            if (run_frame (&r, FALSE)) {
                int x0, x1;
                const int cy = h / 2;
                if (square_span (r.px, w, cy, &x0, &x1)) {
                    *reach = reach_right (on, r.px, w, cy, x1);
                    /* the corner of the frame is far from everything */
                    ok = !memcmp (at (on, w, 0, 0), at (r.px, w, 0, 0), 3) &&
                         !memcmp (at (on, w, w - 1, h - 1), at (r.px, w, w - 1, h - 1), 3);
                }
            }
            walpe_feature_set ("bloom", TRUE);
            g_free (on);
        }
    }
    run_close (&r);
    return ok;
}

static void test_sizes (EGLSurface small)
{
    static const struct { int w, h; gboolean grid; } sz[] = {
        { 1280, 720, TRUE }, { 2560, 1440, TRUE }, { 3840, 2160, TRUE },
        { 1283, 721, FALSE }, { 641, 361, FALSE },
    };
    int lo = 1 << 30, hi = -1, r720 = -1;
    for (guint i = 0; i < G_N_ELEMENTS (sz); i++) {
        EGLSurface s = surface_new (sz[i].w, sz[i].h);
        if (s == EGL_NO_SURFACE || !eglMakeCurrent (dpy, s, s, ctx)) {
            printf ("note: no %dx%d pbuffer here: that size is not rendered\n", sz[i].w, sz[i].h);
            if (s != EGL_NO_SURFACE)
                eglDestroySurface (dpy, s);
            eglMakeCurrent (dpy, small, small, ctx);
            continue;
        }
        const int err0 = gl_errors;
        int reach = -1;
        Run on;
        gboolean ok = size_pair (sz[i].w, sz[i].h, 0, &reach, &on);
        result (ok, "%dx%d: bloom is drawn, the corners of the frame stay as they are", sz[i].w, sz[i].h);
        result (gl_errors == err0, "%dx%d: no GL error", sz[i].w, sz[i].h);
        result (reach >= 24 && reach <= 72, "%dx%d: the glow reaches %d pixels beside the square",
                sz[i].w, sz[i].h, reach);
        result (ok && on.blits == 1 && on.blit_dw == COPY_SIDE (sz[i].w) &&
                on.blit_dh == COPY_SIDE (sz[i].h),
                "%dx%d: one copy of the frame, %dx%d", sz[i].w, sz[i].h, on.blit_dw, on.blit_dh);
        if (sz[i].grid && reach >= 0) {
            lo = MIN (lo, reach);
            hi = MAX (hi, reach);
        }
        if (i == 0) {
            r720 = reach;
            /* the same monitor at render scale 50: the frame is a texture
             * of half the size - no copy for bloom, only the final
             * enlargement - and the glow keeps its reach on the monitor */
            int r50 = -1;
            ok = size_pair (sz[i].w, sz[i].h, 50, &r50, &on);
            result (ok, "1280x720 at render scale 50: bloom is drawn");
            result (ok && on.blits == 1 && on.blit_dw == sz[i].w && on.blit_dh == sz[i].h,
                    "render scale 50: the only blit is the enlargement to the window (%d blit(s), "
                    "last %dx%d)", on.blits, on.blit_dw, on.blit_dh);
            result (r50 >= 24 && r50 <= 96 && abs (r50 - r720) <= 28,
                    "render scale 50: the glow reaches %d monitor pixels (%d at full size)", r50, r720);
            result (gl_errors == err0, "render scale 50: no GL error");
        }
        eglMakeCurrent (dpy, small, small, ctx);
        eglDestroySurface (dpy, s);
    }
    if (hi >= 0)
        result (hi - lo <= 10, "720p / 1440p / 4K: the glow reaches the same number of pixels (%d..%d)",
                lo, hi);

    /* tiny windows: drawn without an error; below 16 pixels a side there
     * is no bloom */
    static const struct { int w, h; gboolean bloom; } tiny[] = {
        { 33, 17, TRUE }, { 17, 33, TRUE }, { 40, 15, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (tiny); i++) {
        EGLSurface s = surface_new (tiny[i].w, tiny[i].h);
        if (s == EGL_NO_SURFACE || !eglMakeCurrent (dpy, s, s, ctx)) {
            if (s != EGL_NO_SURFACE)
                eglDestroySurface (dpy, s);
            eglMakeCurrent (dpy, small, small, ctx);
            continue;
        }
        const int err0 = gl_errors;
        /* all of the window is the white square's neighbourhood here */
        char *plain = make_scene (320, 180, 60, "1 1 1", 0, "");
        char *bloom = make_scene (320, 180, 60, "1 1 1", 0, BLOOM_ON);
        Run rb;
        guint8 *a = shot (plain, tiny[i].w, tiny[i].h, NULL, NULL);
        guint8 *b = shot (bloom, tiny[i].w, tiny[i].h, NULL, &rb);
        result (a && b && gl_errors == err0, "%dx%d: drawn without a GL error", tiny[i].w, tiny[i].h);
        if (tiny[i].bloom)
            result (b && rb.blits == 1, "%dx%d: bloom runs (%d blit)", tiny[i].w, tiny[i].h, rb.blits);
        else
            result (same_rgb (a, b, tiny[i].w, tiny[i].h) && rb.blits == 0,
                    "%dx%d: too small for bloom, the frame is left alone", tiny[i].w, tiny[i].h);
        g_free (a);
        g_free (b);
        eglMakeCurrent (dpy, small, small, ctx);
        eglDestroySurface (dpy, s);
    }
}

/* -------------------------------------------------------------------- 5. */

static void test_clip (void)
{
    char *dir = make_scene (W, H, SQ, "1 1 1", 0, BLOOM_ON);
    guint8 *full = shot (dir, W, H, NULL, NULL);
    Run r;
    if (!full || !run_open (&r, dir, W, H, NULL)) {
        result (FALSE, "the scene for the clip test renders");
        g_free (full);
        return;
    }
    /* only the left 120 columns can be seen: the square (columns 144..175)
     * is hidden, its glow reaches into the visible part */
    const WalpeBox box = { 0, 0, 120, H };
    walpe_clip_set (&box, 1, W, H);
    gboolean drew = run_frame (&r, FALSE);
    walpe_clip_off ();
    gboolean same = drew, lit = FALSE;
    for (int y = 0; drew && y < H; y++)
        for (int x = 0; x < 120; x++) {
            same &= !memcmp (at (r.px, W, x, y), at (full, W, x, y), 3);
            lit |= at (full, W, x, y)[0] > 0;
        }
    result (lit, "the glow of the hidden square reaches into the visible part");
    result (same, "inside the visible box the clipped frame has the pixels of the whole frame");
    /* nothing of the window can be seen: no bloom is added to such a
     * frame, so no layer is drawn in full for it either (as in a scene
     * without bloom, whose layers are all left out then) */
    walpe_clip_set (NULL, 0, W, H);
    gboolean hidden = run_frame (&r, FALSE);
    walpe_clip_off ();
    result (hidden && r.draws == 0 && r.blits == 0,
            "nothing visible: no layer is drawn and no copy is made (%d draws, %d blits)", r.draws, r.blits);
    run_close (&r);
    g_free (full);
}

/* -------------------------------------------------------------------- 6. */

static gboolean centroid (const guint8 *px, int w, int h, double *cx, double *cy)
{
    double sx = 0, sy = 0, n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (at (px, w, x, y)[0] >= 128) {
                sx += x + 0.5;
                sy += y + 0.5;
                n++;
            }
    if (n < 1)
        return FALSE;
    *cx = sx / n;
    *cy = sy / n;
    return TRUE;
}

#define SHAKE_ON ",\"camerashake\":true,\"camerashakeamplitude\":20.0,\"camerashakeroughness\":1.0," \
                 "\"camerashakespeed\":3.0"

static void test_shake (const guint8 *rest)
{
    /* ---- a fixed instant of the scene's clock */
    const double T = 1.0;
    float off[2];
    walpe_camera_shake (T, 20.0f, 1.0f, 3.0f, (float) W, (float) H, off);
    result (fabsf (off[0]) > 4 && fabsf (off[1]) > 4 && fabsf (off[0]) < 64 && fabsf (off[1]) < 38.4f,
            "at %.1f s the view is %.1f, %.1f scene units from rest", T, (double) off[0], (double) off[1]);
    g_setenv ("WALPE_FX_TIME", "1.0", TRUE);
    char *dir = make_scene (W, H, SQ, "1 1 1", 0, SHAKE_ON);
    Run r;
    if (!run_open (&r, dir, W, H, NULL) || !run_frame (&r, FALSE)) {
        result (FALSE, "the scene with camera shake renders");
        run_close (&r);
        g_unsetenv ("WALPE_FX_TIME");
        return;
    }
    guint8 *moved = keep (&r);
    double cx = 0, cy = 0;
    gboolean found = centroid (moved, W, H, &cx, &cy);
    /* the view centre moved by off: the picture moves the other way (the
     * window shows 1 pixel per scene unit, rows count upwards like y) */
    result (found && fabs (cx - (W / 2.0 - off[0])) <= 0.75 && fabs (cy - (H / 2.0 - off[1])) <= 0.75,
            "the square is drawn at %.1f, %.1f (expected %.1f, %.1f)", cx, cy, W / 2.0 - off[0],
            H / 2.0 - off[1]);
    result (!same_rgb (moved, rest, W, H), "the picture differs from the scene at rest");
    result (!run_frame (&r, FALSE), "a clock that stands still: the next tick draws nothing");
    /* the same instant on another load: the same picture (fixed seeds) */
    guint8 *again = shot (dir, W, H, NULL, NULL);
    result (same_rgb (again, moved, W, H), "the same instant on a second load: the same picture, byte for byte");
    g_free (again);
    /* the switch */
    walpe_feature_set ("camera_shake", FALSE);
    result (run_frame (&r, FALSE) && same_rgb (r.px, rest, W, H),
            "camera shake switched off: the picture of the scene at rest, byte for byte");
    result (!run_frame (&r, FALSE), "... and the scene is idle");
    walpe_feature_set ("camera_shake", TRUE);
    result (run_frame (&r, FALSE) && same_rgb (r.px, moved, W, H),
            "switched on again: the moved picture is back, byte for byte");
    run_close (&r);
    g_free (moved);
    g_unsetenv ("WALPE_FX_TIME");

    /* amplitude 0 and speed 0 are no shake: the scene is the scene at rest
     * and does not ask for frames */
    static const char *const none[] = {
        ",\"camerashake\":true,\"camerashakeamplitude\":0,\"camerashakespeed\":3.0",
        ",\"camerashake\":true,\"camerashakeamplitude\":20.0,\"camerashakespeed\":0",
    };
    for (guint i = 0; i < G_N_ELEMENTS (none); i++) {
        char *d0 = make_scene (W, H, SQ, "1 1 1", 0, none[i]);
        if (run_open (&r, d0, W, H, NULL)) {
            gboolean first = run_frame (&r, FALSE);
            result (first && same_rgb (r.px, rest, W, H), "%s: the picture of the scene at rest",
                    i ? "speed 0" : "amplitude 0");
            g_usleep (3000);
            result (!run_frame (&r, FALSE) && r.c->next_frame_us == 0, "%s: no frames are asked for",
                    i ? "speed 0" : "amplitude 0");
        } else {
            result (FALSE, "a scene with a shake that cannot move loads");
        }
        run_close (&r);
    }

    /* ---- the clock running: every tick draws; three layers that stand
     * still under the view are not copied for a bake that the next frame
     * could not use */
    char *live = make_scene (W, H, SQ, "1 1 1", 3, SHAKE_ON);
    if (!run_open (&r, live, W, H, NULL)) {
        result (FALSE, "the running scene with camera shake loads");
        run_close (&r);
        return;
    }
    int drawn = 0, blits = 0;
    gint64 next = 0;
    for (int f = 0; f < 8; f++) {
        g_usleep (3000);
        if (run_frame (&r, FALSE)) {
            drawn++;
            if (f >= 3)
                blits += r.blits;
        }
        next = r.c->next_frame_us;
    }
    result (drawn == 8, "the clock running: all of 8 ticks draw a frame (%d)", drawn);
    result (next > 0, "the scene asks for the next frame by itself");
    result (blits == 0, "no frame is copied while the view sways (%d blits in 5 frames)", blits);
    walpe_feature_set ("camera_shake", FALSE);
    gboolean d1 = run_frame (&r, FALSE);
    result (d1 && same_rgb (r.px, rest, W, H), "switched off while running: the picture at rest");
    /* nothing else moves in this scene: without the sway it has no frame
     * clock, as the same scene without camera shake has none */
    result (r.c->next_frame_us == 0, "... and the scene asks for no more frames");
    g_usleep (3000);
    gboolean d2 = run_frame (&r, FALSE), d3 = run_frame (&r, FALSE);
    result (!d2 && !d3, "... and nothing is drawn any more");
    result (r.c->next_frame_us == 0, "... nor asked for");
    walpe_feature_set ("camera_shake", TRUE);
    g_usleep (3000);
    result (run_frame (&r, FALSE), "switched on again: frames are drawn again");
    result (r.c->next_frame_us > 0, "... and asked for again");
    run_close (&r);
}

/* bloom and shake in one scene: the glow belongs to the moved picture */
static void test_both (void)
{
    g_setenv ("WALPE_FX_TIME", "1.0", TRUE);
    float off[2];
    walpe_camera_shake (1.0, 20.0f, 1.0f, 3.0f, (float) W, (float) H, off);
    char *dir = make_scene (W, H, SQ, "1 1 1", 0, BLOOM_ON SHAKE_ON);
    guint8 *px = shot (dir, W, H, NULL, NULL);
    g_unsetenv ("WALPE_FX_TIME");
    if (!px) {
        result (FALSE, "the scene with bloom and camera shake renders");
        return;
    }
    /* the brightest glow sits right beside the moved square */
    const int sx = (int) lround (W / 2.0 - off[0]), sy = (int) lround (H / 2.0 - off[1]);
    const int gx = sx + SQ / 2 + 3;
    result (gx < W && sy >= 0 && sy < H && at (px, W, gx, sy)[0] >= 8 && at (px, W, gx, sy)[0] < 250,
            "the glow follows the moved square (%d beside it)", gx < W ? at (px, W, gx, sy)[0] : -1);
    /* and where the square would be at rest there is at most its far glow */
    result (at (px, W, W / 2, H / 2)[0] < 128 || (abs (sx - W / 2) < SQ / 2 && abs (sy - H / 2) < SQ / 2),
            "nothing bright is left at the place of rest");
    g_free (px);
}

int main (void)
{
    setvbuf (stdout, NULL, _IONBF, 0);
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skipped (needs the EGL surfaceless platform)\n");
        return 77;
    }
    dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[128];
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, cfgs, 128, &nc) || nc < 1) {
        printf ("skipped (needs an EGL pbuffer config with desktop GL)\n");
        return 77;
    }
    ecfg = cfgs[0];
    for (int i = 0; i < nc; i++) {   /* an 8-bit one without multisampling, as the daemon's window */
        EGLint red = 0, samples = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &red);
        eglGetConfigAttrib (dpy, cfgs[i], EGL_SAMPLES, &samples);
        if (red == 8 && samples == 0) {
            ecfg = cfgs[i];
            break;
        }
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    ctx = eglCreateContext (dpy, ecfg, EGL_NO_CONTEXT, ca);
    EGLSurface small = ctx != EGL_NO_CONTEXT ? surface_new (W, H) : EGL_NO_SURFACE;
    if (ctx == EGL_NO_CONTEXT || small == EGL_NO_SURFACE || !eglMakeCurrent (dpy, small, small, ctx)) {
        printf ("skipped (needs a GL 3.3 context on a pbuffer)\n");
        return 77;
    }
    char *tmp = g_dir_make_tmp ("walpe-postfx-gl-home-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    static const char *const unset[] = {
        "WALPE_FEATURES", "WALPE_BLOOM_FORMAT", "WALPE_SHAKE_AMOUNT", "WALPE_SHAKE_RATE",
        "WALPE_SHAKE_COVER", "WALPE_FX_TIME", "WALPE_BAKE", "WALPE_FRAME_SKIP", "WALPE_RENDER_SCALE",
        "WALPE_PROP_PROBE", "WALPE_SKIP_IDS", "WALPE_NOFX",
    };
    for (guint i = 0; i < G_N_ELEMENTS (unset); i++)
        g_unsetenv (unset[i]);
    full_copy = !g_strcmp0 (g_getenv ("WALPE_BLOOM_COPY"), "full");
    printf ("the frame in the window is read through a %s copy\n", full_copy ? "1:1" : "half-size");
    scene_dirs = g_ptr_array_new_with_free_func (g_free);
    walpe_glutil_init ();
    wrap_counters ();

    /* the scene every picture is compared with: the white square on black,
     * no bloom, no shake */
    char *plain = make_scene (W, H, SQ, "1 1 1", 0, "");
    Run ref_run;
    guint8 *ref = shot (plain, W, H, NULL, &ref_run);
    if (!ref) {
        result (FALSE, "the plain scene renders");
    } else {
        result (ref_run.blits == 0, "the plain scene makes no copy of its frame");
        test_not_drawn (ref);
        test_square (ref, &ref_run);
        test_idle (ref);
        test_sizes (small);
        test_clip ();
        test_shake (ref);
        test_both ();
    }
    result (gl_errors == 0, "no GL error in the whole run (%d)", gl_errors);
    g_free (ref);

    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, small);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    for (guint i = 0; i < scene_dirs->len; i++)
        rm_rf (g_ptr_array_index (scene_dirs, i));
    g_ptr_array_unref (scene_dirs);
    rm_rf (tmp);
    g_free (tmp);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
