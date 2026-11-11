/* texload_test.c - how a layer picture gets to the GPU (engine/glutil.c,
 * common/tex.c, the first-frame load of engine/scenenode.c):
 *  1. walpe_gl_tex_halve_cpu is the 2x2 box with the rounding of a
 *     GL_LINEAR blit to half size, for 4, 2 and 1 bytes per texel;
 *  2. a reduced level of a file without stored levels is reduced before
 *     the upload: the full-size texture never exists (peak GL bytes), and
 *     the texels are those of the GL blit of the full texture;
 *     an odd-sized picture keeps the GL blit, unless WALPE_TEX_CPU_REDUCE=2
 *     (the test "texload-odd" runs this file with it);
 *  3. a PNG in a container: its size from the picture's header alone, the
 *     decode on a worker thread (walpe_gl_tex_predecode) picked up by the
 *     load, reduced by the worker when the load was announced, decoded
 *     again when the load asks for something else, dropped when cancelled,
 *     not started when the cache already holds the file;
 *  4. without a share group a reduced copy is made from the caller's own
 *     full-size texture, not by decoding the file again (the file is
 *     deleted first);
 *  5. a scene with PNG layers shown at half size: drawn right, nothing
 *     left to decode after the first frame, and at no moment a full-size
 *     texture on the GPU.
 * Needs a GL 3.3 context with a pbuffer (Mesa surfaceless platform); skips
 * itself without one. Writes its files to temporary directories.
 * "The texels of the GL blit" is asked bit for bit only of a renderer whose
 * half-size blit has the rounding of walpe_gl_tex_halve_cpu, which the test
 * asks the renderer itself (blit_rounding): Mesa's software renderer has it
 * on x86 with SSSE3 and not on aarch64, where its 8-bit filter truncates. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <string.h>
#include "engine/glutil.h"
#include "engine/content.h"
#include "common/tex.h"
#include "common/premult.h"

static int fails, checks;
static gboolean exact_driver;   /* the half-size blit rounds as the CPU reduction does: bit for bit */
static int blit_slack = 1;      /* else: what one halving may differ by, in 1/255 */

static void check (gboolean ok, const char *what)
{
    checks++;
    printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

/* a TEXB0003 container with one raw level: pixels, or a picture file */
static char *write_tex (const char *dir, const char *name, int w, int h, const guint8 *data, gsize len)
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
    put32 (a, (guint32) len);
    put32 (a, (guint32) len);
    g_byte_array_append (a, data, (guint) len);
    char *path = g_build_filename (dir, name, NULL);
    char *d = g_path_get_dirname (path);
    g_mkdir_with_parents (d, 0700);
    g_free (d);
    g_file_set_contents (path, (const char *) a->data, a->len, NULL);
    g_byte_array_unref (a);
    return path;
}

static guint64 rs = 88172645463325252ull;
static guint8 rnd8 (void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (guint8) (rs >> 24);
}

/* kind 0: colour + alpha noise, 1: opaque gray noise, 2: gray + alpha noise, 3: one opaque colour */
static guint8 *noise_rgba (int w, int h, int kind)
{
    guint8 *d = g_malloc ((gsize) w * h * 4);
    for (gsize i = 0; i < (gsize) w * h; i++) {
        guint8 r = rnd8 (), g = rnd8 (), b = rnd8 (), a = rnd8 ();
        if (kind == 1 || kind == 2)
            g = b = r;
        if (kind == 1)
            a = 255;
        if (kind == 3) {
            r = 200;
            g = 100;
            b = 50;
            a = 255;
        }
        d[i * 4] = r;
        d[i * 4 + 1] = g;
        d[i * 4 + 2] = b;
        d[i * 4 + 3] = a;
    }
    return d;
}

