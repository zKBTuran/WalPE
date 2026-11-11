/* weparticles-test - particle instance overrides "count" and "rate".
 *
 * Wallpaper Engine defines them for a particle instance (IParticleSystemInstance
 * in lib.sceneScript.d.ts): count is the "multiplier of particle being
 * emitted", rate the "playback rate simulation and emission". Scripts set
 * them (walpe_weparticles_set_override: 3151551777 switches its night birds
 * off with count 0, 3042492564 makes click sparks with count = n / frametime
 * for one frame); the scene file's values follow the same definition when
 * WALPE_WE_STATIC_OVERRIDES_AS_WE is set, and version 0.12's reading otherwise
 * (count = only the cap of live particles, rate = factor on the emission).
 *
 * One system: 20 particles a second from one point, 4 s life, 100 units/s to
 * the right, cap 400, no forces. No GL, no files; 30 steps a second. */
#include "engine/weparticles.h"
#include "common/util.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf (__VA_ARGS__); printf ("\n"); } \
                           else { printf ("ok: "); printf (__VA_ARGS__); printf ("\n"); } } while (0)

static const char *SYS_JSON =
    "{ \"emitter\": [ { \"name\": \"boxrandom\", \"id\": 1, \"distancemax\": \"0 0 0\", \"rate\": 20 } ],"
    "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 4, \"max\": 4 },"
    "                     { \"name\": \"velocityrandom\", \"id\": 3, \"min\": \"100 0 0\", \"max\": \"100 0 0\" } ],"
    "  \"operator\": [ { \"name\": \"movement\", \"id\": 4, \"gravity\": \"0 0 0\", \"drag\": 0 } ],"
    "  \"renderer\": [ { \"name\": \"spritetrail\", \"id\": 5, \"length\": 0.05, \"maxlength\": 10 } ],"
    "  \"maxcount\": 400, \"starttime\": 0 }";

static WalpeWeParticles *make (float count, float rate)
{
    JsonNode *n = wpj_parse_data (SYS_JSON, -1);
    WalpeWeParticleOverride ov;
    walpe_we_particle_override_default (&ov);
    ov.count = count;
    ov.rate = rate;
    WalpeWeParticles *p = n ? walpe_weparticles_new (json_node_get_object (n), "/nonexistent", NULL, &ov) : NULL;
    if (n)
        json_node_unref (n);
    return p;
}

static void run (WalpeWeParticles *p, double seconds)
{
    for (int k = 0; k < (int) (seconds * 30 + 0.5); k++)
        walpe_weparticles_step (p, 1.0 / 30.0);
}

static void set (WalpeWeParticles *p, float count, float rate, guint scripted)
{
    WalpeWeParticleOverride ov;
    walpe_we_particle_override_default (&ov);
    ov.count = count;
    ov.rate = rate;
    walpe_weparticles_set_override (p, &ov, scripted);
}

