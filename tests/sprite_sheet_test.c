/* sprite_sheet_test - animated (sprite sheet) textures of image layers, the
 * parts that need no GL (scenenode.h), and what scripts may do to them.
 *
 *  1. the frame table as it is played: timing, texture coordinates (also
 *     of a turned frame), frames on a second image left out, a frame
 *     without a duration, how often the sheet may be halved;
 *  2. the table is the wallpaper's: a frame that does not lie on the
 *     sheet, has no area or is no number refuses the whole table (the
 *     numbers of the out-of-bounds read once found in the particle sprite
 *     analysis among them); the first frame is kept as a still when it is
 *     sound, else nothing is; absurd counts and sizes;
 *  3. the clock: pause, stop, set a frame or a time, rate (backwards,
 *     absurd), not looping, joining the scene's clock, bad time steps;
 *  4. a sheet written to a file and read by the container parser gives
 *     the same table; a hostile one is refused the same way;
 *  5. scripts (skipped without QuickJS): getTextureAnimation () of a layer
 *     with a sheet reads and drives its clock, every value stays inside
 *     the table, a layer without a sheet or an index outside the scene
 *     gets the inert object; setCameraTransforms () hands over zoom and
 *     pan and nothing else; Layer.parallaxDepth is the host's where the
 *     host has one and the plain value elsewhere. */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "common/tex.h"
#include "engine/scenenode.h"
#include "engine/scenescript.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)
#define NEAR(a, b) (fabs ((double) (a) - (double) (b)) < 1e-4)

/* 4 frames of 32 x 16 on a 64 x 32 sheet: 0.1, 0.2, 0 (-> 0.1) and 0.1 s;
 * one more on a second image of the file */
static const WalpeTexFrame good[5] = {
    { 0, 0.1f, 0, 0, 32, 0, 0, 16 },
    { 0, 0.2f, 32, 0, 32, 0, 0, 16 },
    { 0, 0.0f, 0, 16, 32, 0, 0, 16 },
    { 1, 0.5f, 0, 0, 32, 0, 0, 16 },
    { 0, 0.1f, 32, 16, 32, 0, 0, 16 },
};