static guint8 *png_of (const guint8 *rgba, int w, int h, gboolean alpha, gsize *len)
{
    guint8 *src = g_malloc ((gsize) w * h * (alpha ? 4 : 3));
    for (gsize i = 0; i < (gsize) w * h; i++)
        memcpy (src + i * (alpha ? 4 : 3), rgba + i * 4, alpha ? 4 : 3);
    GdkPixbuf *pb = gdk_pixbuf_new_from_data (src, GDK_COLORSPACE_RGB, alpha, 8, w, h,
                                              w * (alpha ? 4 : 3), NULL, NULL);
    gchar *buf = NULL;
    gsize n = 0;
    if (!gdk_pixbuf_save_to_buffer (pb, &buf, &n, "png", NULL, NULL))
        n = 0;
    g_object_unref (pb);
    g_free (src);
    *len = n;
    return (guint8 *) buf;
}

/* level 0 of a texture as stored, BGRA order */
static guint8 *readback (GLuint tex, int *w, int *h)
{
    GLint tw = 0, th = 0;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    guint8 *buf = g_malloc0 ((gsize) MAX (tw, 1) * MAX (th, 1) * 4 + 16);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    *w = tw;
    *h = th;
    return buf;
}

static int max_diff (const guint8 *a, const guint8 *b, gsize n)
{
    int m = 0;
    for (gsize i = 0; i < n; i++) {
        int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (d > m)
            m = d;
    }
    return m;
}

/* the same texels from the two ways of making them, `halvings` steps below
 * the picture */
static gboolean same_texture (GLuint a, GLuint b, int want_w, int want_h, int halvings)
{
    int aw, ah, bw, bh;
    guint8 *pa = readback (a, &aw, &ah), *pb = readback (b, &bw, &bh);
    gboolean ok = aw == want_w && ah == want_h && bw == want_w && bh == want_h;
    if (ok) {
        int m = max_diff (pa, pb, (gsize) aw * ah * 4);
        ok = exact_driver ? m == 0 : m <= blit_slack * halvings;
        if (!ok)
            printf ("     largest difference %d/255\n", m);
    }
    g_free (pa);
    g_free (pb);
    return ok;
}

static void wait_decodes (gboolean (*done) (const WalpeTexDecodeStats *), int ms)
{
    for (int i = 0; i < ms; i++) {
        WalpeTexDecodeStats st;
        walpe_gl_tex_predecode_stats (&st);
        if (done (&st))
            return;
        g_usleep (1000);
    }
}
static guint want_started;
static gboolean st_started (const WalpeTexDecodeStats *st) { return st->started >= want_started; }
static gboolean st_idle (const WalpeTexDecodeStats *st) { return st->pending == 0 && st->bytes == 0; }
static gboolean st_small (const WalpeTexDecodeStats *st) { return st->bytes > 0 && st->bytes < 256 * 128 * 4; }

static void halve_ref (const guint8 *s, int w, int h, int bpp, guint8 *o)
{
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++)
            for (int c = 0; c < bpp; c++) {
                int a = s[((gsize) (2 * y) * w + 2 * x) * bpp + c];
                int b = s[((gsize) (2 * y) * w + 2 * x + 1) * bpp + c];
                int cc = s[((gsize) (2 * y + 1) * w + 2 * x) * bpp + c];
                int d = s[((gsize) (2 * y + 1) * w + 2 * x + 1) * bpp + c];
                o[((gsize) y * (w / 2) + x) * bpp + c] =
                    (guint8) ((((a + b + 1) >> 1) + ((cc + d + 1) >> 1) + 1) >> 1);
            }
}

/* The largest difference, in 1/255, between a GL_LINEAR blit to exactly
 * half size - what the engine's reduction on the GPU is - and the 2x2 box
 * with the rounding of walpe_gl_tex_halve_cpu, on 64x64 texels of noise.
 * 0: the renderer's blit rounds like the CPU reduction (Mesa's software
 * renderer where its 8-bit filter has a rounding multiply: x86 with
 * SSSE3). Its filter for every other CPU (aarch64; x86 without SSSE3)
 * truncates both interpolations and comes out up to 2/255 lower. */
