/* postfx_test.c - scene bloom and camera shake, the parts that need no GL.
 *
 *  1. the two switches, rows "bloom" and "camera_shake" of the registry of
 *     renderer features (engine/features.h): defaults, set, serial; with
 *     WALPE_FEATURES in the environment (second run of this binary,
 *     argument "env") the environment decides;
 *  2. config.json: the switches survive a save / load, a file without them
 *     means on, members of "features" this build has no feature for stay;
 *  3. bloom: the bright pass on single colours (threshold, saturation,
 *     strength, tint), when a bloom cannot change a pixel, the blur
 *     weights of both levels;
 *  4. bloom: the plan of a run at 720p, 1080p, 1440p and 4K, at odd and
 *     tiny sizes, for each way the frame is read and both levels - target
 *     sizes, lookups that cover every source texel, a glow whose reach is
 *     8 pixels of the reference size per tap whatever the render size;
 *  5. camera shake: no motion for amplitude 0 / speed 0 / bad numbers,
 *     the reach, no jumps, two axes that differ, the same instant gives the
 *     same offset, and - the point of a time-based shake - the same path
 *     at 30, 60 and 144 frames per second.
 *
 * `meson test -C build postfx postfx-env` */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "common/config.h"
#include "engine/bloom.h"
#include "engine/camshake.h"
#include "engine/features.h"

static int checks, fails;
#define CHECK(c, ...) \
    do { \
        checks++; \
        if (!(c)) { \
            fails++; \
            printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf (__VA_ARGS__); \
            printf ("\n"); \
        } \
    } while (0)
#define NEAR(a, b) (fabs ((double) (a) - (double) (b)) < 1e-4)

/* ------------------------------------------------------------ switches */

/* "bloom" and "camera_shake" are rows of the registry of renderer features
 * (engine/features.h): tests/features_test.c checks the registry as such,
 * this is what the two rows promise. */
static void test_switches (gboolean env)
{
    CHECK (walpe_feature_find ("bloom") == WALPE_FEAT_BLOOM &&
           !g_strcmp0 (walpe_feature_name (WALPE_FEAT_BLOOM), "bloom"), "name of the bloom switch");
    CHECK (walpe_feature_find ("camera_shake") == WALPE_FEAT_CAMERA_SHAKE &&
           !g_strcmp0 (walpe_feature_name (WALPE_FEAT_CAMERA_SHAKE), "camera_shake"),
           "name of the shake switch");
    CHECK (walpe_feature_default (WALPE_FEAT_BLOOM) && walpe_feature_default (WALPE_FEAT_CAMERA_SHAKE),
           "both are on unless somebody says otherwise");
    /* a running scene asks with every frame: no wallpaper is loaded again */
    CHECK (!walpe_feature_reloads (WALPE_FEAT_BLOOM) && !walpe_feature_reloads (WALPE_FEAT_CAMERA_SHAKE),
           "both apply to running scenes (no reload)");
    CHECK (!walpe_feature ((WalpeFeature) 99), "an unknown switch reads off");
    if (env) {
        /* run with WALPE_FEATURES=bloom=off,camera_shake=on,other=off */
        CHECK (!walpe_feature (WALPE_FEAT_BLOOM), "environment: bloom is off");
        CHECK (walpe_feature (WALPE_FEAT_CAMERA_SHAKE), "environment: camera shake is on");
        CHECK (walpe_feature_fixed (WALPE_FEAT_BLOOM) && walpe_feature_fixed (WALPE_FEAT_CAMERA_SHAKE),
               "environment: both are fixed");
        guint s0 = walpe_feature_serial ();
        /* (the name exists, so the call succeeds - and changes nothing) */
        CHECK (walpe_feature_set ("bloom", TRUE) && walpe_feature_set ("camera_shake", FALSE),
               "environment: the names are known");
        CHECK (!walpe_feature (WALPE_FEAT_BLOOM) && walpe_feature (WALPE_FEAT_CAMERA_SHAKE),
               "environment: the values stay");
        CHECK (walpe_feature_serial () == s0, "environment: the serial stays");
        return;
    }
    CHECK (walpe_feature (WALPE_FEAT_BLOOM) && walpe_feature (WALPE_FEAT_CAMERA_SHAKE),
           "both features are on by default");
    CHECK (!walpe_feature_fixed (WALPE_FEAT_BLOOM) && !walpe_feature_fixed (WALPE_FEAT_CAMERA_SHAKE),
           "nothing fixes them");
    guint s0 = walpe_feature_serial ();
    CHECK (walpe_feature_set ("bloom", TRUE) && walpe_feature (WALPE_FEAT_BLOOM), "on -> on");
    CHECK (walpe_feature_serial () == s0, "... is no change: the serial stays");
    CHECK (walpe_feature_set ("bloom", FALSE), "on -> off");
    CHECK (!walpe_feature (WALPE_FEAT_BLOOM) && walpe_feature (WALPE_FEAT_CAMERA_SHAKE),
           "bloom off leaves the shake alone");
    CHECK (walpe_feature_serial () == s0 + 1, "the serial moved once");
    CHECK (walpe_feature_set ("camera_shake", FALSE) && !walpe_feature (WALPE_FEAT_CAMERA_SHAKE), "shake off");
    CHECK (walpe_feature_set ("bloom", TRUE) && walpe_feature_set ("camera_shake", TRUE), "both on again");
    CHECK (walpe_feature (WALPE_FEAT_BLOOM) && walpe_feature (WALPE_FEAT_CAMERA_SHAKE), "both read on");
    CHECK (walpe_feature_serial () == s0 + 4, "four changes, four steps of the serial");
    CHECK (!walpe_feature_set ("no_such_switch", TRUE), "an unknown switch cannot be set");
}

