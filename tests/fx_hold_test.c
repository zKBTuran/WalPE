/* What the effect executor keeps over frames, against what the opt-in
 * shader modes change about who draws a result (the earlier build merged the
 * presets and the shader-cost work untested together).
 *
 *   fx-hold-test
 *
 * With an EGL device or Mesa's surfaceless platform (skipped without),
 * small effects written to a temporary project, through the executor:
 *
 * 1. effect update rate x fused runs (fx_rate 2 + fx_quality fast): a set
 *    whose result a fused run delivers is held on the frames in between
 *    (no pass drawn), its redraws are the pictures of the unheld set, and
 *    the members' own targets go when the fused pass takes over;
 * 2. the held prefix in front of an effect that is drawn every frame, when
 *    a fused run is that prefix, and when a fused run reaches across its
 *    end (then nothing is held: the fused picture is not split);
 * 3. outputs kept over frames (static cache, its "soft" form for sets that
 *    follow the pointer, the static prefix) never sit in a target lent
 *    from the pool, where the next layer overwrites them: a fused run or
 *    a skipped identity pass (fx_specialize: opacity 1) as the last chain;
 * 4. the comparison a specialised program has to pass (fx_verify.h) reads
 *    the same verdict whatever times its first draws happen at, and sees
 *    a difference that only shows later in the effect's cycle;
 * 5. constant values written with commas ("0.0, 1.0": annotation defaults
 *    of workshop shaders) are read as the vectors they are.
 *
 * The engine file is included so the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#include <epoxy/egl.h>
#include <glib/gstdio.h>
#include <stdio.h>

static int fails, checks;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    printf ("%s %s%s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? "  [" : "", detail ? detail : "",
            detail ? "]" : "");
    if (!ok)
        fails++;
}

static char *tmpdir;

static void put (const char *rel, const char *text)
{
    char *p = g_build_filename (tmpdir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

/* a single-pass effect "name" from its two shader bodies */
static void put_effect (const char *name, const char *vert, const char *frag)
{
    char *e = g_strdup_printf ("effects/%s/effect.json", name);
    char *ej = g_strdup_printf ("{\"passes\":[{\"material\":\"materials/effects/%s.json\"}]}", name);
    char *m = g_strdup_printf ("materials/effects/%s.json", name);
    char *mj = g_strdup_printf ("{\"passes\":[{\"shader\":\"effects/%s\"}]}", name);
    char *v = g_strdup_printf ("shaders/effects/%s.vert", name), *f = g_strdup_printf ("shaders/effects/%s.frag", name);
    put (e, ej);
    put (m, mj);
    put (v, vert);
    put (f, frag);
    g_free (e); g_free (ej); g_free (m); g_free (mj); g_free (v); g_free (f);
}

#define W 64
#define H 48
#define NPX (W * H * 4)

/* a smooth picture: what displacement effects are used on */
static GLuint smooth_tex (void)
{
    static guint8 px[NPX];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            guint8 *p = &px[(y * W + x) * 4];
            p[0] = (guint8) (127.0 + 120.0 * sin (x * 0.21) * cos (y * 0.17));
            p[1] = (guint8) (127.0 + 120.0 * sin (x * 0.11 + y * 0.13));
            p[2] = (guint8) (40 + 3 * x);
            p[3] = 255;
        }
    GLuint t = 0;
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

static gpointer build_set (const char *effects_json, gboolean fast)
{
    char *wrapped = g_strdup_printf ("{\"effects\":%s}", effects_json);
    JsonNode *root = wpj_parse_data (wrapped, -1);
    g_free (wrapped);
    if (!root)
        return NULL;
    g_atomic_int_set (&fxs_quality, fast);
    gpointer set = fx_build_for_object (tmpdir, wpj_arr (json_node_get_object (root), "effects"), "test", NULL);
    json_node_unref (root);
    return set;
}

static void read_tex (GLuint tex, guint8 *out)
{
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

static float ptr_x = 0.5f, ptr_y = 0.5f;

/* one frame of a scene with this one layer: frame counter `frame`, effect
 * update divisor `div` (all = every animated set), over an input that
 * stands still (in_static) or not; *run / *held = passes drawn / held */
static GLuint frame_of (gpointer set, GLuint in, double t, guint frame, int div, gboolean all,
                        gboolean in_static, guint8 *out, guint *run, guint *held)
{
    guint r0 = 0, r1 = 0, h0 = 0, h1 = 0;
    fx_set_frame_info (W, H, ptr_x, ptr_y, 1.0f / 30);
    fx_set_quality (div, all, WALPE_POST_FULL, 30.0, frame);
    fx_pass_stats (&r0, NULL, FALSE);
    fx_pass_stats_held (&h0, FALSE);
    GLuint o = fx_run_for_object (set, in, W, H, W, H, t, 1.0f, in_static);
    fx_pass_stats (&r1, NULL, FALSE);
    fx_pass_stats_held (&h1, FALSE);
    if (out)
        read_tex (o, out);
    if (run)
        *run = r1 - r0;
    if (held)
        *held = h1 - h0;
    return o;
}

static int max_diff (const guint8 *a, const guint8 *b)
{
    int m = 0;
    for (int i = 0; i < NPX; i++)
        m = MAX (m, abs ((int) a[i] - (int) b[i]));
    return m;
}

static const char *plain_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec2 v_TexCoord;\nvoid main() {\n"
    "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n";
/* static colour effect */
static const char *gain_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"1 1 1\"}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb = c.rgb * g_Gain * g_Tint;\n\tgl_FragColor = c;\n}\n";
/* an effect the identity recogniser accepts: one multiply of the alpha by
 * the user's value. fxs_identity (src/engine/fx_rewrite.h) knows the main
 * function of such a fragment shader by its key (fx_keys.h) and looks for
 * the two plain statements in the vertex code. The main function is this
 * test's own: main () registers its key in the engine's slot */
#define OP_MAIN "void main() {\n  vec4 px = texSample2D(g_Texture0, v_TexCoord.xy);\n" \
                "  px.a = px.a * g_UserAlpha;\n  gl_FragColor = px;\n}\n"
static const char *op_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec4 v_TexCoord;\n\nvoid main() {\n  v_TexCoord.zw = vec2(0.0);\n  v_TexCoord.xy = a_TexCoord;\n"
    "  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n}\n";
static const char *op_frag =
    "uniform float g_UserAlpha; // {\"material\":\"alpha\",\"default\":1}\n"
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\nvarying vec4 v_TexCoord;\n\n" OP_MAIN;
/* two slow displacement effects of different make */
static const char *wv_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nuniform float g_Angle; // {\"material\":\"angle\",\"default\":0}\n"
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\nvarying vec2 v_Dir;\n"
    "void main() {\n    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "\tv_TexCoord = a_TexCoord.xyxy;\n\tv_Dir = vec2(cos(g_Angle), sin(g_Angle));\n}\n";
static const char *wv_frag =
    "varying vec4 v_TexCoord;\nvarying vec2 v_Dir;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Time;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.02}\n"
    "uniform float g_Freq; // {\"material\":\"freq\",\"default\":9}\n"
    "void main() {\n\tvec2 at = v_TexCoord.xy;\n"
    "\tat += v_Dir * sin(dot(at, v_Dir.yx) * g_Freq + g_Time) * g_Amp;\n"
    "\tgl_FragColor = texSample2D(g_Texture0, at);\n}\n";
static const char *sh_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nuniform float g_Time;\n"
    "uniform vec2 g_Amp; // {\"material\":\"amp\",\"default\":\"0.01 0.02\"}\n"
    "void main() {\n\tvec2 flow = vec2(sin(v_TexCoord.y * 7.0), cos(v_TexCoord.x * 5.0));\n"
    "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord + flow * g_Amp * sin(g_Time * 2.0));\n}\n";
/* a displacement that follows the pointer and nothing else */
static const char *pd_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform vec2 g_PointerPosition;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.05}\n"
    "void main() {\n\tvec2 uv = v_TexCoord + (g_PointerPosition - vec2(0.5)) * g_Amp * sin(v_TexCoord.x * 9.0);\n"
    "\tgl_FragColor = texSample2D(g_Texture0, uv);\n}\n";
/* follows the pointer and is no pure displacement: drawn every frame,
 * never part of a fused run */
static const char *pm_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform vec2 g_PointerPosition;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.5}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb *= 1.0 - g_Amp * distance(v_TexCoord, g_PointerPosition);\n\tgl_FragColor = c;\n}\n";
/* a slow colour pulse: animated, no displacement */
static const char *pu_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nuniform float g_Time;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.4}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb *= 1.0 - g_Amp * (0.5 + 0.5 * sin(g_Time));\n\tgl_FragColor = c;\n}\n";