static int blit_rounding (void)
{
    enum { N = 64 };
    guint8 *px = g_malloc (N * N * 4), *want = g_malloc (N * N), *got = g_malloc0 (N * N);
    guint32 r = 12345;      /* (its own numbers: the pictures of the checks stay what they were) */
    for (int i = 0; i < N * N * 4; i++) {
        r = r * 1664525u + 1013904223u;
        px[i] = (guint8) (r >> 24);
    }
    halve_ref (px, N, N, 4, want);
    GLint pr = 0, pd = 0;
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &pr);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &pd);
    GLuint t[2], fb[2];
    glGenTextures (2, t);
    glGenFramebuffers (2, fb);
    for (int i = 0; i < 2; i++) {      /* (rows of whole RGBA texels: any pixel-store alignment fits) */
        glBindTexture (GL_TEXTURE_2D, t[i]);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, N >> i, N >> i, 0, GL_RGBA, GL_UNSIGNED_BYTE, i ? NULL : px);
    }
    glBindFramebuffer (GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, fb[1]);
    glFramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[1], 0);
    glBlitFramebuffer (0, 0, N, N, 0, 0, N / 2, N / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, fb[1]);
    glReadPixels (0, 0, N / 2, N / 2, GL_RGBA, GL_UNSIGNED_BYTE, got);
    int m = max_diff (want, got, N * N);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint) pr);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, (GLuint) pd);
    glDeleteFramebuffers (2, fb);
    glDeleteTextures (2, t);
    glBindTexture (GL_TEXTURE_2D, 0);
    g_free (px);
    g_free (want);
    g_free (got);
    return m;
}

