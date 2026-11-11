/* clip_test.c - "Skip hidden areas" in the renderer (src/engine/clip.c,
 * fx_clip.h, the hooks in glutil.c / scenenode.c / scenenode_effects.c).
 *
 * 1. Which texture lookups of an effect shader are made at the fragment's
 *    own coordinate (fx_clip_traits): shaders in WE's dialect, written
 *    here, that do and do not - offsets, a rewritten varying, a dead #if
 *    branch, a macro hiding the lookup, a sampler handed to a function, a
 *    local hiding the varying, blur taps from an array.
 * 2. What each pass of a chain has to fill (fx_chain_plan).
 * 3. With an EGL device or Mesa's surfaceless platform: the same scene is
 *    loaded twice and rendered frame by frame with the same virtual clock
 *    and the same random seeds - once in full, once clipped to the visible
 *    boxes of several window layouts (tiled, floating, corners, a sliver,
 *    random boxes that change every frame) over a frame buffer filled
 *    with rubbish. Inside the boxes every pixel must be the same, byte
 *    for byte. The fragments both renderings shade are counted by the GL
 *    (GL_SAMPLES_PASSED).
 *
 *    clip-test                     checks 1-3 on a scene made up here
 *                                  (solid layers, effects with shaders of
 *                                  its own, a compose layer)
 *    clip-test <wallpaper dir> [frames] [WxH]
 *                                  check 3 on a real scene, with numbers:
 *                                  on a machine with a GPU this is the
 *                                  check of the real driver
 *    clip-test --write-scene [compose]
 *                                  keeps the made-up scene (with the
 *                                  compose layer if asked) and prints
 *                                  its directory: a wallpaper to try
 *                                  the daemon with
 * Exit status: 0 = every compared pixel equal, 1 = not (or no verdict:
 * two copies of the scene differ even when both are drawn in full). */
#include "../src/engine/scenenode_effects.c"
#include <epoxy/egl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include "engine/content.h"
#include "common/config.h"

/* the same particles in both copies of a scene: the executable's
 * definition wins over libglib's for the statically linked engine */
static guint32 rand_seq;
GRand *g_rand_new (void)
{
    return g_rand_new_with_seed (20261110u + 7919u * rand_seq++);
}

/* the wall clock stands still: a clock or a date in a scene (scripts,
 * g_Daytime) reads the same in both copies however long a frame takes */
#define FROZEN_SEC 1762776000L
int gettimeofday (struct timeval *tv, void *tz)
{
    (void) tz;
    tv->tv_sec = FROZEN_SEC;
    tv->tv_usec = 0;
    return 0;
}

time_t time (time_t *t)
{
    if (t)
        *t = FROZEN_SEC;
    return FROZEN_SEC;
}

int clock_gettime (clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_REALTIME || id == CLOCK_REALTIME_COARSE) {
        ts->tv_sec = FROZEN_SEC;
        ts->tv_nsec = 0;
        return 0;
    }
    return (int) syscall (SYS_clock_gettime, id, ts);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf ("FAIL: " __VA_ARGS__); printf ("\n"); } } while (0)

/* ------------------------------------------------------------ 1. traits */

static const char *V_IDENT =
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
    "uniform vec4 g_Texture1Resolution;\n"
    "void main() {\n\tgl_Position = vec4(a_Position, 1.0);\n\tv_TexCoord = a_TexCoord.xyxy;\n"
    "#if MASK\n\tv_TexCoord.zw = v_TexCoord.xy * (g_Texture1Resolution.zw / g_Texture1Resolution.xy);\n"
    "#endif\n}\n";

static guint traits (const char *v, const char *f, int mask)
{
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert (combos, g_strdup ("MASK"), GINT_TO_POINTER (mask));
    char *vs = fx_build_source (v, combos, TRUE), *fs = fx_build_source (f, combos, FALSE);
    EffPass p;
    memset (&p, 0, sizeof (p));
    fx_vertex_traits (&p, vs);
    fx_clip_traits (&p, vs, fs);
    g_free (vs);
    g_free (fs);
    g_hash_table_unref (combos);
    return p.local_units;
}

