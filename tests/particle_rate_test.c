/* particle-rate-test - the particle simulation as a function of scene time,
 * no GL.
 *
 * A system that was stepped to scene time T holds the same particles
 * whatever the frame rate was: the same number of them, each with the age
 * T - (its place in the emitter's sequence) / rate, and - where nothing but
 * a constant velocity moves it - at the same place. The test steps the same
 * system at 30, 60, 144 and 200 Hz and with irregular steps (1 .. 40 ms from
 * a fixed sequence) and compares each run with the closed form:
 *   1. a rate emitter (several rates), long-lived particles;
 *   2. a burst, with and without a rate behind it;
 *   3. particles that die: who is alive at T;
 *   4. a full system: the cap holds at every rate;
 *   5. the instance's "rate" (the system's clock): 1 s at rate 2 is 2 s;
 *   6. eventspawn children: one burst per parent particle at any rate;
 *   7. frames of no length (the first frame of a scene, a frame drawn while
 *      the scene is paused) change nothing, bit for bit;
 *   8. two systems stepped in turns do not see each other (same numbers as
 *      alone, bit for bit), and a second run repeats the first.
 * Forces (gravity, drag, turbulence, control points) are integrated with
 * one explicit step per frame and are not part of this test.
 *
 * The particle files are written to a private temp dir. */
#include "engine/weparticles.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the engine's generators come from g_rand_new (): a fixed sequence here */
static guint32 seed_n;
GRand *g_rand_new (void)
{
    return g_rand_new_with_seed (0x1f123bb5u + 7919u * seed_n++);
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                 printf (__VA_ARGS__); printf ("\n"); } } while (0)

static char *tmpdir;