/* -------------------------------------------------------------- config */

/* config.json "features" (common/config.h; the store is the registry's and
 * is tested as such by tests/features_test.c): the two names in it, and a
 * file written by a build that had only these two switches */
static void test_config (const char *tmp)
{
    char *dir = g_build_filename (tmp, "walpe", NULL);
    char *path = g_build_filename (dir, "config.json", NULL);
    g_mkdir_with_parents (dir, 0700);
    g_unlink (path);

    WalpeConfig *c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == -1 && walpe_config_get_feature (c, "camera_shake") == -1,
           "no file: neither switch is set (the defaults hold: both on)");
    walpe_config_save (c);
    char *text = NULL;
    g_file_get_contents (path, &text, NULL, NULL);
    CHECK (text && !strstr (text, "\"features\""), "never set: no \"features\" member is written");
    g_free (text);
    walpe_config_set_feature (c, "bloom", FALSE);
    walpe_config_save (c);
    walpe_config_free (c);

    c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == 0 && walpe_config_get_feature (c, "camera_shake") == -1,
           "bloom off survives save / load");
    walpe_config_set_feature (c, "bloom", TRUE);
    walpe_config_set_feature (c, "camera_shake", FALSE);
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == 1 && walpe_config_get_feature (c, "camera_shake") == 0,
           "camera shake off (and bloom on) survive save / load");
    walpe_config_free (c);

    /* the file of a build that knew these two switches only and wrote
     * what was off; a member of yet another build stays */
    g_file_set_contents (path,
                         "{\"features\":{\"bloom\":false,\"some_other_feature\":false,"
                         "\"camera_shake\":true}}", -1, NULL);
    c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == 0 && walpe_config_get_feature (c, "camera_shake") == 1,
           "read from a file with other members");
    walpe_config_set_feature (c, "bloom", TRUE);
    walpe_config_save (c);
    walpe_config_free (c);
    text = NULL;
    g_file_get_contents (path, &text, NULL, NULL);
    CHECK (text && strstr (text, "\"some_other_feature\""), "a member this build has no feature for is kept");
    g_free (text);
    c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == 1 && walpe_config_get_feature (c, "camera_shake") == 1,
           "... and both read on again");
    walpe_config_free (c);

    /* values that are not switches change nothing */
    g_file_set_contents (path, "{\"features\":{\"bloom\":\"maybe\",\"camera_shake\":null}}", -1, NULL);
    c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == -1 && walpe_config_get_feature (c, "camera_shake") == -1,
           "a value that is no switch leaves the default");
    walpe_config_free (c);
    g_file_set_contents (path, "{\"features\":17}", -1, NULL);
    c = walpe_config_load ();
    CHECK (walpe_config_get_feature (c, "bloom") == -1 && walpe_config_get_feature (c, "camera_shake") == -1,
           "\"features\" that is no object leaves the defaults");
    walpe_config_free (c);
    g_unlink (path);
    g_free (path);
    g_free (dir);
}

/* --------------------------------------------------------------- bloom */