static void test_traits (void)
{
    /* tint: the picture at its own coordinate, the mask at a scaled one */
    const char *f_tint =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
        "void main() {\n\tvec4 albedo = texSample2D(g_Texture0, v_TexCoord.xy);\n\tfloat mask = 1.0;\n"
        "#if MASK\n\tmask = texSample2D(g_Texture1, v_TexCoord.zw).r * mask;\n#endif\n"
        "\tgl_FragColor = albedo * mask;\n}\n";
    guint u = traits (V_IDENT, f_tint, 1);
    CHECK ((u & 1) && !(u & 2), "tint with mask: units 0x%x (want 0 local, 1 not)", u);
    u = traits (V_IDENT, f_tint, 0);
    CHECK ((u & 3) == 3, "tint without mask: units 0x%x (the mask lookup is not compiled)", u);

    /* displaced lookup (water, shake, scroll ...) */
    const char *f_shift =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
        "void main() {\n\tvec2 texCoord = v_TexCoord.xy;\n\ttexCoord += sin(g_Time) * 0.01;\n"
        "\tgl_FragColor = texSample2D(g_Texture0, texCoord);\n}\n";
    CHECK (!(traits (V_IDENT, f_shift, 0) & 1), "displaced lookup counted as local");
    const char *f_shift2 =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy + vec2(0.01, 0.0));\n}\n";
    CHECK (!(traits (V_IDENT, f_shift2, 0) & 1), "offset in the lookup counted as local");
    /* one local and one displaced lookup of the same texture */
    const char *f_both =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy) +\n"
        "\t\ttexSample2D(g_Texture0, v_TexCoord.xy * 0.5);\n}\n";
    CHECK (!(traits (V_IDENT, f_both, 0) & 1), "second, scaled lookup missed");

    const char *f_plain =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy);\n}\n";
    CHECK (traits (V_IDENT, f_plain, 0) & 1, "plain lookup not local");
    const char *f_lod =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = texSample2DLod(g_Texture0, v_TexCoord.xy, 0.0);\n}\n";
    CHECK (traits (V_IDENT, f_lod, 0) & 1, "plain lod lookup not local");

    /* the vertex shader moves the coordinate */
    const char *v_scroll =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "uniform float g_Time;\nvoid main() {\n\tgl_Position = vec4(a_Position, 1.0);\n"
        "\tv_TexCoord.xy = a_TexCoord;\n\tv_TexCoord.xy += g_Time * 0.1;\n\tv_TexCoord.zw = a_TexCoord;\n}\n";
    CHECK (!(traits (v_scroll, f_plain, 0) & 1), "varying moved by += counted as identity");
    const char *v_else =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "uniform float g_Flip;\nvoid main() {\n\tgl_Position = vec4(a_Position, 1.0);\n"
        "\tif (g_Flip > 0.5) v_TexCoord = a_TexCoord.xyxy;\n\telse v_TexCoord = a_TexCoord.xyxy * 2.0;\n}\n";
    CHECK (!(traits (v_else, f_plain, 0) & 1), "second write after `else` missed");
    const char *v_scale =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n\tgl_Position = vec4(a_Position, 1.0);\n\tv_TexCoord = a_TexCoord.xyxy * 0.5;\n}\n";
    CHECK (!(traits (v_scale, f_plain, 0) & 1), "scaled coordinate counted as identity");
    const char *v_x =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n\tgl_Position = vec4(a_Position, 1.0);\n\tv_TexCoord = a_TexCoord.xyxy;\n"
        "\tv_TexCoord.x = 1.0 - v_TexCoord.x;\n}\n";
    CHECK (!(traits (v_x, f_plain, 0) & 1), "mirrored x counted as identity");
    /* the same text in a branch that is not compiled changes nothing */
    const char *v_dead =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n\tgl_Position = vec4(a_Position, 1.0);\n\tv_TexCoord = a_TexCoord.xyxy;\n"
        "#if MASK\n\tv_TexCoord.x = 1.0 - v_TexCoord.x;\n#endif\n}\n";
    CHECK (traits (v_dead, f_plain, 0) & 1, "dead #if branch was read");
    CHECK (!(traits (v_dead, f_plain, 1) & 1), "live #if branch was not read");
    /* a vertex shader that moves the quad itself */
    const char *v_move =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "uniform float g_Time;\nvoid main() {\n\tvec3 p = a_Position;\n\tp.x += sin(g_Time);\n"
        "\tgl_Position = vec4(p, 1.0);\n\tv_TexCoord = a_TexCoord.xyxy;\n}\n";
    CHECK (traits (v_move, f_plain, 0) == 0, "a pass that moves its vertices has local lookups");

    /* blur: taps from an array of coordinates */
    const char *v_blur =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord[3];\n"
        "void main() {\n\tgl_Position = vec4(a_Position, 1.0);\n"
        "\tv_TexCoord[0] = a_TexCoord - vec2(0.01, 0.0);\n\tv_TexCoord[1] = a_TexCoord;\n"
        "\tv_TexCoord[2] = a_TexCoord + vec2(0.01, 0.0);\n}\n";
    const char *f_blur =
        "varying vec2 v_TexCoord[3];\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = 0.3 * texSample2D(g_Texture0, v_TexCoord[0]) +\n"
        "\t\t0.4 * texSample2D(g_Texture0, v_TexCoord[1]) + 0.3 * texSample2D(g_Texture0, v_TexCoord[2]);\n}\n";
    CHECK (!(traits (v_blur, f_blur, 0) & 1), "blur taps counted as local");

    /* what hides a lookup */
    const char *f_macro =
        "#define SAMPLE(uv) texSample2D(g_Texture0, uv * 0.5)\n"
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = SAMPLE(v_TexCoord.xy);\n}\n";
    CHECK (traits (V_IDENT, f_macro, 0) == 0, "lookup inside a macro: units 0x%x", traits (V_IDENT, f_macro, 0));
    const char *f_func =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "vec4 far(sampler2D t, vec2 uv) { return texSample2D(t, uv + 0.1); }\n"
        "void main() {\n\tgl_FragColor = far(g_Texture0, v_TexCoord.xy);\n}\n";
    CHECK (traits (V_IDENT, f_func, 0) == 0, "sampler handed to a function");
    const char *f_global =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "vec4 far(vec2 uv) { return texSample2D(g_Texture0, uv + 0.1); }\n"
        "void main() {\n\tgl_FragColor = far(v_TexCoord.xy);\n}\n";
    CHECK (!(traits (V_IDENT, f_global, 0) & 1), "lookup in a helper function");
    const char *f_shadow =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "vec4 pick(vec2 v_TexCoord) { return texSample2D(g_Texture0, v_TexCoord); }\n"
        "void main() {\n\tgl_FragColor = pick(vec2(0.5, 0.5));\n}\n";
    CHECK (!(traits (V_IDENT, f_shadow, 0) & 1), "parameter with the varying's name");
    const char *f_size =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
        "void main() {\n\tgl_FragColor = texelFetch(g_Texture0, ivec2(3, 4), 0);\n}\n";
    CHECK (!(traits (V_IDENT, f_size, 0) & 1), "texelFetch counted as local");
    const char *f_other =
        "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Noise;\n"
        "void main() {\n\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy) + texSample2D(g_Noise, v_TexCoord.xy);\n}\n";
    CHECK (traits (V_IDENT, f_other, 0) == 0, "sampler of another name");
    const char *v_tex =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "uniform sampler2D g_Texture0;\nvoid main() {\n\tgl_Position = vec4(a_Position, 1.0);\n"
        "\tv_TexCoord = a_TexCoord.xyxy;\n\tv_TexCoord.zw = texSample2DLod(g_Texture0, a_TexCoord, 0.0).xy;\n}\n";
    CHECK (traits (v_tex, f_plain, 0) == 0, "vertex shader reading a texture");
}