static void cache_checks (const char *dir)
{
    WalpeTexInfo ti, ti0;
    gint64 live = 0, peak = 0;
    walpe_gl_set_shared (TRUE);

    /* 1. the CPU box, every width class of the vector loops and their tails */
    static const int sizes[][2] = { { 2, 2 }, { 6, 4 }, { 38, 10 }, { 66, 2 }, { 256, 128 }, { 254, 6 } };
    for (int bpp = 1; bpp <= 4; bpp = bpp == 2 ? 4 : bpp + 1) {
        gboolean ok = TRUE;
        for (guint i = 0; i < G_N_ELEMENTS (sizes); i++) {
            int w = sizes[i][0], h = sizes[i][1];
            gsize n = (gsize) w * h * bpp;
            guint8 *d = g_malloc (n), *ref = g_malloc (n);
            for (gsize k = 0; k < n; k++)
                d[k] = rnd8 ();
            halve_ref (d, w, h, bpp, ref);
            walpe_gl_tex_halve_cpu (d, w, h, bpp);
            ok = ok && !memcmp (d, ref, (gsize) (w / 2) * (h / 2) * bpp);
            g_free (d);
            g_free (ref);
        }
        char *what = g_strdup_printf ("halving on the CPU, %d byte(s) per texel: avg (avg (a, b), avg (c, d))", bpp);
        check (ok, what);
        g_free (what);
    }

    /* 2. reduced before the upload == the GL blit of the full texture */
    for (int kind = 0; kind < 3; kind++) {
        guint8 *px = noise_rgba (256, 128, kind);
        char *name = g_strdup_printf ("noise%d.tex", kind);
        char *f = write_tex (dir, name, 256, 128, px, 256 * 128 * 4);
        int bpt = kind == 1 ? 1 : kind == 2 ? 2 : 4;
        walpe_gl_tex_live_stats (&live, NULL, TRUE);
        GLuint c1 = walpe_gl_tex_cached (f, WALPE_GL_TEX_LEVEL (1), GL_REPEAT, TRUE, &ti);
        walpe_gl_tex_live_stats (NULL, &peak, FALSE);
        char *what = g_strdup_printf ("LEVEL(1) of %s: %d bytes on the GPU at most, never the full picture",
                                      name, 128 * 64 * bpt);
        check (c1 && ti.phys_w == 128 && ti.phys_h == 64 && ti.lod == 1 && ti.width == 256 &&
               ti.bytes == 128 * 64 * bpt && peak - live == 128 * 64 * bpt, what);
        g_free (what);
        GLuint c2 = walpe_gl_tex_cached (f, WALPE_GL_TEX_LEVEL (2), GL_REPEAT, TRUE, &ti);
        /* the GL way: the full texture is in the cache, its copies are blits */
        GLuint g0 = walpe_gl_tex_cached (f, 0, GL_CLAMP_TO_EDGE, TRUE, &ti0);
        GLuint g1 = walpe_gl_tex_cached (f, WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, TRUE, &ti);
        GLuint g2 = walpe_gl_tex_cached (f, WALPE_GL_TEX_LEVEL (2), GL_CLAMP_TO_EDGE, TRUE, &ti);
        what = exact_driver ? g_strdup ("  ... the same texels as the GL blit of the full texture (bit for bit)")
                            : g_strdup_printf ("  ... the same texels as the GL blit of the full texture "
                                               "(+-%d for each halving)", blit_slack);
        check (c2 && g0 && g1 && g2 && g1 != c1 && same_texture (c1, g1, 128, 64, 1) &&
               same_texture (c2, g2, 64, 32, 2), what);
        g_free (what);
        GLint fa = 0, fb = 0;
        glBindTexture (GL_TEXTURE_2D, c1);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fa);
        glBindTexture (GL_TEXTURE_2D, g1);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fb);
        check (fa == fb && fa == (kind == 1 ? GL_R8 : kind == 2 ? GL_RG8 : GL_RGBA8),
               "  ... and the same channel layout");
        if (kind == 0) {
            /* 4. no share group: the copy comes from the caller's full texture */
            walpe_gl_tex_release (g1);
            walpe_gl_set_shared (FALSE);
            GLuint o0 = walpe_gl_tex_cached (f, 0, GL_CLAMP_TO_EDGE, TRUE, &ti0);
            g_unlink (f);
            GLuint o1 = walpe_gl_tex_cached_own (f, WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, TRUE, &ti,
                                                 o0, &ti0);
            check (o0 && o1 && o1 != o0 && ti.phys_w == 128 && ti.lod == 1 && ti.width == 256 &&
                   same_texture (o1, c1, 128, 64, 1),
                   "no share group: a reduced copy from the caller's own full texture (the file is gone)");
            walpe_gl_tex_release (o0);
            walpe_gl_tex_release (o1);
            walpe_gl_set_shared (TRUE);
        } else {
            walpe_gl_tex_release (g1);
        }
        walpe_gl_tex_release (c1);
        walpe_gl_tex_release (c2);
        walpe_gl_tex_release (g0);
        walpe_gl_tex_release (g2);
        g_free (name);
        g_free (f);
        g_free (px);
    }

    /* 2b. WALPE_TEX_CPU_REDUCE=2 (opt-in): odd sizes are reduced before the
     * upload too, close to the GL blit but not equal to it */
    {
        const char *mode = g_getenv ("WALPE_TEX_CPU_REDUCE");
        gboolean odd_cpu = mode && *mode == '2';
        int worst = 0;
        gboolean sized = TRUE, never_full = TRUE;
        for (int kind = 0; kind < 2; kind++) {
            /* noise, and a smooth picture (what layer art is closer to) */
            guint8 *px = noise_rgba (255, 129, 0);
            if (kind == 1)
                for (int y = 0; y < 129; y++)
                    for (int x = 0; x < 255; x++) {
                        guint8 *q = px + ((gsize) y * 255 + x) * 4;
                        q[0] = (guint8) x;
                        q[1] = (guint8) (y * 2);
                        q[2] = (guint8) ((x + y) / 2);
                        q[3] = 255;
                    }
            char *name = g_strdup_printf ("odd%d.tex", kind);
            char *f = write_tex (dir, name, 255, 129, px, 255 * 129 * 4);
            for (int up = 0; up < 2; up++) {
                guint fl = WALPE_GL_TEX_LEVEL (1) | (up ? WALPE_GL_TEX_CEIL : 0);
                int nw = up ? 128 : 127, nh = up ? 65 : 64;
                walpe_gl_tex_live_stats (&live, NULL, TRUE);
                GLuint c1 = walpe_gl_tex_cached (f, fl, GL_REPEAT, FALSE, &ti);
                walpe_gl_tex_live_stats (NULL, &peak, FALSE);
                sized = sized && c1 && ti.phys_w == nw && ti.phys_h == nh && ti.lod == 1 && ti.width == 255;
                never_full = never_full && peak - live == (gint64) nw * nh * 4;
                GLuint g0 = walpe_gl_tex_cached (f, 0, GL_CLAMP_TO_EDGE, FALSE, &ti0);
                GLuint g1 = walpe_gl_tex_cached (f, fl, GL_CLAMP_TO_EDGE, FALSE, &ti);
                int aw, ah, bw, bh;
                guint8 *pa = readback (c1, &aw, &ah), *pb = readback (g1, &bw, &bh);
                sized = sized && aw == nw && ah == nh && bw == nw && bh == nh;
                if (sized)
                    worst = MAX (worst, max_diff (pa, pb, (gsize) nw * nh * 4));
                g_free (pa);
                g_free (pb);
                walpe_gl_tex_release (c1);
                walpe_gl_tex_release (g0);
                walpe_gl_tex_release (g1);
            }
            g_free (name);
            g_free (f);
            g_free (px);
        }
        if (odd_cpu) {
            char *what = g_strdup_printf ("WALPE_TEX_CPU_REDUCE=2: a 255x129 picture is reduced before the upload "
                                          "(127x64 / 128x65), within 3/255 of the GL blit (here %d/255)", worst);
            check (sized && never_full && worst <= 3, what);
            g_free (what);
        } else {
            check (sized && !never_full && worst == 0,
                   "an odd-sized picture (255x129) keeps the GL blit: full size on the GPU for a moment");
        }
    }

    /* 3. a PNG in a container */
    WalpeTexDecodeStats st;
    char *fp[5];
    guint8 *src[5];
    for (int i = 0; i < 5; i++) {
        gsize n = 0;
        src[i] = noise_rgba (256, 128, 0);
        guint8 *png = png_of (src[i], 256, 128, TRUE, &n);
        char *name = g_strdup_printf ("pic%d.tex", i);
        /* the container of the second one declares another size than its picture has */
        fp[i] = write_tex (dir, name, i == 1 ? 512 : 256, i == 1 ? 512 : 128, png, n);
        g_free (name);
        g_free (png);
    }
    int pw = 0, ph = 0;
    WalpeTexProbe pr;
    check (walpe_tex_probe (fp[1], &pr) && pr.picture && pr.width == 512 &&
           walpe_tex_picture_size (fp[1], &pw, &ph) && pw == 256 && ph == 128,
           "a PNG in a container: its size from the picture's header, not the container's");
    char *f_raw = g_build_filename (dir, "noise1.tex", NULL);
    check (!walpe_tex_picture_size (f_raw, &pw, &ph), "  ... and none for a container of pixels");
    g_free (f_raw);

    /* what the loads below must give: premultiplied, halved */
    guint8 *want = g_malloc (256 * 128 * 4);