static void test_bloom_maths (void)
{
    const float one[3] = { 1, 1, 1 };
    float o[3];
    /* white, strength 2, threshold 0.65: (1 - 0.65) x 2 = 0.70 on every
     * channel (2 c - grey (c) of a grey is c) */
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, 2.0f, 0.65f, one, o);
    CHECK (NEAR (o[0], 0.70) && NEAR (o[1], 0.70) && NEAR (o[2], 0.70), "white: %g %g %g",
           (double) o[0], (double) o[1], (double) o[2]);
    /* below and exactly at the threshold: nothing at all */
    walpe_bloom_bright ((const float[3]) { 0.5f, 0.5f, 0.5f }, 2.0f, 0.65f, one, o);
    CHECK (o[0] == 0 && o[1] == 0 && o[2] == 0, "grey below the threshold: %g", (double) o[0]);
    walpe_bloom_bright ((const float[3]) { 0.65f, 0.2f, 0.1f }, 2.0f, 0.65f, one, o);
    CHECK (o[0] == 0 && o[1] == 0 && o[2] == 0, "exactly at the threshold: %g", (double) o[0]);
    /* it is the brightest channel that is compared with the threshold */
    walpe_bloom_bright ((const float[3]) { 0.0f, 0.0f, 0.9f }, 1.0f, 0.65f, one, o);
    CHECK (o[2] > 0 && o[0] == 0 && o[1] == 0, "a dark colour with one bright channel: %g %g %g",
           (double) o[0], (double) o[1], (double) o[2]);
    /* a saturated colour keeps its hue and gets more saturated:
     * red 1 -> kept 0.35, 2 x 0.35 - 0.2989 x 0.35 = 0.5954, x 2 */
    walpe_bloom_bright ((const float[3]) { 1, 0, 0 }, 2.0f, 0.65f, one, o);
    CHECK (NEAR (o[0], 2.0 * (0.70 - 0.2989 * 0.35)) && o[1] == 0 && o[2] == 0, "red: %g %g %g",
           (double) o[0], (double) o[1], (double) o[2]);
    /* strength and tint multiply; no tint = white; never negative */
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, 2.0f, 0.65f, (const float[3]) { 1, 0, 0.5f }, o);
    CHECK (NEAR (o[0], 0.70) && o[1] == 0 && NEAR (o[2], 0.35), "tint: %g %g %g", (double) o[0],
           (double) o[1], (double) o[2]);
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, 1.0f, 0.65f, NULL, o);
    CHECK (NEAR (o[0], 0.35), "no tint given = white: %g", (double) o[0]);
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, 0.0f, 0.65f, one, o);
    CHECK (o[0] == 0 && o[1] == 0 && o[2] == 0, "strength 0: %g", (double) o[0]);
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, -3.0f, 0.65f, one, o);
    CHECK (o[0] == 0, "a negative strength is none: %g", (double) o[0]);
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, 1.0f, 0.2f, (const float[3]) { -1, 1, 1 }, o);
    CHECK (o[0] == 0 && o[1] > 0, "a negative tint channel gives nothing there: %g", (double) o[0]);
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, NAN, 0.65f, one, o);
    CHECK (o[0] == 0, "a strength that is no number: %g", (double) o[0]);
    walpe_bloom_bright ((const float[3]) { 1, 1, 1 }, 1.0f, NAN, one, o);
    CHECK (o[0] == 0, "a threshold that is no number: %g", (double) o[0]);
    /* threshold 0: everything blooms, in proportion to its brightness */
    walpe_bloom_bright ((const float[3]) { 0.5f, 0.5f, 0.5f }, 1.0f, 0.0f, one, o);
    CHECK (NEAR (o[0], 0.25), "threshold 0, grey 0.5: %g", (double) o[0]);

    /* when the chain is not run at all */
    WalpeBloomParams p = { .strength = 2.0f, .threshold = 0.65f, .tint = { 1, 1, 1 } };
    CHECK (walpe_bloom_visible (&p), "the default values bloom");
    p.strength = 0.0f;
    CHECK (!walpe_bloom_visible (&p), "strength 0 changes no pixel");
    p.strength = -1.0f;
    CHECK (!walpe_bloom_visible (&p), "nor does a negative strength");
    p.strength = 2.0f;
    p.threshold = 1.0f;
    CHECK (!walpe_bloom_visible (&p), "threshold 1 excludes everything");
    p.threshold = 1.5f;
    CHECK (!walpe_bloom_visible (&p), "so does a threshold above 1");
    p.threshold = 0.99f;
    p.strength = 3.52f;
    CHECK (walpe_bloom_visible (&p), "threshold 0.99 with strength 3.52 can still add a step");
    p.threshold = 0.65f;
    p.strength = 2.0f;
    p.tint[0] = p.tint[1] = p.tint[2] = 0.0f;
    CHECK (!walpe_bloom_visible (&p), "a black tint changes no pixel");
    p.tint[2] = 0.3f;
    CHECK (walpe_bloom_visible (&p), "one tint channel is enough");
    p.tint[0] = p.tint[1] = p.tint[2] = 1.0f;
    p.strength = 0.0001f;
    CHECK (!walpe_bloom_visible (&p), "a glow below a quarter of a 10-bit step is none");
    p.strength = NAN;
    CHECK (!walpe_bloom_visible (&p), "a strength that is no number");
    p.strength = 2.0f;
    p.threshold = NAN;
    CHECK (!walpe_bloom_visible (&p), "a threshold that is no number");
    CHECK (!walpe_bloom_visible (NULL), "no values at all");
    /* what walpe_bloom_visible refuses really gives less than a 10-bit
     * step for the colour that blooms most (a pure blue: 2 c - grey is
     * 1.886 c) */
    p.threshold = 0.65f;
    p.strength = 0.0003f;
    if (!walpe_bloom_visible (&p)) {
        walpe_bloom_bright ((const float[3]) { 0, 0, 1 }, p.strength, p.threshold, p.tint, o);
        CHECK (o[2] < 0.5f / 1023.0f, "refused values stay below half a 10-bit step: %g", (double) o[2]);
    }
}