/* -------------------------------------------------------------- 2. plan */

static void pass_reads (EffPass *p, int unit)
{
    p->dyn = g_renew (FxUniform, p->dyn, p->n_dyn + 1);
    p->dyn[p->n_dyn++] = (FxUniform) { 0, FXU_TEXTURE, (guint8) unit };
}

/* the clip's serial: the same for the same boxes from frame to frame (set
 * before, off after), another one for other boxes, 0 without a clip */
static void test_serial (void)
{
    const WalpeBox a[2] = { { 0, 0, 100, 50 }, { 0, 50, 40, 90 } }, b[1] = { { 0, 0, 100, 50 } };
    walpe_clip_off ();
    CHECK (walpe_clip_serial () == 0, "no clip: serial 0");
    walpe_clip_set (a, 2, 200, 100);
    guint s1 = walpe_clip_serial ();
    CHECK (s1 != 0, "a clip has a serial");
    walpe_clip_off ();
    CHECK (walpe_clip_serial () == 0, "off: serial 0");
    walpe_clip_set (a, 2, 200, 100);
    CHECK (walpe_clip_serial () == s1, "the same boxes in the next frame: the same serial");
    walpe_clip_retarget (7, 100, 50);
    CHECK (walpe_clip_serial () == s1, "a retargeted frame: the same serial");
    walpe_clip_set (a, 2, 200, 100);
    CHECK (walpe_clip_serial () == s1, "... and after it");
    walpe_clip_suspend (TRUE);
    CHECK (walpe_clip_serial () == s1 && !walpe_clip_active (), "suspended: the same serial");
    walpe_clip_suspend (FALSE);
    walpe_clip_set (b, 1, 200, 100);
    guint s2 = walpe_clip_serial ();
    CHECK (s2 != 0 && s2 != s1, "other boxes: another serial");
    walpe_clip_set (a, 2, 200, 100);
    CHECK (walpe_clip_serial () != s1 && walpe_clip_serial () != s2, "back to the first boxes: a new serial");
    walpe_clip_set (a, 2, 400, 100);
    guint s3 = walpe_clip_serial ();
    walpe_clip_set (a, 2, 400, 100);
    CHECK (s3 != 0 && walpe_clip_serial () == s3, "the same boxes in another window size: its own serial");
    walpe_clip_set (NULL, 0, 200, 100);
    guint s4 = walpe_clip_serial ();
    walpe_clip_off ();
    walpe_clip_set (NULL, 0, 200, 100);
    CHECK (s4 != 0 && s4 != s3 && walpe_clip_serial () == s4, "nothing visible is a clip too");
    walpe_clip_off ();
}