#define FX(name) "{\"file\":\"effects/" name "/effect.json\"}"
#define FXC(name, consts) "{\"file\":\"effects/" name "/effect.json\",\"passes\":[{\"constantshadervalues\":{" consts "}}]}"

static int own_targets (struct FxSet *set)
{
    int n = 0;
    for (int i = 0; i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c)
            continue;
        n += c->out_tex && !c->out_pool;
        for (int t = 0; t < c->n_targets; t++)
            n += c->targets[t].tex && !c->targets[t].pool;
    }
    return n;
}

/* another layer of the same size draws: whatever sits in a pooled target
 * of this size is overwritten */
static void disturb (struct FxSet *other, GLuint in)
{
    static guint8 scratch[NPX];
    fx_set_quality (1, FALSE, WALPE_POST_FULL, 30.0, 0);
    fx_run_for_object (other, in, W, H, W, H, 0.0, 1.0f, FALSE);
    read_tex (in, scratch);
}

/* ---- 1. effect update rate x fused runs */
static void hold_fuse_checks (GLuint in, struct FxSet *other)
{
    static guint8 a[NPX], b[NPX], c[NPX], ref[NPX];
    const char *run3 = "[" FXC ("wv", "\"amp\":0.03,\"angle\":0.4") "," FX ("sh") ","
                       FXC ("wv", "\"amp\":0.02,\"freq\":14,\"angle\":2.0") "]";
    for (int all = 0; all <= 1; all++) {
        struct FxSet *set = build_set (run3, TRUE), *plain = build_set (run3, TRUE);
        char *d;
        guint run = 0, held = 0;
        result (set && set->fuse && set->fuse->n == 1 && set->fuse->g[0].last == 2,
                "fast: the three displacement effects are one fused run", NULL);
        /* the set before it was fused: its last chain owned the held output */
        for (int i = 0; i < set->n; i++) {
            EffectChain *m = set->chains[i];
            fx_make_rt (&m->out_fbo, &m->out_tex, W, H);
            m->out_w = W, m->out_h = H;
        }
        const int before = own_targets (set);
        GLuint o2 = frame_of (set, in, 2 / 30.0, 2, 2, all, TRUE, a, &run, &held);
        d = g_strdup_printf ("frame 2: %u pass(es) drawn, own targets %d -> %d", run, before, own_targets (set));
        result (run == 1 && own_targets (set) == 0 && before == 3,
                all ? "all: the redraw is the one fused pass; the members' own targets are gone"
                    : "slow: the redraw is the one fused pass; the members' own targets are gone", d);
        g_free (d);
        disturb (other, in);
        GLuint o3 = frame_of (set, in, 3 / 30.0, 3, 2, all, TRUE, b, &run, &held);
        d = g_strdup_printf ("frame 3: %u drawn, %u held, texture %u -> %u, max difference to frame 2: %d",
                             run, held, o2, o3, max_diff (a, b));
        result (run == 0 && held == 3 && o3 == o2 && max_diff (a, b) == 0,
                all ? "all: the set a fused run delivers is held in between, and its picture survives another layer's run"
                    : "slow: the set a fused run delivers is held in between, and its picture survives another layer's run", d);
        g_free (d);
        disturb (other, in);
        frame_of (set, in, 4 / 30.0, 4, 2, all, TRUE, c, &run, &held);
        frame_of (plain, in, 4 / 30.0, 4, 1, FALSE, TRUE, ref, NULL, NULL);
        d = g_strdup_printf ("frame 4: %u drawn, max difference to the unheld set: %d", run, max_diff (c, ref));
        result (run == 1 && max_diff (c, ref) == 0, "the redraw of a held fused set is the unheld picture of that frame", d);
        g_free (d);
        /* the fused pass stops standing in (a member is switched off):
         * the set is redrawn at once, one by one, and held again */
        fx_set_enabled (set, 1, FALSE);
        fx_set_enabled (plain, 1, FALSE);
        frame_of (set, in, 5 / 30.0, 5, 2, all, TRUE, a, &run, &held);
        frame_of (plain, in, 5 / 30.0, 5, 1, FALSE, TRUE, ref, NULL, NULL);
        d = g_strdup_printf ("frame 5: %u drawn, max difference %d", run, max_diff (a, ref));
        result (run == 2 && max_diff (a, ref) == 0, "a member switched off: redrawn at once, one by one", d);
        g_free (d);
        frame_of (set, in, 6 / 30.0, 6, 2, all, TRUE, a, &run, &held);
        disturb (other, in);
        frame_of (set, in, 7 / 30.0, 7, 2, all, TRUE, b, &run, &held);
        d = g_strdup_printf ("frame 7: %u drawn, %u held, max difference to frame 6: %d, own targets %d",
                             run, held, max_diff (a, b), own_targets (set));
        result (run == 0 && held == 2 && max_diff (a, b) == 0 &&
                !(set->fuse->g[0].chain->out_tex && !set->fuse->g[0].chain->out_pool),
                "held again without the fused pass, whose own target is gone", d);
        g_free (d);
        fx_free_for_object (set);
        fx_free_for_object (plain);
    }
}