static void test_bloom_kernel (void)
{
    float k[13];
    for (int level = WALPE_BLOOM_FULL; level <= WALPE_BLOOM_REDUCED; level++) {
        const int n = walpe_bloom_kernel (level, k);
        const int want = level == WALPE_BLOOM_FULL ? 13 : 7;
        const double pitch = level == WALPE_BLOOM_FULL ? 1.0 : 2.0;
        CHECK (n == want, "level %d: %d taps", level, n);
        double sum = 0, var = 0;
        for (int i = 0; i < n; i++) {
            sum += k[i];
            var += k[i] * (i - n / 2) * pitch * (i - n / 2) * pitch;
            CHECK (fabsf (k[i] - k[n - 1 - i]) < 1e-6f, "level %d: symmetric at %d", level, i);
            CHECK (k[i] > 0, "level %d: tap %d has weight", level, i);
            if (i > 0 && i <= n / 2)
                CHECK (k[i] > k[i - 1], "level %d: rises to the centre at %d", level, i);
        }
        for (int i = n; i < 13; i++)
            CHECK (k[i] == 0, "level %d: nothing past the taps (%d)", level, i);
        CHECK (fabs (sum - 1.0) < 1e-5, "level %d: the weights sum to 1 (%g)", level, sum);
        /* a Gaussian of sigma 7/3 taps cut at +-6: its variance is a
         * little below (7/3)^2 = 5.44; the reduced level samples the same
         * curve at every second tap */
        CHECK (var > 4.8 && var < 5.6, "level %d: variance %g taps^2", level, var);
    }
    walpe_bloom_kernel (WALPE_BLOOM_FULL, k);
    /* centre weight of that Gaussian: about 1 / (7/3 sqrt (2 pi)) = 0.171
     * (a little more, the tails beyond +-6 being cut off) */
    CHECK (fabsf (k[6] - 0.1718f) < 0.0005f, "centre weight %g", (double) k[6]);
    CHECK (fabsf (k[5] / k[6] - expf (-9.0f / 98.0f)) < 1e-5f, "next to the centre: %g", (double) k[5]);
    CHECK (k[0] > 0.0058f && k[0] < 0.0068f, "outermost weight %g", (double) k[0]);
}

static void check_plan (int fw, int fh, int rw, int rh, WalpeBloomSource src, int level)
{
    WalpeBloomPlan p;
    char tag[96];
    g_snprintf (tag, sizeof (tag), "%dx%d (reference %dx%d) source %d level %d", fw, fh, rw, rh,
                (int) src, level);
    gboolean ok = walpe_bloom_plan (fw, fh, rw, rh, src, level, &p);
    CHECK (ok, "%s: there is a plan", tag);
    if (!ok)
        return;
    const int div = level == WALPE_BLOOM_REDUCED ? 8 : 4;
    const int src_div = src == WALPE_BLOOM_SRC_HALF_COPY ? 2 : 1;
    /* targets: the frame divided, rounded up - they cover it, with less
     * than one target pixel to spare, and are never empty */
    CHECK (p.bright_w >= 1 && p.bright_h >= 1 && p.blur_w >= 1 && p.blur_h >= 1, "%s: no empty target", tag);
    CHECK (p.bright_w * div >= fw && (p.bright_w - 1) * div < fw && p.bright_h * div >= fh &&
           (p.bright_h - 1) * div < fh, "%s: bright target %dx%d", tag, p.bright_w, p.bright_h);
    CHECK (p.blur_w * div * 2 >= fw && (p.blur_w - 1) * div * 2 < fw && p.blur_h * div * 2 >= fh &&
           (p.blur_h - 1) * div * 2 < fh, "%s: blur target %dx%d", tag, p.blur_w, p.blur_h);
    /* the copy: none when the frame is a texture, half size from a
     * window, full size only when a blit may not scale */
    if (src == WALPE_BLOOM_SRC_TEXTURE) {
        CHECK (p.copy_w == 0 && p.copy_h == 0, "%s: a frame that is a texture is not copied", tag);
        CHECK (p.src_w == fw && p.src_h == fh, "%s: it is read where it is", tag);
    } else if (src == WALPE_BLOOM_SRC_HALF_COPY) {
        CHECK (p.copy_w == (fw + 1) / 2 && p.copy_h == (fh + 1) / 2, "%s: half-size copy %dx%d", tag,
               p.copy_w, p.copy_h);
        CHECK (p.src_w == p.copy_w && p.src_h == p.copy_h, "%s: the copy is what is read", tag);
        CHECK ((guint64) p.copy_w * p.copy_h * 4 <= (guint64) (fw + 1) * (fh + 1),
               "%s: the copy has a quarter of the frame's pixels", tag);
    } else {
        CHECK (p.copy_w == fw && p.copy_h == fh && p.src_w == fw && p.src_h == fh, "%s: 1:1 copy", tag);
    }
    /* the linear lookups of the bright pass read every source texel under
     * a target pixel exactly once: taps x 2 texels x source scale = div */
    CHECK (p.bright_taps == 1 || p.bright_taps == 2 || p.bright_taps == 4, "%s: %d lookups a side", tag,
           p.bright_taps);
    CHECK (p.bright_taps * 2 * src_div == div, "%s: %d lookups cover %d source texels", tag,
           p.bright_taps, div / src_div);
    /* the blur: 13 taps 8 reference pixels apart, or 7 taps 16 apart: the
     * outermost tap is 48 reference pixels from the centre at every size */
    CHECK (p.blur_taps == (level == WALPE_BLOOM_REDUCED ? 7 : 13), "%s: %d blur taps", tag, p.blur_taps);
    const int ref_w = rw > 0 ? rw : fw, ref_h = rh > 0 ? rh : fh;
    const double reach_x = p.blur_step[0] * (p.blur_taps / 2) * ref_w;
    const double reach_y = p.blur_step[1] * (p.blur_taps / 2) * ref_h;
    CHECK (fabs (reach_x - 48.0) < 1e-3 && fabs (reach_y - 48.0) < 1e-3,
           "%s: the outermost tap is %g x %g reference pixels out", tag, reach_x, reach_y);
    /* work and memory */
    CHECK (p.px_small == (guint64) p.bright_w * p.bright_h + 2 * (guint64) p.blur_w * p.blur_h,
           "%s: small-target pixels", tag);
    CHECK (p.px_add == (guint64) fw * fh, "%s: the add covers the frame", tag);
    CHECK (p.bytes == 4 * ((guint64) p.copy_w * p.copy_h + p.px_small), "%s: bytes", tag);
    if (fw >= 256 && fh >= 256) {
        /* small targets: 1/16 + 2/64 of the frame (reduced: a quarter of
         * that), up to rounding */
        const double share = (double) p.px_small / ((double) fw * fh);
        const double want = level == WALPE_BLOOM_REDUCED ? 0.0234375 : 0.09375;
        CHECK (share > want * 0.98 && share < want * 1.06, "%s: small targets are %.4f of the frame", tag,
               share);
    }
}