static void test_plan (void)
{
    const FxClipNeed r = { FXN_RECT, 1, { { 0.25f, 0.25f, 0.5f, 0.5f } } };
    /* one pass with a displaced lookup of the input: the pass fills the
     * rectangle, the input is needed whole */
    EffectChain *ch = g_new0 (EffectChain, 1);
    ch->n_passes = 1;
    pass_reads (&ch->passes[0], 0);
    FxClipNeed in = fx_chain_plan (ch, r, 400, 200);
    CHECK (ch->passes[0].need.mode == FXN_RECT && in.mode == FXN_FULL, "one displaced pass");
    /* the same lookup local: the input is needed around the rectangle */
    ch->passes[0].local_units = 1;
    in = fx_chain_plan (ch, r, 400, 200);
    CHECK (in.mode == FXN_RECT && in.n == 1 && in.r[0][0] < 0.25f && in.r[0][0] > 0.24f &&
           in.r[0][2] > 0.5f && in.r[0][3] < 0.52f, "one local pass: %d %.4f..%.4f", in.mode,
           in.r[0][0], in.r[0][2]);
    int px[FXN_MAX][4];
    CHECK (fx_need_pixels (&ch->passes[0].need, 400, 200, px) == 1 && px[0][0] == 100 &&
           px[0][1] == 50 && px[0][2] == 100 && px[0][3] == 50, "pixels of the rectangle: %d %d %d %d",
           px[0][0], px[0][1], px[0][2], px[0][3]);
    /* two rectangles (the strips left and right of a window): each is
     * kept, the pass draws both and nothing between them */
    const FxClipNeed two = { FXN_RECT, 2, { { 0.0f, 0.0f, 0.1f, 1.0f }, { 0.9f, 0.0f, 1.0f, 1.0f } } };
    in = fx_chain_plan (ch, two, 400, 200);
    CHECK (in.mode == FXN_RECT && in.n == 2 && in.r[0][2] < 0.11f && in.r[1][0] > 0.89f,
           "two rectangles stay two: %d %d", in.mode, in.n);
    CHECK (fx_need_pixels (&ch->passes[0].need, 400, 200, px) == 2 && px[0][2] == 40 && px[1][0] == 360 &&
           px[1][2] == 40, "pixels of two rectangles");
    /* rectangles that overlap: the same pixels, none of them twice (a
     * blending pass must not see its own output) */
    const FxClipNeed lap = { FXN_RECT, 2, { { 0.0f, 0.0f, 0.5f, 0.5f }, { 0.4f, 0.4f, 1.0f, 1.0f } } };
    {
        int nl = fx_need_pixels (&lap, 400, 200, px), area = 0;
        gboolean apart = TRUE;
        for (int i = 0; i < nl; i++) {
            area += px[i][2] * px[i][3];
            for (int j = i + 1; j < nl; j++)
                apart = apart && (px[i][0] + px[i][2] <= px[j][0] || px[j][0] + px[j][2] <= px[i][0] ||
                                  px[i][1] + px[i][3] <= px[j][1] || px[j][1] + px[j][3] <= px[i][1]);
        }
        CHECK (nl >= 1 && nl <= FXN_MAX && apart && area == 200 * 100 + 240 * 120 - 40 * 20,
               "overlapping rectangles: %d boxes, apart %d, %d pixels", nl, apart, area);
    }
    /* more rectangles than there is room for: one around all of them */
    FxClipNeed many = { FXN_NONE, 0, { { 0 } } };
    for (int i = 0; i < FXN_MAX + 2; i++) {
        const FxClipNeed one = { FXN_RECT, 1, { { 0.1f * (float) i, 0.0f, 0.1f * (float) i + 0.05f, 0.5f } } };
        fx_need_add (&many, &one);
    }
    CHECK (many.mode == FXN_RECT && many.n >= 1 && many.n <= FXN_MAX, "many rectangles: %d", many.n);
    {
        float lo = 1, hi = 0;
        for (int i = 0; i < many.n; i++) {
            lo = fminf (lo, many.r[i][0]);
            hi = fmaxf (hi, many.r[i][2]);
        }
        CHECK (lo <= 0.0f && hi >= 0.1f * (float) (FXN_MAX + 1) + 0.05f - 1e-6f,
               "many rectangles cover all of them: %.3f..%.3f", lo, hi);
    }
    /* nothing seen: nothing drawn, nothing needed */
    const FxClipNeed none = { FXN_NONE, 0, { { 0 } } };
    in = fx_chain_plan (ch, none, 400, 200);
    CHECK (ch->passes[0].need.mode == FXN_NONE && in.mode == FXN_NONE, "nothing seen");
    g_free (ch->passes[0].dyn);
    g_free (ch);

    /* blur as WE builds it: downsample -> A, blur x A -> B, blur y B -> A
     * (taps: displaced), combine A + the effect input (both local).
     * Only the combine pass can be cut; the blurred target it reads is
     * needed around the rectangle, and blur y needs all of B. */
    ch = g_new0 (EffectChain, 1);
    ch->n_passes = 4;
    ch->n_targets = 2;
    strcpy (ch->targets[0].name, "A");
    strcpy (ch->targets[1].name, "B");
    ch->targets[0].scale = ch->targets[1].scale = 4;
    strcpy (ch->passes[0].target, "A");
    pass_reads (&ch->passes[0], 0);                 /* previous = input, displaced */
    strcpy (ch->passes[1].target, "B");
    pass_reads (&ch->passes[1], 0);
    ch->passes[1].n_binds = 1;
    strcpy (ch->passes[1].binds[0].name, "A");
    strcpy (ch->passes[2].target, "A");
    pass_reads (&ch->passes[2], 0);
    ch->passes[2].n_binds = 1;
    strcpy (ch->passes[2].binds[0].name, "B");
    pass_reads (&ch->passes[3], 0);
    pass_reads (&ch->passes[3], 2);
    ch->passes[3].local_units = 0xff;
    ch->passes[3].n_binds = 2;
    strcpy (ch->passes[3].binds[0].name, "A");
    strcpy (ch->passes[3].binds[1].name, "previous");
    ch->passes[3].binds[1].index = 2;
    in = fx_chain_plan (ch, r, 400, 200);
    CHECK (ch->passes[3].need.mode == FXN_RECT, "blur: combine is cut");
    CHECK (ch->passes[2].need.mode == FXN_RECT && ch->passes[2].need.r[0][0] < 0.25f - 1.0f / 100.0f,
           "blur: blur y fills the rectangle and two quarter-size texels around it (%d %.4f)",
           ch->passes[2].need.mode, ch->passes[2].need.r[0][0]);
    CHECK (ch->passes[1].need.mode == FXN_FULL && ch->passes[0].need.mode == FXN_FULL,
           "blur: the passes before a displaced lookup are whole");
    CHECK (in.mode == FXN_FULL, "blur: input");
    for (int i = 0; i < 4; i++)
        g_free (ch->passes[i].dyn);
    g_free (ch);

    /* a target nothing reads afterwards is not drawn */
    ch = g_new0 (EffectChain, 1);
    ch->n_passes = 2;
    ch->n_targets = 1;
    strcpy (ch->targets[0].name, "T");
    ch->targets[0].scale = 1;
    strcpy (ch->passes[0].target, "T");
    pass_reads (&ch->passes[0], 0);
    pass_reads (&ch->passes[1], 0);
    ch->passes[1].local_units = 1;
    ch->passes[1].n_binds = 1;
    strcpy (ch->passes[1].binds[0].name, "previous");   /* reads the input, not T */
    in = fx_chain_plan (ch, r, 400, 200);
    CHECK (ch->passes[0].need.mode == FXN_NONE && ch->passes[1].need.mode == FXN_RECT &&
           in.mode == FXN_RECT, "unread target");
    g_free (ch->passes[0].dyn);
    g_free (ch->passes[1].dyn);
    g_free (ch);
}

/* --------------------------------------------------- 3. the made-up scene */

