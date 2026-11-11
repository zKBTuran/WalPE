/* particle-draw-test - how WE particle systems reach the GPU (GPU round).
 *
 * 1. Instanced quads draw the picture of the vertex-by-vertex submission
 *    they replace (sprites turned about three axes, trails, a system
 *    scaled and turned by its object) from 40 bytes a particle.
 * 2. Sprites outside the view are not sent; an object with nothing alive
 *    makes no draw call and uploads nothing.
 * 3. The reduced-resolution layer: large soft sprites drawn at 1/2 or 1/4
 *    and composited once give the picture of the direct draw to within a
 *    few steps of an 8-bit channel, for additive and normal blending and
 *    both mixed; small sprites never use it; off means off.
 * 4. The particle density setting thins a system out.
 * Every comparison draws ONE simulation state twice (the second render
 * advances it by 0 s). The systems' random numbers are a fixed sequence
 * all the same (g_rand_new below), so a run that passes passes every time.
 * Needs a GL 3.3 context without a window (EGL device or Mesa's
 * surfaceless platform); skipped where there is none. */
#include "engine/weparticles.h"
#include "engine/glutil.h"
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the engine's particle generators come from g_rand_new (): here a seeded
 * sequence instead of the clock's, so every run draws the same particles */
static guint32 seed_n;
GRand *g_rand_new (void)
{
    return g_rand_new_with_seed (0x9e3779b9u + 7919u * seed_n++);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf (__VA_ARGS__); printf ("\n"); } } while (0)

enum { W = 640, H = 360 };
static GLuint fbo, fbtex;
static const float white[3] = { 1, 1, 1 };
static char *tmpdir;   /* holds materials/add.json: an additive particle material */

static WalpeWeParticles *make (const char *json)
{
    JsonParser *jp = json_parser_new ();
    GError *err = NULL;
    if (!json_parser_load_from_data (jp, json, -1, &err)) {
        printf ("FAIL: test json: %s\n", err->message);
        fails++;
        g_object_unref (jp);
        return NULL;
    }
    WalpeWeParticles *p = walpe_weparticles_new (json_node_get_object (json_parser_get_root (jp)),
                                                 tmpdir, NULL, NULL);
    g_object_unref (jp);
    if (p)
        walpe_weparticles_gl_init (p);
    return p;
}

/* scene (W x H units, origin bottom-left, y up) -> clip, object at the
 * centre scaled sx, sy and turned by rot */
static void object_matrix (float sx, float sy, float rot, float *mvp, float *world, float *tail)
{
    WalpeMat3 t, o;
    walpe_mat3_identity (&t);
    walpe_mat3_scale (&t, 2.0f / W, 2.0f / H);
    walpe_mat3_translate (&t, -1.0f, -1.0f);
    walpe_mat3_identity (&o);
    walpe_mat3_scale (&o, sx, sy);
    walpe_mat3_rotate (&o, rot);
    walpe_mat3_translate (&o, W / 2.0f, H / 2.0f);
    WalpeMat3 m;
    walpe_mat3_mul (&m, &t, &o);
    memcpy (mvp, m.m, sizeof (m.m));
    memcpy (world, o.m, sizeof (o.m));
    memcpy (tail, t.m, sizeof (t.m));
}

/* one frame of the objects over a grey-blue ground with a hard edge down
 * the middle (what the particles overlap); dt seconds of simulation */