static void test_bloom_plan (void)
{
    static const int sizes[][2] = {
        { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },   /* the common render sizes */
        { 3440, 1440 }, { 1080, 1920 }, { 5120, 2880 },                  /* wide, portrait, 5K */
        { 1366, 768 }, { 1921, 1081 }, { 1283, 721 }, { 1279, 719 },     /* not multiples of 8 */
        { 641, 361 }, { 33, 17 }, { 17, 33 }, { 16, 16 },                /* odd and tiny */
    };
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
        for (int src = WALPE_BLOOM_SRC_TEXTURE; src <= WALPE_BLOOM_SRC_FULL_COPY; src++)
            for (int level = WALPE_BLOOM_FULL; level <= WALPE_BLOOM_REDUCED; level++) {
                /* the frame is the reference (native) */
                check_plan (sizes[i][0], sizes[i][1], sizes[i][0], sizes[i][1], (WalpeBloomSource) src,
                            level);
                check_plan (sizes[i][0], sizes[i][1], 0, 0, (WalpeBloomSource) src, level);
                /* the frame is rendered at half and at three quarters of
                 * the reference (a reduced render scale) */
                check_plan (sizes[i][0], sizes[i][1], sizes[i][0] * 2, sizes[i][1] * 2,
                            (WalpeBloomSource) src, level);
                check_plan (sizes[i][0], sizes[i][1], sizes[i][0] * 4 / 3, sizes[i][1] * 4 / 3,
                            (WalpeBloomSource) src, level);
            }
    WalpeBloomPlan p, q;
    /* too small for bloom, and sizes nobody has */
    CHECK (!walpe_bloom_plan (15, 400, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &p), "15 wide: no plan");
    CHECK (!walpe_bloom_plan (400, 15, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &p), "15 high: no plan");
    CHECK (!walpe_bloom_plan (0, 0, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &p), "0 x 0: no plan");
    CHECK (!walpe_bloom_plan (-5, 100, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &p), "negative: no plan");
    CHECK (!walpe_bloom_plan (100000, 100, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &p), "100000 wide: no plan");
    CHECK (p.bright_w == 0 && p.px_add == 0, "a refused plan is empty");
    /* the numbers of the standard chain at 1080p, as a fixed point */
    CHECK (walpe_bloom_plan (1920, 1080, 1920, 1080, WALPE_BLOOM_SRC_HALF_COPY, WALPE_BLOOM_FULL, &p),
           "1080p from the window");
    CHECK (p.copy_w == 960 && p.copy_h == 540 && p.bright_w == 480 && p.bright_h == 270 &&
           p.blur_w == 240 && p.blur_h == 135 && p.bright_taps == 1 && p.blur_taps == 13,
           "1080p: copy %dx%d bright %dx%d blur %dx%d", p.copy_w, p.copy_h, p.bright_w, p.bright_h,
           p.blur_w, p.blur_h);
    CHECK (NEAR (p.blur_step[0] * 1920, 8.0) && NEAR (p.blur_step[1] * 1080, 8.0),
           "1080p: taps 8 pixels apart");
    /* in texels of the blur target that is one texel: the blur of the
     * standard chain */
    CHECK (NEAR (p.blur_step[0] * p.blur_w, 1.0) && NEAR (p.blur_step[1] * p.blur_h, 1.0),
           "1080p: one blur texel per tap");
    /* 4K: the same chain with twice the targets; the glow reaches as many
     * pixels, i.e. half as far across the picture */
    CHECK (walpe_bloom_plan (3840, 2160, 3840, 2160, WALPE_BLOOM_SRC_HALF_COPY, WALPE_BLOOM_FULL, &q),
           "4K from the window");
    CHECK (q.bright_w == 960 && q.blur_w == 480 && NEAR (q.blur_step[0] * 3840, 8.0) &&
           NEAR (q.blur_step[0] * 2, p.blur_step[0]), "4K: targets double, the step in pixels stays");
    /* 4K monitor, frame rendered at half size: the targets (the work) are
     * those of 1080p, the glow reaches as far on the monitor as at 4K */
    CHECK (walpe_bloom_plan (1920, 1080, 3840, 2160, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &q),
           "half-size frame for a 4K reference");
    CHECK (q.bright_w == 480 && q.blur_w == 240 && q.copy_w == 0 && q.bright_taps == 2,
           "half-size frame: 1080p targets, no copy");
    CHECK (NEAR (q.blur_step[0] * 3840, 8.0) && NEAR (q.blur_step[0] * q.blur_w, 0.5),
           "half-size frame: taps 8 monitor pixels = half a blur texel apart");
    /* the reduced level: a quarter of the small-target pixels, the same reach */
    CHECK (walpe_bloom_plan (1920, 1080, 1920, 1080, WALPE_BLOOM_SRC_HALF_COPY, WALPE_BLOOM_REDUCED, &q),
           "1080p reduced");
    CHECK (q.bright_w == 240 && q.blur_w == 120 && q.blur_taps == 7 && q.bright_taps == 2 &&
           q.px_add == p.px_add, "reduced: targets halved again");
    CHECK (NEAR (q.blur_step[0] * 3, p.blur_step[0] * 6), "reduced: 3 wide steps = 6 steps");
    /* (at a size that divides by 16 the small targets are exactly a quarter) */
    CHECK (walpe_bloom_plan (2560, 1440, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_FULL, &p) &&
           walpe_bloom_plan (2560, 1440, 0, 0, WALPE_BLOOM_SRC_TEXTURE, WALPE_BLOOM_REDUCED, &q) &&
           q.px_small * 4 == p.px_small && p.bright_taps == 2 && q.bright_taps == 4,
           "1440p: reduced shades a quarter of the small-target pixels");
}