static void put (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

static const char *SYN_VERT =
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
    "void main() {\n\tgl_Position = vec4(a_Position, 1.0);\n\tv_TexCoord = a_TexCoord.xyxy;\n}\n";

static void effect (const char *dir, const char *name, const char *frag)
{
    char *rel = g_strdup_printf ("shaders/effects/%s.vert", name);
    put (dir, rel, SYN_VERT);
    g_free (rel);
    rel = g_strdup_printf ("shaders/effects/%s.frag", name);
    put (dir, rel, frag);
    g_free (rel);
    rel = g_strdup_printf ("materials/effects/%s.json", name);
    char *t = g_strdup_printf ("{\"passes\":[{\"shader\":\"effects/%s\",\"blending\":\"normal\","
                               "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\","
                               "\"cullmode\":\"nocull\"}]}", name);
    put (dir, rel, t);
    g_free (t);
    g_free (rel);
    rel = g_strdup_printf ("effects/%s/effect.json", name);
    t = g_strdup_printf ("{\"name\":\"%s\",\"passes\":[{\"material\":\"materials/effects/%s.json\"}]}",
                         name, name);
    put (dir, rel, t);
    g_free (t);
    g_free (rel);
}

/* compose = with a compose layer in the middle (everything below it is
 * then drawn in full) */
static char *make_scene (gboolean compose)
{
    char *dir = g_dir_make_tmp ("walpe-clip-XXXXXX", NULL);
    put (dir, "project.json", "{\"file\":\"scene.json\",\"type\":\"scene\",\"title\":\"clip test\"}");
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "models/compose.json", "{\"passthrough\":true}");
    /* a picture from the coordinate and the time: local */
    effect (dir, "pattern",
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
            "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord.xy);\n"
            "\tvec2 p = v_TexCoord.xy * 23.0;\n"
            "\tfloat s = sin(p.x * 3.1 + g_Time * 2.0) * cos(p.y * 2.3 - g_Time);\n"
            "\tgl_FragColor = vec4(c.rgb * (0.5 + 0.5 * vec3(s, frac(p.x), frac(p.y))), c.a);\n}\n");
    /* a colour change: local */
    effect (dir, "recolor",
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord.xy);\n"
            "\tgl_FragColor = vec4(c.bgr * vec3(0.9, 1.0, 0.8) + vec3(0.05, 0.0, 0.1), c.a);\n}\n");
    /* a moving displacement: not local */
    effect (dir, "wobble",
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
            "void main() {\n\tvec2 uv = v_TexCoord.xy;\n"
            "\tuv += vec2(sin(g_Time * 3.0 + uv.y * 31.0), cos(g_Time * 2.0 + uv.x * 17.0)) * 0.03;\n"
            "\tgl_FragColor = texSample2D(g_Texture0, uv);\n}\n");
    /* two passes through a half-size target, both lookups local */
    put (dir, "shaders/effects/half.vert", SYN_VERT);
    put (dir, "shaders/effects/half.frag",
         "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
         "void main() {\n\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy) * vec4(1.0, 0.7, 0.6, 1.0);\n}\n");
    put (dir, "shaders/effects/join.vert", SYN_VERT);
    put (dir, "shaders/effects/join.frag",
         "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
         "void main() {\n\tvec4 a = texSample2D(g_Texture0, v_TexCoord.xy);\n"
         "\tvec4 b = texSample2D(g_Texture1, v_TexCoord.xy);\n\tgl_FragColor = mix(a, b, 0.5);\n}\n");
    put (dir, "materials/effects/half.json", "{\"passes\":[{\"shader\":\"effects/half\"}]}");
    put (dir, "materials/effects/join.json", "{\"passes\":[{\"shader\":\"effects/join\"}]}");
    put (dir, "effects/twopass/effect.json",
         "{\"name\":\"twopass\",\"passes\":["
         "{\"material\":\"materials/effects/half.json\",\"target\":\"_rt_half\","
         "\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
         "{\"material\":\"materials/effects/join.json\","
         "\"bind\":[{\"name\":\"_rt_half\",\"index\":0},{\"name\":\"previous\",\"index\":1}]}],"
         "\"fbos\":[{\"name\":\"_rt_half\",\"scale\":2}]}");

    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"clearcolor\":\"0.10 0.20 0.30\",\"cameraparallax\":false,"
        "\"orthogonalprojection\":{\"width\":640,\"height\":360}},\"objects\":[");
#define FX(name) "{\"file\":\"effects/" name "/effect.json\",\"id\":1,\"name\":\"\",\"passes\":[{\"id\":2}],\"visible\":true}"
#define FX2(name) "{\"file\":\"effects/" name "/effect.json\",\"id\":1,\"name\":\"\",\"passes\":[{\"id\":2},{\"id\":3}],\"visible\":true}"
#define SOLID(id, x, y, w, h, ang, alpha, col, fx) \
    "{\"id\":" #id ",\"name\":\"l" #id "\",\"image\":\"models/solid.json\",\"origin\":\"" #x " " #y " 0\"," \
    "\"scale\":\"1 1 1\",\"angles\":\"0 0 " #ang "\",\"size\":\"" #w " " #h "\",\"alpha\":" #alpha "," \
    "\"color\":\"" col "\",\"visible\":true,\"effects\":[" fx "]},"
    g_string_append (s, SOLID (1, 320, 180, 640, 360, 0, 1.0, "0.9 0.8 0.7", FX ("pattern")));
    g_string_append (s, SOLID (2, 200, 200, 300, 200, 0.35, 1.0, "0.4 0.9 0.5", FX ("pattern") "," FX ("wobble")));
    g_string_append (s, SOLID (3, 480, 120, 256, 256, 0, 0.8, "0.9 0.3 0.4", FX ("wobble") "," FX ("recolor")));
    g_string_append (s, SOLID (4, 320, 90, 320, 180, 0, 0.7, "0.5 0.6 1.0", FX ("pattern") "," FX2 ("twopass")));
    if (compose)
        g_string_append (s,
            "{\"id\":5,\"name\":\"compose\",\"image\":\"models/compose.json\",\"origin\":\"260 170 0\","
            "\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"size\":\"300 200\",\"alpha\":1.0,\"visible\":true,"
            "\"effects\":[" FX ("recolor") "," FX ("wobble") "]},");
    g_string_append (s, SOLID (6, 560, 300, 140, 100, -0.2, 1.0, "1.0 0.9 0.2", FX ("pattern") "," FX ("recolor")));
    g_string_append (s, SOLID (7, 60, 40, 100, 60, 0, 0.9, "0.2 0.9 0.9", FX ("recolor")));
    /* no effects at all, mirrored, and one with a WE blend mode drawn by
     * copying what is under it */
    g_string_append (s,
        "{\"id\":8,\"name\":\"l8\",\"image\":\"models/solid.json\",\"origin\":\"100 300 0\",\"scale\":\"-1 1 1\","
        "\"angles\":\"0 0 0.5\",\"size\":\"120 80\",\"alpha\":0.6,\"color\":\"1 0.2 0.9\",\"visible\":true},");
    g_string_append (s,
        "{\"id\":9,\"name\":\"l9\",\"image\":\"models/solid.json\",\"origin\":\"400 250 0\",\"scale\":\"1 1 1\","
        "\"angles\":\"0 0 0\",\"size\":\"200 120\",\"alpha\":0.9,\"color\":\"0.8 0.5 0.3\",\"colorBlendMode\":11,"
        "\"visible\":true,\"effects\":[" FX ("pattern") "]}");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    return dir;
}