/* ---- 2. the held prefix */
static void held_prefix_checks (GLuint in, struct FxSet *other)
{
    static guint8 a[NPX], ref[NPX];
    guint run = 0, held = 0;
    char *d;
    /* [wv sh] fused, then an effect that follows the pointer: slow mode
     * holds the two in front of it */
    const char *pre = "[" FXC ("wv", "\"amp\":0.03,\"angle\":0.4") "," FX ("sh") "," FX ("pm") "]";
    struct FxSet *set = build_set (pre, TRUE), *plain = build_set (pre, TRUE);
    result (set && set->fuse && set->fuse->n == 1 && set->fuse->g[0].last == 1,
            "fast: two displacement effects in front of a pointer effect are one fused run", NULL);
    /* (the pointer rests, so frame 3 = the prefix as of frame 2 under the
     * pointer effect = the picture of frame 2) */
    frame_of (set, in, 2 / 30.0, 2, 2, FALSE, TRUE, ref, &run, &held);
    disturb (other, in);
    frame_of (set, in, 3 / 30.0, 3, 2, FALSE, TRUE, a, &run, &held);
    d = g_strdup_printf ("frame 3: %u drawn, %u held, max difference to frame 2: %d", run, held, max_diff (a, ref));
    result (run == 1 && held == 2 && max_diff (a, ref) == 0,
            "slow: a fused run in front of an every-frame effect is held as the prefix", d);
    g_free (d);
    frame_of (set, in, 4 / 30.0, 4, 2, FALSE, TRUE, a, &run, &held);
    frame_of (plain, in, 4 / 30.0, 4, 1, FALSE, TRUE, ref, NULL, NULL);
    d = g_strdup_printf ("frame 4: %u drawn, max difference %d", run, max_diff (a, ref));
    result (run == 2 && max_diff (a, ref) == 0, "the redraw of a held fused prefix is the unheld picture", d);
    g_free (d);
    fx_free_for_object (set);
    fx_free_for_object (plain);

    /* [wv pd] is one fused run, but pd follows the pointer: the prefix
     * would end inside the run - nothing is held, the picture stays the
     * fused one in every frame */
    const char *across = "[" FXC ("wv", "\"amp\":0.03,\"angle\":0.4") "," FX ("pd") "]";
    set = build_set (across, TRUE);
    plain = build_set (across, TRUE);
    result (set && set->fuse && set->fuse->n == 1 && set->fuse->g[0].last == 1,
            "fast: a slow displacement and a pointer displacement are one fused run", NULL);
    gboolean same = TRUE;
    guint drawn = 0;
    for (guint f = 2; f <= 5; f++) {
        ptr_x = 0.3f + 0.05f * (float) f;
        frame_of (set, in, f / 30.0, f, 2, FALSE, TRUE, a, &run, &held);
        drawn += run;
        disturb (other, in);
        frame_of (plain, in, f / 30.0, f, 1, FALSE, TRUE, ref, NULL, NULL);
        same &= max_diff (a, ref) == 0;
    }
    ptr_x = 0.5f;
    d = g_strdup_printf ("%u passes over 4 frames", drawn);
    result (same && drawn == 4, "slow: a fused run across the end of the held prefix is drawn whole in every frame", d);
    g_free (d);
    fx_free_for_object (set);
    fx_free_for_object (plain);
}

/* ---- 3. kept outputs and the pool */
static void kept_output_checks (GLuint in, struct FxSet *other)
{
    static guint8 a[NPX], b[NPX], ref[NPX];
    guint run = 0, held = 0;
    char *d;
    /* soft cache x fused run: two pointer displacements, pointer at rest */
    const char *soft = "[" FX ("pd") "," FXC ("pd", "\"amp\":0.08") "]";
    struct FxSet *set = build_set (soft, TRUE);
    result (set && set->fuse && set->fuse->n == 1, "fast: two pointer displacements are one fused run", NULL);
    for (guint f = 1; f <= 3; f++)
        frame_of (set, in, f / 30.0, f, 1, FALSE, TRUE, a, &run, &held);
    disturb (other, in);
    frame_of (set, in, 4 / 30.0, 4, 1, FALSE, TRUE, b, &run, &held);
    d = g_strdup_printf ("frame 4: %u drawn, max difference to frame 3: %d", run, max_diff (a, b));
    result (max_diff (a, b) == 0, "a fused set kept while the pointer rests survives another layer's run", d);
    g_free (d);
    result (run == 0, "... and is served from the cache", NULL);
    fx_free_for_object (set);

    /* static cache x identity pass: [gain, opacity 1] */
    const char *ident = "[" FXC ("gain", "\"gain\":0.6,\"tint\":\"1 0.5 0.25\"") "," FX ("opacity") "]";
    set = build_set (ident, FALSE);
    frame_of (set, in, 1 / 30.0, 1, 1, FALSE, TRUE, a, &run, &held);
    d = g_strdup_printf ("%u pass(es)", run);
    result (run == 1, "exact: an opacity-1 pass behind another effect is not drawn", d);
    g_free (d);
    disturb (other, in);
    frame_of (set, in, 2 / 30.0, 2, 1, FALSE, TRUE, b, &run, &held);
    d = g_strdup_printf ("frame 2: %u drawn, max difference to frame 1: %d", run, max_diff (a, b));
    result (max_diff (a, b) == 0, "a static set that ends in a skipped identity pass survives another layer's run", d);
    g_free (d);
    result (run == 0, "... and is served from the cache", NULL);
    fx_free_for_object (set);

    /* static prefix x identity pass: [gain, opacity 1, pulse] */
    const char *pfx = "[" FXC ("gain", "\"gain\":0.6,\"tint\":\"1 0.5 0.25\"") "," FX ("opacity") "," FX ("pu") "]";
    set = build_set (pfx, FALSE);
    struct FxSet *plain = build_set (pfx, FALSE);
    frame_of (set, in, 1 / 30.0, 1, 1, FALSE, TRUE, a, &run, &held);
    disturb (other, in);
    frame_of (set, in, 2 / 30.0, 2, 1, FALSE, TRUE, b, &run, &held);
    frame_of (plain, in, 2 / 30.0, 2, 1, FALSE, FALSE, ref, NULL, NULL);
    d = g_strdup_printf ("frame 2: %u drawn, max difference to the uncached set: %d", run, max_diff (b, ref));
    result (max_diff (b, ref) == 0, "a static prefix that ends in a skipped identity pass survives another layer's run", d);
    g_free (d);
    result (run == 1, "... and only the animated effect behind it is drawn", NULL);
    fx_free_for_object (set);
    fx_free_for_object (plain);

    /* the layer colour pass in front of an identity pass: [opacity 1, pulse] tinted */
    const char *tp = "[" FX ("opacity") "," FX ("pu") "]";
    const float tint[3] = { 0.5f, 1.0f, 0.75f };
    set = build_set (tp, FALSE);
    plain = build_set (tp, FALSE);
    fx_set_input_tint (set, tint);
    fx_set_input_tint (plain, tint);
    frame_of (set, in, 1 / 30.0, 1, 1, FALSE, TRUE, a, &run, &held);
    disturb (other, in);
    frame_of (set, in, 2 / 30.0, 2, 1, FALSE, TRUE, b, &run, &held);
    frame_of (plain, in, 2 / 30.0, 2, 1, FALSE, FALSE, ref, NULL, NULL);
    d = g_strdup_printf ("frame 2: %u drawn, max difference to the uncached set: %d", run, max_diff (b, ref));
    result (max_diff (b, ref) == 0, "the kept layer-colour pass in front of a skipped identity pass survives another layer's run", d);
    g_free (d);
    fx_free_for_object (set);
    fx_free_for_object (plain);

    /* update rate x identity pass: [pulse, opacity 1] held */
    const char *hi = "[" FX ("pu") "," FX ("opacity") "]";
    set = build_set (hi, FALSE);
    frame_of (set, in, 2 / 30.0, 2, 2, FALSE, TRUE, a, &run, &held);
    disturb (other, in);
    frame_of (set, in, 3 / 30.0, 3, 2, FALSE, TRUE, b, &run, &held);
    d = g_strdup_printf ("frame 3: %u drawn, %u held, max difference to frame 2: %d", run, held, max_diff (a, b));
    result (run == 0 && held >= 1 && max_diff (a, b) == 0,
            "a slow set that ends in a skipped identity pass is held in between", d);
    g_free (d);
    fx_free_for_object (set);
}