static void frame (WalpeWeParticles **ps, int n, double dt, float sx, float sy, float rot,
                   guint8 *px)
{
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glViewport (0, 0, W, H);
    glDisable (GL_SCISSOR_TEST);
    glClearColor (0.10f, 0.14f, 0.22f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glEnable (GL_SCISSOR_TEST);
    glScissor (W / 2, 0, W / 2, H);
    glClearColor (0.55f, 0.50f, 0.42f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glDisable (GL_SCISSOR_TEST);
    float mvp[9], world[9], tail[9];
    object_matrix (sx, sy, rot, mvp, world, tail);
    walpe_weparticles_begin_frame (fbo, W, H);
    for (int i = 0; i < n; i++) {
        walpe_weparticles_set_world (ps[i], world);
        walpe_weparticles_set_view (ps[i], tail, W / 2.0f, H / 2.0f, 0.0f, 1.0f, 400.0f);
        walpe_weparticles_render (ps[i], mvp, dt, 1.0f, white);
    }
    walpe_weparticles_end_frame ();
    if (px) {
        glBindFramebuffer (GL_FRAMEBUFFER, fbo);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
}

typedef struct { int max; long differing, over2; double psnr; } Diff;

static Diff compare (const guint8 *a, const guint8 *b)
{
    Diff d = { 0, 0, 0, 99.0 };
    double se = 0;
    for (long i = 0; i < (long) W * H; i++) {
        int m = 0;
        for (int c = 0; c < 3; c++) {
            int e = abs ((int) a[i * 4 + c] - (int) b[i * 4 + c]);
            m = MAX (m, e);
            se += (double) e * e;
        }
        d.max = MAX (d.max, m);
        d.differing += m > 0;
        d.over2 += m > 2;
    }
    if (se > 0)
        d.psnr = 10.0 * log10 (255.0 * 255.0 * 3.0 * W * H / se);
    return d;
}

static long lit (const guint8 *a, const guint8 *ground)
{
    long n = 0;
    for (long i = 0; i < (long) W * H; i++)
        n += memcmp (a + i * 4, ground + i * 4, 3) != 0;
    return n;
}

/* ---- systems (no material: the engine's soft dot) */

#define SPRITES(maxc, rate, life, smin, smax, box, extra, renderer, flags) \
    "{\"emitter\":[{\"name\":\"boxrandom\",\"distancemax\":\"" box "\",\"rate\":" #rate "}]," \
    "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":" #life ",\"max\":" #life "}," \
    "{\"name\":\"sizerandom\",\"min\":" #smin ",\"max\":" #smax "}," \
    "{\"name\":\"velocityrandom\",\"min\":\"-30 -60 0\",\"max\":\"30 -10 0\"}," \
    "{\"name\":\"colorrandom\",\"min\":\"120 160 255\",\"max\":\"255 255 255\"}" extra "]," \
    "\"operator\":[{\"name\":\"movement\",\"gravity\":\"0 -20 0\"}," \
    "{\"name\":\"alphafade\",\"fadeintime\":0.1,\"fadeouttime\":0.9}]," \
    "\"renderer\":[" renderer "],\"maxcount\":" #maxc ",\"starttime\":3,\"flags\":" #flags "}"

static const char *SYS_SPRITES = SPRITES (600, 200, 3, 10, 26, "300 170 0",
    ",{\"name\":\"alpharandom\",\"min\":0.4,\"max\":1},"
    "{\"name\":\"rotationrandom\",\"min\":\"0 0 0\",\"max\":\"1.2 0.9 6.28\"},"
    "{\"name\":\"angularvelocityrandom\",\"min\":-2,\"max\":2}",
    "{\"name\":\"sprite\"}", 0);
static const char *SYS_TRAILS = SPRITES (400, 200, 2, 6, 12, "300 170 0",
    ",{\"name\":\"alpharandom\",\"min\":0.5,\"max\":1}",
    "{\"name\":\"spritetrail\",\"length\":0.08,\"maxlength\":12}", 0);
static const char *SYS_WORLD = SPRITES (300, 100, 3, 14, 30, "200 100 0",
    ",{\"name\":\"alpharandom\",\"min\":0.5,\"max\":1},{\"name\":\"rotationrandom\"}",
    "{\"name\":\"sprite\"}", 1);
static const char *SYS_PERSP = SPRITES (300, 150, 2, 8, 14, "300 170 200",
    ",{\"name\":\"alpharandom\",\"min\":0.5,\"max\":1}",
    "{\"name\":\"spritetrail\",\"length\":0.05,\"maxlength\":10}", 4);
/* twice the view wide: about half of it is off screen */
static const char *SYS_WIDE = SPRITES (800, 250, 3, 8, 14, "640 170 0",
    ",{\"name\":\"alpharandom\",\"min\":0.5,\"max\":1}", "{\"name\":\"sprite\"}", 0);
/* fog: six dots, each larger than the view, faint */
static const char *SYS_FOG = SPRITES (6, 2, 3, 1400, 2200, "220 100 0",
    ",{\"name\":\"alpharandom\",\"min\":0.10,\"max\":0.18},{\"name\":\"rotationrandom\"}",
    "{\"name\":\"sprite\"}", 0);
static const char *SYS_BURST =
    "{\"emitter\":[{\"name\":\"boxrandom\",\"distancemax\":\"200 100 0\",\"rate\":0,"
    "\"instantaneous\":50,\"duration\":0.1}],"
    "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":0.5,\"max\":0.5},"
    "{\"name\":\"sizerandom\",\"min\":20,\"max\":20}],"
    "\"operator\":[{\"name\":\"movement\"}],\"renderer\":[{\"name\":\"sprite\"}],\"maxcount\":50}";

static void test_instanced (const char *name, const char *json, float sx, float sy, float rot)
{
    static guint8 a[W * H * 4], b[W * H * 4], ground[W * H * 4];
    WalpeWeParticles *p = make (json);
    if (!p)
        return;
    WalpePtStats st;
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_OFF);
    frame (NULL, 0, 0, 1, 1, 0, ground);
    for (int k = 0; k < 20; k++)
        frame (&p, 1, 1.0 / 30, sx, sy, rot, NULL);
    walpe_weparticles_stats (&st, TRUE);
    walpe_weparticles_set_instanced (TRUE);
    frame (&p, 1, 0.0, sx, sy, rot, a);
    walpe_weparticles_stats (&st, TRUE);
    guint drawn = st.drawn;
    guint64 bytes = st.bytes;
    walpe_weparticles_set_instanced (FALSE);
    frame (&p, 1, 0.0, sx, sy, rot, b);
    walpe_weparticles_stats (&st, TRUE);
    walpe_weparticles_set_instanced (TRUE);
    Diff d = compare (a, b);
    long n = lit (a, ground);
    printf ("%-12s %u sprites, %lu bytes instanced / %lu vertex by vertex; %ld pixels drawn, %ld "
            "differ (max %d)\n", name, drawn, (unsigned long) bytes, (unsigned long) st.bytes, n,
            d.differing, d.max);
    CHECK (drawn > 50 && n > 2000, "%s: %u sprites on %ld pixels: nothing to compare", name, drawn, n);
    CHECK (st.drawn == drawn, "%s: %u sprites instanced, %u vertex by vertex", name, drawn, st.drawn);
    CHECK (bytes == (guint64) drawn * 40, "%s: %lu bytes for %u sprites, want 40 each", name,
           (unsigned long) bytes, drawn);
    CHECK (st.bytes == (guint64) drawn * 360, "%s: reference: %lu bytes for %u sprites", name,
           (unsigned long) st.bytes, drawn);
    /* the same vertices; a software rasterizer may round a pixel of a
     * quad differently when it gets the quads one by one */
    CHECK (d.max <= 1 && d.differing * 2000 <= n, "%s: instanced picture differs: %ld pixels, max %d",
           name, d.differing, d.max);
    walpe_weparticles_free (p);
}

static void test_cull_idle (void)
{
    WalpeWeParticles *p = make (SYS_WIDE);
    WalpePtStats st;
    if (p) {
        for (int k = 0; k < 20; k++)
            frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        frame (&p, 1, 0.0, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        printf ("wide emitter: %u alive, %u sent, %u off screen\n", st.live, st.drawn, st.culled);
        CHECK (st.culled > st.live / 4, "only %u of %u sprites culled from an emitter twice the "
               "view's width", st.culled, st.live);
        CHECK (st.drawn + st.culled <= st.live && st.drawn + st.culled + st.live / 4 >= st.live,
               "%u sent + %u culled of %u alive", st.drawn, st.culled, st.live);
        walpe_weparticles_free (p);
    }
    p = make (SYS_BURST);
    if (p) {
        frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        CHECK (st.draws == 1 && st.drawn == 50, "burst: %u draws, %u sprites", st.draws, st.drawn);
        for (int k = 0; k < 30; k++)
            frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);   /* all dead, the emitter is over */
        walpe_weparticles_stats (&st, TRUE);
        frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        printf ("after the burst: %u alive, %u draws, %lu bytes\n", st.live, st.draws,
                (unsigned long) st.bytes);
        CHECK (st.live == 0 && st.draws == 0 && st.uploads == 0 && st.bytes == 0,
               "idle system: %u alive, %u draws, %u uploads", st.live, st.draws, st.uploads);
        walpe_weparticles_free (p);
    }
}

/* cos / sin of a sprite's angle: once in the life of a sprite that does
 * not turn (rotationrandom, no angular velocity), not once a frame; a
 * sprite that turns gets them every frame, and the picture of either is
 * the one the vertex-by-vertex reference draws. */
static const char *SYS_TURNED = SPRITES (500, 150, 3, 10, 26, "300 170 0",
    ",{\"name\":\"alpharandom\",\"min\":0.4,\"max\":1},"
    "{\"name\":\"rotationrandom\",\"min\":\"0 0 0.2\",\"max\":\"0 0 6.28\"}",
    "{\"name\":\"sprite\"}", 0);
static const char *SYS_TURNING = SPRITES (500, 150, 3, 10, 26, "300 170 0",
    ",{\"name\":\"alpharandom\",\"min\":0.4,\"max\":1},"
    "{\"name\":\"rotationrandom\",\"min\":\"0 0 0.2\",\"max\":\"0 0 6.28\"},"
    "{\"name\":\"angularvelocityrandom\",\"min\":0.5,\"max\":2}",
    "{\"name\":\"sprite\"}", 0);

static void test_rot_cache (void)
{
    WalpePtStats st;
    WalpeWeParticles *p = make (SYS_TURNED);
    if (p) {
        frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);   /* the 3 s of "starttime" */
        walpe_weparticles_stats (&st, TRUE);
        guint drawn = 0, trig = 0;
        for (int k = 0; k < 30; k++) {            /* one second: 150 are born */
            frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
            walpe_weparticles_stats (&st, TRUE);
            drawn += st.drawn;
            trig += st.trig;
        }
        printf ("turned sprites: %u drawn in 30 frames, cos / sin computed for %u\n", drawn, trig);
        CHECK (drawn > 30 * 300, "turned sprites: only %u drawn in 30 frames", drawn);
        CHECK (trig >= 100 && trig <= 200,
               "turned sprites: cos / sin computed %u times for about 150 new sprites in 30 "
               "frames (%u drawn)", trig, drawn);
        walpe_weparticles_free (p);
    }
    p = make (SYS_TURNING);
    if (p) {
        frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        CHECK (st.drawn > 300 && st.trig >= st.drawn,
               "turning sprites: cos / sin computed for %u of %u drawn", st.trig, st.drawn);
        walpe_weparticles_free (p);
    }
    test_instanced ("turned", SYS_TURNED, 1.3f, 0.8f, 0.2f);
    test_instanced ("turning", SYS_TURNING, 1.3f, 0.8f, 0.2f);
}

/* the same system with the additive material */
static WalpeWeParticles *make_additive (const char *json)
{
    char *j = g_strconcat ("{\"material\":\"materials/add.json\",", json + 1, NULL);
    WalpeWeParticles *p = make (j);
    g_free (j);
    return p;
}

/* fog, small sprites, fog again: drawn directly and through the layer */
static void layer_case (const char *name, WalpeWeParticles **ps, guint want_composites,
                        gboolean ps_additive)
{
    static guint8 a[W * H * 4], b[W * H * 4];
    WalpePtStats st;
    if (!ps[0] || !ps[1] || !ps[2])
        return;
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_OFF);
    for (int k = 0; k < 30; k++)
        frame (ps, 3, 1.0 / 30, 1, 1, 0, NULL);
    walpe_weparticles_stats (&st, TRUE);
    frame (ps, 3, 0.0, 1, 1, 0, a);
    walpe_weparticles_stats (&st, TRUE);
    double direct = st.shaded;
    CHECK (st.lowres_systems == 0 && st.composites == 0, "%s, off: %u systems in the layer", name,
           st.lowres_systems);
    CHECK (st.area > 6.0, "%s: fog covers %.2f screen areas: not a test of anything", name, st.area);
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
    frame (ps, 3, 0.0, 1, 1, 0, b);   /* the systems enter the layer */
    frame (ps, 3, 0.0, 1, 1, 0, b);
    walpe_weparticles_stats (&st, TRUE);
    frame (ps, 3, 0.0, 1, 1, 0, b);
    walpe_weparticles_stats (&st, TRUE);
    Diff d = compare (a, b);
    printf ("%s: %.2f screen areas blended directly, %.2f with the reduced layer (%u systems, "
            "%u composites); picture: max %d, %ld pixels over 2, PSNR %.1f dB\n", name, direct,
            st.shaded, st.lowres_systems, st.composites, d.max, d.over2, d.psnr);
    CHECK (st.lowres_systems == 2, "%s: %u systems in the layer, want the two fogs", name,
           st.lowres_systems);
    CHECK (st.composites == want_composites, "%s: %u composites, want %u", name, st.composites,
           want_composites);
    CHECK (st.shaded < direct * 0.5, "%s: the layer blends %.2f screen areas, directly %.2f", name,
           st.shaded, direct);
    /* Twelve faint screen-sized quads. Additive ones round in the layer
     * as on the frame: what differs is the layer's resolution. Normal
     * ones are twelve 8-bit blends in a row on the frame against two
     * shorter chains and a composite here, each rounding by up to half a
     * step: about one step RMS (48 dB) between ANY two orders of doing
     * them, whatever the resolution. Measured with Mesa llvmpipe: max 4
     * and 52 dB all additive, max 5 to 7 and 47 to 49 dB otherwise; the
     * limits leave room for another GL's rounding and are far from what
     * a wrong blend gives (max 52, 27 dB when a normal system was let
     * into a layer holding additive ones). */
    gboolean all_add = want_composites == 1 && st.composites == 1 && ps_additive;
    CHECK (d.max <= (all_add ? 8 : 12) && d.psnr >= (all_add ? 49.0 : 45.0),
           "%s: reduced layer: max difference %d, PSNR %.1f dB", name, d.max, d.psnr);
    CHECK (d.differing > 0, "%s: the layer changed no pixel at all: was it used?", name);
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_OFF);
    frame (ps, 3, 0.0, 1, 1, 0, b);
    walpe_weparticles_stats (&st, TRUE);
    d = compare (a, b);
    CHECK (d.max == 0 && st.lowres_systems == 0, "%s, off again: max difference %d, %u systems",
           name, d.max, st.lowres_systems);
    for (int i = 0; i < 3; i++)
        walpe_weparticles_free (ps[i]);
}