static void test_table (void)
{
    char *why = (char *) "x";
    WalpeSpriteAnim *sp = walpe_sprite_anim_new (good, 5, 64, 32, &why);
    CHECK (sp != NULL, "animation made");
    if (!sp)
        return;
    CHECK (why && strstr (why, "left out"), "the frame on a second image is mentioned: %s", why ? why : "-");
    g_free (why);
    CHECK (walpe_sprite_anim_query (sp, "frames") == 4, "frames: %g", walpe_sprite_anim_query (sp, "frames"));
    CHECK (walpe_sprite_anim_query (sp, "skipped") == 1 && walpe_sprite_anim_query (sp, "refused") == 0, "one left out, not refused");
    CHECK (NEAR (walpe_sprite_anim_query (sp, "duration"), 0.5), "duration: %g", walpe_sprite_anim_query (sp, "duration"));
    CHECK (walpe_sprite_anim_query (sp, "playing") == 1 && walpe_sprite_anim_query (sp, "rate") == 1 &&
           walpe_sprite_anim_query (sp, "loop") == 1, "plays at rate 1, looping");
    int fw = 0, fh = 0;
    walpe_sprite_anim_frame_size (sp, &fw, &fh);
    CHECK (fw == 32 && fh == 16, "frame size %d x %d", fw, fh);
    /* every rectangle sits on multiples of 16: the sheet may be halved four times */
    CHECK (walpe_sprite_anim_query (sp, "level") == 4, "level: %g", walpe_sprite_anim_query (sp, "level"));
    float uv[6];
    walpe_sprite_anim_uv (sp, 1, uv);
    CHECK (NEAR (uv[0], 0.5) && NEAR (uv[1], 0) && NEAR (uv[2], 0.5) && NEAR (uv[3], 0) && NEAR (uv[4], 0) &&
           NEAR (uv[5], 0.5), "frame 1: %g %g %g %g %g %g", uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]);
    walpe_sprite_anim_uv (sp, 3, uv);   /* the fifth entry of the file */
    CHECK (NEAR (uv[0], 0.5) && NEAR (uv[1], 0.5), "frame 3: %g %g", uv[0], uv[1]);
    float lo[6], hi[6];
    walpe_sprite_anim_uv (sp, -7, lo);
    walpe_sprite_anim_uv (sp, 1 << 30, hi);
    CHECK (NEAR (lo[0], 0) && NEAR (lo[1], 0) && NEAR (hi[0], 0.5) && NEAR (hi[1], 0.5),
           "a frame number outside the table gives its first / last frame");
    /* frame timing: 0 .. 0.1 frame 0, .. 0.3 frame 1, .. 0.4 frame 2, .. 0.5 frame 3, then again */
    static const struct { double dt; int frame; } steps[] = {
        { 0.0, 0 }, { 0.05, 0 }, { 0.06, 1 }, { 0.18, 1 }, { 0.02, 2 }, { 0.1, 3 }, { 0.1, 0 }, { 0.5, 0 }, { 1.12, 1 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (steps); i++) {
        int f = walpe_sprite_anim_step (sp, steps[i].dt);
        CHECK (f == steps[i].frame, "step %u: frame %d, expected %d", i, f, steps[i].frame);
    }
    /* 0.13 s into the loop, frame 1 until 0.3: 0.17 s to go */
    CHECK (NEAR (walpe_sprite_anim_next_change (sp), 0.17), "next change in %g s", walpe_sprite_anim_next_change (sp));
    walpe_sprite_anim_free (sp);
    walpe_sprite_anim_free (NULL);

    /* a frame turned by a quarter (its x axis runs down the sheet) is a
     * frame like any other */
    const WalpeTexFrame turned[2] = {
        { 0, 0.1f, 0, 0, 30, 0, 0, 10 },
        { 0, 0.1f, 40, 0, 0, 30, -10, 0 },   /* corners 40 0, 40 30, 30 0, 30 30 */
    };
    sp = walpe_sprite_anim_new (turned, 2, 64, 32, NULL);
    CHECK (sp && walpe_sprite_anim_query (sp, "frames") == 2, "turned frame accepted");
    if (sp) {
        walpe_sprite_anim_uv (sp, 1, uv);
        CHECK (NEAR (uv[0], 40 / 64.0) && NEAR (uv[2], 0) && NEAR (uv[3], 30 / 32.0) && NEAR (uv[4], -10 / 64.0) &&
               NEAR (uv[5], 0), "turned frame: %g %g %g %g %g %g", uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]);
        /* 30, 10 and 40 are not all multiples of 4: one halving at most */
        CHECK (walpe_sprite_anim_query (sp, "level") == 1, "level of a sheet on even texels: %g",
               walpe_sprite_anim_query (sp, "level"));
        walpe_sprite_anim_free (sp);
    }
    /* odd rectangles: the sheet is never halved */
    const WalpeTexFrame odd[2] = { { 0, 0.1f, 0, 0, 31, 0, 0, 16 }, { 0, 0.1f, 31, 0, 33, 0, 0, 16 } };
    sp = walpe_sprite_anim_new (odd, 2, 64, 32, NULL);
    CHECK (sp && walpe_sprite_anim_query (sp, "level") == 0, "odd frames: level 0");
    walpe_sprite_anim_free (sp);
    /* one frame: a still */
    sp = walpe_sprite_anim_new (good, 1, 64, 32, NULL);
    CHECK (sp && walpe_sprite_anim_query (sp, "frames") == 1 && walpe_sprite_anim_query (sp, "playing") == 0 &&
           walpe_sprite_anim_next_change (sp) < 0 && walpe_sprite_anim_step (sp, 5.0) == 0,
           "one frame is a still");
    walpe_sprite_anim_free (sp);
    /* durations: none, negative, no number, hours */
    const WalpeTexFrame dur[4] = {
        { 0, -3.0f, 0, 0, 32, 0, 0, 16 }, { 0, NAN, 32, 0, 32, 0, 0, 16 },
        { 0, 1e30f, 0, 16, 32, 0, 0, 16 }, { 0, 1e-9f, 32, 16, 32, 0, 0, 16 },
    };
    sp = walpe_sprite_anim_new (dur, 4, 64, 32, NULL);
    CHECK (sp && NEAR (walpe_sprite_anim_query (sp, "duration"), 0.1 + 0.1 + 3600.0 + 0.1),
           "durations are brought into range: %g", sp ? walpe_sprite_anim_query (sp, "duration") : -1.0);
    walpe_sprite_anim_free (sp);
}

static void refused_still (const WalpeTexFrame *fr, int n, int w, int h, const char *what)
{
    char *why = NULL;
    WalpeSpriteAnim *sp = walpe_sprite_anim_new (fr, n, w, h, &why);
    CHECK (sp && walpe_sprite_anim_query (sp, "refused") == 1 && walpe_sprite_anim_query (sp, "frames") == 1 &&
           walpe_sprite_anim_query (sp, "playing") == 0 && why && *why,
           "%s: refused, first frame as a still (%s)", what, why ? why : "no reason given");
    if (sp) {
        float uv[6];
        walpe_sprite_anim_uv (sp, 5, uv);
        for (int k = 0; k < 6; k++)
            CHECK (isfinite (uv[k]) && uv[k] >= -0.001f && uv[k] <= 1.001f, "%s: coordinate %d = %g", what, k, uv[k]);
        CHECK (walpe_sprite_anim_step (sp, 100.0) == 0 && walpe_sprite_anim_next_change (sp) < 0, "%s: stands still", what);
    }
    g_free (why);
    walpe_sprite_anim_free (sp);
}