/* ---- 4. the comparison of specialised programs */

/* leans on what the GPU does with a division by zero: (x + 0.25) / 0 is
 * +inf at run time, clamped to 1. A compiler that sees the zero as a
 * constant may make anything of it. hz: in every frame; hzt: only from
 * g_Time = 10 s on */
static const char *hz_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Div; // {\"material\":\"div\",\"default\":0}\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tfloat t = clamp((c.r + 0.25) / g_Div, 0.0, 1.0);\n\tc.rgb *= t * g_Gain;\n\tgl_FragColor = c;\n}\n";
static const char *hzt_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nuniform float g_Time;\n"
    "uniform float g_Div; // {\"material\":\"div\",\"default\":0}\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tfloat t = clamp((c.r + 0.25) / g_Div, 0.0, 1.0);\n"
    "\tif (g_Time >= 10.0)\n\t\tc.rgb *= t * g_Gain;\n\tgl_FragColor = c;\n}\n";
/* the same division in a pass that writes a float target, with a result
 * above 1 either way (an 8-bit comparison sees 1 and 1), read back and
 * scaled into the picture by the second pass */
static const char *fl1_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Div; // {\"material\":\"div\",\"default\":0}\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tfloat t = clamp((c.r + 0.25) / g_Div, 0.0, 1.0);\n\tgl_FragColor = vec4(2.0 + 4.0 * t * g_Gain);\n}\n";
static const char *fl2_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture1; // {\"hidden\":true}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb *= texSample2D(g_Texture1, v_TexCoord).r * 0.125;\n\tgl_FragColor = c;\n}\n";

/* 33 draws at t0, t0 + dt, ... with the comparison on: does the first
 * pass draw with baked constants afterwards? pic = the frame at `at` */
static gboolean verdict_of (const char *json, GLuint in, double t0, double dt, double at, guint8 *pic)
{
    struct FxSet *set = build_set (json, FALSE);
    if (!set)
        return FALSE;
    for (guint i = 0; i < 33; i++)
        frame_of (set, in, t0 + dt * i, i + 1, 1, FALSE, FALSE, NULL, NULL, NULL);
    gboolean baked = set->chains[0]->passes[0].spec && set->chains[0]->passes[0].spec->n_baked > 0;
    if (pic)
        frame_of (set, in, at, 40, 1, FALSE, FALSE, pic, NULL, NULL);
    fx_free_for_object (set);
    return baked;
}

static void generic_pic (const char *json, GLuint in, double at, guint8 *pic)
{
    g_atomic_int_set (&fxs_spec_on, 0);
    struct FxSet *set = build_set (json, FALSE);
    g_atomic_int_set (&fxs_spec_on, 1);
    frame_of (set, in, at, 40, 1, FALSE, FALSE, pic, NULL, NULL);
    fx_free_for_object (set);
}