static void put (const char *rel, const char *text)
{
    char *p = g_build_filename (tmpdir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    if (!g_file_set_contents (p, text, -1, NULL)) {
        printf ("FAIL: cannot write %s\n", p);
        fails++;
    }
    g_free (d);
    g_free (p);
}

static WalpeWeParticles *make (const char *json)
{
    seed_n = 0;   /* every system of the test draws the same numbers */
    JsonNode *n = wpj_parse_data (json, -1);
    WalpeWeParticles *p = n ? walpe_weparticles_new (json_node_get_object (n), tmpdir, NULL, NULL) : NULL;
    if (n)
        json_node_unref (n);
    if (!p) {
        printf ("FAIL: system not created\n");
        fails++;
    }
    return p;
}

/* how the time up to T is cut into steps */
typedef struct {
    const char *name;
    int hz;            /* 0: irregular, 1 .. 40 ms */
    int zero_every;    /* > 0: a step of no length before every n-th step */
} Pattern;

static const Pattern patterns[] = {
    { "30 Hz", 30, 0 }, { "60 Hz", 60, 0 }, { "144 Hz", 144, 0 }, { "200 Hz", 200, 0 },
    { "irregular steps", 0, 0 },
};

/* other: a second system stepped in turns with p (or NULL) */
static void advance (WalpeWeParticles *p, const Pattern *pat, double T, WalpeWeParticles *other)
{
    if (pat->hz > 0) {
        int n = (int) lround (T * pat->hz);
        for (int i = 0; i < n; i++) {
            if (pat->zero_every > 0 && i % pat->zero_every == 0)
                walpe_weparticles_step (p, 0.0);
            walpe_weparticles_step (p, 1.0 / pat->hz);
            if (other)
                walpe_weparticles_step (other, 1.0 / 60.0);
        }
        return;
    }
    GRand *r = g_rand_new_with_seed (20261111u);
    double t = 0;
    while (t < T - 1e-12) {
        double dt = 0.001 + 0.039 * g_rand_double (r);
        if (t + dt > T)
            dt = T - t;
        walpe_weparticles_step (p, dt);
        t += dt;
    }
    g_rand_free (r);
}

static int cmp_float (const void *a, const void *b)
{
    float x = *(const float *) a, y = *(const float *) b;
    return x < y ? -1 : x > y;
}

/* ages of system `sys`, oldest last; *n = how many; *drift = how far the
 * worst particle is from origin + velocity x age */
static float *ages_of (WalpeWeParticles *p, int sys, int *n, float *drift)
{
    int cnt = walpe_weparticles_sys_count (p, sys);
    float *a = g_new0 (float, MAX (cnt, 1));
    float pos[3], vel[3], age;
    *drift = 0;
    for (int i = 0; i < cnt && walpe_weparticles_sys_peek (p, sys, i, pos, vel, &age); i++) {
        a[i] = age;
        for (int c = 0; c < 3; c++)
            *drift = MAX (*drift, fabsf (pos[c] - vel[c] * age));
    }
    qsort (a, (size_t) cnt, sizeof (float), cmp_float);
    *n = cnt;
    return a;
}

/* one emitter at the origin: `rate` a second, `burst` at once, particles
 * that live `life` seconds and fly at 120 / -45 / 0 units a second */
static char *sys_json (double rate, int burst, double life, int maxcount, const char *children)
{
    char r[G_ASCII_DTOSTR_BUF_SIZE], l[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr (r, sizeof r, rate);
    g_ascii_dtostr (l, sizeof l, life);
    return g_strdup_printf (
        "{ \"emitter\": [ { \"name\": \"boxrandom\", \"id\": 1, \"distancemax\": \"0 0 0\", \"rate\": %s,"
        "                   \"instantaneous\": %d } ],"
        "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": %s, \"max\": %s },"
        "                     { \"name\": \"velocityrandom\", \"id\": 3, \"min\": \"120 -45 0\", \"max\": \"120 -45 0\" } ],"
        "  \"operator\": [ { \"name\": \"movement\", \"id\": 4, \"gravity\": \"0 0 0\", \"drag\": 0 } ],"
        "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 5 } ],"
        "  \"children\": [ %s ],"
        "  \"maxcount\": %d, \"starttime\": 0 }", r, burst, l, l, children ? children : "", maxcount);
}

/* the particles a rate emitter has alive at T: the k-th was due at k / rate
 * (k = 1, 2, ...) and lives `life` seconds; ages oldest last */
static float *want_ages (double rate, double life, double T, int *n)
{
    int kmax = (int) floor (rate * T + 1e-9);
    float *a = g_new0 (float, MAX (kmax, 1));
    *n = 0;
    for (int k = kmax; k >= 1; k--) {
        double age = T - k / rate;
        if (age < life)
            a[(*n)++] = (float) age;
    }
    return a;
}

/* a rate emitter against the closed form, at every pattern */
static void check_rate (double rate, double life, double T, const char *what)
{
    for (guint k = 0; k < G_N_ELEMENTS (patterns); k++) {
        char *j = sys_json (rate, 0, life, 20000, NULL);
        WalpeWeParticles *p = make (j);
        g_free (j);
        if (!p)
            return;
        advance (p, &patterns[k], T, NULL);
        int n, wn;
        float drift;
        float *a = ages_of (p, 0, &n, &drift), *w = want_ages (rate, life, T, &wn);
        CHECK (n == wn, "%s, %g a second, %s: %d particles alive at %g s, %d expected", what, rate,
               patterns[k].name, n, T, wn);
        float worst = 0;
        for (int i = 0; i < MIN (n, wn); i++)
            worst = MAX (worst, fabsf (a[i] - w[i]));
        CHECK (worst < 5e-4f, "%s, %g a second, %s: a particle's age is %.5f s from T - k / rate", what, rate,
               patterns[k].name, (double) worst);
        CHECK (drift < 0.08f, "%s, %g a second, %s: a particle is %.4f units from velocity x age", what, rate,
               patterns[k].name, (double) drift);
        g_free (a);
        g_free (w);
        walpe_weparticles_free (p);
    }
}

/* everything the test can read of a system, for bit-for-bit comparisons */
static GByteArray *snapshot (WalpeWeParticles *p)
{
    GByteArray *b = g_byte_array_new ();
    for (int sys = 0; walpe_weparticles_sys_count (p, sys) >= 0; sys++) {
        int cnt = walpe_weparticles_sys_count (p, sys);
        g_byte_array_append (b, (const guint8 *) &cnt, sizeof cnt);
        float rec[8];
        for (int i = 0; i < cnt && walpe_weparticles_sys_peek (p, sys, i, rec, rec + 3, rec + 6); i++) {
            rec[7] = walpe_weparticles_sys_alpha (p, sys, i);
            g_byte_array_append (b, (const guint8 *) rec, sizeof rec);
        }
    }
    return b;
}

static gboolean same_bytes (GByteArray *a, GByteArray *b)
{
    return a->len == b->len && !memcmp (a->data, b->data, a->len);
}

int main (void)
{
    GError *err = NULL;
    tmpdir = g_dir_make_tmp ("walpe-ptrate-XXXXXX", &err);
    if (!tmpdir) {
        printf ("FAIL: no temp dir: %s\n", err->message);
        return 1;
    }
    /* three drops at once that stay where they are born and live long */
    put ("particles/drops.json",
         "{ \"emitter\": [ { \"name\": \"sphererandom\", \"id\": 1, \"distancemax\": 0, \"instantaneous\": 3, \"rate\": 0 } ],"
         "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 50, \"max\": 50 } ],"
         "  \"operator\": [ { \"name\": \"movement\", \"id\": 3 } ],"
         "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 4 } ], \"maxcount\": 3 }");
    const double T = 2.0;

    /* --- 1. rate emitters, nothing dies --- */
    /* (rate x T is no whole number: the particle due exactly at T would be
     * a matter of rounding) */
    static const double rates[] = { 7.3, 13.7, 50.3, 333.3 };
    for (guint r = 0; r < G_N_ELEMENTS (rates); r++)
        check_rate (rates[r], 100.0, T, "rate emitter");

    /* --- 2. a burst is due at time 0, once --- */
    for (int with_rate = 0; with_rate < 2; with_rate++)
        for (guint k = 0; k < G_N_ELEMENTS (patterns); k++) {
            char *j = sys_json (with_rate ? 10.3 : 0.0, 17, 100.0, 20000, NULL);
            WalpeWeParticles *p = make (j);
            g_free (j);
            if (!p)
                continue;
            advance (p, &patterns[k], T, NULL);
            int n, wn = 0;
            float drift;
            float *a = ages_of (p, 0, &n, &drift);
            float *w = with_rate ? want_ages (10.3, 100.0, T, &wn) : NULL;
            CHECK (n == 17 + wn, "burst of 17%s, %s: %d particles", with_rate ? " and 10.3 a second" : "",
                   patterns[k].name, n);
            float worst = 0;
            for (int i = 0; i < MIN (n, 17); i++)   /* the 17 oldest */
                worst = MAX (worst, fabsf (a[n - 1 - i] - (float) T));
            CHECK (worst < 5e-4f, "burst%s, %s: its particles are %.5f s from age T", with_rate ? " + rate" : "",
                   patterns[k].name, (double) worst);
            CHECK (drift < 0.08f, "burst%s, %s: a particle is %.4f units from velocity x age",
                   with_rate ? " + rate" : "", patterns[k].name, (double) drift);
            g_free (a);
            g_free (w);
            walpe_weparticles_free (p);
        }

    /* --- 3. particles that die: alive at T are those younger than their life
     * (40.3 a second, 0.52 s: no age within 8 ms of the limit at T) --- */
    check_rate (40.3, 0.52, T, "short-lived particles");

    /* --- 4. the cap: 300 a second into a system of 64 that live 1 s --- */
    for (guint k = 0; k < G_N_ELEMENTS (patterns); k++) {
        char *j = sys_json (300.7, 0, 1.0, 64, NULL);
        WalpeWeParticles *p = make (j);
        g_free (j);
        if (!p)
            continue;
        int over = 0;
        const Pattern half = patterns[k];
        for (int part = 0; part < 4; part++) {
            advance (p, &half, T / 4, NULL);
            over += walpe_weparticles_sys_count (p, 0) > 64;
        }
        int n = walpe_weparticles_sys_count (p, 0);
        CHECK (over == 0 && n >= 60 && n <= 64, "full system, %s: %d of 64 alive at %g s (never more than 64: %s)",
               patterns[k].name, n, T, over ? "no" : "yes");
        walpe_weparticles_free (p);
    }

    /* --- 5. the instance's "rate" is the system's clock --- */
    for (guint k = 0; k < G_N_ELEMENTS (patterns); k++) {
        char *j = sys_json (13.7, 0, 100.0, 20000, NULL);
        WalpeWeParticles *p = make (j);
        g_free (j);
        if (!p)
            continue;
        WalpeWeParticleOverride ov;
        walpe_we_particle_override_default (&ov);
        ov.rate = 2.0f;
        walpe_weparticles_set_override (p, &ov, WALPE_WE_OV_RATE);
        advance (p, &patterns[k], T / 2, NULL);
        int n, wn;
        float drift;
        float *a = ages_of (p, 0, &n, &drift), *w = want_ages (13.7, 100.0, T, &wn);
        float worst = 0;
        for (int i = 0; i < MIN (n, wn); i++)
            worst = MAX (worst, fabsf (a[i] - w[i]));
        CHECK (n == wn && worst < 5e-4f && drift < 0.08f,
               "rate 2, %s: %g s of the frame clock are %g s of the system (%d particles, %d expected; ages %.5f s "
               "off, places %.4f)", patterns[k].name, T / 2, T, n, wn, (double) worst, (double) drift);
        g_free (a);
        g_free (w);
        walpe_weparticles_free (p);
    }

    /* --- 6. eventspawn children: three drops per parent particle --- */
    for (guint k = 0; k < G_N_ELEMENTS (patterns); k++) {
        char *j = sys_json (23.3, 0, 100.0, 1000,
                            "{ \"id\": 9, \"name\": \"particles/drops.json\", \"type\": \"eventspawn\", \"maxcount\": 1000 }");
        WalpeWeParticles *p = make (j);
        g_free (j);
        if (!p)
            continue;
        advance (p, &patterns[k], T, NULL);
        int n0 = walpe_weparticles_sys_count (p, 0), n1 = walpe_weparticles_sys_count (p, 1);
        CHECK (n0 == 46 && n1 == 3 * n0, "eventspawn, %s: %d parents (46 expected), %d drops (3 each)",
               patterns[k].name, n0, n1);
        /* every drop lies where a parent was born: the emitter */
        float pos[3], vel[3], age, far = 0;
        for (int i = 0; walpe_weparticles_sys_peek (p, 1, i, pos, vel, &age); i++)
            far = MAX (far, hypotf (pos[0], pos[1]));
        CHECK (far < 1e-3f, "eventspawn, %s: a drop is %.4f units from where its parent was born",
               patterns[k].name, (double) far);
        walpe_weparticles_free (p);
    }

    /* --- 7. steps of no length change nothing --- */
    {
        char *j = sys_json (50.3, 5, 0.52, 20000,
                            "{ \"id\": 9, \"name\": \"particles/drops.json\", \"type\": \"eventdeath\", \"maxcount\": 1000 }");
        const Pattern plain = { "60 Hz", 60, 0 }, zeros = { "60 Hz with empty frames", 60, 7 };
        WalpeWeParticles *a = make (j), *b = make (j);
        if (a && b) {
            advance (a, &plain, T, NULL);
            walpe_weparticles_step (b, 0.0);
            walpe_weparticles_step (b, 0.0);
            advance (b, &zeros, T, NULL);
            walpe_weparticles_step (b, 0.0);
            GByteArray *sa = snapshot (a), *sb = snapshot (b);
            CHECK (sa->len > 64 && same_bytes (sa, sb),
                   "frames of no length: the same particles as without them (%u bytes of state, %s)", sa->len,
                   same_bytes (sa, sb) ? "equal" : "different");
            g_byte_array_unref (sa);
            g_byte_array_unref (sb);
        }
        walpe_weparticles_free (a);
        walpe_weparticles_free (b);

        /* --- 8. a second run repeats the first; a system stepped in turns
         * with another one holds what it holds alone --- */
        WalpeWeParticles *c = make (j), *d = make (j);
        char *oj = sys_json (333.3, 40, 0.3, 500, NULL);
        seed_n = 100;
        JsonNode *on = wpj_parse_data (oj, -1);
        WalpeWeParticles *other = on ? walpe_weparticles_new (json_node_get_object (on), tmpdir, NULL, NULL) : NULL;
        if (on)
            json_node_unref (on);
        g_free (oj);
        if (c && d && other) {
            const Pattern at144 = { "144 Hz", 144, 0 };
            advance (c, &at144, T, NULL);
            advance (d, &at144, T, other);
            GByteArray *sc = snapshot (c), *sd = snapshot (d);
            CHECK (sc->len > 64 && same_bytes (sc, sd),
                   "two systems stepped in turns: the first holds what it holds alone (%u bytes of state, %s)",
                   sc->len, same_bytes (sc, sd) ? "equal" : "different");
            CHECK (walpe_weparticles_count (other) > 0, "(the second system ran: %d particles)",
                   walpe_weparticles_count (other));
            WalpeWeParticles *e = make (j);
            if (e) {
                advance (e, &at144, T, NULL);
                GByteArray *se = snapshot (e);
                CHECK (same_bytes (sc, se), "a second run with the same seed repeats the first, bit for bit");
                g_byte_array_unref (se);
                walpe_weparticles_free (e);
            }
            g_byte_array_unref (sc);
            g_byte_array_unref (sd);
        }
        walpe_weparticles_free (c);
        walpe_weparticles_free (d);
        walpe_weparticles_free (other);
        g_free (j);
    }

    /* the temp dir */
    char *f = g_build_filename (tmpdir, "particles", "drops.json", NULL);
    g_unlink (f);
    g_free (f);
    f = g_build_filename (tmpdir, "particles", NULL);
    g_rmdir (f);
    g_free (f);
    g_rmdir (tmpdir);
    g_free (tmpdir);

    printf ("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