/* -------------------------------------------------------- camera shake */

static void test_shake (void)
{
    float o[2], p[2];
    walpe_camera_shake (3.0, 0.0f, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "amplitude 0: %g %g", (double) o[0], (double) o[1]);
    walpe_camera_shake (3.0, 1.0f, 1.0f, 0.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "speed 0 rests at the centre: %g %g", (double) o[0], (double) o[1]);
    walpe_camera_shake (3.0, 1.0f, 1.0f, -2.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "a negative speed is none");
    walpe_camera_shake (-1.0, 1.0f, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "negative time: %g %g", (double) o[0], (double) o[1]);
    walpe_camera_shake (NAN, 1.0f, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "time that is no number");
    walpe_camera_shake (INFINITY, 1.0f, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "infinite time");
    walpe_camera_shake (1e300, 1.0f, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "a time beyond any clock: %g %g", (double) o[0], (double) o[1]);
    walpe_camera_shake (2.0, NAN, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "amplitude that is no number");
    walpe_camera_shake (2.0, 1.0f, NAN, 3.0f, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "roughness that is no number");
    walpe_camera_shake (2.0, 1.0f, 1.0f, INFINITY, 1920, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "infinite speed");
    walpe_camera_shake (2.0, 1.0f, 1.0f, 3.0f, NAN, 1080, o);
    CHECK (o[0] == 0 && o[1] == 0, "a scene size that is no number");

    CHECK (NEAR (walpe_camera_shake_reach (1.0f), 0.01) && NEAR (walpe_camera_shake_reach (-0.5f), 0.005) &&
           walpe_camera_shake_reach (0.0f) == 0 && NEAR (walpe_camera_shake_reach (1e9f), 1.0) &&
           walpe_camera_shake_reach (NAN) == 0, "reach: 1 %% of the scene per unit of amplitude");

    /* a minute at 60 fps, amplitude 1: inside 1 % of the scene, moving,
     * without jumps */
    const float bx = 0.01f * 1920, by = 0.01f * 1080;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, step = 0;
    walpe_camera_shake (0.0, 1.0f, 1.0f, 3.0f, 1920, 1080, p);
    for (int f = 1; f <= 3600; f++) {
        walpe_camera_shake (f / 60.0, 1.0f, 1.0f, 3.0f, 1920, 1080, o);
        minx = MIN (minx, o[0]);
        maxx = MAX (maxx, o[0]);
        miny = MIN (miny, o[1]);
        maxy = MAX (maxy, o[1]);
        step = MAX (step, MAX (fabsf (o[0] - p[0]) / bx, fabsf (o[1] - p[1]) / by));
        p[0] = o[0];
        p[1] = o[1];
    }
    CHECK (minx >= -bx - 1e-3f && maxx <= bx + 1e-3f && miny >= -by - 1e-3f && maxy <= by + 1e-3f,
           "inside the reach: x %g..%g of +-%g, y %g..%g of +-%g", (double) minx, (double) maxx,
           (double) bx, (double) miny, (double) maxy, (double) by);
    CHECK (maxx - minx > 0.6f * bx && maxy - miny > 0.6f * by, "it moves: x range %g, y range %g",
           (double) (maxx - minx), (double) (maxy - miny));
    CHECK (step < 0.3f, "no jumps: the largest step of a frame is %.3f of the reach", (double) step);
    /* a rough shake stays inside the same reach */
    float far = 0;
    for (int f = 0; f <= 3600; f++) {
        walpe_camera_shake (f / 60.0, 1.0f, 10.0f, 3.0f, 1920, 1080, o);
        far = MAX (far, MAX (fabsf (o[0]) / bx, fabsf (o[1]) / by));
    }
    CHECK (far <= 1.0f + 1e-4f, "roughness 10 stays inside the reach: %.4f", (double) far);
    /* roughness 0 is the slow sway alone: smoother than roughness 1 */
    float step0 = 0;
    walpe_camera_shake (0.0, 1.0f, 0.0f, 3.0f, 1920, 1080, p);
    for (int f = 1; f <= 3600; f++) {
        walpe_camera_shake (f / 60.0, 1.0f, 0.0f, 3.0f, 1920, 1080, o);
        step0 = MAX (step0, fabsf (o[0] - p[0]) / bx);
        p[0] = o[0];
    }
    CHECK (step0 < step, "roughness 0 moves in smaller steps (%.3f) than roughness 1 (%.3f)",
           (double) step0, (double) step);

    /* the two axes are not the same curve */
    walpe_camera_shake (7.3, 1.0f, 1.0f, 3.0f, 1000, 1000, o);
    CHECK (fabsf (o[0] - o[1]) > 1e-4f, "x and y differ: %g %g", (double) o[0], (double) o[1]);
    /* the same instant gives the same offset - every time, in every
     * process: the noise has fixed seeds */
    walpe_camera_shake (7.3, 1.0f, 1.0f, 3.0f, 1000, 1000, p);
    CHECK (o[0] == p[0] && o[1] == p[1], "the same instant, the same offset");
    /* amplitude scales it, a negative one mirrors it, the scene's size scales it */
    walpe_camera_shake (7.3, 2.0f, 1.0f, 3.0f, 1000, 1000, p);
    CHECK (NEAR (p[0], 2 * o[0]) && NEAR (p[1], 2 * o[1]), "amplitude 2 = twice: %g vs %g",
           (double) p[0], (double) o[0]);
    walpe_camera_shake (7.3, -1.0f, 1.0f, 3.0f, 1000, 1000, p);
    CHECK (NEAR (p[0], -o[0]) && NEAR (p[1], -o[1]), "amplitude -1 mirrors");
    walpe_camera_shake (7.3, 1.0f, 1.0f, 3.0f, 3000, 500, p);
    CHECK (NEAR (p[0], 3 * o[0]) && NEAR (p[1], 0.5 * o[1]), "the scene's size scales each axis");
    /* speed is a factor on time */
    walpe_camera_shake (7.3 / 2.0, 1.0f, 1.0f, 6.0f, 1000, 1000, p);
    CHECK (NEAR (p[0], o[0]) && NEAR (p[1], o[1]), "twice the speed at half the time");
    /* days of running time: finite and inside the reach */
    walpe_camera_shake (86400.0 * 400, 1.0f, 1.0f, 3.0f, 1920, 1080, o);
    CHECK (isfinite (o[0]) && isfinite (o[1]) && fabsf (o[0]) <= bx + 1e-3f && fabsf (o[1]) <= by + 1e-3f,
           "after 400 days: %g %g", (double) o[0], (double) o[1]);
    /* values beyond the clamps are taken as the clamps */
    walpe_camera_shake (7.3, 1e9f, 1.0f, 3.0f, 1000, 1000, o);
    walpe_camera_shake (7.3, WALPE_CAMSHAKE_MAX_AMPLITUDE, 1.0f, 3.0f, 1000, 1000, p);
    CHECK (o[0] == p[0] && o[1] == p[1], "amplitude is clamped");
    walpe_camera_shake (7.3, 1.0f, 1.0f, 1e9f, 1000, 1000, o);
    walpe_camera_shake (7.3, 1.0f, 1.0f, WALPE_CAMSHAKE_MAX_SPEED, 1000, 1000, p);
    CHECK (o[0] == p[0] && o[1] == p[1], "speed is clamped");
}