static void refused_all (const WalpeTexFrame *fr, int n, int w, int h, const char *what)
{
    char *why = NULL;
    WalpeSpriteAnim *sp = walpe_sprite_anim_new (fr, n, w, h, &why);
    CHECK (sp == NULL, "%s: nothing of the table is used", what);
    g_free (why);
    walpe_sprite_anim_free (sp);
}

static void test_hostile (void)
{
    WalpeTexFrame t[4];
    /* one frame of an otherwise sound table, changed */
#define BAD(i, member, value, what) do { memcpy (t, good, sizeof t); t[2] = good[4]; t[3] = good[1]; \
        t[i].member = (value); if ((i) == 0) refused_all (t, 4, 64, 32, what); else refused_still (t, 4, 64, 32, what); } while (0)
    BAD (1, x, 33.0f, "one texel over the right edge");
    BAD (1, y, 17.0f, "one texel over the lower edge");
    BAD (2, x, -1.0f, "left of the picture");
    BAD (2, y, -0.5f, "above the picture");
    BAD (3, w1, 2147483520.0f, "a width whose sum with x wraps as an integer");
    BAD (3, x, 2147483520.0f, "an x no picture has");
    BAD (1, h1, 3.4e38f, "the largest float as a height");
    BAD (1, w1, INFINITY, "an infinite width");
    BAD (2, w2, NAN, "an axis that is no number");
    BAD (2, h2, -INFINITY, "an axis of minus infinity");
    BAD (3, w1, 0.0f, "a frame without width");
    BAD (3, h1, 0.0f, "a frame without height");
    BAD (1, w1, 0.01f, "a frame of a hundredth of a texel");
    BAD (1, w2, 40.0f, "an x axis that leaves the sheet downwards");
    BAD (2, h2, 40.0f, "a y axis that leaves the sheet sideways");
    BAD (3, w1, -40.0f, "a mirrored frame that leaves the sheet");
    /* the first frame itself: no still to fall back on */
    BAD (0, x, 2147483520.0f, "first frame outside");
    BAD (0, w1, NAN, "first frame no number");
    BAD (0, h1, 1e30f, "first frame larger than any sheet");
#undef BAD
    /* a mirrored frame that stays on the sheet is sound */
    memcpy (t, good, sizeof t);
    t[1].x = 64;
    t[1].w1 = -32;
    WalpeSpriteAnim *sp = walpe_sprite_anim_new (t, 3, 64, 32, NULL);
    CHECK (sp && walpe_sprite_anim_query (sp, "refused") == 0 && walpe_sprite_anim_query (sp, "frames") == 3,
           "a mirrored frame on the sheet is played");
    walpe_sprite_anim_free (sp);
    /* frames that are sound for a larger sheet than the texture is */
    refused_still (good, 5, 48, 32, "a table made for a wider picture");
    refused_all (good, 5, 16, 8, "a table made for a much larger picture");
    /* no table, no sheet, absurd sizes and counts */
    CHECK (walpe_sprite_anim_new (NULL, 4, 64, 32, NULL) == NULL, "no table");
    CHECK (walpe_sprite_anim_new (good, 0, 64, 32, NULL) == NULL, "no frames");
    CHECK (walpe_sprite_anim_new (good, -5, 64, 32, NULL) == NULL, "a negative count");
    CHECK (walpe_sprite_anim_new (good, 5, 0, 32, NULL) == NULL, "a sheet without width");
    CHECK (walpe_sprite_anim_new (good, 5, 64, -1, NULL) == NULL, "a negative height");
    CHECK (walpe_sprite_anim_new (good, 5, 1 << 20, 32, NULL) == NULL, "a sheet no texture can be");
    /* only frames of other images: nothing to show */
    WalpeTexFrame other[2] = { good[3], good[3] };
    refused_all (other, 2, 64, 32, "every frame on a second image");
    /* more frames than a file can hold: only the first is looked at (the
     * array is not read beyond it) */
    char *why = NULL;
    sp = walpe_sprite_anim_new (good, 70000, 64, 32, &why);
    CHECK (sp && walpe_sprite_anim_query (sp, "refused") == 1 && walpe_sprite_anim_query (sp, "frames") == 1 &&
           why && strstr (why, "more than"), "70000 frames: %s", why ? why : "-");
    g_free (why);
    walpe_sprite_anim_free (sp);
    /* a large sound table: 4096 frames of one texel */
    WalpeTexFrame *many = g_new0 (WalpeTexFrame, 4096);
    for (int i = 0; i < 4096; i++)
        many[i] = (WalpeTexFrame) { 0, 0.01f, (float) (i % 64), (float) (i / 64), 1, 0, 0, 1 };
    sp = walpe_sprite_anim_new (many, 4096, 64, 64, NULL);
    CHECK (sp && walpe_sprite_anim_query (sp, "frames") == 4096 && NEAR (walpe_sprite_anim_query (sp, "duration"), 40.96),
           "4096 frames play");
    if (sp) {
        int f = walpe_sprite_anim_step (sp, 20.485);
        CHECK (f == 2048, "frame at 20.485 s: %d", f);
        float uv[6];
        walpe_sprite_anim_uv (sp, f, uv);
        CHECK (NEAR (uv[0], 0) && NEAR (uv[1], 0.5), "its place on the sheet: %g %g", uv[0], uv[1]);
    }
    walpe_sprite_anim_free (sp);
    g_free (many);
}

