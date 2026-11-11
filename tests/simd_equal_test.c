/* simd_equal_test.c - the pixel loops that run on the CPU give the same
 * bytes on every architecture and with every kernel.
 *
 * WalPE has hand-written x86 kernels (SSE2 in the 2x2 texture halving,
 * SSE2 / AVX2 in the video box filter) and plain C everywhere else. This
 * test feeds fixed pseudo-random pictures through
 *   - the texture halving before the upload (walpe_gl_tex_halve_cpu),
 *   - the YUV 4:2:0 -> BGRA converters, full and half size,
 *   - the alpha premultiply loop
 * and compares each result twice: with a reference loop written here in
 * the most obvious way, and with a 64-bit hash that was computed outside
 * of any C compiler (integer arithmetic in a script). The first comparison
 * says which side is wrong when something differs; the second one is the
 * same number on x86-64 and on aarch64, so a build that passes on both has
 * shown that both produce exactly these bytes.
 *
 * (The video box filter's kernels are compared with their C loop by
 * tests/video_test.c, which is built with the video module.)
 *
 * No display, no GL context, no files. */
#include <glib.h>
#include <string.h>
#include "engine/glutil.h"
#include "engine/yuvconv.h"
#include "common/premult.h"

/* xorshift32: the same bytes wherever this runs */
static void fill (guint8 *p, gsize n, guint32 seed)
{
    guint32 s = seed;
    for (gsize i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        p[i] = (guint8) (s >> 11);
    }
}

