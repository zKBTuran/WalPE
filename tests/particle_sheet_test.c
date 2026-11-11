/* particle-sheet-test - version 0.12.1: sprite-sheet frames of a particle.
 *
 * WE stretches a sheet over the particle's lifetime (x sequencemultiplier)
 * and blends neighbouring frames unless the system says "no frame
 * blending". Without the blend a 64-frame puff that lives 6 s changes 10
 * times a second whatever the frame rate: the stepping reported as
 * "smoke and fog are 30 fps". No GL, no files. */
#include "engine/weparticles.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf (__VA_ARGS__); printf ("\n"); } } while (0)

enum { SEQ = 0, RANDOM = 1, ONCE = 2 };

/* distinct pictures over one particle life rendered at `fps` */
static int pictures (int n, int mode, float mult, gboolean blend, double life_s, double fps,
                     double *max_step)
{
    int steps = (int) (life_s * fps), count = 0;
    double prev = -1, mx = 0;
    for (int k = 0; k < steps; k++) {
        int f0, f1;
        float t = walpe_weparticles_sheet_frames (n, mode, mult, (float) (k / (double) steps), 0,
                                                  blend, &f0, &f1);
        double pos = f0 + t;
        if (pos != prev)
            count++;
        if (prev >= 0 && pos >= prev)
            mx = fmax (mx, pos - prev);
        prev = pos;
    }
    if (max_step)
        *max_step = mx;
    return count;
}

/* ---- how smooth a sprite is: the counts of second differences
 * (walpe_weparticles_soft_hist) against the plain loop over every byte.
 * The engine counts eight bytes at a time and takes a word of zeros in
 * all three rows at once: the counts must not know. */
static void plain_hist (const guint8 *px, gsize stride, int w, int h, guint (*hist)[511])
{
    for (int k = 1; k <= 2; k++) {
        for (int y = 0; y < h; y++)
            for (int i = k * 4; i < (w - k) * 4; i++) {
                const guint8 *r = px + (gsize) y * stride;
                hist[k - 1][abs ((int) r[i - k * 4] - 2 * (int) r[i] + (int) r[i + k * 4])]++;
            }
        for (int y = k; y < h - k; y++)
            for (int i = 0; i < w * 4; i++)
                hist[2 + k - 1][abs ((int) px[(gsize) (y - k) * stride + i] -
                                     2 * (int) px[(gsize) y * stride + i] +
                                     (int) px[(gsize) (y + k) * stride + i])]++;
    }
}

static guint32 lcg (guint32 *st)
{
    *st = *st * 1664525u + 1013904223u;
    return *st >> 8;
}

/* kind 0: nothing; 1: noise; 2: a soft dot on an empty ground; 3: a ramp
 * everywhere; 4: single bytes set in an empty picture (zero runs of every
 * length around them); 5: noise in every other 8-texel column */
static void soft_picture (guint8 *px, gsize stride, int w, int h, int kind, guint32 seed)
{
    guint32 st = seed;
    memset (px, 0, stride * (gsize) h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guint8 *q = px + (gsize) y * stride + (gsize) x * 4;
            float dx = (x + 0.5f) / (float) w * 2 - 1, dy = (y + 0.5f) / (float) h * 2 - 1;
            float r = sqrtf (dx * dx + dy * dy);
            guint8 v = 0;
            switch (kind) {
            case 1: q[0] = (guint8) lcg (&st); q[1] = (guint8) lcg (&st); q[2] = (guint8) lcg (&st);
                    q[3] = (guint8) lcg (&st); break;
            case 2: v = r < 0.5f ? (guint8) ((1 - r * 2) * (1 - r * 2) * 255.0f + 0.5f) : 0;
                    q[0] = q[3] = v; q[1] = (guint8) (v / 2); q[2] = (guint8) (v / 3); break;
            case 3: q[0] = (guint8) (x * 255 / w); q[1] = (guint8) (y * 255 / h);
                    q[2] = (guint8) ((x + y) & 255); q[3] = 255; break;
            case 4: if (lcg (&st) % 23 == 0) q[lcg (&st) & 3] = (guint8) (1 + lcg (&st) % 255); break;
            case 5: if ((x / 8) & 1) { q[0] = (guint8) lcg (&st); q[3] = (guint8) lcg (&st); } break;
            default: break;
            }
        }
}