static void verify_checks (GLuint in)
{
    static guint8 a[NPX], b[NPX], ref[NPX];
    char *d;
    put_effect ("hz", plain_vert, hz_frag);
    put_effect ("hzt", plain_vert, hzt_frag);
    g_atomic_int_set (&fxs_verify_on, 1);
    g_atomic_int_set (&fxs_verify_n, 6);
    /* what this driver's compiler makes of the constant zero (the texts
     * differ in a constant, so each has its own verdict) */
    const gboolean folds = !verdict_of ("[" FXC ("hz", "\"gain\":0.8") "]", in, 0.0, 0.11, 1.0, NULL);
    printf ("     (this driver's compiler %s what the GPU computes for a division by a constant zero)\n",
            folds ? "does not give" : "gives");
    /* the first 32 draws early in the scene, as they are at 30..144 fps,
     * and late (a pass that comes on screen after 20 s) */
    gboolean early = verdict_of ("[" FXC ("hzt", "\"gain\":0.8") "]", in, 0.0, 0.11, 30.0, a);
    gboolean late = verdict_of ("[" FXC ("hzt", "\"gain\":0.81") "]", in, 20.0, 0.37, 30.0, b);
    d = g_strdup_printf ("draws at 0..3.5 s: %s, at 20..32 s: %s", early ? "baked" : "generic", late ? "baked" : "generic");
    result (early == late, "verify: the verdict does not depend on the times of the first 32 draws", d);
    g_free (d);
    generic_pic ("[" FXC ("hzt", "\"gain\":0.8") "]", in, 30.0, ref);
    d = g_strdup_printf ("max difference at 30 s: %d", max_diff (a, ref));
    result (max_diff (a, ref) <= 1 && (!folds || !early),
            "verify: a difference that only shows from 10 s on is seen: the picture stays the generic program's", d);
    g_free (d);

    /* while a pass is being compared its frames are the generic
     * program's, with the frame's own time and pointer (not a probe's) */
    const char *dynfx[2] = { "[" FXC ("pm", "\"amp\":0.45") "]", "[" FXC ("pu", "\"amp\":0.35") "]" };
    for (int e = 0; e < 2; e++) {
        g_atomic_int_set (&fxs_spec_on, 0);
        struct FxSet *gen = build_set (dynfx[e], FALSE);
        g_atomic_int_set (&fxs_spec_on, 1);
        struct FxSet *set = build_set (dynfx[e], FALSE);
        int worst = 0;
        ptr_x = 0.3f, ptr_y = 0.6f;
        for (guint i = 0; i < 36; i++) {
            /* (time stands still from frame 3 on: nothing is sent again
             * unless the comparison says so) */
            const double t = i < 3 ? 0.1 * i : 0.2;
            frame_of (set, in, t, i + 1, 1, FALSE, FALSE, a, NULL, NULL);
            frame_of (gen, in, t, i + 1, 1, FALSE, FALSE, ref, NULL, NULL);
            worst = MAX (worst, max_diff (a, ref));
        }
        ptr_x = ptr_y = 0.5f;
        d = g_strdup_printf ("worst difference over 36 frames: %d; baked at the end: %d", worst,
                             set->chains[0]->passes[0].spec->n_baked);
        result (worst <= 1 && set->chains[0]->passes[0].spec->n_baked > 0,
                e ? "verify: frames of a time-driven pass under comparison are drawn at the frame's time"
                  : "verify: frames of a pointer-driven pass under comparison are drawn at the frame's pointer", d);
        g_free (d);
        fx_free_for_object (set);
        fx_free_for_object (gen);
    }

    /* a float target is compared as float */
    put ("effects/fl/effect.json",
         "{\"passes\":[{\"material\":\"materials/effects/fl1.json\",\"target\":\"_rt_flbuf\"},"
         "{\"material\":\"materials/effects/fl2.json\",\"bind\":[{\"name\":\"_rt_flbuf\",\"index\":1}]}],"
         "\"fbos\":[{\"name\":\"_rt_flbuf\",\"format\":\"r16f\",\"scale\":1}]}");
    put ("materials/effects/fl1.json", "{\"passes\":[{\"shader\":\"effects/fl1\"}]}");
    put ("materials/effects/fl2.json", "{\"passes\":[{\"shader\":\"effects/fl2\"}]}");
    put ("shaders/effects/fl1.vert", plain_vert);
    put ("shaders/effects/fl1.frag", fl1_frag);
    put ("shaders/effects/fl2.vert", plain_vert);
    put ("shaders/effects/fl2.frag", fl2_frag);
    const char *fl = "[" FXC ("fl", "\"gain\":0.9") "]";
    gboolean baked = verdict_of (fl, in, 0.0, 0.11, 1.0, a);
    generic_pic (fl, in, 1.0, ref);
    d = g_strdup_printf ("first pass %s, max difference to the generic picture: %d", baked ? "baked" : "generic",
                         max_diff (a, ref));
    result (max_diff (a, ref) <= 1 && (!folds || !baked),
            "verify: a pass that writes a float target is compared beyond 0..1", d);
    g_free (d);
    /* a picture of one colour proves nothing: over a black input (a video
     * before its first frame, the frame under a compose layer at load) the
     * pass stays on probation; the comparisons count from the first
     * picture on */
    {
        static guint8 zero[NPX];
        GLuint black = 0;
        glGenTextures (1, &black);
        glBindTexture (GL_TEXTURE_2D, black);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        const char *gb = "[" FXC ("gain", "\"gain\":0.55,\"tint\":\"1 0.5 0.25\"") "]";
        struct FxSet *set = build_set (gb, FALSE);
        int v0 = 0, x0 = 0, v1 = 0, x1 = 0;
        fx_verify_stats (&v0, &x0);
        for (guint i = 0; i < 40; i++)
            frame_of (set, black, 0.11 * i, i + 1, 1, FALSE, FALSE, NULL, NULL, NULL);
        fx_verify_stats (&v1, &x1);
        const struct FxSpec *sp = set->chains[0]->passes[0].spec;
        gboolean waiting = sp->probation && sp->n_baked == 0 && sp->passed == 0 && v1 == v0 && x1 == x0;
        /* the picture arrives with draw 41: seen behind draw 64, the other
         * five comparisons behind the 31 draws after it */
        generic_pic (gb, in, 5.0, ref);
        int worst = 0;
        guint baked_at = 0;
        for (guint i = 40; i < 110; i++) {
            frame_of (set, in, 5.0, i + 1, 1, FALSE, FALSE, a, NULL, NULL);
            worst = MAX (worst, max_diff (a, ref));
            if (!baked_at && set->chains[0]->passes[0].spec->n_baked > 0)
                baked_at = i + 1;
        }
        fx_verify_stats (&v1, &x1);
        d = g_strdup_printf ("after 40 black frames: %s; baked behind draw %u; worst difference to the generic picture %d",
                             waiting ? "still on probation, nothing counted" : "decided", baked_at, worst);
        result (waiting && worst <= 1 && baked_at == 64 + 31 && v1 == v0 + 1 && x1 == x0,
                "verify: comparisons over a one-colour picture are not counted", d);
        g_free (d);
        fx_free_for_object (set);
        glDeleteTextures (1, &black);
    }
    /* the comparison itself, on two programs that differ only above 1:
     * the same picture for an 8-bit target (which stores 1 for both), not
     * for a float one. And what it leaves behind. */
    {
        const char *vs = "#version 330\nin vec3 a_Position;\nin vec2 a_TexCoord;\nout vec2 uv;\n"
                         "void main() { gl_Position = vec4(a_Position, 1.0); uv = a_TexCoord; }\n";
        const char *f2 = "#version 330\nin vec2 uv;\nout vec4 c;\nuniform float g_Time;\n"
                         "void main() { c = vec4(2.0 + 0.0 * g_Time); }\n";
        const char *f5 = "#version 330\nin vec2 uv;\nout vec4 c;\nuniform float g_Time;\n"
                         "void main() { c = vec4(5.0 + 0.0 * g_Time); }\n";
        const char *ft = "#version 330\nin vec2 uv;\nout vec4 c;\nuniform float g_Time;\n"
                         "void main() { c = vec4(g_Time > 2000.0 ? 0.25 : 2.0); }\n";
        GLuint g = fxs_link_quiet (vs, f2, "t"), c5 = fxs_link_quiet (vs, f5, "t"), ct = fxs_link_quiet (vs, ft, "t");
        EffPass fake = { 0 };
        fake.program = g;
        GLuint fb[2] = { 0, 0 }, tx[2] = { 0, 0 };
        fx_make_rt_fmt (&fb[0], &tx[0], 40, 24, FX_FMT_R16F);
        fx_make_rt_fmt (&fb[1], &tx[1], 40, 24, FX_FMT_RGBA8);
        int md[4] = { -1, -1, -1, -1 };
        gboolean same[4] = { FALSE, FALSE, FALSE, FALSE }, kept = TRUE;
        for (int k = 0; k < 2 && g && c5 && ct; k++) {
            glBindFramebuffer (GL_FRAMEBUFFER, fb[k]);
            glViewport (0, 0, 40, 24);
            glUseProgram (g);
            fx_bind_pass_vao ();
            glDisable (GL_BLEND);
            gboolean flat = FALSE;
            same[k] = fxs_compare (&fake, c5, 0, &md[k], &flat);
            kept &= flat;   /* (both programs draw one colour) */
            GLint vp[4] = { 0 }, cur = 0, dfb = 0;
            glGetIntegerv (GL_VIEWPORT, vp);
            glGetIntegerv (GL_CURRENT_PROGRAM, &cur);
            glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
            kept &= vp[2] == 40 && vp[3] == 24 && (GLuint) cur == g && (GLuint) dfb == fb[k];
        }
        d = g_strdup_printf ("float target: %s (%d/255), 8-bit target: %s (%d/255)", same[0] ? "same" : "different",
                             md[0], same[1] ? "same" : "different", md[1]);
        result (g && c5 && ct && !same[0] && same[1] && kept,
                "verify: 2.0 against 5.0 is a difference in a float target and none in an 8-bit one", d);
        g_free (d);
        /* a candidate that differs only late in time: none of the first
         * probes shows it, a later round does (the times reach an hour) */
        glBindFramebuffer (GL_FRAMEBUFFER, fb[1]);
        glViewport (0, 0, 40, 24);
        glUseProgram (g);
        gboolean fl = FALSE;
        gboolean first = g && ct && fxs_compare (&fake, ct, 0, &md[2], &fl);
        gboolean any = FALSE;
        for (guint r = 1; r < 6 && g && ct; r++)
            any |= !fxs_compare (&fake, ct, r, &md[3], &fl);
        result (first && any, "verify: the six comparisons probe times beyond half an hour", NULL);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        fx_drop_rt (&fb[0], &tx[0], 40, 24);
        fx_drop_rt (&fb[1], &tx[1], 40, 24);
        glDeleteProgram (g);
        glDeleteProgram (c5);
        glDeleteProgram (ct);
        walpe_glutil_bind_quad_vao ();
    }
    g_atomic_int_set (&fxs_verify_on, 0);
}

