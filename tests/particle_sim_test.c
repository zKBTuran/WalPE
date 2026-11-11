/* particle-sim-test - the particle simulation, no GL:
 * child systems that follow events of their parent (1 - 9), control points
 * placed in the scene (10), a burst whose emitter's duration is shorter
 * than the first step (11), the two readings of alphafade's fade-out time
 * (12; meson runs this source a second time with WALPE_PT_FADE=span).
 *
 * Wallpaper Engine starts one instance of an "eventfollow" / "eventspawn" /
 * "eventdeath" child system per parent particle. The child file's maxcount
 * is the cap of ONE instance; the child reference's "maxcount" is how many
 * instances may run at a time (WE's shooting star preset: glow file
 * maxcount 4, reference maxcount 100 = the parent's own maxcount; rain
 * splash drops: 5 and 350; pentagram shine: 1 and 500). An eventfollow
 * instance starts when its particle is born (its "instantaneous" emitters
 * fire then: the glow at the head of a shooting star), "probability" is the
 * chance that a particle gets one, and unless the child file has the
 * world-space flag its particles move with the particle they belong to (a
 * flock behind its lead bird).
 *
 * the earlier build kept one pool of the file's maxcount for all instances (4 glows for
 * 35 stars, 5 drops for 350 splashes), never fired a follow child's burst
 * (no glow at all on a shooting star) and left follow particles behind.
 *
 * No GL; the particle files are written to a private temp dir. */
#include "engine/weparticles.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* the engine's generators come from g_rand_new (): a fixed sequence here */
static guint32 seed_n;
GRand *g_rand_new (void)
{
    return g_rand_new_with_seed (0x2545f491u + 104729u * seed_n++);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf (__VA_ARGS__); printf ("\n"); } \
                           else { printf ("ok: "); printf (__VA_ARGS__); printf ("\n"); } } while (0)

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

static WalpeWeParticles *make (const char *json, float count)
{
    JsonNode *n = wpj_parse_data (json, -1);
    WalpeWeParticleOverride ov;
    walpe_we_particle_override_default (&ov);
    ov.count = count;
    WalpeWeParticles *p = n ? walpe_weparticles_new (json_node_get_object (n), tmpdir, NULL, &ov) : NULL;
    if (n)
        json_node_unref (n);
    if (!p) {
        printf ("FAIL: system not created\n");
        fails++;
    }
    return p;
}

static void run (WalpeWeParticles *p, double seconds)
{
    for (int k = 0; k < (int) (seconds * 60 + 0.5); k++)
        walpe_weparticles_step (p, 1.0 / 60.0);
}

/* how many live particles of system `sys` sit on a live particle of the
 * root (within eps), and the largest distance of the others to the nearest */
static int on_parent (WalpeWeParticles *p, int sys, float eps, float *worst)
{
    float cp[3], cv[3], ca, pp[3], pv[3], pa;
    int hit = 0;
    *worst = 0;
    for (int i = 0; walpe_weparticles_sys_peek (p, sys, i, cp, cv, &ca); i++) {
        float best = 1e30f;
        for (int k = 0; walpe_weparticles_sys_peek (p, 0, k, pp, pv, &pa); k++) {
            float d = hypotf (cp[0] - pp[0], cp[1] - pp[1]);
            best = MIN (best, d);
        }
        if (best <= eps)
            hit++;
        else
            *worst = MAX (*worst, best);
    }
    return hit;
}

/* a parent: `rate` particles a second from one point, `life` seconds,
 * 100 units/s to the right and 30 up, cap `max`; one child reference */
static char *parent_json (double rate, double life, int max, const char *child_ref)
{
    return g_strdup_printf (
        "{ \"emitter\": [ { \"name\": \"boxrandom\", \"id\": 1, \"distancemax\": \"0 0 0\", \"rate\": %g } ],"
        "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": %g, \"max\": %g },"
        "                     { \"name\": \"velocityrandom\", \"id\": 3, \"min\": \"100 30 0\", \"max\": \"100 30 0\" } ],"
        "  \"operator\": [ { \"name\": \"movement\", \"id\": 4, \"gravity\": \"0 0 0\", \"drag\": 0 } ],"
        "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 5 } ],"
        "  \"children\": [ %s ],"
        "  \"maxcount\": %d, \"starttime\": 0 }", rate, life, life, child_ref, max);
}