#define EXPECT_HALF(i) \
    do { \
        for (gsize k = 0; k < 256 * 128; k++) { \
            want[k * 4] = src[i][k * 4 + 2]; \
            want[k * 4 + 1] = src[i][k * 4 + 1]; \
            want[k * 4 + 2] = src[i][k * 4]; \
            want[k * 4 + 3] = src[i][k * 4 + 3]; \
        } \
        walpe_premultiply_rgba8 (want, 256 * 128 * 4); \
        walpe_gl_tex_halve_cpu (want, 256, 128, 4); \
    } while (0)
    int rw, rh;
    walpe_gl_tex_predecode_stats (&st);
    guint started0 = st.started, taken0 = st.taken;

    /* decoded ahead, picked up by the load */
    walpe_gl_tex_predecode (fp[0], 0, 256 * 128 * 4);
    want_started = started0 + 1;
    wait_decodes (st_started, 5000);
    walpe_gl_tex_live_stats (&live, NULL, TRUE);
    GLuint p0 = walpe_gl_tex_cached (fp[0], WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, TRUE, &ti);
    walpe_gl_tex_live_stats (NULL, &peak, FALSE);
    walpe_gl_tex_predecode_stats (&st);
    EXPECT_HALF (0);
    guint8 *got = readback (p0, &rw, &rh);
    check (p0 && st.started == started0 + 1 && st.taken == taken0 + 1 && st.pending == 0 && st.bytes == 0 &&
           ti.width == 256 && ti.height == 128 && ti.phys_w == 128 && ti.lod == 1 && ti.premult &&
           rw == 128 && rh == 64 && !memcmp (got, want, 128 * 64 * 4) && peak - live == 128 * 64 * 4,
           "decoded by a worker, taken by the load, uploaded reduced (premultiplied, halved)");
    g_free (got);

    /* the cache holds the file: no second decode is started (the other monitor) */
    gboolean reserved = walpe_gl_tex_predecode (fp[0], 0, 256 * 128 * 4);
    walpe_gl_tex_predecode_stats (&st);
    check (!reserved && st.pending == 0 && st.started == started0 + 1,
           "a file the cache already holds is not decoded ahead again");

    /* announced: whoever reduces it, the result is the same */
    walpe_gl_tex_predecode (fp[2], 0, 256 * 128 * 4);
    walpe_gl_tex_predecode_request (fp[2], WALPE_GL_TEX_LEVEL (1), TRUE);
    GLuint p2 = walpe_gl_tex_cached (fp[2], WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, TRUE, &ti);
    EXPECT_HALF (2);
    got = readback (p2, &rw, &rh);
    walpe_gl_tex_predecode_stats (&st);
    check (p2 && rw == 128 && rh == 64 && !memcmp (got, want, 128 * 64 * 4) && st.pending == 0 &&
           st.bytes == 0, "announced load: the same texels, nothing left pending");
    g_free (got);

    /* announced, reduced by the worker, then asked for at full size and straight */
    walpe_gl_tex_predecode_stats (&st);
    started0 = st.started;
    walpe_gl_tex_predecode (fp[3], 0, 256 * 128 * 4);
    walpe_gl_tex_predecode_request (fp[3], WALPE_GL_TEX_LEVEL (1), TRUE);
    wait_decodes (st_small, 3000);   /* the worker holds the reduced picture (or lost the race: fine too) */
    GLuint p3 = walpe_gl_tex_cached (fp[3], 0, GL_CLAMP_TO_EDGE, FALSE, &ti);
    got = readback (p3, &rw, &rh);
    gboolean straight = rw == 256 && rh == 128;
    for (gsize k = 0; straight && k < 256 * 128; k++)
        straight = got[k * 4] == src[3][k * 4 + 2] && got[k * 4 + 1] == src[3][k * 4 + 1] &&
                   got[k * 4 + 2] == src[3][k * 4] && got[k * 4 + 3] == src[3][k * 4 + 3];
    walpe_gl_tex_predecode_stats (&st);
    check (p3 && ti.phys_w == 256 && ti.lod == 0 && !ti.premult && straight && st.pending == 0 &&
           st.bytes == 0, "a load that asks for something else than was announced gets what it asks for");
    g_free (got);

    /* cancelled: dropped, whatever state it was in */
    walpe_gl_tex_predecode (fp[4], 0, 256 * 128 * 4);
    walpe_gl_tex_predecode (fp[4], 0, 256 * 128 * 4);   /* two layers, one decode */
    walpe_gl_tex_predecode_cancel (fp[4], 0);
    walpe_gl_tex_predecode_stats (&st);
    gboolean still = st.pending == 1;
    walpe_gl_tex_predecode_cancel (fp[4], 0);
    wait_decodes (st_idle, 5000);
    walpe_gl_tex_predecode_stats (&st);
    check (still && st.pending == 0 && st.bytes == 0,
           "two requests share one decode; cancelled by both it is dropped");

    /* an RGB picture (no alpha channel): opaque */
    {
        gsize n = 0;
        guint8 *png = png_of (src[4], 256, 128, FALSE, &n);
        char *f = write_tex (dir, "rgb.tex", 256, 128, png, n);
        GLuint t = walpe_gl_tex_cached (f, 0, GL_CLAMP_TO_EDGE, TRUE, &ti);
        got = readback (t, &rw, &rh);
        gboolean ok = t && rw == 256 && rh == 128;
        for (gsize k = 0; ok && k < 256 * 128; k++)
            ok = got[k * 4] == src[4][k * 4 + 2] && got[k * 4 + 1] == src[4][k * 4 + 1] &&
                 got[k * 4 + 2] == src[4][k * 4] && got[k * 4 + 3] == 255;
        check (ok, "an RGB picture: its colours, alpha 255");
        g_free (got);
        g_free (png);
        g_free (f);
        walpe_gl_tex_release (t);
    }
    walpe_gl_tex_release (p0);
    walpe_gl_tex_release (p2);
    walpe_gl_tex_release (p3);
    check (glGetError () == GL_NO_ERROR, "no GL error");
    g_free (want);
    for (int i = 0; i < 5; i++) {
        g_free (fp[i]);
        g_free (src[i]);
    }
}