/* ---- 5. constants written with commas */

static const char *cv_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform vec2 g_Bounds; // {\"material\":\"bounds\",\"default\":\"0.25, 0.75\"}\n"
    "uniform vec3 g_Col; // {\"material\":\"col\",\"default\":\"1 1 1\"}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb *= g_Col * mix(g_Bounds.x, g_Bounds.y, v_TexCoord.x);\n\tgl_FragColor = c;\n}\n";

/* level bars fed by the audio spectrum. Their lower and upper end are one
 * vec2 whose annotation default is written with a comma, the way shader
 * authors write it ("0.0, 1.0"): read as (0, 0) the bars have no height */
static const char *ab_frag =
    "uniform float g_AudioSpectrum16Left[16];\n"
    "uniform vec2 g_Ends; // {\"material\":\"ends\",\"default\":\"0.0, 1.0\"}\n"
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\nvarying vec2 v_TexCoord;\n\n"
    "void main() {\n    int band = int(floor(v_TexCoord.x * 15.99));\n"
    "    float top = g_Ends.x + (g_Ends.y - g_Ends.x) * g_AudioSpectrum16Left[band];\n"
    "    vec3 under = texSample2D(g_Texture0, v_TexCoord).rgb;\n"
    "    gl_FragColor = vec4(1.0 - v_TexCoord.y <= top ? vec3(1.0) : under, 1.0);\n}\n";

static void constant_checks (GLuint in)
{
    static guint8 a[NPX], ref[NPX];
    char *d;
    put_effect ("cv", plain_vert, cv_frag);
    put_effect ("ab", plain_vert, ab_frag);
    {
        /* with sound at half level the bars stand half as high as the
         * layer (they stayed at 0 when the upper bound was lost) */
        static WalpeAudioSpectrum half;
        for (int i = 0; i < 16; i++)
            half.left16[i] = 0.5f;
        g_atomic_int_set (&fxs_spec_on, 0);
        struct FxSet *bars = build_set ("[" FX ("ab") "]", FALSE);
        g_atomic_int_set (&fxs_spec_on, 1);
        fx_set_audio_frame (&half);
        frame_of (bars, in, 0.0, 1, 1, FALSE, FALSE, a, NULL, NULL);
        fx_set_audio_frame (NULL);
        int white = 0;
        for (int i = 0; i < W * H; i++)
            white += a[i * 4] == 255 && a[i * 4 + 1] == 255 && a[i * 4 + 2] == 255;
        d = g_strdup_printf ("%d of %d pixels are bar", white, W * H);
        result (white > W * H * 2 / 5 && white < W * H * 3 / 5,
                "audio bars with bounds \"0.0, 1.0\" rise with the spectrum", d);
        g_free (d);
        fx_free_for_object (bars);
    }
    for (int spec = 0; spec <= 1; spec++) {
        /* spec 0: one program per pass, the 0.12 path; 1: baked */
        g_atomic_int_set (&fxs_spec_on, spec);
        struct FxSet *blank = build_set ("[" FXC ("cv", "\"bounds\":\"0.25 0.75\"") "]", FALSE);
        struct FxSet *deflt = build_set ("[" FX ("cv") "]", FALSE);
        frame_of (blank, in, 0.0, 1, 1, FALSE, FALSE, ref, NULL, NULL);
        frame_of (deflt, in, 0.0, 1, 1, FALSE, FALSE, a, NULL, NULL);
        float v[4] = { 0 };
        int n = fx_get_constant (deflt, 0, 0, "bounds", v);
        d = g_strdup_printf ("max difference %d; reads back %d value(s): %g %g", max_diff (a, ref), n, v[0], v[1]);
        result (max_diff (a, ref) == 0 && n == 2 && v[0] == 0.25f && v[1] == 0.75f,
                spec ? "baked: an annotation default \"0.25, 0.75\" is the vec2 (0.25, 0.75)"
                     : "an annotation default \"0.25, 0.75\" is the vec2 (0.25, 0.75)", d);
        g_free (d);
        fx_free_for_object (blank);
        fx_free_for_object (deflt);
        blank = build_set ("[" FXC ("cv", "\"bounds\":\"0.5 1.0\",\"col\":\"1 0.5 0.25\"") "]", FALSE);
        deflt = build_set ("[" FXC ("cv", "\"bounds\":\"0.5, 1.0\",\"col\":\"1,0.5,0.25\"") "]", FALSE);
        frame_of (blank, in, 0.0, 1, 1, FALSE, FALSE, ref, NULL, NULL);
        frame_of (deflt, in, 0.0, 1, 1, FALSE, FALSE, a, NULL, NULL);
        d = g_strdup_printf ("max difference %d", max_diff (a, ref));
        result (max_diff (a, ref) == 0,
                spec ? "baked: instance values \"0.5, 1.0\" and \"1,0.5,0.25\" are read like \"0.5 1.0\" and \"1 0.5 0.25\""
                     : "instance values \"0.5, 1.0\" and \"1,0.5,0.25\" are read like \"0.5 1.0\" and \"1 0.5 0.25\"", d);
        g_free (d);
        fx_free_for_object (blank);
        fx_free_for_object (deflt);
    }
    g_atomic_int_set (&fxs_spec_on, 1);
    float f[4] = { 9, 9, 9, 9 };
    result (fx_scan_floats ("0.97 0.997", f) == 2 && f[0] == 0.97f && f[1] == 0.997f && f[2] == 9 &&
            fx_scan_floats ("1,2 ,3 , 4", f) == 4 && f[3] == 4 && fx_scan_floats (", 1", f) == 1 &&
            fx_scan_floats ("x", f) == 0, "value strings: blanks and commas separate, nothing else changes", NULL);
}