static void test_layer (void)
{
    WalpePtStats st;
    /* additive sprites and an all-additive layer commute: both fogs share
     * one layer and one composite */
    WalpeWeParticles *c0[3] = { make_additive (SYS_FOG), make_additive (SYS_SPRITES),
                                make_additive (SYS_FOG) };
    layer_case ("add fog, add sprites, add fog", c0, 1, TRUE);
    /* a normal fog after additive ones: a layer of its own (where the
     * additive fog clips the frame at white, dimming it afterwards is not
     * what one layer would compute) */
    WalpeWeParticles *c1[3] = { make_additive (SYS_FOG), make_additive (SYS_SPRITES), make (SYS_FOG) };
    layer_case ("add fog, add sprites, fog", c1, 2, FALSE);
    /* normal sprites need the layer on the frame first */
    WalpeWeParticles *c2[3] = { make_additive (SYS_FOG), make (SYS_SPRITES), make (SYS_FOG) };
    layer_case ("add fog, sprites, fog", c2, 2, FALSE);
    /* normal fog first: additive sprites after it must not be dimmed by it */
    WalpeWeParticles *c3[3] = { make (SYS_FOG), make_additive (SYS_SPRITES), make_additive (SYS_FOG) };
    layer_case ("fog, add sprites, add fog", c3, 2, FALSE);
    /* two normal fogs with nothing between: one layer */
    WalpeWeParticles *c4[3] = { make (SYS_FOG), make (SYS_FOG), make_additive (SYS_SPRITES) };
    layer_case ("fog, fog, add sprites", c4, 1, FALSE);

    /* small sprites alone never use the layer, whatever the mode */
    WalpeWeParticles *p = make (SYS_SPRITES);
    if (p) {
        walpe_weparticles_set_lowres (WALPE_PT_LOWRES_ALL);
        for (int k = 0; k < 10; k++)
            frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        frame (&p, 1, 0.0, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        CHECK (st.lowres_systems == 0 && st.drawn > 100, "small sprites: %u systems in the layer",
               st.lowres_systems);
        walpe_weparticles_free (p);
    }
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
}

static void test_density (void)
{
    WalpePtStats st;
    guint live[2] = { 0, 0 };
    for (int run = 0; run < 2; run++) {
        walpe_weparticles_set_density (run ? 50 : 100);
        WalpeWeParticles *p = make (SYS_SPRITES);
        if (!p)
            return;
        for (int k = 0; k < 150; k++)
            frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        frame (&p, 1, 1.0 / 30, 1, 1, 0, NULL);
        walpe_weparticles_stats (&st, TRUE);
        live[run] = st.live;
        walpe_weparticles_free (p);
    }
    walpe_weparticles_set_density (100);
    /* a new value of a setting is a change of the picture, the same value
     * is not (what a renderer that skips unchanged frames compares) */
    guint serial = walpe_weparticles_settings_serial ();
    walpe_weparticles_set_density (100);
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
    walpe_weparticles_set_instanced (TRUE);
    CHECK (walpe_weparticles_settings_serial () == serial, "settings serial moved without a change");
    walpe_weparticles_set_density (50);
    walpe_weparticles_set_instanced (FALSE);
    CHECK (walpe_weparticles_settings_serial () == serial + 2, "settings serial: %u, want %u",
           walpe_weparticles_settings_serial (), serial + 2);
    walpe_weparticles_set_density (100);
    walpe_weparticles_set_instanced (TRUE);
    printf ("density: %u alive at 100 %%, %u at 50 %%\n", live[0], live[1]);
    CHECK (live[0] >= 585 && live[0] <= 600, "100 %%: %u alive, want about 600", live[0]);
    CHECK (live[1] >= 285 && live[1] <= 300, "50 %%: %u alive, want about 300", live[1]);
}

/* ---- particle objects in a scene: cases found on real wallpapers ---- */

/* medium soft dots, at most a few of the view: sharp enough for the layer
 * at 1/2 only (a dot is 150 - 200 pixels of a 64-texel picture) */
static const char *SYS_MEDIUM = SPRITES (24, 12, 3, 300, 400, "300 170 0",
    ",{\"name\":\"alpharandom\",\"min\":0.5,\"max\":0.9}", "{\"name\":\"sprite\"}", 0);
/* the same with a fog as its (static) child system */
#define SYS_MEDIUM_FOG_CHILD "{\"children\":[{\"id\":9,\"name\":\"particles/b4fog.json\"}],"

/* "particle_instancing=off" is a second way to submit the same quads: the
 * reduced-resolution layer stays as the "particle_lowres" setting says
 * (the earlier build switched the layer off with it) */
static void test_instancing_keeps_layer (void)
{
    static guint8 a[W * H * 4], b[W * H * 4];
    WalpeWeParticles *ps[3] = { make_additive (SYS_FOG), make_additive (SYS_SPRITES),
                                make_additive (SYS_FOG) };
    WalpePtStats st;
    if (!ps[0] || !ps[1] || !ps[2])
        return;
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
    walpe_weparticles_set_instanced (TRUE);
    for (int k = 0; k < 30; k++)
        frame (ps, 3, 1.0 / 30, 1, 1, 0, NULL);
    walpe_weparticles_stats (&st, TRUE);
    frame (ps, 3, 0.0, 1, 1, 0, a);
    walpe_weparticles_stats (&st, TRUE);
    guint in_layer = st.lowres_systems, composites = st.composites;
    double shaded = st.shaded;
    walpe_weparticles_set_instanced (FALSE);
    frame (ps, 3, 0.0, 1, 1, 0, b);
    walpe_weparticles_stats (&st, TRUE);
    walpe_weparticles_set_instanced (TRUE);
    Diff d = compare (a, b);
    printf ("instancing off, layer auto: %u systems in the layer instanced, %u vertex by vertex "
            "(%u / %u composites, %.2f / %.2f screen areas blended); picture: %ld pixels differ, "
            "max %d\n", in_layer, st.lowres_systems, composites, st.composites, shaded, st.shaded,
            d.differing, d.max);
    CHECK (in_layer == 2, "instanced: %u systems in the layer, want the two fogs", in_layer);
    CHECK (st.lowres_systems == 2 && st.composites == composites,
           "instancing off: %u systems in the layer (%u composites), want 2 (%u): the two settings "
           "are independent", st.lowres_systems, st.composites, composites);
    CHECK (d.max <= 1 && d.differing * 2000 <= (long) W * H,
           "instancing off, layer auto: picture differs from the instanced one: %ld pixels, max %d",
           d.differing, d.max);
    /* and off stays off */
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_OFF);
    walpe_weparticles_set_instanced (FALSE);
    frame (ps, 3, 0.0, 1, 1, 0, b);
    walpe_weparticles_stats (&st, TRUE);
    walpe_weparticles_set_instanced (TRUE);
    CHECK (st.lowres_systems == 0 && st.composites == 0, "instancing off, layer off: %u systems in it",
           st.lowres_systems);
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
    for (int i = 0; i < 3; i++)
        walpe_weparticles_free (ps[i]);
}