/* the FNV-1a loop with the offset the engine's own hashes use */
#define HASH_SEED 1469598103934665603ull
static guint64 hash_add (guint64 h, const guint8 *p, gsize n)
{
    for (gsize i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- 2x2 halving ------------------------------------------------------ */

static void ref_halve (const guint8 *d, int w, int h, int bpp, guint8 *o)
{
    int nw = w / 2, nh = h / 2;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < bpp; c++) {
                const guint8 *r0 = d + (gsize) (2 * y) * w * bpp, *r1 = r0 + (gsize) w * bpp;
                int a = r0[(2 * x) * bpp + c], b = r0[(2 * x + 1) * bpp + c];
                int e = r1[(2 * x) * bpp + c], f = r1[(2 * x + 1) * bpp + c];
                o[((gsize) y * nw + x) * bpp + c] =
                    (guint8) ((((a + b + 1) >> 1) + ((e + f + 1) >> 1) + 1) >> 1);
            }
}

static void t_halve (void)
{
    static const guint64 want[3] = {
        0x480c18a2ebc1f2d0ull,      /* 1 byte per texel */
        0x0a49d585520193a0ull,      /* 2 */
        0x28978312280cf74dull,      /* 4 */
    };
    static const int bpps[3] = { 1, 2, 4 };
    for (int k = 0; k < 3; k++) {
        int bpp = bpps[k];
        guint64 hh = HASH_SEED;
        /* every width from 2 to 75: all the vector step counts and all the
         * left-over columns; a height of 5 leaves a row that is not read */
        for (int w = 2; w < 76; w++)
            for (int hi = 0; hi < 2; hi++) {
                int h = hi ? 5 : 2;
                gsize n = (gsize) w * h * bpp, on = (gsize) (w / 2) * (h / 2) * bpp;
                guint8 *src = g_malloc (n), *work = g_malloc (n), *ref = g_malloc (on ? on : 1);
                fill (src, n, (guint32) (0x1234567 + bpp * 1000 + w * 10 + h));
                memcpy (work, src, n);
                ref_halve (src, w, h, bpp, ref);
                walpe_gl_tex_halve_cpu (work, w, h, bpp);
                if (memcmp (work, ref, on) != 0)
                    g_error ("halving %dx%d, %d byte(s) per texel: differs from the reference loop",
                             w, h, bpp);
                hh = hash_add (hh, work, on);
                g_free (src);
                g_free (work);
                g_free (ref);
            }
        g_assert_cmphex (hh, ==, want[k]);
    }
}

/* ---- YUV -> BGRA ------------------------------------------------------- */

static guint8 clamp8 (int t) { return (guint8) (t < 0 ? 0 : t > 255 ? 255 : t); }

/* arithmetic shift of a possibly negative value, without leaving it to
 * the compiler what >> does to one */
static int asr (int v, int n) { return v >= 0 ? v >> n : -((-v + (1 << n) - 1) >> n); }

static void t_yuv_full (void)
{
    static const guint64 want[6] = {
        0xb3992729bf63147full, 0x3c8f4597506e079eull, 0xc33c647ecfaa0d0aull,
        0xd3c734fe5d1e83a5ull, 0xe0740e99e88e56fbull, 0xd0015c4afc2150b4ull,
    };
    static const int sz[6][2] = { { 1, 1 }, { 2, 2 }, { 16, 8 }, { 17, 9 }, { 31, 5 }, { 64, 3 } };
    for (guint mode = 0; mode < 6; mode++) {
        int k[6];
        walpe_yuv_coeffs (mode, k);     /* ky, yoff, rv, gu, gv, bu */
        guint64 hh = HASH_SEED;
        for (int i = 0; i < 6; i++) {
            int w = sz[i][0], h = sz[i][1];
            int ys = w + 3, uvs = (w + 1) / 2 + 5, ch = (h + 1) / 2, ds = 4 * w + 4;
            guint32 seed = (guint32) (0x2000 + mode * 100 + i * 7);
            guint8 *y = g_malloc ((gsize) ys * h), *u = g_malloc ((gsize) uvs * ch);
            guint8 *v = g_malloc ((gsize) uvs * ch), *dst = g_malloc ((gsize) ds * h);
            fill (y, (gsize) ys * h, seed);
            fill (u, (gsize) uvs * ch, seed + 1);
            fill (v, (gsize) uvs * ch, seed + 2);
            memset (dst, 0xAA, (gsize) ds * h);
            walpe_yuv420_bgra_ex (y, u, v, ys, uvs, dst, ds, w, h, mode);
            for (int r = 0; r < h; r++) {
                for (int c = 0; c < w; c++) {
                    int Y = y[r * ys + c] - k[1];
                    if (Y < 0)
                        Y = 0;
                    Y *= k[0];
                    int U = u[(r / 2) * uvs + c / 2] - 128, V = v[(r / 2) * uvs + c / 2] - 128;
                    const guint8 *px = dst + (gsize) r * ds + c * 4;
                    guint8 b = clamp8 (asr (Y + k[5] * U + 128, 8));
                    guint8 g = clamp8 (asr (Y - k[3] * U - k[4] * V + 128, 8));
                    guint8 rr = clamp8 (asr (Y + k[2] * V + 128, 8));
                    if (px[0] != b || px[1] != g || px[2] != rr || px[3] != 255)
                        g_error ("YUV mode %u, %dx%d, pixel %d,%d: %u %u %u %u, the reference "
                                 "says %u %u %u 255", mode, w, h, c, r, px[0], px[1], px[2], px[3],
                                 b, g, rr);
                }
                /* nothing is written behind the row */
                for (int p = 4 * w; p < ds; p++)
                    g_assert_cmpuint (dst[(gsize) r * ds + p], ==, 0xAA);
                hh = hash_add (hh, dst + (gsize) r * ds, (gsize) w * 4);
            }
            g_free (y);
            g_free (u);
            g_free (v);
            g_free (dst);
        }
        g_assert_cmphex (hh, ==, want[mode]);
    }
}

static void t_yuv_half (void)
{
    static const guint64 want[6] = {
        0x2d61545921842106ull, 0xed61f5ec572289c5ull, 0xdb778dd87dd10499ull,
        0xcd764be54b2f43ccull, 0xceda2d5a360bf783ull, 0xd217615586797127ull,
    };
    static const int sz[4][2] = { { 1, 1 }, { 8, 4 }, { 15, 7 }, { 33, 2 } };
    for (guint mode = 0; mode < 6; mode++) {
        int k[6];
        walpe_yuv_coeffs (mode, k);
        guint64 hh = HASH_SEED;
        for (int i = 0; i < 4; i++) {
            int ow = sz[i][0], oh = sz[i][1];
            int ys = 2 * ow + 3, uvs = ow + 5, ds = 4 * ow + 4;
            guint32 seed = (guint32) (0x3000 + mode * 100 + i * 7);
            guint8 *y = g_malloc ((gsize) ys * 2 * oh), *u = g_malloc ((gsize) uvs * oh);
            guint8 *v = g_malloc ((gsize) uvs * oh), *dst = g_malloc ((gsize) ds * oh);
            fill (y, (gsize) ys * 2 * oh, seed);
            fill (u, (gsize) uvs * oh, seed + 1);
            fill (v, (gsize) uvs * oh, seed + 2);
            memset (dst, 0xAA, (gsize) ds * oh);
            walpe_yuv420_bgra_half (y, u, v, ys, uvs, dst, ds, ow, oh, mode);
            for (int r = 0; r < oh; r++) {
                const guint8 *y0 = y + (gsize) (2 * r) * ys, *y1 = y0 + ys;
                for (int c = 0; c < ow; c++) {
                    int Ys = y0[2 * c] + y0[2 * c + 1] + y1[2 * c] + y1[2 * c + 1] - 4 * k[1];
                    if (Ys < 0)
                        Ys = 0;
                    int U = u[r * uvs + c] - 128, V = v[r * uvs + c] - 128, Y = Ys * k[0];
                    const guint8 *px = dst + (gsize) r * ds + c * 4;
                    guint8 b = clamp8 (asr (Y + 4 * k[5] * U + 512, 10));
                    guint8 g = clamp8 (asr (Y - 4 * k[3] * U - 4 * k[4] * V + 512, 10));
                    guint8 rr = clamp8 (asr (Y + 4 * k[2] * V + 512, 10));
                    if (px[0] != b || px[1] != g || px[2] != rr || px[3] != 255)
                        g_error ("YUV half size, mode %u, %dx%d, pixel %d,%d differs from the "
                                 "reference", mode, ow, oh, c, r);
                }
                for (int p = 4 * ow; p < ds; p++)
                    g_assert_cmpuint (dst[(gsize) r * ds + p], ==, 0xAA);
                hh = hash_add (hh, dst + (gsize) r * ds, (gsize) ow * 4);
            }
            g_free (y);
            g_free (u);
            g_free (v);
            g_free (dst);
        }
        g_assert_cmphex (hh, ==, want[mode]);
    }
}

/* ---- premultiply ------------------------------------------------------- */

static void t_premultiply (void)
{
    enum { N = 4099 };      /* not a multiple of any vector width */
    guint8 *src = g_malloc (4 * N), *work = g_malloc (4 * N);
    fill (src, 4 * N, 0x4242);
    for (int i = 0; i < 64; i += 4)     /* opaque and clear pixels among the others */
        src[4 * (i * 61 % N) + 3] = (i & 4) ? 255 : 0;
    memcpy (work, src, 4 * N);
    walpe_premultiply_rgba8 (work, 4 * N);
    for (int i = 0; i < N; i++) {
        guint a = src[4 * i + 3];
        for (int c = 0; c < 3; c++) {
            guint want = a == 255 ? src[4 * i + c] : src[4 * i + c] * a / 255;
            if (work[4 * i + c] != want)
                g_error ("premultiply: pixel %d channel %d: %u * %u / 255 gave %u", i, c,
                         src[4 * i + c], a, work[4 * i + c]);
        }
        g_assert_cmpuint (work[4 * i + 3], ==, a);
    }
    g_assert_cmphex (hash_add (HASH_SEED, work, 4 * N), ==, 0xfa675751041768c2ull);
    g_free (src);
    g_free (work);
}

int main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/simd-equal/halve", t_halve);
    g_test_add_func ("/simd-equal/yuv-full", t_yuv_full);
    g_test_add_func ("/simd-equal/yuv-half", t_yuv_half);
    g_test_add_func ("/simd-equal/premultiply", t_premultiply);
    return g_test_run ();
}