static void test_soft_hist (void)
{
    static const int sizes[][2] = { { 4, 4 }, { 5, 4 }, { 4, 9 }, { 7, 7 }, { 16, 16 }, { 33, 17 },
                                    { 59, 57 }, { 64, 64 }, { 130, 35 }, { 257, 129 } };
    guint cases = 0, zero_words = 0;
    for (guint si = 0; si < G_N_ELEMENTS (sizes); si++)
        for (int kind = 0; kind < 6; kind++)
            for (int pad = 0; pad < 2; pad++) {
                int w = sizes[si][0], h = sizes[si][1];
                /* (rows further apart than the picture is wide: a frame of a sheet) */
                gsize stride = (gsize) w * 4 + (pad ? 12 : 0);
                guint8 *px = g_malloc (stride * (gsize) h);
                soft_picture (px, stride, w, h, kind, 77u + si * 13u + (guint) kind);
                guint (*want)[511] = g_malloc0 (sizeof (guint[4][511]));
                guint (*got)[511] = g_malloc0 (sizeof (guint[4][511]));
                plain_hist (px, stride, w, h, want);
                walpe_weparticles_soft_hist (px, stride, w, h, got);
                CHECK (!memcmp (want, got, sizeof (guint[4][511])),
                       "second differences of a %dx%d picture (kind %d, stride %zu) differ from "
                       "the plain count", w, h, kind, stride);
                /* every difference is counted, the ones of an empty word too */
                for (int a = 0; a < 4; a++) {
                    guint64 n = 0;
                    for (int i = 0; i <= 510; i++)
                        n += got[a][i];
                    int k = (a & 1) + 1;
                    guint64 all = a < 2 ? (guint64) MAX (w - 2 * k, 0) * 4 * (guint64) h
                                        : (guint64) w * 4 * (guint64) MAX (h - 2 * k, 0);
                    CHECK (n == all, "%dx%d kind %d table %d: %" G_GUINT64_FORMAT " differences "
                           "counted, the picture has %" G_GUINT64_FORMAT, w, h, kind, a, n, all);
                }
                if (kind == 0 || kind == 2)
                    zero_words += got[0][0] >= 8;
                g_free (px);
                g_free (want);
                g_free (got);
                cases++;
            }
    /* counts are added to what the tables hold (one table set for all frames of a sheet) */
    {
        guint8 px[16 * 16 * 4];
        soft_picture (px, 64, 16, 16, 1, 5);
        guint (*a)[511] = g_malloc0 (sizeof (guint[4][511]));
        guint (*b)[511] = g_malloc0 (sizeof (guint[4][511]));
        walpe_weparticles_soft_hist (px, 64, 16, 16, a);
        walpe_weparticles_soft_hist (px, 64, 16, 16, a);
        plain_hist (px, 64, 16, 16, b);
        plain_hist (px, 64, 16, 16, b);
        CHECK (!memcmp (a, b, sizeof (guint[4][511])), "two frames into one set of tables");
        g_free (a);
        g_free (b);
    }
    CHECK (zero_words > 0, "no case with empty words was looked at");
    printf ("sprite smoothness: %u pictures, counts equal to the plain loop\n", cases);
}