/* One object whose child system (a fog: layer at 1/4) is drawn before its
 * own sharper dots (layer at 1/2): the layer changes its factor inside the
 * render call and the dots must be drawn with the new layer's matrix.
 * The reference is the same two systems as two objects (each render call
 * sends its matrices): the same particles, the same order, the same
 * layers. the earlier build kept the 1/4 matrix for the dots: 0.6 % nearer to the centre
 * of the view. */
static void test_layer_factor_change (void)
{
    static guint8 a[W * H * 4], b[W * H * 4];
    char *pd = g_build_filename (tmpdir, "particles", NULL);
    char *pf = g_build_filename (pd, "b4fog.json", NULL);
    char *fog = g_strconcat ("{\"material\":\"materials/add.json\",", SYS_FOG + 1, NULL);
    g_mkdir_with_parents (pd, 0700);
    g_file_set_contents (pf, fog, -1, NULL);
    char *both = g_strconcat (SYS_MEDIUM_FOG_CHILD "\"material\":\"materials/add.json\",",
                              SYS_MEDIUM + 1, NULL);
    WalpePtStats st;
    Diff d = { 0 };
    guint lay[2] = { 0, 0 }, comp[2] = { 0, 0 };
    for (int v = 0; v < 2; v++) {
        /* the same random sequences for both variants: the dots' system is
         * made first, the fog second */
        seed_n = 4242;
        WalpeWeParticles *ps[2] = { NULL, NULL };
        int n = 0;
        if (v == 0) {
            ps[n++] = make (both);
        } else {
            WalpeWeParticles *dots = make_additive (SYS_MEDIUM);
            ps[n++] = make_additive (SYS_FOG);   /* drawn first, as a child is */
            ps[n++] = dots;
        }
        if (!ps[0] || (v == 1 && !ps[1])) {
            fails++;
            printf ("FAIL: layer factor change: systems not made\n");
            break;
        }
        walpe_weparticles_set_lowres (WALPE_PT_LOWRES_OFF);
        for (int k = 0; k < 30; k++)
            frame (ps, n, 1.0 / 30, 1, 1, 0, NULL);
        frame (ps, n, 0.0, 1, 1, 0, NULL);
        frame (ps, n, 0.0, 1, 1, 0, NULL);   /* no layer in the last frame: no factor to keep */
        walpe_weparticles_stats (&st, TRUE);
        walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
        frame (ps, n, 0.0, 1, 1, 0, v ? b : a);   /* fog enters at 1/4, the dots at 1/2 */
        walpe_weparticles_stats (&st, TRUE);
        lay[v] = st.lowres_systems;
        comp[v] = st.composites;
        for (int i = 0; i < n; i++)
            walpe_weparticles_free (ps[i]);
    }
    d = compare (a, b);
    printf ("layer factor change: one object %u systems in the layer / %u composites, two objects "
            "%u / %u; pictures: %ld pixels differ, max %d\n", lay[0], comp[0], lay[1], comp[1],
            d.differing, d.max);
    CHECK (lay[0] == 2 && comp[0] == 2 && lay[1] == 2 && comp[1] == 2,
           "layer factor change: want the fog at 1/4 and the dots at 1/2 in both variants (two "
           "layers): %u systems / %u composites, %u / %u", lay[0], comp[0], lay[1], comp[1]);
    CHECK (d.max <= 1 && d.differing * 2000 <= (long) W * H,
           "layer factor change: the dots of a parent drawn after its fog child are misplaced: "
           "%ld pixels differ from the two-object picture, max %d", d.differing, d.max);
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
    g_remove (pf);
    g_rmdir (pd);
    g_free (both);
    g_free (fog);
    g_free (pf);
    g_free (pd);
}