/* ---- post-processing "reduced" and "off" (presets Balanced and Low) ---- */

static const char *copy_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "void main() {\n\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord);\n}\n";
static const char *dark_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n\tc.rgb *= 0.5;\n\tgl_FragColor = c;\n}\n";

/* one run of a set on a compose layer (or not) at a post-processing level */
static void post_run (gpointer set, GLuint in, int post, gboolean compose, guint frame, guint8 *out,
                      guint *run)
{
    guint r0 = 0, r1 = 0;
    fx_set_frame_info (W, H, ptr_x, ptr_y, 1.0f / 30);
    fx_set_quality (1, FALSE, post, 30.0, frame);
    fx_set_run_compose (compose);
    fx_pass_stats (&r0, NULL, FALSE);
    GLuint o = fx_run_for_object (set, in, W, H, W, H, frame / 30.0, 1.0f, FALSE);
    fx_pass_stats (&r1, NULL, FALSE);
    fx_set_run_compose (FALSE);
    fx_set_quality (1, FALSE, WALPE_POST_FULL, 30.0, frame);
    if (out)
        read_tex (o, out);
    if (run)
        *run = r1 - r0;
}

/* the scene's question before it captures the frame for a compose layer */
static gboolean post_dropped_off (gpointer set)
{
    fx_set_quality (1, FALSE, WALPE_POST_OFF, 30.0, 0);
    gboolean d = fx_post_dropped (set);
    fx_set_quality (1, FALSE, WALPE_POST_FULL, 30.0, 0);
    return d;
}