int main (void)
{
    GError *err = NULL;
    tmpdir = g_dir_make_tmp ("walpe-ptchild-XXXXXX", &err);
    if (!tmpdir) {
        printf ("FAIL: no temp dir: %s\n", err->message);
        return 1;
    }
    /* a glow: one particle at once, 1.5 s, no velocity, cap 4 (WE's
     * shootingstarglow has exactly this shape) */
    put ("particles/glow.json",
         "{ \"emitter\": [ { \"name\": \"sphererandom\", \"id\": 1, \"distancemax\": 0, \"instantaneous\": 1, \"rate\": 0 } ],"
         "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 1.5, \"max\": 1.5 } ],"
         "  \"operator\": [ { \"name\": \"movement\", \"id\": 3 } ],"
         "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 4 } ], \"maxcount\": 4 }");
    /* a trail left behind in the world (flag 1): 10 a second, 1 s, cap 10
     * (WE's firefliestrail) */
    put ("particles/trail.json",
         "{ \"emitter\": [ { \"name\": \"sphererandom\", \"id\": 1, \"distancemax\": 0, \"rate\": 10 } ],"
         "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 1, \"max\": 1 } ],"
         "  \"operator\": [ { \"name\": \"movement\", \"id\": 3 } ],"
         "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 4 } ], \"flags\": 1, \"maxcount\": 10 }");
    /* drops: three at once, 1 s, cap 3 (the rain splash drops) */
    put ("particles/drops.json",
         "{ \"emitter\": [ { \"name\": \"sphererandom\", \"id\": 1, \"distancemax\": 4, \"instantaneous\": 3, \"rate\": 0 } ],"
         "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 1, \"max\": 1 } ],"
         "  \"operator\": [ { \"name\": \"movement\", \"id\": 3 } ],"
         "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 4 } ], \"flags\": 1, \"maxcount\": 3 }");
    /* a follower with its own drift and a rate emitter: 2 a second, 30 s,
     * cap 1 (the birds behind a lead bird) */
    put ("particles/wing.json",
         "{ \"emitter\": [ { \"name\": \"sphererandom\", \"id\": 1, \"distancemax\": 0, \"rate\": 2 } ],"
         "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 30, \"max\": 30 },"
         "                     { \"name\": \"velocityrandom\", \"id\": 3, \"min\": \"-10 0 0\", \"max\": \"-10 0 0\" } ],"
         "  \"operator\": [ { \"name\": \"movement\", \"id\": 4 } ],"
         "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 5 } ], \"flags\": 0, \"maxcount\": 1 }");

    char *j;
    float worst;
    int n0, n1, on;

    /* --- 1. eventfollow, burst only: every particle has its glow, on it --- */
    j = parent_json (10, 2, 100, "{ \"id\": 9, \"name\": \"particles/glow.json\", \"type\": \"eventfollow\", \"maxcount\": 100 }");
    WalpeWeParticles *p = make (j, 1);
    g_free (j);
    if (p) {
        CHECK (walpe_weparticles_systems (p) == 2 && walpe_weparticles_sys_count (p, 2) == -1,
               "one parent, one child system");
        run (p, 1.0);
        n0 = walpe_weparticles_sys_count (p, 0);
        n1 = walpe_weparticles_sys_count (p, 1);
        on = on_parent (p, 1, 0.01f, &worst);
        CHECK (n0 >= 9 && n0 <= 11, "follow: %d parents alive after 1 s", n0);
        CHECK (n1 == n0, "follow, burst only: one glow per parent particle (%d glows, %d parents)", n1, n0);
        CHECK (on == n1, "follow, not world space: the glows sit on their particles (%d of %d, worst %.1f away)",
               on, n1, (double) worst);
        /* 3 s in: parents live 2 s (20 alive), glows 1.5 s (15 alive) */
        run (p, 2.0);
        n0 = walpe_weparticles_sys_count (p, 0);
        n1 = walpe_weparticles_sys_count (p, 1);
        on = on_parent (p, 1, 0.01f, &worst);
        CHECK (n0 >= 19 && n0 <= 21 && n1 >= 14 && n1 <= 16,
               "follow: 20 parents carry 15 glows: more than one instance's cap of 4 (%d, %d)", n0, n1);
        CHECK (on == n1, "follow: every glow still on its particle after parents died around it (%d of %d, worst %.1f)",
               on, n1, (double) worst);
        walpe_weparticles_free (p);
    }

    /* --- 2. the reference's maxcount = instances at a time --- */
    j = parent_json (10, 2, 100, "{ \"id\": 9, \"name\": \"particles/glow.json\", \"type\": \"eventfollow\", \"maxcount\": 2 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 3.0);
        n1 = walpe_weparticles_sys_count (p, 1);
        CHECK (n1 >= 1 && n1 <= 8, "follow, reference maxcount 2: at most 2 instances x 4 (%d)", n1);
        walpe_weparticles_free (p);
    }

    /* --- 3. eventfollow, world space, rate: a trail behind each particle --- */
    j = parent_json (2, 2.5, 100, "{ \"id\": 9, \"name\": \"particles/trail.json\", \"type\": \"eventfollow\", \"maxcount\": 20 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 2.4);   /* four parents, none has died yet */
        n0 = walpe_weparticles_sys_count (p, 0);
        n1 = walpe_weparticles_sys_count (p, 1);
        /* parents 1.9, 1.4, 0.9 and 0.4 s old x 10 a second x at most 1 s */
        CHECK (n0 == 4 && n1 >= 28 && n1 <= 38,
               "follow, world space: %d parents leave %d trail particles (one pool of 10 held them before)", n0, n1);
        /* a trail particle stays where its parent was when it was emitted:
         * `age` seconds x the parent's speed behind one of the parents */
        float cp[3], cv[3], ca, pp[3], pv[3], pa;
        const float speed = hypotf (100.0f, 30.0f);
        int behind = 0, away = 0;
        for (int i = 0; walpe_weparticles_sys_peek (p, 1, i, cp, cv, &ca); i++) {
            float best = 1e30f, nearest = 1e30f;
            for (int k = 0; walpe_weparticles_sys_peek (p, 0, k, pp, pv, &pa); k++) {
                float d = hypotf (cp[0] - pp[0], cp[1] - pp[1]);
                best = MIN (best, fabsf (d - speed * ca));
                nearest = MIN (nearest, d);
            }
            behind += best < 2.5f;
            away += nearest > 10.0f;
        }
        CHECK (behind == n1 && away >= n1 / 3,
               "follow, world space: the trail stays behind (%d of %d where their parent was, %d more than 10 units "
               "from any parent)", behind, n1, away);
        walpe_weparticles_free (p);
    }

    /* --- 4. eventspawn: a burst where each particle is born --- */
    j = parent_json (20, 3, 100, "{ \"id\": 9, \"name\": \"particles/drops.json\", \"type\": \"eventspawn\", \"maxcount\": 100 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 3.0);
        n1 = walpe_weparticles_sys_count (p, 1);
        /* 20 births a second x 3 drops x 1 s */
        CHECK (n1 >= 54 && n1 <= 63, "spawn: 20 births a second keep %d drops alive (3, one instance's cap, before)", n1);
        float pos[3], vel[3], age, far = 0;
        for (int i = 0; walpe_weparticles_sys_peek (p, 1, i, pos, vel, &age); i++)
            far = MAX (far, hypotf (pos[0], pos[1]));
        CHECK (far <= 6.0f, "spawn: the drops stay where the particle was born (farthest %.1f from the emitter)", (double) far);
        walpe_weparticles_free (p);
    }

    /* --- 5. eventdeath --- */
    j = parent_json (20, 1, 100, "{ \"id\": 9, \"name\": \"particles/drops.json\", \"type\": \"eventdeath\", \"maxcount\": 100 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 0.9);
        CHECK (walpe_weparticles_sys_count (p, 1) == 0, "death: nothing before the first particle dies");
        run (p, 2.1);
        n1 = walpe_weparticles_sys_count (p, 1);
        float pos[3], vel[3], age, near = 1e30f;
        for (int i = 0; walpe_weparticles_sys_peek (p, 1, i, pos, vel, &age); i++)
            near = MIN (near, hypotf (pos[0], pos[1]));
        CHECK (n1 >= 54 && n1 <= 63 && near > 90.0f,
               "death: %d drops alive, all where a particle died (nearest %.0f from the emitter)", n1, (double) near);
        walpe_weparticles_free (p);
    }

    /* --- 6. probability: the chance that a particle gets an instance --- */
    j = parent_json (100, 4, 400, "{ \"id\": 9, \"name\": \"particles/glow.json\", \"type\": \"eventfollow\", \"maxcount\": 400, \"probability\": 0.5 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 1.0);
        n0 = walpe_weparticles_sys_count (p, 0);
        n1 = walpe_weparticles_sys_count (p, 1);
        CHECK (n0 >= 98 && n0 <= 101 && n1 >= 35 && n1 <= 65, "follow, probability 0.5: %d glows on %d parents", n1, n0);
        walpe_weparticles_free (p);
    }

    /* --- 7. a follower with a rate emitter and its own drift: carried along --- */
    j = parent_json (0.5, 30, 1, "{ \"id\": 9, \"name\": \"particles/wing.json\", \"type\": \"eventfollow\", \"maxcount\": 1 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 6.0);
        float pp[3], pv[3], pa, cp[3], cv[3], ca;
        gboolean a = walpe_weparticles_sys_peek (p, 0, 0, pp, pv, &pa);
        gboolean b = walpe_weparticles_sys_peek (p, 1, 0, cp, cv, &ca);
        CHECK (a && b && walpe_weparticles_sys_count (p, 1) == 1, "follower: one lead particle, one follower");
        if (a && b) {
            /* the follower was born on the lead `ca` seconds ago and has
             * drifted -10 units/s against it since */
            float want = pp[0] - 10.0f * ca;
            CHECK (fabsf (cp[0] - want) < 1.0f && fabsf (cp[1] - pp[1]) < 1.0f && pp[0] > 300.0f,
                   "follower: carried by the lead (lead x %.1f, follower x %.1f, expected %.1f; the earlier build left it at about %.0f)",
                   (double) pp[0], (double) cp[0], (double) want, (double) (pp[0] - 110.0f * ca));
        }
        walpe_weparticles_free (p);
    }

    /* --- 8. a static child keeps its own cap and position --- */
    j = parent_json (10, 2, 100, "{ \"id\": 9, \"name\": \"particles/trail.json\", \"origin\": \"50 0 0\" }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 4.0);
        n1 = walpe_weparticles_sys_count (p, 1);
        float pos[3], vel[3], age;
        gboolean a = walpe_weparticles_sys_peek (p, 1, 0, pos, vel, &age);
        CHECK (n1 >= 9 && n1 <= 10 && a && fabsf (pos[0] - 50.0f) < 0.01f,
               "static child: runs on its own at its offset, cap 10 (%d alive)", n1);
        walpe_weparticles_free (p);
    }

    /* --- 9. stop () while followers are attached, then play again --- */
    j = parent_json (30, 1, 100, "{ \"id\": 9, \"name\": \"particles/glow.json\", \"type\": \"eventfollow\", \"maxcount\": 100 }");
    p = make (j, 1);
    g_free (j);
    if (p) {
        run (p, 2.0);
        walpe_weparticles_set_playing (p, -1);
        CHECK (walpe_weparticles_count (p) == 0, "stop (): parents and followers gone");
        walpe_weparticles_set_playing (p, 1);
        run (p, 2.0);
        n0 = walpe_weparticles_sys_count (p, 0);
        n1 = walpe_weparticles_sys_count (p, 1);
        on = on_parent (p, 1, 0.01f, &worst);
        /* glows outlive their 1 s parents by half a second: those are left
         * where the parent died */
        CHECK (n0 >= 29 && n0 <= 31 && n1 >= 43 && n1 <= 47 && on >= n0 - 1,
               "play () again: %d parents, %d glows, %d of them on a live parent", n0, n1, on);
        walpe_weparticles_free (p);
    }

    /* --- 10. a control point placed in the scene ("Worldspace", flags 2) --- */
    for (int world = 0; world < 2; world++) {
        /* still particles at the system's origin, attracted by control
         * point 1 = "100 0 0"; the object sits at 300 0 in the scene. As a
         * place in the scene the point is 200 to the left of the system, as
         * an offset in the system 100 to its right. */
        char *cj = g_strdup_printf (
            "{ \"controlpoint\": [ { \"id\": 0, \"flags\": 0, \"offset\": \"0 0 0\" },"
            "                      { \"id\": 1, \"flags\": %d, \"offset\": \"0 0 0\" } ],"
            "  \"emitter\": [ { \"name\": \"boxrandom\", \"id\": 1, \"distancemax\": \"0 0 0\", \"rate\": 20 } ],"
            "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 4, \"max\": 4 } ],"
            "  \"operator\": [ { \"name\": \"movement\", \"id\": 3 },"
            "                  { \"name\": \"controlpointattract\", \"id\": 4, \"controlpoint\": 1, \"scale\": 50, \"threshold\": 2000 } ],"
            "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 5 } ], \"maxcount\": 100 }", world ? 2 : 0);
        JsonNode *n = wpj_parse_data (cj, -1);
        WalpeWeParticleOverride ov;
        walpe_we_particle_override_default (&ov);
        ov.cp[1][0] = 100.0f;
        ov.cp_set = 1u << 1;
        p = n ? walpe_weparticles_new (json_node_get_object (n), tmpdir, NULL, &ov) : NULL;
        if (n)
            json_node_unref (n);
        g_free (cj);
        if (!p) {
            fails++;
            printf ("FAIL: control point system not created\n");
            continue;
        }
        /* the object: twice as large, at 300 0 (column-major mat3) */
        const float wm[9] = { 2, 0, 0, 0, 2, 0, 300, 0, 1 };
        walpe_weparticles_set_world (p, wm);
        run (p, 1.0);
        float pos[3], vel[3], age;
        float minx = 1e30f, maxx = -1e30f;
        for (int i = 0; walpe_weparticles_peek (p, i, pos, vel, &age); i++) {
            minx = MIN (minx, pos[0]);
            maxx = MAX (maxx, pos[0]);
        }
        if (world)
            /* scene 100 0 is (100 - 300) / 2 = -100 in the system */
            CHECK (maxx <= 0.0f && minx < -5.0f,
                   "control point in the scene: particles are pulled to the left, where it is (x %.1f .. %.1f)",
                   (double) minx, (double) maxx);
        else
            CHECK (minx >= 0.0f && maxx > 5.0f,
                   "control point in the system: particles are pulled to its offset (x %.1f .. %.1f)",
                   (double) minx, (double) maxx);
        walpe_weparticles_free (p);
    }

    /* --- 11. a burst whose emitter lasts less than the first step --- */
    {
        JsonNode *n = wpj_parse_data (
            "{ \"emitter\": [ { \"name\": \"boxrandom\", \"id\": 1, \"distancemax\": \"0 0 0\", \"rate\": 0,"
            "                   \"instantaneous\": 12, \"duration\": 0.02 } ],"
            "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 2, \"max\": 2 } ],"
            "  \"operator\": [ { \"name\": \"movement\", \"id\": 3 } ],"
            "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 4 } ], \"maxcount\": 50 }", -1);
        p = n ? walpe_weparticles_new (json_node_get_object (n), tmpdir, NULL, NULL) : NULL;
        if (n)
            json_node_unref (n);
        if (p) {
            walpe_weparticles_step (p, 0.05);   /* longer than the emitter's 0.02 s */
            n0 = walpe_weparticles_count (p);
            walpe_weparticles_step (p, 0.05);
            CHECK (n0 == 12 && walpe_weparticles_count (p) == 12,
                   "burst of 12 with a duration of 0.02 s, first step 0.05 s: %d alive, once (%d)", n0,
                   walpe_weparticles_count (p));
            walpe_weparticles_free (p);
        }
    }

    /* --- 12. alphafade 0.1 / 0.9: where the fade-out is ---
     * default: fadeouttime is the point of the life where fading out starts
     * (plateau from 0.1 to 0.9); WALPE_PT_FADE=span: it is the length of the
     * fade-out (in over a tenth, out over the other nine tenths). The
     * second meson test of this source runs with the switch. */
    {
        const char *fe = g_getenv ("WALPE_PT_FADE");
        gboolean span = fe && !g_ascii_strcasecmp (fe, "span");
        JsonNode *n = wpj_parse_data (
            "{ \"emitter\": [ { \"name\": \"boxrandom\", \"id\": 1, \"distancemax\": \"0 0 0\", \"rate\": 0,"
            "                   \"instantaneous\": 1 } ],"
            "  \"initializer\": [ { \"name\": \"lifetimerandom\", \"id\": 2, \"min\": 10, \"max\": 10 } ],"
            "  \"operator\": [ { \"name\": \"alphafade\", \"id\": 3, \"fadeintime\": 0.1, \"fadeouttime\": 0.9 } ],"
            "  \"renderer\": [ { \"name\": \"sprite\", \"id\": 4 } ], \"maxcount\": 4 }", -1);
        p = n ? walpe_weparticles_new (json_node_get_object (n), tmpdir, NULL, NULL) : NULL;
        if (n)
            json_node_unref (n);
        if (p) {
            float at[3] = { 0, 0, 0 };
            static const float when[3] = { 0.5f, 5.0f, 9.5f };       /* seconds of a life of ten */
            static const float want_from[3] = { 0.5f, 1.0f, 0.5f };
            static const float want_span[3] = { 0.5f, 5.0f / 9.0f, 0.5f / 9.0f };
            float t = 0;
            for (int k = 0; k < 3; k++) {
                while (t < when[k] - 1e-4f) {
                    float dt = MIN (0.05f, when[k] - t);
                    walpe_weparticles_step (p, dt);
                    t += dt;
                }
                at[k] = walpe_weparticles_sys_alpha (p, 0, 0);
            }
            const float *want = span ? want_span : want_from;
            printf ("%s: alphafade 0.1 / 0.9 (%s): alpha %.3f, %.3f, %.3f at 5 %%, 50 %%, 95 %% of the life\n",
                    fabsf (at[0] - want[0]) < 0.03f && fabsf (at[1] - want[1]) < 0.03f &&
                    fabsf (at[2] - want[2]) < 0.03f ? "ok" : "..",
                    span ? "WALPE_PT_FADE=span" : "default", at[0], at[1], at[2]);
            for (int k = 0; k < 3; k++)
                CHECK (fabsf (at[k] - want[k]) < 0.03f,
                       "alphafade 0.1 / 0.9 at %.1f s of 10 (%s): alpha %.3f, want %.3f", when[k],
                       span ? "WALPE_PT_FADE=span" : "default", at[k], want[k]);
            walpe_weparticles_free (p);
        }
    }

    /* the temp dir: only the four files written above */
    static const char *files[] = { "particles/glow.json", "particles/trail.json", "particles/drops.json",
                                   "particles/wing.json" };
    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        char *f = g_build_filename (tmpdir, files[i], NULL);
        g_unlink (f);
        g_free (f);
    }
    char *d = g_build_filename (tmpdir, "particles", NULL);
    g_rmdir (d);
    g_free (d);
    g_rmdir (tmpdir);
    g_free (tmpdir);

    if (fails) {
        printf ("%d check(s) failed\n", fails);
        return 1;
    }
    printf ("all checks passed\n");
    return 0;
}