static void test_clock (void)
{
    WalpeSpriteAnim *sp = walpe_sprite_anim_new (good, 5, 64, 32, NULL);
    if (!sp) {
        CHECK (FALSE, "animation made");
        return;
    }
    /* pause: the clock stands; setFrame works while paused */
    CHECK (walpe_sprite_anim_control (sp, "play", 0, 0), "pause");
    int f0 = walpe_sprite_anim_step (sp, 0);
    CHECK (walpe_sprite_anim_step (sp, 10.0) == f0 && walpe_sprite_anim_query (sp, "playing") == 0 &&
           walpe_sprite_anim_next_change (sp) < 0, "paused");
    walpe_sprite_anim_control (sp, "frame", 3, 0);
    CHECK (walpe_sprite_anim_step (sp, 1.0) == 3 && walpe_sprite_anim_query (sp, "frame") == 3, "setFrame (3) while paused");
    walpe_sprite_anim_control (sp, "frame", 99, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 3, "setFrame past the end shows the last frame");
    walpe_sprite_anim_control (sp, "frame", 1e300, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 3, "setFrame (1e300) shows the last frame");
    walpe_sprite_anim_control (sp, "frame", -5, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 0, "setFrame below 0 shows the first frame");
    walpe_sprite_anim_control (sp, "frame", 2.9, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 2, "setFrame (2.9) shows frame 2");
    walpe_sprite_anim_control (sp, "frame", NAN, 0);
    walpe_sprite_anim_control (sp, "frame", INFINITY, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 2, "setFrame with no number changes nothing");
    /* play continues from there; stop = paused at time zero */
    walpe_sprite_anim_control (sp, "play", 1, 0);
    CHECK (walpe_sprite_anim_step (sp, 0.05) == 2 && walpe_sprite_anim_step (sp, 0.06) == 3, "play from frame 2");
    walpe_sprite_anim_control (sp, "play", -1, 0);
    CHECK (walpe_sprite_anim_step (sp, 5.0) == 0 && walpe_sprite_anim_query (sp, "playing") == 0, "stop");
    /* rate: twice as fast, backwards, absurd values */
    walpe_sprite_anim_control (sp, "play", 1, 0);
    walpe_sprite_anim_control (sp, "rate", 2, 0);
    CHECK (walpe_sprite_anim_step (sp, 0.06) == 1, "rate 2: 0.06 s shows frame 1");
    CHECK (NEAR (walpe_sprite_anim_next_change (sp), 0.09), "rate 2: next change in %g s", walpe_sprite_anim_next_change (sp));
    walpe_sprite_anim_control (sp, "frame", 0, 0);
    walpe_sprite_anim_control (sp, "rate", -1, 0);
    CHECK (walpe_sprite_anim_step (sp, 0.05) == 3, "rate -1 from frame 0 wraps to the last frame");
    walpe_sprite_anim_control (sp, "rate", NAN, 0);
    CHECK (walpe_sprite_anim_query (sp, "rate") == -1, "a rate that is no number is ignored");
    walpe_sprite_anim_control (sp, "rate", 1e300, 0);
    CHECK (walpe_sprite_anim_query (sp, "rate") == 1000, "a huge rate is bounded: %g", walpe_sprite_anim_query (sp, "rate"));
    int f = walpe_sprite_anim_step (sp, 0.016);
    CHECK (f >= 0 && f < 4, "... and stays inside the table: %d", f);
    walpe_sprite_anim_control (sp, "rate", 0, 0);
    f = walpe_sprite_anim_step (sp, 0);
    CHECK (walpe_sprite_anim_step (sp, 3.0) == f && walpe_sprite_anim_next_change (sp) < 0, "rate 0 stands still");
    walpe_sprite_anim_control (sp, "rate", 1, 0);
    /* time */
    walpe_sprite_anim_control (sp, "time", 0.35, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 2 && NEAR (walpe_sprite_anim_query (sp, "time"), 0.35), "setCurrentTime (0.35): frame 2");
    walpe_sprite_anim_control (sp, "time", 10.35, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 2, "a time past the end wraps");
    walpe_sprite_anim_control (sp, "time", -0.05, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 3, "a negative time wraps from the end");
    walpe_sprite_anim_control (sp, "time", NAN, 0);
    CHECK (walpe_sprite_anim_step (sp, 0) == 3, "a time that is no number changes nothing");
    /* join: back on the scene's clock */
    walpe_sprite_anim_control (sp, "join", 1, 10.35);
    CHECK (walpe_sprite_anim_step (sp, 0) == 2, "join at 10.35 s = 0.35 s into the loop: frame 2");
    walpe_sprite_anim_control (sp, "join", 1, NAN);
    CHECK (walpe_sprite_anim_step (sp, 0) == 0, "join without a clock: the start");
    /* not looping: runs to the last frame and stops there; play starts over */
    walpe_sprite_anim_control (sp, "loop", 0, 0);
    CHECK (walpe_sprite_anim_step (sp, 0.25) == 1 && walpe_sprite_anim_step (sp, 5.0) == 3 &&
           walpe_sprite_anim_query (sp, "playing") == 0 && walpe_sprite_anim_query (sp, "loop") == 0,
           "without loop the animation stops on its last frame");
    CHECK (walpe_sprite_anim_step (sp, 5.0) == 3, "... and stays there");
    walpe_sprite_anim_control (sp, "play", 1, 0);
    CHECK (walpe_sprite_anim_step (sp, 0.05) == 0 && walpe_sprite_anim_query (sp, "playing") == 1, "play starts it over");
    walpe_sprite_anim_control (sp, "loop", 1, 0);
    /* unknown controls, bad time steps */
    CHECK (!walpe_sprite_anim_control (sp, "nonsense", 1, 0) && !walpe_sprite_anim_control (sp, NULL, 1, 0) &&
           !walpe_sprite_anim_control (NULL, "play", 1, 0), "unknown control is refused");
    CHECK (walpe_sprite_anim_query (sp, "nonsense") == 0 && walpe_sprite_anim_query (NULL, "frames") == 0, "unknown query is 0");
    f = walpe_sprite_anim_step (sp, 0);
    CHECK (walpe_sprite_anim_step (sp, NAN) == f && walpe_sprite_anim_step (sp, -3) == f &&
           walpe_sprite_anim_step (sp, INFINITY) == f, "time steps that are no steps are ignored");
    f = walpe_sprite_anim_step (sp, 1e18);
    CHECK (f >= 0 && f < 4, "an absurd time step stays inside the table: %d", f);
    walpe_sprite_anim_free (sp);
}

