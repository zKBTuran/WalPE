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

int main (void)
{
    int f0, f1;
    float t;

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