/* ---- 5. a scene ---- */
#define VW 320
#define VH 180

static void put (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

static void redraw_cb (gpointer u) { (void) u; }

static void scene_checks (const char *dir)
{
    /* two 640x360 pictures of one colour each, PNG in a container; scene 640x360 on a 320x180 view */
    for (int i = 0; i < 2; i++) {
        guint8 *px = noise_rgba (640, 360, 3);
        if (i == 1)
            for (gsize k = 0; k < 640 * 360; k++)
                px[k * 4] = 10;
        gsize n = 0;
        guint8 *png = png_of (px, 640, 360, TRUE, &n);
        char *name = g_strdup_printf ("materials/p%d.tex", i);
        g_free (write_tex (dir, name, 640, 360, png, n));
        g_free (name);
        name = g_strdup_printf ("materials/p%d.json", i);
        char *js = g_strdup_printf ("{\"passes\":[{\"blending\":\"translucent\",\"cullmode\":\"nocull\","
                                    "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\","
                                    "\"shader\":\"genericimage2\",\"textures\":[\"p%d\"]}]}", i);
        put (dir, name, js);
        g_free (js);
        g_free (name);
        name = g_strdup_printf ("models/p%d.json", i);
        js = g_strdup_printf ("{\"autosize\":true,\"material\":\"materials/p%d.json\"}", i);
        put (dir, name, js);
        g_free (js);
        g_free (name);
        g_free (png);
        g_free (px);
    }
    put (dir, "scene.json",
         "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
         "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
         "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":360,\"width\":640},"
         "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":["
         "{\"id\":1,\"name\":\"shown\",\"image\":\"models/p0.json\",\"origin\":\"320 180 0\","
         "\"size\":\"640 360\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"visible\":true},"
         "{\"id\":2,\"name\":\"hidden\",\"image\":\"models/p1.json\",\"origin\":\"320 180 0\","
         "\"size\":\"640 360\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"visible\":false}]}");
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"texload test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");

    gint64 live0 = 0, live = 0, peak = 0;
    WalpeTexDecodeStats st;
    walpe_gl_tex_live_stats (&live0, NULL, TRUE);
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c || (c->request_redraw = redraw_cb, !c->gl_init (c, &err))) {
        check (FALSE, "the scene loads");
        return;
    }
    walpe_gl_tex_live_stats (&live, NULL, FALSE);
    check (walpe_gl_tex_async () ? live == live0 : live > live0,
           "a scene with PNG layers: no picture is uploaded while it is parsed");
    guint8 *px = g_malloc0 (VW * VH * 4);
    for (int f = 0; f < 2; f++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, VW, VH);
        c->gl_render (c, VW, VH, 0.0f, 0.0f);
        glFinish ();
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glReadPixels (0, 0, VW, VH, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const guint8 *mid = px + ((gsize) (VH / 2) * VW + VW / 2) * 4;
    check (abs (mid[0] - 200) <= 1 && abs (mid[1] - 100) <= 1 && abs (mid[2] - 50) <= 1,
           "  ... the shown layer is drawn with its picture");
    walpe_gl_tex_live_stats (&live, &peak, FALSE);
    walpe_gl_tex_predecode_stats (&st);
    check (st.pending == 0 && st.bytes == 0, "  ... nothing is left to decode after the first frame");
    /* shown + hidden at 320x180 RGBA8 = 460800 bytes; one full picture alone is 921600 */
    check (live - live0 == 2 * VW * VH * 4 && peak - live0 == 2 * VW * VH * 4,
           "  ... both layers at the size they are drawn at, and never more than that on the GPU");
    g_free (px);
    c->gl_free (c);
    c->free (c);
    walpe_gl_tex_live_stats (&live, NULL, FALSE);
    walpe_gl_tex_predecode_stats (&st);
    check (live == live0 && st.pending == 0, "  ... everything released with the scene");
}

static void rm_rf (const char *path)
{
    if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *d = g_dir_open (path, 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name (d))) {
            char *p = g_build_filename (path, n, NULL);
            rm_rf (p);
            g_free (p);
        }
        if (d)
            g_dir_close (d);
        g_rmdir (path);
    } else {
        g_unlink (path);
    }
}