/* ---- 4: through the container parser */

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

/* a 64 x 32 raw RGBA sheet with the frame table fr[n] */
static char *write_sheet (const char *dir, const char *name, const WalpeTexFrame *fr, int n)
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
        put32 (a, 0xff204060u);
    g_byte_array_append (a, (const guint8 *) "TEXS0003\0", 9);
    put32 (a, (guint32) n); put32 (a, 32); put32 (a, 16);
    for (int i = 0; i < n; i++) {
        put32 (a, (guint32) fr[i].image);
        putf (a, fr[i].time);
        putf (a, fr[i].x); putf (a, fr[i].y);
        putf (a, fr[i].w1); putf (a, fr[i].w2); putf (a, fr[i].h2); putf (a, fr[i].h1);
    }
    char *p = g_build_filename (dir, name, NULL);
    g_file_set_contents (p, (const char *) a->data, (gssize) a->len, NULL);
    g_byte_array_unref (a);
    return p;
}

static void test_file (void)
{
    char *dir = g_dir_make_tmp ("walpe-sprite-test-XXXXXX", NULL);
    if (!dir) {
        CHECK (FALSE, "temp dir");
        return;
    }
    char *p = write_sheet (dir, "good.tex", good, 5);
    WalpeTex *t = walpe_tex_load (p);
    CHECK (t && t->nframes == 5 && t->frames && t->width == 64 && t->height == 32 &&
           (t->tex_flags & WALPE_TEXF_IS_GIF), "the parser reads the sheet and its table");
    if (t && t->frames) {
        WalpeSpriteAnim *sp = walpe_sprite_anim_new (t->frames, t->nframes, t->width, t->height, NULL);
        CHECK (sp && walpe_sprite_anim_query (sp, "frames") == 4 && NEAR (walpe_sprite_anim_query (sp, "duration"), 0.5),
               "... and it plays as the table says");
        walpe_sprite_anim_free (sp);
    }
    walpe_tex_free (t);
    g_unlink (p);
    g_free (p);

    /* the numbers of the particle sprite analysis's out-of-bounds read, in
     * the third frame: 2147483520 as x and as width */
    WalpeTexFrame bad[4];
    memcpy (bad, good, sizeof bad);
    bad[2].x = 2147483520.0f;
    bad[2].w1 = 2147483520.0f;
    p = write_sheet (dir, "bad.tex", bad, 4);
    t = walpe_tex_load (p);
    CHECK (t && t->nframes == 4, "the parser hands the hostile table on as it is");
    if (t && t->frames)
        refused_still (t->frames, t->nframes, t->width, t->height, "hostile table from a file");
    walpe_tex_free (t);
    g_unlink (p);
    g_free (p);
    g_rmdir (dir);
    g_free (dir);
}