static void rm_rf (const char *path)
{
    GDir *d = g_dir_open (path, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (path, n, NULL);
        if (g_file_test (p, G_FILE_TEST_IS_DIR) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK))
            rm_rf (p);
        else
            g_unlink (p);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (path);
}

/* ------------------------------------------------- 3. clipped == unclipped */

static void redraw_cb (gpointer u) { (void) u; }

/* seq: the copy's own place in the sequence of random seeds (a generator
 * a scene makes at its first frame gets the same seed in both copies) */
static WalpeContent *load_scene (const char *path, guint32 *seq)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = path };
    rand_seq = 0;
    WalpeContent *c = walpe_content_new (path, &opts, &err);
    if (!c) {
        printf ("load failed: %s\n", err ? err->message : "?");
        return NULL;
    }
    c->request_redraw = redraw_cb;
    if (!c->gl_init (c, &err)) {
        printf ("gl_init failed: %s\n", err ? err->message : "?");
        return NULL;
    }
    *seq = rand_seq;
    return c;
}

typedef struct { const char *name; int n; WalpeBox b[WALPE_CLIP_MAX]; } Layout;

/* window layouts of a w x h monitor: what stays visible (y down) */
static int layouts (int w, int h, Layout *L)
{
    int bar = MAX (2, h * 22 / 1080), n = 0;
    L[n++] = (Layout) { "tiled: right 45 % visible", 1, { { w * 55 / 100, bar, w, h } } };
    L[n++] = (Layout) { "tiled: left half visible", 1, { { 0, bar, w / 2, h } } };
    L[n++] = (Layout) { "tiled: lower right quarter", 1, { { w / 2, (h + bar) / 2, w, h } } };
    /* a floating window in the middle: the frame around it */
    int fx0 = w * 5 / 16, fx1 = w * 12 / 16, fy0 = h * 2 / 8, fy1 = h * 6 / 8;
    L[n++] = (Layout) { "floating: frame around a window", 4,
                        { { 0, bar, w, fy0 }, { 0, fy0, fx0, fy1 }, { fx1, fy0, w, fy1 },
                          { 0, fy1, w, h } } };
    L[n++] = (Layout) { "two corners", 2, { { 0, 0, w * 3 / 10, h * 4 / 10 },
                                            { w * 6 / 10, h * 5 / 10, w, h } } };
    L[n++] = (Layout) { "sliver 24 px", 1, { { w * 4 / 10, 0, w * 4 / 10 + 24, h } } };
    L[n++] = (Layout) { "monocle: nothing visible", 0, { { 0, 0, 0, 0 } } };
    L[n++] = (Layout) { "random boxes, new every frame", -1, { { 0, 0, 0, 0 } } };
    L[n++] = (Layout) { "all visible (one box = the window)", 1, { { 0, 0, w, h } } };
    /* CLIP_TEST_WINDOW="x,y,w,h": one more, the frame around that window
     * (a case seen on the desktop, to look at here) */
    int wx, wy, ww, wh;
    const char *win = g_getenv ("CLIP_TEST_WINDOW");
    if (win && sscanf (win, "%d,%d,%d,%d", &wx, &wy, &ww, &wh) == 4 && wx >= 0 && wy >= bar && ww > 0 &&
        wh > 0 && wx + ww <= w && wy + wh <= h)
        L[n++] = (Layout) { "window (CLIP_TEST_WINDOW)", 4,
                            { { 0, bar, w, wy }, { 0, wy, wx, wy + wh }, { wx + ww, wy, w, wy + wh },
                              { 0, wy + wh, w, h } } };
    return n;
}