/* The shake is a function of the scene's clock: a monitor that draws 30,
 * 60 or 144 frames a second samples one and the same path. */
static void test_shake_frame_rates (void)
{
    static const int rates[3] = { 30, 60, 144 };
    const double seconds = 12.0;
    const float amp = 1.0f, rough = 1.0f, speed = 3.0f, sw = 3840, sh = 2160;
    /* 1. at instants all three rates draw a frame at (every 1/6 s:
     * frame 5 k of 30, 10 k of 60, 24 k of 144) the view is at the very
     * same place */
    int same = 0, instants = 0;
    for (int k = 0; k <= (int) (seconds * 6); k++) {
        float a[2], b[2], c[2];
        walpe_camera_shake ((5 * k) / 30.0, amp, rough, speed, sw, sh, a);
        walpe_camera_shake ((10 * k) / 60.0, amp, rough, speed, sw, sh, b);
        walpe_camera_shake ((24 * k) / 144.0, amp, rough, speed, sw, sh, c);
        instants++;
        if (a[0] == b[0] && a[1] == b[1] && a[0] == c[0] && a[1] == c[1])
            same++;
    }
    CHECK (same == instants, "30 / 60 / 144 fps: the same place at %d of %d common instants", same, instants);
    /* 2. the distance travelled in those seconds is the same path
     * measured with a finer ruler: more frames never shorten it and add
     * only a little; a frame-based shake would travel 2 and 4.8 times as
     * far at 60 and 144 fps */
    double len[3], maxstep[3];
    for (int r = 0; r < 3; r++) {
        float p[2], o[2];
        len[r] = maxstep[r] = 0;
        walpe_camera_shake (0.0, amp, rough, speed, sw, sh, p);
        for (int f = 1; f <= (int) (seconds * rates[r]); f++) {
            walpe_camera_shake ((double) f / rates[r], amp, rough, speed, sw, sh, o);
            double d = hypot (o[0] - p[0], o[1] - p[1]);
            len[r] += d;
            maxstep[r] = MAX (maxstep[r], d);
            p[0] = o[0];
            p[1] = o[1];
        }
    }
    CHECK (len[1] >= len[0] * 0.999 && len[2] >= len[1] * 0.999, "path length grows with the ruler: %g %g %g",
           len[0], len[1], len[2]);
    CHECK (len[1] < len[0] * 1.15 && len[2] < len[0] * 1.20,
           "... and only a little: %g at 30, %g at 60, %g at 144 fps", len[0], len[1], len[2]);
    /* 3. the step of one frame shrinks with the frame time */
    CHECK (maxstep[1] < maxstep[0] && maxstep[2] < maxstep[1],
           "the largest step of a frame: %g at 30, %g at 60, %g at 144 fps", maxstep[0], maxstep[1],
           maxstep[2]);
    CHECK (maxstep[2] < maxstep[0] * 0.45, "at 144 fps a step is well under half of one at 30 fps");
    /* 4. where the view is after a given time does not depend on how
     * many frames were drawn on the way (nothing is accumulated) */
    float direct[2], stepped[2];
    walpe_camera_shake (9.5, amp, rough, speed, sw, sh, direct);
    for (int f = 0; f <= 9 * 144 + 72; f++)
        walpe_camera_shake (f / 144.0, amp, rough, speed, sw, sh, stepped);
    CHECK (direct[0] == stepped[0] && direct[1] == stepped[1], "no state between frames");
}

int main (int argc, char **argv)
{
    setvbuf (stdout, NULL, _IONBF, 0);
    const gboolean env = argc > 1 && !strcmp (argv[1], "env");
    if (!env)
        g_unsetenv ("WALPE_FEATURES");
    char *tmp = g_dir_make_tmp ("walpe-postfx-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    test_switches (env);
    if (!env) {
        test_config (tmp);
        test_bloom_maths ();
        test_bloom_kernel ();
        test_bloom_plan ();
        test_shake ();
        test_shake_frame_rates ();
    }
    char *dir = g_build_filename (tmp, "walpe", NULL);
    g_rmdir (dir);
    g_free (dir);
    g_rmdir (tmp);
    g_free (tmp);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