/* ---- 5: scripts. A host with four layers: 0 and 1 play a sheet, 2 and 3
 * do not; 3 has a parallax depth while par_scene is set. */
#define NLAYERS 4
static WalpeSpriteAnim *anim[NLAYERS];
static char *layer_text[NLAYERS];
static double zoom_set, pan_set[2], depth[2], depth_set[3];
static int zoom_sets, pan_sets, depth_sets, other_sets;
static gboolean par_scene, zoom_known;
static const char *layer_names[NLAYERS] = { "a", "b", "c", "d" };

static int h_find (gpointer u, const char *name)
{
    (void) u;
    for (int i = 0; i < NLAYERS; i++)
        if (!strcmp (layer_names[i], name))
            return i;
    return -1;
}

static const char *h_name (gpointer u, int idx)
{
    (void) u;
    return idx >= 0 && idx < NLAYERS ? layer_names[idx] : NULL;
}

static WalpeSsType h_get (gpointer u, int idx, const char *prop, double *v, int *n, const char **s)
{
    (void) u;
    *n = 0;
    if (idx == -1 && !strcmp (prop, "scene.zoom") && zoom_known) {
        v[0] = zoom_set > 0 ? zoom_set : 0.9;
        return WSS_NUM;
    }
    if (idx < 0 || idx >= NLAYERS)
        return WSS_NONE;
    if (!strncmp (prop, "texanim.", 8)) {
        if (!anim[idx])
            return WSS_NONE;
        v[0] = walpe_sprite_anim_query (anim[idx], prop + 8);
        return !strcmp (prop + 8, "playing") || !strcmp (prop + 8, "loop") ? WSS_BOOL : WSS_NUM;
    }
    if (!strcmp (prop, "parallaxDepth") && par_scene && idx == 3) {
        v[0] = depth[0];
        v[1] = depth[1];
        *n = 2;
        return WSS_VEC;
    }
    if (strcmp (prop, "text"))
        return WSS_NONE;
    *s = layer_text[idx] ? layer_text[idx] : "";
    return WSS_STR;
}

static void h_set (gpointer u, int idx, const char *prop, WalpeSsType t, const double *v, int n,
                   const char *s)
{
    (void) u;
    if (idx == -1 && !strcmp (prop, "scene.zoom")) {
        if (t == WSS_NUM) {
            zoom_set = v[0];
            zoom_sets++;
        }
        return;
    }
    if (idx == -1 && !strcmp (prop, "scene.pan")) {
        if (t == WSS_VEC && n >= 2) {
            pan_set[0] = v[0];
            pan_set[1] = v[1];
            pan_sets++;
        }
        return;
    }
    if (idx < 0 || idx >= NLAYERS) {
        other_sets++;
        return;
    }
    if (!strncmp (prop, "texanim.", 8)) {
        if (anim[idx] && (t == WSS_NUM || t == WSS_BOOL))
            walpe_sprite_anim_control (anim[idx], prop + 8, v[0], 10.35);
        return;
    }
    if (!strcmp (prop, "parallaxDepth")) {
        if (t == WSS_VEC && n >= 2) {
            memcpy (depth_set, v, sizeof (double) * (gsize) MIN (n, 3));
            depth_sets++;
            if (par_scene && idx == 3) {
                depth[0] = v[0];
                depth[1] = v[1];
            }
        }
        return;
    }
    if (strcmp (prop, "text") || t != WSS_STR)
        return;
    g_free (layer_text[idx]);
    layer_text[idx] = g_strdup (s);
}