static guint frame_fragments (GLuint q, WalpeContent *c, guint32 *seq, int w, int h)
{
    rand_seq = *seq;
    /* the back buffer of a window is undefined after a swap */
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor (1.0f, 0.0f, 1.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glViewport (0, 0, w, h);
    glBeginQuery (GL_SAMPLES_PASSED, q);
    c->gl_render (c, w, h, 0.0f, 0.0f);
    glEndQuery (GL_SAMPLES_PASSED);
    GLuint v = 0;
    glGetQueryObjectuiv (q, GL_QUERY_RESULT, &v);
    *seq = rand_seq;
    return v;
}

/* 0 = equal inside the boxes; > 0 = the number of pixels that differ;
 * -1 = not loaded; -2 = the two copies differ without any clip (a scene
 * that is not reproducible - a video texture decoded on its own thread,
 * say - cannot be checked this way) */
static int run_scene (const char *path, int w, int h, int frames, gboolean verbose)
{
    guint32 sa = 0, sb = 0;
    WalpeContent *A = load_scene (path, &sa), *B = A ? load_scene (path, &sb) : NULL;
    if (!A || !B) {
        printf ("%s: cannot be loaded twice\n", path);
        return -1;
    }
    GLuint q = 0;
    glGenQueries (1, &q);
    guint8 *pa = g_malloc ((gsize) w * h * 4), *pb = g_malloc ((gsize) w * h * 4);
    Layout L[12];
    int nl = layouts (w, h, L), bad_total = 0;
    /* CLIP_TEST_LAYOUTS="0,3": only these (numbers in the order printed) */
    const char *pick = g_getenv ("CLIP_TEST_LAYOUTS");
    if (pick && *pick) {
        char **ids = g_strsplit (pick, ",", -1);
        Layout P[12];
        int np = 0;
        for (char **i = ids; *i && np < 12; i++)
            if (atoi (*i) >= 0 && atoi (*i) < nl)
                P[np++] = L[atoi (*i)];
        g_strfreev (ids);
        memcpy (L, P, sizeof (Layout) * (gsize) np);
        nl = np;
    }
    GRand *rr = g_rand_new_with_seed (5);
    /* warm-up: loads, first-use targets, static caches settle */
    for (int f = 0; f < 3; f++) {
        walpe_clip_off ();
        frame_fragments (q, A, &sa, w, h);
        frame_fragments (q, B, &sb, w, h);
    }
    /* control: both copies in full. They must agree before a clipped
     * frame can be compared with a full one. */
    int control = 0;
    for (int f = 0; f < 2; f++) {
        frame_fragments (q, A, &sa, w, h);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pa);
        frame_fragments (q, B, &sb, w, h);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pb);
        for (gsize i = 0; i < (gsize) w * h; i++)
            control += pa[i * 4] != pb[i * 4] || pa[i * 4 + 1] != pb[i * 4 + 1] ||
                       pa[i * 4 + 2] != pb[i * 4 + 2];
    }
    if (control && verbose)
        printf ("control: two copies of the scene, both drawn in full, differ in %d pixels: the "
                "scene is not reproducible, the numbers below say nothing about the clip\n", control);
    if (verbose)
        printf ("%-36s %8s %14s %14s %7s %s\n", "layout", "visible", "frags full", "frags clipped",
                "ratio", "differing pixels");
    for (int l = 0; l < nl; l++) {
        guint64 fa = 0, fb = 0;
        gint64 vis = 0;
        int bad = 0, maxd = 0, compared = 0;
        for (int f = 0; f < frames; f++) {
            WalpeBox bx[WALPE_CLIP_MAX];
            int nb = L[l].n;
            if (nb < 0) {
                /* 1-3 boxes that do not overlap: columns of the window */
                nb = g_rand_int_range (rr, 1, 4);
                for (int i = 0; i < nb; i++) {
                    int c0 = w * i / nb, c1 = w * (i + 1) / nb;
                    int x0 = g_rand_int_range (rr, c0, c1 - 4), x1 = g_rand_int_range (rr, x0 + 2, c1);
                    int y0 = g_rand_int_range (rr, 0, h - 4), y1 = g_rand_int_range (rr, y0 + 2, h);
                    bx[i] = (WalpeBox) { x0, y0, x1, y1 };
                }
            } else {
                memcpy (bx, L[l].b, sizeof (WalpeBox) * (gsize) nb);
            }
            walpe_clip_off ();
            fa += frame_fragments (q, A, &sa, w, h);
            glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pa);
            walpe_clip_set (bx, nb, w, h);
            fx_clip_test_poison = TRUE;     /* what a pass leaves out must not be read */
            fb += frame_fragments (q, B, &sb, w, h);
            fx_clip_test_poison = FALSE;
            walpe_clip_off ();
            glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pb);
            compared++;
            for (int i = 0; i < nb; i++) {
                vis += walpe_box_area (bx[i]);
                for (int y = bx[i].y0; y < bx[i].y1; y++) {
                    const guint8 *ra = pa + (gsize) (h - 1 - y) * w * 4;
                    const guint8 *rb = pb + (gsize) (h - 1 - y) * w * 4;
                    for (int x = bx[i].x0; x < bx[i].x1; x++) {
                        int d = 0;
                        for (int k = 0; k < 3; k++)
                            d = MAX (d, abs ((int) ra[x * 4 + k] - (int) rb[x * 4 + k]));
                        if (d) {
                            if (!bad && verbose)
                                printf ("  first difference: frame %d box %d pixel %d,%d: "
                                        "%d,%d,%d vs %d,%d,%d\n", f, i, x, y, ra[x * 4],
                                        ra[x * 4 + 1], ra[x * 4 + 2], rb[x * 4], rb[x * 4 + 1],
                                        rb[x * 4 + 2]);
                            bad++;
                            maxd = MAX (maxd, d);
                        }
                    }
                }
            }
        }
        if (verbose)
            printf ("%-36s %7.1f%% %14.0f %14.0f %6.1f%% %d%s (max %d, %d frames compared)\n",
                    L[l].name, compared ? 100.0 * (double) vis / compared / ((double) w * h) : 0.0,
                    (double) fa / frames, (double) fb / frames, fa ? 100.0 * (double) fb / (double) fa : 0.0,
                    bad, bad ? " DIFFERENT" : "", maxd, compared);
        bad_total += bad;
    }
    g_rand_free (rr);
    g_free (pa);
    g_free (pb);
    glDeleteQueries (1, &q);
    for (int i = 0; i < 2; i++) {
        WalpeContent *c = i ? B : A;
        if (c->gl_free)
            c->gl_free (c);
        c->free (c);
    }
    return control ? -2 : bad_total;
}

static gboolean gl_open (int w, int h, EGLDisplay *dpy_out)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless") &&
        !g_getenv ("CLIP_TEST_DEVICE"))
        dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        /* the first device (the real GPU where there is one);
         * CLIP_TEST_DEVICE=n picks another */
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        const char *pick = g_getenv ("CLIP_TEST_DEVICE");
        int want = pick ? atoi (pick) : 0;
        if (eglQueryDevicesEXT (8, dev, &nd) && nd > 0)
            dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[CLAMP (want, 0, nd - 1)], NULL);
    }
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[128];
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, cfgs, 128, &nc) || nc < 1)
        return FALSE;
    EGLConfig cfg = cfgs[0];
    for (int i = 0; i < nc; i++) {
        EGLint r = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &r);
        if (r == 8) {       /* as a desktop window: 8 bits per channel */
            cfg = cfgs[i];
            break;
        }
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx))
        return FALSE;
    *dpy_out = dpy;
    return TRUE;
}