/* the oldest live particle: age, distance travelled, speed */
static gboolean oldest (WalpeWeParticles *p, float *age, float *x, float *speed)
{
    float pos[3], vel[3], a;
    gboolean any = FALSE;
    *age = -1;
    for (int i = 0; walpe_weparticles_peek (p, i, pos, vel, &a); i++)
        if (a > *age) {
            *age = a;
            *x = pos[0];
            *speed = sqrtf (vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
            any = TRUE;
        }
    return any;
}

int main (void)
{
    float age, x, speed;
    WalpeWeParticles *p = make (1, 1);
    if (!p) {
        printf ("FAIL: the test system does not load\n");
        return 1;
    }
    run (p, 8);
    int base = walpe_weparticles_count (p);
    CHECK (base >= 78 && base <= 81, "no override: 20/s x 4 s = about 80 alive (%d)", base);
    CHECK (oldest (p, &age, &x, &speed) && fabsf (speed - 100) < 0.01f, "speed 100 (%g)", speed);

    /* --- values a script drives: Wallpaper Engine's meaning --- */
    set (p, 0.13f, 1, WALPE_WE_OV_COUNT);
    run (p, 8);
    int n = walpe_weparticles_count (p);
    CHECK (n >= 9 && n <= 12, "script, count 0.13: about 10 alive (%d)", n);
    set (p, 3, 1, WALPE_WE_OV_COUNT);
    run (p, 8);
    n = walpe_weparticles_count (p);
    CHECK (n >= 236 && n <= 242, "script, count 3: about 240 alive (%d)", n);
    set (p, 0, 1, WALPE_WE_OV_COUNT);
    run (p, 2);
    n = walpe_weparticles_count (p);
    CHECK (n >= 115 && n <= 125, "script, count 0: nothing new, the live ones stay (%d after 2 s)", n);
    run (p, 2.2);
    CHECK (walpe_weparticles_count (p) == 0, "script, count 0: none left after their 4 s (%d)", walpe_weparticles_count (p));
    /* one frame of count = n / frametime (3042492564's click sparks): n times
     * the emitter's rate in particles, whatever the frame time */
    set (p, 5 * 30, 1, WALPE_WE_OV_COUNT);
    run (p, 1.0 / 30.0);
    set (p, 0, 1, WALPE_WE_OV_COUNT);
    run (p, 1);
    n = walpe_weparticles_count (p);
    CHECK (n >= 99 && n <= 100, "script, count = 5 / frametime for one frame: 5 x 20 particles (%d)", n);
    walpe_weparticles_free (p);

    /* rate: the clock of the system. Twice the rate = the same number alive,
     * each for half as long on the wall clock, at the same stored speed
     * (the trail renderer takes its length from that) and the same path. */
    p = make (1, 1);
    WalpeWeParticles *q = make (1, 1);
    set (q, 1, 2, WALPE_WE_OV_RATE);
    run (p, 2);
    run (q, 1);
    float age2, x2, speed2;
    CHECK (oldest (p, &age, &x, &speed) && oldest (q, &age2, &x2, &speed2) && fabsf (age - age2) < 0.07f &&
           fabsf (x - x2) < 7 && speed == speed2,
           "script, rate 2: after 1 s what rate 1 shows after 2 s (age %.2f / %.2f, x %.0f / %.0f, speed %g / %g)",
           age, age2, x, x2, speed, speed2);
    run (p, 6);
    run (q, 7);
    CHECK (abs (walpe_weparticles_count (p) - walpe_weparticles_count (q)) <= 1,
           "script, rate 2: as many alive (%d / %d)", walpe_weparticles_count (p), walpe_weparticles_count (q));
    set (p, 0, 1, WALPE_WE_OV_COUNT);
    set (q, 0, 2, WALPE_WE_OV_COUNT | WALPE_WE_OV_RATE);
    run (p, 2.1);
    run (q, 2.1);
    CHECK (walpe_weparticles_count (p) > 30 && walpe_weparticles_count (q) == 0,
           "script, rate 2: a particle lives 2 s of wall clock instead of 4 (left after 2.1 s: %d / %d)",
           walpe_weparticles_count (p), walpe_weparticles_count (q));
    set (q, 1, 0, WALPE_WE_OV_RATE);
    run (q, 3);
    CHECK (walpe_weparticles_count (q) == 0, "script, rate 0: the system stands still (%d)", walpe_weparticles_count (q));
    walpe_weparticles_free (p);
    walpe_weparticles_free (q);

    /* --- values of the scene file --- */
    p = make (0.13f, 1);
    run (p, 8);
    n = walpe_weparticles_count (p);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (n >= 9 && n <= 12, "scene file, count 0.13: about 10 alive (%d)", n);
    else
        CHECK (n == 52, "scene file, count 0.13, version 0.12: the cap 400 x 0.13 (%d)", n);
    walpe_weparticles_free (p);
    p = make (3, 1);
    run (p, 8);
    n = walpe_weparticles_count (p);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (n >= 236 && n <= 242, "scene file, count 3: about 240 alive (%d)", n);
    else
        CHECK (n >= 78 && n <= 81, "scene file, count 3, version 0.12: as without it (%d)", n);
    walpe_weparticles_free (p);
    /* the cap: count 8 would keep 20/s x 8 x 4 s = 640 alive, the particle
     * file allows 400 (WALPE_WE_CAP_FOLLOWS_COUNT) */
    /* WALPE_PT_CAP=count in the environment: the cap
     * follows count at run time; meson runs this test a second time with it */
    gboolean cap_count = WALPE_WE_CAP_FOLLOWS_COUNT || !g_strcmp0 (g_getenv ("WALPE_PT_CAP"), "count");
    p = make (8, 1);
    run (p, 8);
    n = walpe_weparticles_count (p);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE && !cap_count)
        CHECK (n >= 396 && n <= 400, "scene file, count 8: the file's cap of 400 holds (%d)", n);
    else if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (n >= 630 && n <= 645, "scene file, count 8, cap x count: about 640 alive (%d)", n);
    walpe_weparticles_free (p);
    /* ... and a count a script drives does not move it either */
    p = make (1, 1);
    run (p, 1);
    set (p, 8, 1, WALPE_WE_OV_COUNT);
    run (p, 8);
    n = walpe_weparticles_count (p);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE && !cap_count)
        CHECK (n >= 396 && n <= 400, "script, count 8: the file's cap of 400 holds (%d)", n);
    else if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (n >= 630 && n <= 645, "script, count 8, cap x count: about 640 alive (%d)", n);
    walpe_weparticles_free (p);
    /* a system that fills its cap whatever count is (the dust of
     * 3163060610: 100 a second, 10 - 25 s, maxcount 50, a user's "Count"
     * slider on it): count does nothing under the file's cap and scales
     * the amount with WALPE_PT_CAP=count. Here 20 a second x 4 s = 80
     * against a cap of 400: count 20 and count 40 both sit at the cap. */
    p = make (20, 1);
    run (p, 8);
    n = walpe_weparticles_count (p);
    WalpeWeParticles *q2 = make (40, 1);
    run (q2, 8);
    int n2 = walpe_weparticles_count (q2);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE && !cap_count)
        CHECK (n >= 396 && n <= 400 && n2 >= 396 && n2 <= 400,
               "a system at its cap: count 20 and 40 give the same %d / %d", n, n2);
    else if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (n >= 1570 && n <= 1610 && n2 >= 3150 && n2 <= 3210,
               "WALPE_PT_CAP=count: count 20 and 40 give %d / %d", n, n2);
    walpe_weparticles_free (p);
    walpe_weparticles_free (q2);
    p = make (1, 2);
    run (p, 1);
    gboolean any = oldest (p, &age, &x, &speed);
    run (p, 7);
    n = walpe_weparticles_count (p);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (any && age > 1.85f && age < 2.05f && speed == 100 && n >= 78 && n <= 81,
               "scene file, rate 2: twice the clock (age %.2f after 1 s, speed %g, %d alive)", age, speed, n);
    else
        CHECK (any && age > 0.9f && age < 1.05f && speed == 100 && n >= 158 && n <= 161,
               "scene file, rate 2, version 0.12: twice the emission (age %.2f after 1 s, speed %g, %d alive)", age,
               speed, n);
    walpe_weparticles_free (p);
    p = make (0, 1);
    run (p, 4);
    n = walpe_weparticles_count (p);
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        CHECK (n == 0, "scene file, count 0: nothing (%d)", n);
    else
        CHECK (n >= 78 && n <= 81, "scene file, count 0, version 0.12: not set (%d)", n);
    walpe_weparticles_free (p);

    if (fails) {
        printf ("%d check(s) failed\n", fails);
        return 1;
    }
    printf ("all checks passed\n");
    return 0;
}