static void post_level_checks (GLuint in)
{
    static guint8 src[NPX], full[NPX], red[NPX], pic[NPX], black_px[NPX];
    char d[200];
    read_tex (in, src);
    /* a blur of the old form: two passes, neither names a target (they
     * ping-pong between the chain's two outputs), as the blurprecise that
     * many packages carry */
    put ("effects/blurold/effect.json",
         "{\"passes\":[{\"material\":\"materials/effects/blurold.json\",\"compose\":true},"
         "{\"material\":\"materials/effects/blurold.json\"}]}");
    put ("materials/effects/blurold.json", "{\"passes\":[{\"shader\":\"effects/blurold\"}]}");
    put ("shaders/effects/blurold.vert", plain_vert);
    put ("shaders/effects/blurold.frag", copy_frag);
    /* and one of the current form: the first pass into a named buffer */
    put ("effects/blurnew/effect.json",
         "{\"passes\":[{\"material\":\"materials/effects/blurnew.json\",\"target\":\"_rt_b1\"},"
         "{\"material\":\"materials/effects/blurnew.json\",\"bind\":[{\"name\":\"_rt_b1\",\"index\":0}]}],"
         "\"fbos\":[{\"name\":\"_rt_b1\",\"scale\":1}]}");
    put ("materials/effects/blurnew.json", "{\"passes\":[{\"shader\":\"effects/blurnew\"}]}");
    put ("shaders/effects/blurnew.vert", plain_vert);
    put ("shaders/effects/blurnew.frag", copy_frag);
    put_effect ("bloomdark", plain_vert, dark_frag);
    put_effect ("bloomnil", plain_vert, copy_frag);

    for (int form = 0; form < 2; form++) {
        struct FxSet *set = build_set (form ? "[" FX ("blurnew") "]" : "[" FX ("blurold") "]", FALSE);
        if (!set) {
            result (FALSE, "post-processing: the blur set builds", "");
            continue;
        }
        post_run (set, in, WALPE_POST_FULL, TRUE, 1, full, NULL);
        post_run (set, in, WALPE_POST_REDUCED, TRUE, 2, red, NULL);
        int m = max_diff (full, red);
        g_snprintf (d, sizeof d, "largest difference to the full picture %d / 255", m);
        if (!form)
            result (m <= 2, "post-processing reduced: a blur whose passes name no target shows the "
                    "whole picture (its second output is not halved)", d);
        else
            result (m <= 24, "post-processing reduced: a blur with a named inner buffer stays close "
                    "to the full picture", d);
        post_run (set, in, WALPE_POST_OFF, TRUE, 3, red, NULL);
        m = max_diff (full, red);
        g_snprintf (d, sizeof d, "largest difference to the full picture %d / 255", m);
        result (m <= (form ? 24 : 2), form ? "post-processing off: the same for the named buffer"
                : "post-processing off: the same for the blur without targets", d);
        fx_free_for_object (set);
    }

    /* "off": a chain of a cosmetic family that changes the picture's level stays */
    struct FxSet *dark = build_set ("[" FX ("bloomdark") "]", FALSE);
    struct FxSet *nil = build_set ("[" FX ("bloomnil") "]", FALSE);
    if (!dark || !nil) {
        result (FALSE, "post-processing: the bloom sets build", "");
        return;
    }
    post_run (dark, in, WALPE_POST_FULL, TRUE, 1, full, NULL);
    int worst = 0;
    guint runs = 0, r = 0;
    for (guint f = 2; f <= 41; f++) {
        post_run (dark, in, WALPE_POST_OFF, TRUE, f, pic, &r);
        worst = MAX (worst, max_diff (full, pic));
        runs += r;
    }
    g_snprintf (d, sizeof d, "40 frames: %u passes drawn, largest difference to the full picture %d / 255, "
                "to the picture without the effect %d / 255", runs, worst, max_diff (src, pic));
    result (worst == 0 && runs == 40 && !post_dropped_off (dark),
            "post-processing off: a 'bloom' that halves the picture's brightness is not left out", d);
    /* ... and one that leaves the level alone is left out, once that is known */
    guint first = 0, last = 0;
    gboolean dropped_early = FALSE;
    worst = 0;
    for (guint f = 1; f <= 40; f++) {
        if (f <= 32 && post_dropped_off (nil))
            dropped_early = TRUE;
        post_run (nil, in, WALPE_POST_OFF, TRUE, f, pic, &r);
        worst = MAX (worst, max_diff (src, pic));
        if (f <= 32)
            first += r;
        else
            last += r;
    }
    g_snprintf (d, sizeof d, "passes drawn in frames 1..32: %u, in frames 33..40: %u; picture off by %d / 255",
                first, last, worst);
    result (first == 32 && last == 0 && !dropped_early && post_dropped_off (nil) && worst <= 1,
            "post-processing off: a 'bloom' that leaves the level alone runs for 32 frames, then is left out", d);
    /* not on an ordinary layer, and not at the other levels */
    post_run (nil, in, WALPE_POST_REDUCED, TRUE, 50, pic, &r);
    guint r2 = 0;
    post_run (nil, in, WALPE_POST_OFF, FALSE, 51, pic, &r2);
    g_snprintf (d, sizeof d, "passes drawn: reduced %u, off on an ordinary layer %u", r, r2);
    result (r == 1 && r2 == 1, "post-processing: nothing is left out at 'reduced' or on an ordinary layer", d);
    /* a scene that fades in from black: measurements over one colour say
     * nothing, the verdict waits for a picture */
    GLuint black = 0;
    memset (black_px, 0, sizeof black_px);
    for (int i = 3; i < NPX; i += 4)
        black_px[i] = 255;
    glGenTextures (1, &black);
    glBindTexture (GL_TEXTURE_2D, black);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, black_px);
    struct FxSet *fade = build_set ("[" FX ("bloomdark") "]", FALSE);
    for (guint f = 1; f <= 40 && fade; f++)
        post_run (fade, black, WALPE_POST_OFF, TRUE, f, NULL, NULL);
    runs = 0;
    for (guint f = 41; f <= 44 && fade; f++) {
        post_run (fade, in, WALPE_POST_OFF, TRUE, f, pic, &r);
        runs += r;
    }
    g_snprintf (d, sizeof d, "after 40 black frames: %u passes in 4 frames, difference to the full picture %d / 255",
                runs, max_diff (full, pic));
    result (fade && runs == 4 && max_diff (full, pic) == 0,
            "post-processing off: 40 black frames do not make a darkening 'bloom' count as cosmetic", d);
    glDeleteTextures (1, &black);
    fx_free_for_object (dark);
    fx_free_for_object (nil);
    if (fade)
        fx_free_for_object (fade);
}

static void rm_rf (const char *path)
{
    GDir *dir = g_dir_open (path, 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name (dir)) != NULL) {
            char *c = g_build_filename (path, n, NULL);
            if (g_file_test (c, G_FILE_TEST_IS_DIR) && !g_file_test (c, G_FILE_TEST_IS_SYMLINK))
                rm_rf (c);
            else
                g_unlink (c);
            g_free (c);
        }
        g_dir_close (dir);
    }
    g_rmdir (path);
}

static gboolean gl_run (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip GL checks on one EGL display (no usable GL config)\n");
        return FALSE;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip GL checks on one EGL display (no surfaceless GL 3.3 context)\n");
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return FALSE;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    walpe_glutil_init ();
    GError *err = NULL;
    tmpdir = g_dir_make_tmp ("walpe-fxhold-XXXXXX", &err);
    if (!tmpdir) {
        result (FALSE, "temporary project", err ? err->message : NULL);
        return TRUE;
    }
    put_effect ("gain", plain_vert, gain_frag);
    put_effect ("opacity", op_vert, op_frag);
    put_effect ("wv", wv_vert, wv_frag);
    put_effect ("sh", plain_vert, sh_frag);
    put_effect ("pd", plain_vert, pd_frag);
    put_effect ("pm", plain_vert, pm_frag);
    put_effect ("pu", plain_vert, pu_frag);
    fxs_enabled ();
    g_atomic_int_set (&fxs_spec_on, 1);
    /* the specialised programs from the first frame: what a scene runs
     * with once its passes have been compared */
    g_atomic_int_set (&fxs_verify_on, 0);
    GLuint in = smooth_tex ();
    /* "another layer": one pass into a pooled target of the same size */
    struct FxSet *other = build_set ("[" FXC ("gain", "\"gain\":0.2,\"tint\":\"0 1 0\"") "]", FALSE);
    hold_fuse_checks (in, other);
    held_prefix_checks (in, other);
    kept_output_checks (in, other);
    fx_free_for_object (other);
    verify_checks (in);
    constant_checks (in);
    post_level_checks (in);
    g_atomic_int_set (&fxs_quality, 0);
    glDeleteTextures (1, &in);
    fx_targets_release ();
    gint64 tb = 0;
    int tn = 0;
    fx_targets_stats (&tb, &tn);
    char *d = g_strdup_printf ("targets left: %d = %" G_GINT64_FORMAT " bytes", tn, tb);
    result (tn == 0 && tb == 0, "no effect target is left when the sets are freed and the pool is released", d);
    g_free (d);
    rm_rf (tmpdir);
    g_clear_pointer (&tmpdir, g_free);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

int main (void)
{
    /* the checks set every mode themselves */
    g_unsetenv ("WALPE_FX_SHARE");
    g_unsetenv ("WALPE_FX_SPEC");
    g_unsetenv ("WALPE_FX_QUALITY");
    g_unsetenv ("WALPE_FX_SPEC_VERIFY");
    g_unsetenv ("WALPE_FX_CACHE");
    /* the key of this test's own opacity shader, in the engine's slot */
    fxk_test_register (FXK_OPACITY_MAIN, OP_MAIN);
    int ran = 0;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd; i++)
                ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
    if (!ran)
        printf ("skip GL checks (no EGL device / surfaceless platform)\n");
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