int main (void)
{
    int f0, f1;
    float t;

    test_soft_hist ();

    /* a 64-frame sheet over a 6 s life at 144 fps: 864 frames rendered */
    double step = 0;
    int stepped = pictures (64, SEQ, 1, FALSE, 6.0, 144.0, &step);
    CHECK (stepped == 64, "no blending: %d pictures, want 64 (10.7 a second)", stepped);
    CHECK (step == 1.0, "no blending: largest step %.3f frames, want 1", step);
    int blended = pictures (64, SEQ, 1, TRUE, 6.0, 144.0, &step);
    CHECK (blended >= 850, "blending: %d pictures of 864 rendered frames", blended);
    CHECK (step < 0.08, "blending: largest step %.3f frames, want 64/864 = 0.074", step);
    printf ("64 frames, 6 s life, 144 fps: %d pictures stepped, %d blended (largest step %.3f frame)\n",
            stepped, blended, step);

    /* half way between two frames: both, half each */
    t = walpe_weparticles_sheet_frames (64, SEQ, 1, 1.5f / 64.0f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 1 && f1 == 2 && fabsf (t - 0.5f) < 1e-3f, "mid: %d -> %d at %.3f", f0, f1, t);
    /* "no frame blending": the same frame, nothing mixed in */
    t = walpe_weparticles_sheet_frames (64, SEQ, 1, 1.5f / 64.0f, 0, FALSE, &f0, &f1);
    CHECK (f0 == 1 && t == 0.0f, "flag 2: %d at %.3f", f0, t);

    /* the frame index is what version 0.12 drew, for every life fraction */
    for (int k = 0; k < 1000; k++) {
        float life = k / 1000.0f;
        float f = life * 1.0f;
        int old = (int) ((f - floorf (f)) * 64);
        walpe_weparticles_sheet_frames (64, SEQ, 1, life, 0, TRUE, &f0, &f1);
        CHECK (f0 == old, "sequence life %.3f: frame %d, version 0.12 drew %d", life, f0, old);
        old = (int) (fminf (f, 0.9999f) * 64);
        walpe_weparticles_sheet_frames (64, ONCE, 1, life, 0, TRUE, &f0, &f1);
        CHECK (f0 == old, "once life %.3f: frame %d, version 0.12 drew %d", life, f0, old);
    }

    /* the last frame is held (WE: nextFrame = min (numFrames - 1,
     * currentFrame + 1)): frame 0 must not come back while the puff
     * dissolves */
    t = walpe_weparticles_sheet_frames (64, SEQ, 1, 0.995f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 63 && f1 == 63 && t == 0.0f, "last frame: %d -> %d at %.3f", f0, f1, t);
    t = walpe_weparticles_sheet_frames (64, ONCE, 1, 1.0f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 63 && f1 == 63 && t == 0.0f, "once, end: %d -> %d at %.3f", f0, f1, t);
    t = walpe_weparticles_sheet_frames (64, ONCE, 3, 0.9f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 63 && t == 0.0f, "once x3 stays on the last frame: %d at %.3f", f0, t);

    /* sequencemultiplier 2 (WE's smoke presets): two runs through the
     * sheet, each ending on the held last frame, blending inside */
    t = walpe_weparticles_sheet_frames (64, SEQ, 2, (63.5f / 64.0f) / 2.0f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 63 && f1 == 63 && t == 0.0f, "x2, end of run 1: %d -> %d at %.3f", f0, f1, t);
    t = walpe_weparticles_sheet_frames (64, SEQ, 2, 0.5f + (10.25f / 64.0f) / 2.0f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 10 && f1 == 11 && fabsf (t - 0.25f) < 1e-2f, "x2, run 2: %d -> %d at %.3f", f0, f1, t);
    t = walpe_weparticles_sheet_frames (64, SEQ, 2, 0.999f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 63 && f1 == 63 && t == 0.0f, "x2 end: %d -> %d at %.3f", f0, f1, t);
    /* a fractional multiplier ends mid-sheet: still blending there */
    t = walpe_weparticles_sheet_frames (64, SEQ, 0.5f, 0.99f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 31 && f1 == 32 && t > 0.0f, "x0.5: %d -> %d at %.3f", f0, f1, t);

    /* randomframe: the particle's own frame, never a blend */
    t = walpe_weparticles_sheet_frames (16, RANDOM, 1, 0.37f, 11, TRUE, &f0, &f1);
    CHECK (f0 == 11 && f1 == 11 && t == 0.0f, "randomframe: %d -> %d at %.3f", f0, f1, t);
    t = walpe_weparticles_sheet_frames (16, RANDOM, 1, 0.37f, 99, TRUE, &f0, &f1);
    CHECK (f0 == 15 && t == 0.0f, "randomframe out of range: %d", f0);

    /* not a sheet / broken values */
    t = walpe_weparticles_sheet_frames (1, SEQ, 1, 0.5f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 0 && f1 == 0 && t == 0.0f, "one frame");
    t = walpe_weparticles_sheet_frames (0, SEQ, 1, 0.5f, 0, TRUE, &f0, &f1);
    CHECK (f0 == 0 && f1 == 0 && t == 0.0f, "no frames");
    t = walpe_weparticles_sheet_frames (8, SEQ, -3, 2.0f, 0, TRUE, &f0, &f1);
    CHECK (f0 >= 0 && f0 < 8 && f1 >= 0 && f1 < 8 && t >= 0.0f && t <= 1.0f, "bad input: %d %d %.3f",
           f0, f1, t);
    t = walpe_weparticles_sheet_frames (8, SEQ, NAN, NAN, 0, TRUE, &f0, &f1);
    CHECK (f0 >= 0 && f0 < 8 && f1 >= 0 && f1 < 8 && t >= 0.0f && t <= 1.0f, "nan input: %d %d %.3f",
           f0, f1, t);

    printf (fails ? "particle-sheet: %d FAILED\n" : "particle-sheet: all checks passed\n", fails);
    return fails ? 1 : 0;
}