static WalpeSceneScript *make (void)
{
    static const WalpeSsHost host = { NULL, h_find, h_name, h_get, h_set, NULL,
                                      NULL, NULL, NULL, NULL, NULL };
    for (int i = 0; i < NLAYERS; i++) {
        g_clear_pointer (&layer_text[i], g_free);
        g_clear_pointer (&anim[i], walpe_sprite_anim_free);
    }
    anim[0] = walpe_sprite_anim_new (good, 5, 64, 32, NULL);
    anim[1] = walpe_sprite_anim_new (good, 5, 64, 32, NULL);
    zoom_set = 0;
    pan_set[0] = pan_set[1] = 0;
    zoom_sets = pan_sets = depth_sets = other_sets = 0;
    depth[0] = 0.25;
    depth[1] = 0.5;
    JsonObject *up = json_object_new ();
    WalpeSceneScript *ss = walpe_ss_new (&host, 1920, 1080, up);
    json_object_unref (up);
    return ss;
}

static void run (WalpeSceneScript *ss, int idx, const char *src, int frames)
{
    JsonObject *up = json_object_new ();
    if (!walpe_ss_add (ss, idx, "text", src, NULL, up))
        printf ("note: script on layer %d not added\n", idx);
    walpe_ss_start (ss, up);
    json_object_unref (up);
    for (int f = 1; f <= frames; f++)
        walpe_ss_tick (ss, 1.0 / 30, f / 30.0, 0, 0, FALSE);
}

#define TEXT(i) (layer_text[i] ? layer_text[i] : "(no text)")