/* width and height of the box around the pixels that differ from the ground */
static void lit_box (const guint8 *a, const guint8 *ground, int *bw, int *bh)
{
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (memcmp (a + ((long) y * W + x) * 4, ground + ((long) y * W + x) * 4, 3)) {
                x0 = MIN (x0, x);
                x1 = MAX (x1, x);
                y0 = MIN (y0, y);
                y1 = MAX (y1, y);
            }
    *bw = x1 >= x0 ? x1 - x0 + 1 : 0;
    *bh = y1 >= y0 ? y1 - y0 + 1 : 0;
}

/* One particle that stands at the object's origin with a glow that follows
 * it (WE's shooting star preset in small), the object scaled 4 x 4. The
 * glow's size is 80: a quad of 40 scene units. Under a "world space"
 * parent it is 40 pixels wide, like the parent's own sprites that the
 * object's scale does not reach; under a parent in the object's space it
 * is 160, as every sprite of that object. (the earlier build drew no such glow at all;
 * the first version of the fix drew it 160 wide in both cases.) */
static void test_world_children (void)
{
    static guint8 g[W * H * 4], a[W * H * 4];
    char *pd = g_build_filename (tmpdir, "particles", NULL);
    char *pf = g_build_filename (pd, "b4glow.json", NULL);
    g_mkdir_with_parents (pd, 0700);
    g_file_set_contents (pf,
        "{\"emitter\":[{\"name\":\"sphererandom\",\"distancemax\":0,\"instantaneous\":1,\"rate\":0}],"
        "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":1000,\"max\":1000},"
        "{\"name\":\"sizerandom\",\"min\":80,\"max\":80}],"
        "\"renderer\":[{\"name\":\"sprite\"}],\"maxcount\":1}", -1, NULL);
    int bw[2] = { 0, 0 }, bh[2] = { 0, 0 };
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_OFF);
    frame (NULL, 0, 0.0, 4, 4, 0, g);
    for (int v = 0; v < 2; v++) {
        char *json = g_strdup_printf (
            "{\"children\":[{\"id\":3,\"name\":\"particles/b4glow.json\",\"type\":\"eventfollow\","
            "\"maxcount\":1}],"
            "\"emitter\":[{\"name\":\"boxrandom\",\"distancemax\":\"0 0 0\",\"instantaneous\":1,"
            "\"rate\":0}],"
            "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":1000,\"max\":1000},"
            "{\"name\":\"sizerandom\",\"min\":2,\"max\":2}],"
            "\"renderer\":[{\"name\":\"sprite\"}],\"maxcount\":1,\"flags\":%d}", v ? 0 : 1);
        WalpeWeParticles *p = make (json);
        g_free (json);
        if (!p) {
            fails++;
            printf ("FAIL: world children: system not made\n");
            continue;
        }
        for (int k = 0; k < 4; k++)
            frame (&p, 1, 1.0 / 30, 4, 4, 0, NULL);
        frame (&p, 1, 0.0, 4, 4, 0, a);
        lit_box (a, g, &bw[v], &bh[v]);
        walpe_weparticles_free (p);
    }
    printf ("follow child of size 80 under an object scaled 4 x 4: %d x %d pixels with a world-space "
            "parent, %d x %d with a parent in the object's space\n", bw[0], bh[0], bw[1], bh[1]);
    CHECK (bw[0] >= 20 && bw[0] <= 42 && bh[0] >= 20 && bh[0] <= 42,
           "glow under a world-space parent: %d x %d pixels, want about 40 x 40 (scene units: the "
           "object's scale does not reach it)", bw[0], bh[0]);
    CHECK (bw[1] >= 80 && bw[1] <= 162 && bh[1] >= 80 && bh[1] <= 162,
           "glow under a parent in the object's space: %d x %d pixels, want about 160 x 160",
           bw[1], bh[1]);
    walpe_weparticles_set_lowres (WALPE_PT_LOWRES_AUTO);
    g_remove (pf);
    g_rmdir (pd);
    g_free (pf);
    g_free (pd);
}

