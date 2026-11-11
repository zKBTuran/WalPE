/* parallax_test - camera parallax, the parts that need no GL (scenenode.h):
 * the move of a layer (direction, strength per axis, the offset term and
 * its switch, values that are no numbers) and the lagging pointer (start at
 * rest in the middle, exact arrival `delay` seconds after the pointer
 * stopped, a pointer that is gone keeps its last place, bad input). */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "engine/scenenode.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)
#define NEAR(a, b) (fabs ((double) (a) - (double) (b)) < 1e-3)

static void test_shift (void)
{
    const float scene[2] = { 1920, 1080 }, cam[2] = { 960, 540 };
    float out[2];
    /* pointer in the middle, layer in the middle: nothing moves */
    walpe_parallax_shift ((const float[2]) { 1, 1 }, 0.5f, 0.5f, (const float[2]) { 0.5f, 0.5f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (out[0] == 0 && out[1] == 0, "centre: %g %g", out[0], out[1]);
    /* pointer right and up (0.75 from the left, 0.75 from the bottom): a
     * layer of depth 1 moves left and down by amount x influence x a
     * quarter of the scene */
    walpe_parallax_shift ((const float[2]) { 1, 1 }, 0.5f, 0.5f, (const float[2]) { 0.75f, 0.75f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (NEAR (out[0], -120) && NEAR (out[1], -67.5), "against the pointer: %g %g", out[0], out[1]);
    /* depth scales it, per axis; a negative depth moves with the pointer */
    walpe_parallax_shift ((const float[2]) { 0.5f, 0 }, 0.5f, 0.5f, (const float[2]) { 0.75f, 0.75f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (NEAR (out[0], -60) && out[1] == 0, "per-axis depth: %g %g", out[0], out[1]);
    walpe_parallax_shift ((const float[2]) { -1, -1 }, 0.5f, 0.5f, (const float[2]) { 0.75f, 0.75f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (NEAR (out[0], 120) && NEAR (out[1], 67.5), "negative depth: %g %g", out[0], out[1]);
    /* the offset term: a layer left of the camera is pushed further left
     * by depth x amount x its distance, pointer at rest */
    walpe_parallax_shift ((const float[2]) { 1, 1 }, 0.5f, 0.5f, (const float[2]) { 0.5f, 0.5f },
                          (const float[2]) { 480, 540 }, cam, scene, TRUE, out);
    CHECK (NEAR (out[0], -240) && NEAR (out[1], 0), "offset term: %g %g", out[0], out[1]);
    walpe_parallax_shift ((const float[2]) { 1, 1 }, 0.5f, 0.5f, (const float[2]) { 0.5f, 0.5f },
                          (const float[2]) { 480, 540 }, cam, scene, FALSE, out);
    CHECK (out[0] == 0 && out[1] == 0, "offset term switched off: %g %g", out[0], out[1]);
    /* Without the offset term (the default) a pointer in the middle moves
     * nothing - exactly nothing, not little, whatever numbers the scene
     * and its scripts bring: the picture at rest is the one the scene file
     * describes. Depths, amounts and influences of real scenes (a tiny
     * amount with influence -100 among them), the bounds they are held
     * to, and values that are no numbers; three canvas sizes. */
    static const float num[] = { 0.0f, 1.0f, -1.0f, 0.25f, -0.076f, 0.0005f, 9.9999997e-05f, 5.0f, -100.0f,
                                 100.0f, 1000.0f, -1000.0f, 3.4e38f, NAN, INFINITY, -INFINITY };
    static const float canvas[3][2] = { { 1920, 1080 }, { 3840, 2160 }, { 5760, 3240 } };
    int moved = 0;
    for (guint d = 0; d < G_N_ELEMENTS (num); d++)
        for (guint a = 0; a < G_N_ELEMENTS (num); a++)
            for (guint i = 0; i < G_N_ELEMENTS (num); i++)
                for (int c = 0; c < 3; c++) {
                    out[0] = out[1] = 7.0f;
                    walpe_parallax_shift ((const float[2]) { num[d], -num[d] }, num[a], num[i],
                                          (const float[2]) { 0.5f, 0.5f }, (const float[2]) { 37, 2000 },
                                          (const float[2]) { canvas[c][0] / 2, canvas[c][1] / 2 }, canvas[c],
                                          FALSE, out);
                    moved += !(out[0] == 0 && out[1] == 0);
                }
    CHECK (moved == 0, "at rest in the middle, without the offset term: %d of %d cases moved a layer", moved,
           (int) (G_N_ELEMENTS (num) * G_N_ELEMENTS (num) * G_N_ELEMENTS (num) * 3));
    /* mouse influence 0: the pointer does nothing, the offset stays */
    walpe_parallax_shift ((const float[2]) { 1, 1 }, 0.5f, 0.0f, (const float[2]) { 1.0f, 1.0f },
                          (const float[2]) { 480, 540 }, cam, scene, TRUE, out);
    CHECK (NEAR (out[0], -240) && NEAR (out[1], 0), "influence 0: %g %g", out[0], out[1]);
    /* amount 0 or depth 0: no move at all */
    walpe_parallax_shift ((const float[2]) { 1, 1 }, 0.0f, 0.5f, (const float[2]) { 1.0f, 0.0f },
                          (const float[2]) { 100, 900 }, cam, scene, TRUE, out);
    CHECK (out[0] == 0 && out[1] == 0, "amount 0: %g %g", out[0], out[1]);
    walpe_parallax_shift ((const float[2]) { 0, 0 }, 0.5f, 0.5f, (const float[2]) { 1.0f, 0.0f },
                          (const float[2]) { 100, 900 }, cam, scene, TRUE, out);
    CHECK (out[0] == 0 && out[1] == 0, "depth 0: %g %g", out[0], out[1]);
    /* values that are not numbers do not reach the layer */
    walpe_parallax_shift ((const float[2]) { NAN, INFINITY }, 0.5f, 0.5f, (const float[2]) { 0.75f, 0.75f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (out[0] == 0 && out[1] == 0, "NaN depth: %g %g", out[0], out[1]);
    walpe_parallax_shift ((const float[2]) { 1, 1 }, NAN, 0.5f, (const float[2]) { 0.75f, 0.75f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (out[0] == 0 && out[1] == 0, "NaN amount: %g %g", out[0], out[1]);
    walpe_parallax_shift ((const float[2]) { 1e30f, 1e30f }, 1e30f, 0.5f, (const float[2]) { 0.75f, 0.75f },
                          (const float[2]) { 960, 540 }, cam, scene, TRUE, out);
    CHECK (out[0] == 0 && out[1] == 0, "overflow: %g %g", out[0], out[1]);

    /* The move is a share of the scene, not a number of pixels: the same
     * scene authored at twice the size moves twice as many scene units,
     * which the view (pixels per scene unit) turns into the same share of
     * any monitor. */
    const float scene4k[2] = { 3840, 2160 }, cam4k[2] = { 1920, 1080 };
    float a[2], b[2];
    walpe_parallax_shift ((const float[2]) { 0.3f, 0.2f }, 0.7f, 0.4f, (const float[2]) { 0.9f, 0.2f },
                          (const float[2]) { 500, 300 }, cam, scene, TRUE, a);
    walpe_parallax_shift ((const float[2]) { 0.3f, 0.2f }, 0.7f, 0.4f, (const float[2]) { 0.9f, 0.2f },
                          (const float[2]) { 1000, 600 }, cam4k, scene4k, TRUE, b);
    CHECK (NEAR (a[0] / scene[0], b[0] / scene4k[0]) && NEAR (a[1] / scene[1], b[1] / scene4k[1]),
           "share of the scene: %g %g / %g %g", a[0], a[1], b[0], b[1]);
}

static void test_lag (void)
{
    /* delay 0: on the pointer at once */
    WalpeParallaxLag l;
    walpe_parallax_lag_init (&l, 0);
    CHECK (l.pos[0] == 0.5f && l.pos[1] == 0.5f, "starts in the middle");
    gboolean moving = walpe_parallax_follow (&l, 1.0f, 0.0f, 1 / 60.0f, 0);
    CHECK (!moving && l.pos[0] == 1.0f && l.pos[1] == 0.0f, "no delay: %g %g", l.pos[0], l.pos[1]);

    /* no pointer was ever seen: the caller reports the middle; nothing
     * moves, however long, and no frame is asked for */
    walpe_parallax_lag_init (&l, 0.5f);
    for (int f = 0; f < 100; f++)
        CHECK (!walpe_parallax_follow (&l, 0.5f, 0.5f, 1 / 30.0f, 0.5f) && l.pos[0] == 0.5f &&
               l.pos[1] == 0.5f, "rest without a pointer, frame %d", f);

    /* the first frame of a scene (no time has passed) with the pointer
     * somewhere else: the lagging pointer is still exactly in the middle,
     * so the first picture is the picture at rest; then it travels */
    walpe_parallax_lag_init (&l, 0.1f);
    moving = walpe_parallax_follow (&l, 0.9f, 0.2f, 0.0f, 0.1f);
    CHECK (moving && l.pos[0] == 0.5f && l.pos[1] == 0.5f, "first frame: %g %g", l.pos[0], l.pos[1]);

    /* delay 0.5 s, a jump after a long rest: slow start, exactly there
     * 0.5 s after the pointer stopped, never past it, then no movement */
    walpe_parallax_lag_init (&l, 0.5f);
    CHECK (!walpe_parallax_follow (&l, 0.5f, 0.5f, 1 / 30.0f, 0.5f), "at rest in the middle");
    float prev = 0.5f, first = 0;
    int arrived = -1;
    for (int f = 1; f <= 40; f++) {
        moving = walpe_parallax_follow (&l, 1.0f, 0.5f, 1 / 30.0f, 0.5f);
        if (f == 1)
            first = l.pos[0];
        CHECK (l.pos[0] >= prev - 1e-6f && l.pos[0] <= 1.0f, "frame %d: %g after %g", f, l.pos[0], prev);
        prev = l.pos[0];
        if (!moving && arrived < 0)
            arrived = f;
        if (arrived >= 0)
            CHECK (!moving && l.pos[0] == 1.0f, "frame %d: still moving after the arrival", f);
    }
    CHECK (first > 0.5f && first < 0.56f, "first step of a jump is small: %g", first);
    CHECK (arrived >= 14 && arrived <= 16, "arrives after the delay (15 frames): %d", arrived);
    /* the pointer left the wallpaper there: its last place is reported
     * from then on - the picture stays where it is, it does not snap back */
    for (int f = 0; f < 50; f++)
        CHECK (!walpe_parallax_follow (&l, 1.0f, 0.5f, 1 / 30.0f, 0.5f) && l.pos[0] == 1.0f,
               "a pointer that is gone keeps its place, frame %d", f);

    /* a pointer that keeps moving is followed, lagging behind */
    walpe_parallax_lag_init (&l, 0.5f);
    for (int f = 1; f <= 30; f++)
        walpe_parallax_follow (&l, 0.5f + f * 0.01f, 0.5f, 1 / 30.0f, 0.5f);
    CHECK (l.pos[0] > 0.52f && l.pos[0] < 0.8f, "lags behind a moving pointer: %g (pointer 0.8)", l.pos[0]);

    /* the frame rate does not change when it arrives: 0.5 s at 144 Hz */
    walpe_parallax_lag_init (&l, 0.5f);
    walpe_parallax_follow (&l, 0.5f, 0.5f, 1 / 144.0f, 0.5f);
    arrived = -1;
    for (int f = 1; f <= 200 && arrived < 0; f++)
        if (!walpe_parallax_follow (&l, 0.0f, 1.0f, 1 / 144.0f, 0.5f))
            arrived = f;
    CHECK (arrived >= 70 && arrived <= 74 && l.pos[0] == 0.0f && l.pos[1] == 1.0f,
           "arrives after 72 frames at 144 Hz: %d", arrived);

    /* out-of-range and non-finite input */
    walpe_parallax_lag_init (&l, 0);
    walpe_parallax_follow (&l, 7.0f, NAN, NAN, 0);
    CHECK (l.pos[0] == 1.0f && l.pos[1] == 0.5f, "clamped: %g %g", l.pos[0], l.pos[1]);
    walpe_parallax_lag_init (&l, NAN);
    CHECK (!walpe_parallax_follow (&l, 0.25f, 0.75f, 0.016f, NAN) && l.pos[0] == 0.25f && l.pos[1] == 0.75f,
           "a delay that is no number is none: %g %g", l.pos[0], l.pos[1]);
    walpe_parallax_lag_init (&l, -3.0f);
    CHECK (!walpe_parallax_follow (&l, 0.25f, 0.75f, -1.0f, -3.0f) && l.pos[0] == 0.25f,
           "negative delay and time step: %g", l.pos[0]);
    walpe_parallax_lag_init (&l, 0.5f);
    for (int f = 0; f < 10; f++) {
        walpe_parallax_follow (&l, f & 1 ? INFINITY : -INFINITY, 0.3f, 1e9f, 0.5f);
        CHECK (isfinite (l.pos[0]) && isfinite (l.pos[1]) && l.pos[0] >= 0.0f && l.pos[0] <= 1.0f,
               "stays a position: %g %g", l.pos[0], l.pos[1]);
    }
}

int main (void)
{
    test_shift ();
    test_lag ();
    if (fails) {
        printf ("parallax-test: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("parallax-test: all checks passed\n");
    return 0;
}