static void test_scripts (void)
{
    WalpeSceneScript *ss = make ();
    if (!ss) {
        printf ("note: built without QuickJS, the script part is skipped\n");
        return;
    }
    /* reading */
    run (ss, 0, "'use strict';\nexport function update(value) {\n"
                "  const a = thisLayer.getTextureAnimation();\n"
                "  return [a.frameCount, a.duration.toFixed(2), a.isPlaying(), a.getFrame(), a.rate, a.loop].join(' ');\n}\n", 1);
    CHECK (!strcmp (TEXT (0), "4 0.50 true 0 1 true"), "a script reads the animation: %s", TEXT (0));
    walpe_ss_free (ss);

    /* driving, with values no table has. (The host joins at 10.35 s.) */
    ss = make ();
    run (ss, 1, "'use strict';\nexport function update(value) {\n"
                "  const a = thisLayer.getTextureAnimation(), r = [];\n"
                "  a.pause(); r.push(a.isPlaying());\n"
                "  a.setFrame(1e308); r.push(a.getFrame());\n"
                "  a.setFrame(-5); r.push(a.getFrame());\n"
                "  a.setFrame(2); a.setFrame(NaN); a.setFrame('x'); a.setFrame({}); a.setFrame(undefined); r.push(a.getFrame());\n"
                "  a.setFrame('3'); r.push(a.getFrame());\n"
                "  a.rate = Infinity; a.rate = NaN; r.push(a.rate);\n"
                "  a.rate = 1e300; r.push(a.rate);\n"
                "  a.rate = -2; r.push(a.rate);\n"
                "  a.setCurrentTime(0.15); r.push(a.getFrame());\n"
                "  a.join(); r.push(a.getFrame());\n"
                "  a.stop(); r.push(a.getFrame()); a.play(); r.push(a.isPlaying());\n"
                "  a.loop = false; r.push(a.loop); a.addEndedCallback(() => {});\n"
                "  return r.join(' ');\n}\n", 1);
    CHECK (!strcmp (TEXT (1), "false 3 0 2 3 1 1000 -2 1 2 0 true false"), "a script drives the animation: %s", TEXT (1));
    CHECK (walpe_sprite_anim_query (anim[0], "frame") == 0 && walpe_sprite_anim_query (anim[0], "rate") == 1,
           "... its own layer's, not another one's");
    walpe_ss_free (ss);

    /* other layers of the scene by name; a layer without a sheet and an
     * index outside the scene get the inert object, and nothing is set
     * outside the scene */
    ss = make ();
    run (ss, 2, "'use strict';\nexport function update(value) {\n"
                "  const b = thisScene.getLayer('b').getTextureAnimation(); b.setFrame(2); b.pause();\n"
                "  const c = thisLayer.getTextureAnimation(); c.setFrame(3); c.play(); c.rate = 9;\n"
                "  const o = thisScene.getLayer(99999), n = thisScene.getLayer(-3);\n"
                "  const oa = o ? o.getTextureAnimation() : null; if (oa) { oa.setFrame(1); oa.stop(); }\n"
                "  return [b.getFrame(), c.frameCount, c.getFrame(), c.rate, oa ? oa.frameCount : 'none', n === null].join(' ');\n}\n", 1);
    CHECK (!strcmp (TEXT (2), "2 0 0 9 0 true") || !strcmp (TEXT (2), "2 0 0 9 none true"),
           "other layers, a layer without a sheet, an index outside: %s", TEXT (2));
    CHECK (walpe_sprite_anim_query (anim[1], "frame") == 2 && walpe_sprite_anim_query (anim[1], "playing") == 0,
           "... the named layer's animation was driven");
    CHECK (other_sets == 0, "nothing was set on a layer outside the scene (%d)", other_sets);
    walpe_ss_free (ss);

    /* the camera: zoom and pan reach the host, nothing else does */
    ss = make ();
    zoom_known = TRUE;
    run (ss, 0, "'use strict';\nexport function update(value) {\n"
                "  const t0 = thisScene.getCameraTransforms();\n"
                "  thisScene.setCameraTransforms({ zoom: 1.5, eye: new Vec3(30, -20, 1) });\n"
                "  thisScene.setCameraTransforms({ zoom: -1 }); thisScene.setCameraTransforms({ zoom: NaN });\n"
                "  thisScene.setCameraTransforms({ zoom: 'abc' }); thisScene.setCameraTransforms(null);\n"
                "  thisScene.setCameraTransforms(42); thisScene.setCameraTransforms({ zoom: Infinity });\n"
                "  const t = thisScene.getCameraTransforms();\n"
                "  return [t0.zoom, t.zoom, t.eye.x, t.eye.y, t.up.y].join(' ');\n}\n", 1);
    CHECK (!strcmp (TEXT (0), "0.9 1.5 30 -20 1"), "camera transforms: %s", TEXT (0));
    CHECK (zoom_sets == 1 && zoom_set == 1.5, "one zoom reached the host: %d, %g", zoom_sets, zoom_set);
    CHECK (pan_sets >= 1 && pan_set[0] == 30 && pan_set[1] == -20, "the pan reached the host: %g %g", pan_set[0], pan_set[1]);
    walpe_ss_free (ss);
    /* a host without a zoom to give (scripts are not to move the view):
     * scripts read 1 */
    ss = make ();
    zoom_known = FALSE;
    run (ss, 0, "'use strict';\nexport function update(value) { return '' + thisScene.getCameraTransforms().zoom; }\n", 1);
    CHECK (!strcmp (TEXT (0), "1"), "no zoom from the host reads as 1: %s", TEXT (0));
    walpe_ss_free (ss);

    /* parallaxDepth: the host's value in a scene with camera parallax */
    ss = make ();
    par_scene = TRUE;
    run (ss, 3, "'use strict';\nexport function update(value) {\n"
                "  const d = thisLayer.parallaxDepth; thisLayer.parallaxDepth = new Vec2(0.75, 0.125);\n"
                "  const e = thisLayer.parallaxDepth;\n"
                "  return [d.x, d.y, e.x, e.y, Object.keys(thisLayer).includes('parallaxDepth')].join(' ');\n}\n", 1);
    CHECK (!strcmp (TEXT (3), "0.25 0.5 0.75 0.125 true"), "parallaxDepth with a host value: %s", TEXT (3));
    CHECK (depth_sets == 1 && depth_set[0] == 0.75 && depth_set[1] == 0.125, "the depth reached the host");
    walpe_ss_free (ss);
    /* ... and the plain value it always was elsewhere */
    ss = make ();
    par_scene = FALSE;
    run (ss, 3, "'use strict';\nexport function update(value) {\n"
                "  const d = thisLayer.parallaxDepth; thisLayer.parallaxDepth = new Vec2(0.75, 0.125);\n"
                "  const e = thisLayer.parallaxDepth;\n"
                "  return [d.x, d.y, e.x, e.y, Object.keys(thisLayer).includes('parallaxDepth')].join(' ');\n}\n", 1);
    CHECK (!strcmp (TEXT (3), "1 1 0.75 0.125 true"), "parallaxDepth without a host value: %s", TEXT (3));
    walpe_ss_free (ss);

    for (int i = 0; i < NLAYERS; i++) {
        g_clear_pointer (&layer_text[i], g_free);
        g_clear_pointer (&anim[i], walpe_sprite_anim_free);
    }
}

int main (void)
{
    test_table ();
    test_hostile ();
    test_clock ();
    test_file ();
    test_scripts ();
    if (fails) {
        printf ("sprite-sheet-test: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("sprite-sheet-test: all checks passed\n");
    return 0;
}