static void gl_run (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip on one EGL display (no usable GL config)\n");
        return;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip on one EGL display (no surfaceless GL 3.3 context)\n");
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    walpe_glutil_init ();
    glGenTextures (1, &fbtex);
    glBindTexture (GL_TEXTURE_2D, fbtex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbtex, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf ("skip on one EGL display (no RGBA8 framebuffer)\n");
    } else {
        WalpePtStats st;
        walpe_weparticles_stats (&st, TRUE);   /* turns the counters on */
        test_instanced ("sprites", SYS_SPRITES, 1, 1, 0);
        test_instanced ("trails", SYS_TRAILS, 1, 1, 0);
        test_instanced ("object", SYS_SPRITES, -1.6f, 0.7f, 0.4f);
        test_instanced ("world space", SYS_WORLD, 1.8f, 0.6f, -0.3f);
        test_instanced ("perspective", SYS_PERSP, 1, 1, 0);
        test_cull_idle ();
        test_rot_cache ();
        test_layer ();
        test_density ();
        test_instancing_keeps_layer ();
        test_layer_factor_change ();
        test_world_children ();
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &fbtex);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
}

int main (void)
{
    /* the settings under test are the ones set here, not the caller's */
    g_unsetenv ("WALPE_PT_LOWRES");
    g_unsetenv ("WALPE_PT_LOWRES_TOL");
    g_unsetenv ("WALPE_PT_DENSITY");
    g_unsetenv ("WALPE_PT_INSTANCED");
    int ran = 0;
    GError *err = NULL;
    tmpdir = g_dir_make_tmp ("walpe-ptdraw-XXXXXX", &err);
    if (!tmpdir) {
        printf ("FAIL: temp dir: %s\n", err->message);
        return 1;
    }
    char *md = g_build_filename (tmpdir, "materials", NULL);
    char *mf = g_build_filename (md, "add.json", NULL);
    g_mkdir_with_parents (md, 0700);
    g_file_set_contents (mf, "{\"passes\":[{\"blending\":\"additive\"}]}", -1, NULL);
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && !ran; i++, ran++)
                gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
        ran++;
    }
    if (!ran)
        printf ("skip: no EGL device / surfaceless platform\n");
    g_remove (mf);
    g_rmdir (md);
    g_rmdir (tmpdir);
    g_free (mf);
    g_free (md);
    g_free (tmpdir);
    if (fails) {
        printf ("%d FAILED\n", fails);
        return 1;
    }
    printf ("particle-draw-test: OK\n");
    return 0;
}