int main (void)
{
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skip (no EGL surfaceless platform)\n");
        return 0;
    }
    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip (no usable GL config)\n");
        return 0;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, VW, EGL_HEIGHT, VH, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        printf ("skip (no GL 3.3 context with a pbuffer)\n");
        return 0;
    }
    const char *ren = (const char *) glGetString (GL_RENDERER);
    /* bit for bit where the renderer's own half-size blit is the CPU
     * reduction's box; else what its blit was seen to differ by for each
     * halving, and never more than 2 (a renderer further off than Mesa's
     * truncating filter fails the checks, as it should) */
    int off = blit_rounding ();
    exact_driver = off == 0;
    blit_slack = CLAMP (off, 1, 2);
    if (exact_driver)
        printf ("GL: %s\n", ren ? ren : "?");
    else
        printf ("GL: %s (its half-size blit is up to %d/255 off the rounded 2x2 box: +-%d allowed for each halving)\n",
                ren ? ren : "?", off, blit_slack);
    walpe_glutil_init ();
    char *dir = g_dir_make_tmp ("walpe-texload-XXXXXX", NULL);
    char *sdir = g_dir_make_tmp ("walpe-texload-scene-XXXXXX", NULL);
    if (!dir || !sdir) {
        printf ("skip (no temporary directory)\n");
        return 0;
    }
    cache_checks (dir);
    scene_checks (sdir);
    rm_rf (dir);
    rm_rf (sdir);
    g_free (dir);
    g_free (sdir);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