/* a second, larger pbuffer on the context of gl_open (and back) */
static EGLSurface surf_small, surf_big;
static EGLSurface big_surface (EGLDisplay dpy, int w, int h)
{
    EGLConfig cfg;
    EGLint id = 0, n = 0;
    EGLContext ctx = eglGetCurrentContext ();
    surf_small = eglGetCurrentSurface (EGL_DRAW);
    eglQueryContext (dpy, ctx, EGL_CONFIG_ID, &id);
    const EGLint ca[] = { EGL_CONFIG_ID, id, EGL_NONE }, pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    if (!eglChooseConfig (dpy, ca, &cfg, 1, &n) || n < 1)
        return EGL_NO_SURFACE;
    surf_big = eglCreatePbufferSurface (dpy, cfg, pa);
    if (surf_big == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf_big, surf_big, ctx)) {
        eglMakeCurrent (dpy, surf_small, surf_small, ctx);
        return EGL_NO_SURFACE;
    }
    return surf_big;
}

static void small_surface (EGLDisplay dpy)
{
    eglMakeCurrent (dpy, surf_small, surf_small, eglGetCurrentContext ());
    eglDestroySurface (dpy, surf_big);
}

int main (int argc, char **argv)
{
    /* nothing of the user's: config and probe output in a temp dir; the
     * probe gives the scene a virtual 30 fps clock */
    char *home = g_dir_make_tmp ("walpe-clip-home-XXXXXX", NULL);
    g_setenv ("HOME", home, TRUE);
    g_setenv ("XDG_CONFIG_HOME", home, TRUE);
    g_setenv ("XDG_CACHE_HOME", home, TRUE);
    g_setenv ("XDG_DATA_HOME", home, TRUE);
    g_setenv ("WALPE_PROP_PROBE", home, TRUE);
    g_setenv ("WALPE_PROP_PROBE_FRAME", "1000000", TRUE);
    g_setenv ("WALPE_NO_MEDIA", "1", TRUE);
    if (argc > 1 && !strcmp (argv[1], "--write-scene")) {
        char *dir = make_scene (argc > 2 && !strcmp (argv[2], "compose"));
        printf ("%s\n", dir);
        rm_rf (home);
        return 0;
    }
    const char *scene = argc > 1 ? argv[1] : NULL;
    int frames = argc > 2 ? atoi (argv[2]) : 6, w = 640, h = 360, rc = 0;
    if (argc > 3)
        sscanf (argv[3], "%dx%d", &w, &h);
    if (!scene) {
        test_traits ();
        test_plan ();
        test_serial ();
        printf ("%s shader lookups + pass plans + clip serial\n", failures ? "FAILED:" : "ok:");
    }
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (!gl_open (w, h, &dpy)) {
        printf ("skip the render check (no EGL device / surfaceless platform with pbuffers)\n");
    } else {
        printf ("GL: %s\n", (const char *) glGetString (GL_RENDERER));
        walpe_glutil_init ();
        fx_set_cache_enabled (TRUE);
        if (scene) {
            int bad = run_scene (scene, w, h, frames, TRUE);
            if (bad)
                failures++;
            printf ("%s: %s\n", scene, bad == -2 ? "NOT REPRODUCIBLE (no verdict)" : bad < 0 ? "not loaded" :
                    bad ? "DIFFERENT" : "clipped == unclipped");
        } else {
            for (int k = 0; k < 2; k++) {
                char *dir = make_scene (k == 1);
                int bad = run_scene (dir, w, h, frames, TRUE);
                CHECK (bad == 0, "made-up scene %s: %d pixels differ", k ? "with compose layer" : "", bad);
                printf ("made-up scene%s: %s\n", k ? " with a compose layer" : "",
                        bad ? "DIFFERENT" : "clipped == unclipped");
                if (k == 0 && eglGetCurrentSurface (EGL_DRAW) != EGL_NO_SURFACE) {
                    /* the same scene twice as large on screen: the effect
                     * results are magnified, a pixel at the edge of a box
                     * reads the texel next to its own */
                    EGLSurface big = big_surface (dpy, 2 * w, 2 * h);
                    if (big != EGL_NO_SURFACE) {
                        g_setenv ("CLIP_TEST_LAYOUTS", "0,3,4,7", TRUE);
                        bad = run_scene (dir, 2 * w, 2 * h, 3, TRUE);
                        CHECK (bad == 0, "made-up scene at twice its size: %d pixels differ", bad);
                        printf ("made-up scene at twice its size: %s\n", bad ? "DIFFERENT" : "clipped == unclipped");
                        /* and the check of this check: without the margin
                         * for that texel the comparison has to fail */
                        fx_clip_test_nopad = TRUE;
                        bad = run_scene (dir, 2 * w, 2 * h, 2, FALSE);
                        fx_clip_test_nopad = FALSE;
                        CHECK (bad > 0, "without the filter margin the comparison should see differences (%d)", bad);
                        printf ("  (without the margin for the caller's filter: %d pixels differ - the "
                                "comparison sees it)\n", bad);
                        g_unsetenv ("CLIP_TEST_LAYOUTS");
                        small_surface (dpy);
                    }
                }
                rm_rf (dir);
                g_free (dir);
            }
        }
    }
    rm_rf (home);
    g_free (home);
    rc = failures ? 1 : 0;
    printf ("%s\n", rc ? "clip-test FAILED" : "clip-test ok");
    fflush (NULL);
    _exit (rc);     /* (no EGL teardown: driver threads may still run) */
}
