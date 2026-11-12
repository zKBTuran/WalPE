/* fx_hostile_test - the effect shader pipeline against hostile shader text
 * and effect / material JSON (src/engine/scenenode_effects.c, fxclass.c,
 * fx_srcrepair.h, fx_specialize.h, fx_rewrite.h, fx_fuse.h, fx_verify.h).
 *
 *   fx-hostile-test [section [count [first seed]]]
 *
 * 1. No GL ("text"):
 *    - the time scan (fxclass.c) on brackets nested 100 000 deep and on a
 *      product of 60 000 g_Time factors: no stack overflow, back in time;
 *    - #include expansion: a header that includes itself under 400
 *      spellings, a shader file of 64 MiB, includes 8 deep: the expanded
 *      source stays bounded;
 *    - combo names that are no identifiers (line breaks, megabytes of
 *      text) do not reach the source;
 *    - the annotation scans (combos, sampler combos and defaults) walk a
 *      source once and find what the scans they replace found;
 *    - a source beyond the size limit of the source repairs is compiled
 *      as written (megabytes of declarations cost minutes of repairs);
 *    - the displacement recogniser of the fusing on a final statement
 *      whose names come in two parts;
 *    - the size of a statement with its macros expanded (a statement
 *      beyond the limit is not handed to the driver: checked with GL);
 *    - seeded mutations of seven small effects written for this test,
 *      through every text stage
 *      (hoist, includes, combos, prelude, source inspection / repair, time
 *      scan, the scans of the specialiser and of the fusing).
 * 2. With an EGL device or Mesa's surfaceless platform (skipped without),
 *    "gl": the same mutations, plus hostile effect.json / material /
 *    instance values (targets, binds, combos, constants: huge, negative,
 *    NaN, wrong types), written to a temporary project and run through
 *    the public executor API in every mode (one program per pass, shared,
 *    constants baked with and without the comparison, fx_quality fast):
 *    built, drawn at three sizes, freed. A failed build must leave no
 *    chain behind; nothing may crash.
 * The test has no expected pictures: it is run under ASan + UBSan in the
 * sanitizer builds, and its checks are "came back", "bounded" and "the
 * caches are empty afterwards".
 * WALPE_FX_HOSTILE_CORPUS=dir adds *.frag / *.vert pairs (for example a
 * WALPE_FX_DUMP_SRC directory) as further mutation seeds for local runs.
 *
 * The engine file is included so the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#ifndef FX_MAX_SOURCE   /* a tree without the limit: the checks then show what it is for */
#define FX_MAX_SOURCE (4 * 1024 * 1024)
#endif
#include <epoxy/egl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <time.h>

static int fails, checks;

static void result (gboolean ok, const char *what)
{
    checks++;
    printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush (stdout);
    if (!ok)
        fails++;
}

static void quiet_log (const gchar *d, GLogLevelFlags l, const gchar *m, gpointer u)
{
    (void) d; (void) l; (void) m; (void) u;
}

/* ------------------------------------------------------------ generator */

static guint32 rs;
static guint32 rnd (guint32 n)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return n ? rs % n : 0;
}

/* ---- the seed shaders
 * Written for this test; nothing here is taken from an effect file. What
 * the engine knows by name or by statement is kept the way it wants it:
 * the uniform / attribute / varying names of the shader interface, the
 * annotation comments, and the one statement fx_vertex_traits() knows a
 * full-quad pass by (gl_Position = mul(vec4(a_Position, 1.0), ..)).
 *
 * fx_rewrite.h keys its hoists on statements of stock shaders, which the
 * engine knows by key only (fx_keys.h). A seed that is to reach that code
 * carries statements of this test's own (own_keys[] below): seeds_init ()
 * registers their keys in the engine's slots and puts their text where a
 * seed has an @<entry><part>@ marker (F first, L last, N need_f, V need_v,
 * X never_f). They use the names the hoist table works with (the results
 * pulse and offset, flowPhase, v_Bounds, g_Bounds); the arithmetic is this
 * test's. The rewrites without a table (the blur kernels, the ray march,
 * the pass-through of an opacity pass) are not reached by these seeds; a
 * local corpus reaches them (WALPE_FX_HOSTILE_CORPUS). */

#define QUAD "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
#define VS_HEAD \
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
#define TEX0 "uniform sampler2D g_Texture0; // {\"hidden\":true}\n"

#define VERT_PLAIN \
    VS_HEAD "varying vec2 v_TexCoord;\nvoid main() {\n" QUAD "\tv_TexCoord = vec2(a_TexCoord.x, a_TexCoord.y);\n}\n"

/* sway: time in the vertex shader, a header, a combo */
#define VERT_SWAY(n) \
    "#include \"common.h\"\n" \
    "// [COMBO] {\"material\":\"mode\",\"combo\":\"MODE\",\"type\":\"options\",\"default\":1}\n" \
    "uniform float g_Time;\n" \
    "uniform float g_Speed; // {\"material\":\"speed\",\"default\":2,\"range\":[0,10]}\n" \
    "uniform vec2 g_Scale; // {\"material\":\"scale\",\"default\":\"1 1\"}\n" \
    "uniform vec2 g_Bounds; // {\"material\":\"bounds\",\"default\":\"0 1\"}\n" \
    VS_HEAD "varying vec4 v_TexCoord;\nvarying vec2 v_Bounds;\nvoid main() {\n" QUAD \
    "\tv_TexCoord = vec4(a_TexCoord, a_TexCoord * g_Scale + frac(g_Time * g_Speed * 0.1));\n" \
    "\t@" n "V@\n}\n"

#define FRAG_SWAY_HEAD \
    "#include \"common.h\"\n" \
    "// [COMBO] {\"material\":\"mode\",\"combo\":\"MODE\",\"type\":\"options\",\"default\":1}\n" \
    "varying vec4 v_TexCoord;\nvarying vec2 v_Bounds;\nuniform float g_Time;\n" TEX0 \
    "uniform sampler2D g_Texture1; // {\"material\":\"mask\",\"combo\":\"MASK\"}\n" \
    "uniform float g_Speed; // {\"material\":\"speed\",\"default\":2}\n" \
    "uniform float g_Strength; // {\"material\":\"strength\",\"default\":0.1}\n"

static const char *const seed_vert[] = {
    VERT_PLAIN,
    VERT_SWAY ("1"),
    /* fade: a vec4 varying */
    VS_HEAD "varying vec4 v_TexCoord;\nvoid main() {\n" QUAD "\tv_TexCoord = vec4(a_TexCoord, 0.0, 1.0);\n}\n",
    /* smear: an array of coordinates, filled by a loop */
    VS_HEAD "uniform vec4 g_Texture0Resolution;\n"
    "uniform vec2 g_Dir; // {\"material\":\"scale\",\"default\":\"1 0\"}\n"
    "varying vec2 v_TexCoord[5];\nvoid main() {\n" QUAD
    "\tvec2 hop = g_Dir / g_Texture0Resolution.xy * 1.5;\n"
    "\tfor (int k = 0; k < 5; k++)\n\t\tv_TexCoord[k] = a_TexCoord + hop * (float(k) - 2.0);\n}\n",
    VERT_PLAIN,
    VERT_SWAY ("2"),
    VERT_PLAIN,
};

static const char *const seed_frag[] = {
    /* levels: annotated constants, the prelude's CAST3 */
    "varying vec2 v_TexCoord;\n" TEX0
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"1 0.9 0.8\"}\n"
    "uniform float g_Exp; // {\"material\":\"exponent\",\"default\":1}\n"
    "void main() {\n\tvec4 px = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tvec3 lifted = pow(max(px.rgb * g_Gain, CAST3(0.0)), CAST3(g_Exp));\n"
    "\tpx.rgb = (lifted * 0.6 + px.rgb * 0.4) * g_Tint;\n\tgl_FragColor = px;\n}\n",
    /* sway: a displacement (the family fx_fuse.h fuses) with a header, two
     * combos and a phase that depends on time only; hoist entry 1 */
    FRAG_SWAY_HEAD
    "void main() {\n\t@1N@\n#if MASK\n\t@1X@(g_Texture1, v_TexCoord.zw).g;\n#endif\n"
    "\tfloat offset;\n\t@1F@\n\toffset = 0.5 - 0.5 * cos(time * 1.7);\n\t@1L@\n"
    "\tvec2 moved = v_TexCoord.xy + helper(offset) * g_Strength;\n#if MODE == 1\n\tmoved.y += 0.01;\n#endif\n"
    "\tgl_FragColor = texSample2D(g_Texture0, moved);\n}\n",
    /* fade */
    "varying vec4 v_TexCoord;\n" TEX0
    "uniform float g_UserAlpha; // {\"material\":\"alpha\",\"default\":1}\n"
    "void main() {\n\tvec4 px = texSample2D(g_Texture0, v_TexCoord.xy);\n"
    "\tgl_FragColor = vec4(px.rgb, px.a * g_UserAlpha * v_TexCoord.w);\n}\n",
    /* smear: five taps, the weights as constants */
    "varying vec2 v_TexCoord[5];\n" TEX0
    "void main() {\n\tconst float w0 = 0.34, w1 = 0.23, w2 = 0.1;\n"
    "\tvec4 sum = texSample2D(g_Texture0, v_TexCoord[2]) * w0;\n"
    "\tsum += (texSample2D(g_Texture0, v_TexCoord[1]) + texSample2D(g_Texture0, v_TexCoord[3])) * w1;\n"
    "\tsum += (texSample2D(g_Texture0, v_TexCoord[0]) + texSample2D(g_Texture0, v_TexCoord[4])) * w2;\n"
    "\tgl_FragColor = sum;\n}\n",
    /* throb: a glow whose strength depends on time only; hoist entry 0 */
    "varying vec2 v_TexCoord;\nuniform float g_Time;\n" TEX0
    "uniform float g_Beat; // {\"material\":\"pulsespeed\",\"default\":3}\n"
    "uniform float g_Curve; // {\"material\":\"power\",\"default\":1}\n"
    "uniform vec2 g_Gate; // {\"material\":\"bounds\",\"default\":\"0 1\"}\n"
    "uniform vec3 g_Glow; // {\"material\":\"tint\",\"type\":\"color\",\"default\":\"1 0.5 0.25\"}\n"
    "void main() {\n\tvec4 px = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tfloat @0F@g_Time * g_Beat + 0.4), 0.0, 1.0);\n\t@0L@\n"
    "\tgl_FragColor = vec4(px.rgb + g_Glow * (pulse * px.a * 0.6), px.a);\n}\n",
    /* sway, the other spelling fx_rewrite.h knows; hoist entry 2 */
    FRAG_SWAY_HEAD
    "void main() {\n\t@2N@\n#if MASK\n\t@2X@(g_Texture1, v_TexCoord.zw).b;\n#endif\n"
    "\tfloat offset;\n\t@2F@0.9, -0.31, 0.07, 0.002));\n"
    "\toffset = dot(sin(sines * 6.2831853), vec4(0.4, 0.3, 0.2, 0.1)) * 0.5 + 0.5;\n\t@2L@\n"
    "\tvec2 moved = v_TexCoord.xy + vec2(offset - 0.5, 0.0) * g_Strength;\n"
    "\tgl_FragColor = texSample2D(g_Texture0, moved);\n}\n",
    /* streak: a loop with a constant bound around texture reads, time in
     * the coordinate */
    "varying vec2 v_TexCoord;\nuniform float g_Time;\n" TEX0
    "uniform float g_Reach; // {\"material\":\"strength\",\"default\":0.02}\n"
    "uniform float g_Turn; // {\"material\":\"speed\",\"default\":0.5}\n"
    "void main() {\n\tconst int STEPS = 6;\n"
    "\tvec2 dir = vec2(cos(g_Time * g_Turn), sin(g_Time * g_Turn)) * g_Reach;\n\tvec4 acc = vec4(0.0);\n"
    "\tfor (int i = 0; i < STEPS; i++)\n"
    "\t\tacc += texSample2D(g_Texture0, v_TexCoord + dir * (float(i) / float(STEPS - 1) - 0.5));\n"
    "\tgl_FragColor = acc / float(STEPS);\n}\n",
};
#define N_TPL G_N_ELEMENTS (seed_frag)
/* the hoist entry a seed carries the statements of, -1: none */
static const int seed_hoist[N_TPL] = { -1, 1, -1, -1, 0, 2, -1 };

/* the seeds with the markers filled in, and this test's key statements
 * as snippets for the mutations */
static char *tpl_vert[N_TPL], *tpl_frag[N_TPL];
static GPtrArray *rt_snips;

/* this test's statements for the slots the hoist table names (fx_keys.h) */
static const struct { FxKeyId id; const char *text; } own_keys[] = {
    { FXK_PULSE_FIRST, "pulse = clamp(g_Gate.x + g_Gate.y * sin(" },
    { FXK_PULSE_LAST, "pulse = pulse * pulse * g_Curve;" },
    { FXK_SHAKE_FIRST_A, "float time = flowPhase + g_Time * g_Speed * 0.5;" },
    { FXK_SHAKE_FIRST_B, "vec4 sines = fract(vec4(flowPhase) + g_Time * g_Speed * vec4(" },
    { FXK_SHAKE_LAST, "offset = clamp(v_Bounds.y * (offset - v_Bounds.x), 0.0, 1.0);" },
    { FXK_SHAKE_PHASE0, "float flowPhase = float(0);" },
    { FXK_SHAKE_VBOUNDS, "v_Bounds = vec2(g_Bounds.x, 1.0 / (g_Bounds.y - g_Bounds.x));" },
    { FXK_SHAKE_PHASEMAP, "flowPhase += texSample2D" },
};

static const char *own_text (FxKeyId id)
{
    for (guint i = 0; i < G_N_ELEMENTS (own_keys); i++)
        if (own_keys[i].id == id)
            return own_keys[i].text;
    return "";
}

static char *fill (const char *src)
{
    GString *s = g_string_new (src);
    for (guint i = 0; i < 3; i++) {
        const FxsHoist *h = i < G_N_ELEMENTS (fxs_hoists) ? &fxs_hoists[i] : NULL;
        const FxKeyId part[5] = { h ? h->first : FXK_NONE, h ? h->last : FXK_NONE, h ? h->need_f : FXK_NONE,
                                  h ? h->need_v : FXK_NONE, h ? h->never_f : FXK_NONE };
        for (guint k = 0; k < 5; k++) {
            char mark[8];
            g_snprintf (mark, sizeof (mark), "@%u%c@", i, "FLNVX"[k]);
            const char *text = own_text (part[k]);
            for (char *at; (at = strstr (s->str, mark)) != NULL;) {
                gssize pos = at - s->str;
                g_string_erase (s, pos, (gssize) strlen (mark));
                g_string_insert (s, pos, text);
            }
        }
    }
    return g_string_free (s, FALSE);
}

static void seeds_init (void)
{
    rt_snips = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < G_N_ELEMENTS (own_keys); i++) {
        fxk_test_register (own_keys[i].id, own_keys[i].text);
        g_ptr_array_add (rt_snips, g_strdup_printf ("\t%s\n", own_keys[i].text));
    }
    for (guint i = 0; i < G_N_ELEMENTS (fxs_hoists); i++)
        for (guint k = 0; k < G_N_ELEMENTS (fxs_hoists[i].subst) && fxs_hoists[i].subst[k]; k++)
            g_ptr_array_add (rt_snips, g_strdup_printf ("\t%s\n", fxs_hoists[i].subst[k]));
    for (guint k = 0; k < N_TPL; k++) {
        tpl_vert[k] = fill (seed_vert[k]);
        tpl_frag[k] = fill (seed_frag[k]);
    }
}

static void seeds_free (void)
{
    for (guint k = 0; k < N_TPL; k++) {
        g_free (tpl_vert[k]);
        g_free (tpl_frag[k]);
    }
    g_ptr_array_unref (rt_snips);
}

static const char *const snippets[] = {
    "#if 1 / 0\n", "#if -9223372036854775808 / -1\n", "#if 1 % 0\n", "#if (1 << 99)\n", "#if 2147483648 * 2147483648 * 4\n",
    "#if defined(\n", "#if ((((((((((((((((((((1))))))))))))))))))))\n", "#elif 1\n", "#else\n", "#endif\n", "#endif\n#endif\n",
    "#ifdef\n", "#ifndef X\n", "#if X == Y\n#define X Y\n#define Y X\n", "#define A A\n", "#define A B\n#define B A\n",
    "#define F(x) F(x) F(x)\n", "#define G(x,y) G(y,x) G(x,y) G(x,x)\nfloat q = G(1,2);\n", "#define H(x) x x x x x x x x\n",
    "#define E0 1\n#define E1 E0 E0 E0 E0\n#define E2 E1 E1 E1 E1\n#define E3 E2 E2 E2 E2\n#define E4 E3 E3 E3 E3\n"
    "#define E5 E4 E4 E4 E4\n#define E6 E5 E5 E5 E5\n#define E7 E6 E6 E6 E6\n#define E8 E7 E7 E7 E7\nfloat e = E8;\n",
    "#define M(\n", "#define M(a,\n", "#define\n", "#undef\n", "#define M(a) a ## a\n", "#define M(a) #a\n", "M(M(M(M(1))))\n",
    "#include \"common.h\"\n", "#include \"self.h\"\n", "#include \"../../../../../../etc/passwd\"\n", "#include \"\"\n",
    "#include \"\n", "#include <common.h>\n", "#include\n", "#require LightingV1\n", "#require\n", "#line 2147483647\n",
    "#line -5\n", "#version 100\n", "#extension all : enable\n", "#pragma optimize(off)\n", "#error x\n", "#\n", "# if 1\n",
    "// [COMBO] {\"material\":\"m\",\"combo\":\"MASK\",\"type\":\"options\",\"default\":2147483648}\n",
    "// [COMBO] {\"combo\":\"A B\\n#define main x\",\"default\":1e99}\n", "// [COMBO] {\"combo\":\"\",\"default\":\"x\"}\n",
    "// [COMBO] {{{{{{{{{{{{{{{{{{{{\n", "// [COMBO] }\n", "// [COMBO] {\"combo\":7,\"default\":null}\n", "// [COMBO] {\"combo\":\"C\",\"default\":-2147483649}\n",
    "uniform float g_X; // {\"material\":\"x\",\"default\":\"nan inf -inf 1e999\"}\n",
    "uniform int g_N; // {\"material\":\"n\",\"default\":4294967296}\n", "uniform int g_N2; // {\"material\":\"n2\",\"default\":\"1e30\"}\n",
    "uniform bool g_B; // {\"material\":\"b\",\"default\":\"2147483648\"}\n",
    "uniform vec4 g_V; // {\"material\":\"v\",\"default\":\"1 2 3 4 5 6 7 8 9\"}\n", "uniform vec3 g_C; // {\"material\":\"c\",\"default\":{\"a\":{\"b\":{}}}}\n",
    "uniform float g_Y; // {\"material\":\"y\",\"default\":1,\"require\":{\"MODE\":{\"x\":[1,{}]}}}\n", "uniform float g_Z; // {\n",
    "uniform float g_Z2; // {\"material\":\"\\u0000\",\"default\":\"\"}\n", "uniform float g_Arr[3]; // {\"material\":\"arr\",\"default\":\"1 2 3\"}\n",
    "uniform float g_Big[100000];\n", "uniform float g_Neg[-1];\n", "uniform float g_Zero[0];\n", "uniform sampler2D g_Texture9;\n",
    "uniform sampler2D g_Texture7; // {\"material\":\"t7\",\"combo\":\"T7\",\"default\":\"util/black\"}\n",
    "uniform sampler2D g_Texture2; // {\"default\":\"_rt_FullFrameBuffer\"}\n", "uniform sampler2D g_Texture3; // {\"default\":\"_rt_imageLayerComposite_-1_a\"}\n",
    "uniform sampler2D g_Texture4; // {\"default\":\"_alias_lightCookie\"}\n", "uniform sampler2D g_Texture5; // {\"default\":\"../../../x\"}\n",
    "uniform vec4 g_Texture0Resolution;\n", "uniform vec4 g_Texture7Resolution;\n", "uniform float g_Time;\n", "uniform vec2 g_Time;\n",
    "uniform float g_AudioSpectrum16Left[16];\n", "uniform float g_AudioSpectrum64Right[1];\n", "uniform float g_AudioSpectrum32Left[4096];\n",
    "uniform float g_AudioSpectrum99Left[3];\n", "uniform vec2 g_PointerPosition;\n", "uniform vec3 g_Screen;\n", "uniform float g_Daytime;\n",
    "uniform mat4 g_ModelViewProjectionMatrixInverse;\n", "uniform mat3 g_EffectTextureProjectionMatrix;\n", "uniform mat4 g_Mats[40];\n",
    "uniform vec4 g_Color4;\n", "uniform float g_UserAlpha;\n", "uniform float g_UserAlpha; // {\"material\":\"alpha\",\"default\":1}\n",
    "uniform vec2 g_TexelSize;\n", "uniform vec2 g_TexelSizeHalf;\n", "uniform float g_Frametime;\n", "uniform vec4 g_Texture0Rotation;\n",
    "uniform float " "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; // {\"material\":\"long\",\"default\":2}\n",
    "varying vec2 v_TexCoord;\n", "varying vec4 v_TexCoord;\n", "varying float v_Extra[64];\n", "attribute vec4 a_Position;\n", "attribute vec3 a_Color;\n",
    "void main() { }\n", "void main()\n", "void main() {\n", "}\n", "{\n", "((((((((((((((((((((((((((((((((\n", "))))))))))))))))))))))))))))))))\n",
    "\tgl_FragColor = vec4(g_Time * ((((((((((((((((((((((((((((((((1.0)))))))))))))))))))))))))))))))));\n",
    "\tfloat t = g_Time * g_Time * g_Time * g_Time / g_Time / 0.0 * -g_Time;\n", "\tfloat t2 = frac(g_Time * vec4(1, 2, 3, 4)).x;\n",
    "\tfloat t3 = mod(g_Time, 0.0);\n", "\tfloat t4 = (g_Time + 1.0) * (g_Time);\n", "\tfloat t5 = unknown(g_Time);\n", "g_Time\n", "g_Time *\n", "* g_Time\n",
    "\tint i = 7 % 0;\n", "\tfloat m = 5.0 % 2.0;\n", "\tvec3 v3 = 1.0;\n", "\tvec2 v2 = vec4(1.0);\n", "\tfloat f = vec3(1.0);\n", "\tbool b = 1.0 + true;\n",
    "\tuint u = 3;\n\tint iu = u + 1;\n", "\tfloat r = true ? 1 : 2.0;\n", "\tconst float cf = g_Time;\n", "\treturn 1.0;\n", "\tv_TexCoord.y += 1.0;\n",
    "\tfloat p = pow(-1.0, g_Exp) + pow(g_Gain, pow(g_Exp, pow(g_Gain, g_Exp)));\n", "\tfloat span = g_Bounds.y - g_Bounds.x, d0 = 1.0 / span;\n",
    "\tvec4 tx = texSample2D(g_Texture0, v_TexCoord, 1.0, 2.0) + texSample2D(g_Texture0);\n", "\tvec4 tl = texSample2DLod(g_Texture0, v_TexCoord.xy, 99999.0);\n",
    "\tfor (int k = 0; k < 4; k++) { gl_FragColor += texSample2D(g_Texture0, v_TexCoord.xy + float(k)); }\n",
    "\tif (g_Time > 1e38) discard;\n", "\tclip(-1.0);\n", "\tpx.a *= g_UserAlpha;\n", "\tgl_FragColor = px;\n",
    "/*", "*/", "/* /* */", "//\\\n", "\\\n", "\r\n", "\r", "\f", "\v", "\t\t\t\t\t\t\t\t", "\x01", "\x7f", "\xff\xfe", "\xc3", "\xe3\x81\x82",
    "\"", "'", "`", "$", "@", "?", ":", ";", ";;;;;;;;", ",", ".", "..", "...", "->", "::", "<<", ">>=", "==", "!", "~", "^", "&&", "||",
    "1e999", "-1e999", "0x7fffffff", "0xffffffffffffffffff", "4294967296", "-2147483649", "1.#INF", "nan", "inf", "0.0.0", "1e", "1e+", ".e1", "0b1", "1u", "1f", "1.0lf",
    "int", "float", "void", "return", "struct S { float a; };\n", "struct {\n", "layout(location = 9999) out vec4 o;\n", "precision highp float;\n",
    "in vec2 x;\n", "out vec4 y;\n", "flat in float _we_hoist0;\n", "float _we_hoist0;\n", "#define _we_hoist0 1\n", "vec2 _wf0(vec2 _wpx) { return _wpx; }\n",
    "#define texSample2D\n", "#define main main2\n", "#define g_Texture0 g_Texture1\n", "#define gl_FragColor x\n", "#undef GLSL\n", "#define GLSL 1\n", "#line 1\n",
    "#define varying out\n#define attribute in\n", "void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n", "#version 330\n",
};

static const char *snip (void)
{
    guint k = rnd ((guint32) (G_N_ELEMENTS (snippets) + rt_snips->len));
    return k < G_N_ELEMENTS (snippets) ? snippets[k] : rt_snips->pdata[k - G_N_ELEMENTS (snippets)];
}

static char *long_line (int kind, int n)
{
    GString *s = g_string_new (NULL);
    switch (kind) {
    case 0:
        g_string_append (s, "\tfloat ll = g_Time * ");
        for (int i = 0; i < n; i++)
            g_string_append_c (s, '(');
        g_string_append (s, "1.0");
        for (int i = 0; i < n; i++)
            g_string_append_c (s, ')');
        g_string_append (s, ";\n");
        break;
    case 1:
        g_string_append (s, "\tfloat lp = 1.0");
        for (int i = 0; i < n; i++)
            g_string_append (s, " * g_Time");
        g_string_append (s, ";\n");
        break;
    case 2:
        g_string_append (s, "\tfloat ls = 0.0");
        for (int i = 0; i < n; i++)
            g_string_append_printf (s, " + a%d.x[%d]", i, i);
        g_string_append (s, ";\n");
        break;
    case 3:
        for (int i = 0; i < n; i++)
            g_string_append (s, "{ ");
        g_string_append_c (s, '\n');
        break;
    case 4:
        g_string_append (s, "\tfloat lq = ");
        for (int i = 0; i < n; i++)
            g_string_append (s, "true ? 1.0 : ");
        g_string_append (s, "2.0;\n");
        break;
    case 5:
        for (int i = 0; i < n; i++)
            g_string_append_printf (s, "#if X%d\n", i);
        break;
    case 6:
        g_string_append (s, "\tfloat lf = ");
        for (int i = 0; i < n; i++)
            g_string_append (s, "sin(");
        g_string_append (s, "g_Time");
        for (int i = 0; i < n; i++)
            g_string_append_c (s, ')');
        g_string_append (s, ";\n");
        break;
    default:
        g_string_append (s, "\tfloat lv = ");
        for (int i = 0; i < n; i++)
            g_string_append (s, "vec4(vec3(vec2(");
        g_string_append (s, "1.0");
        for (int i = 0; i < n; i++)
            g_string_append (s, ")))");
        g_string_append (s, ".x;\n");
        break;
    }
    return g_string_free (s, FALSE);
}

/* declarations that compile: what they carry is in the annotation */
static const char *const decls[] = {
    "uniform float g_Xa; // {\"material\":\"speed\",\"default\":\"nan\"}\n",
    "uniform float g_Xb; // {\"material\":\"gain\",\"default\":\"1e999\"}\n",
    "uniform int g_Xc; // {\"material\":\"n\",\"default\":4294967296}\n",
    "uniform int g_Xd; // {\"material\":\"n2\",\"default\":\"-1e30\"}\n",
    "uniform bool g_Xe; // {\"material\":\"b\",\"default\":\"1e30\"}\n",
    "uniform vec4 g_Xf; // {\"material\":\"tint\",\"default\":\"1 2 3 4 5 6 7 8 9\"}\n",
    "uniform vec3 g_Xg; // {\"material\":\"scale\",\"default\":{\"a\":{\"b\":{}}}}\n",
    "uniform vec2 g_Xh; // {\"material\":\"bounds\",\"default\":\"0 0\",\"require\":{\"MODE\":{\"x\":[1,{}]}}}\n",
    "uniform float g_Xi; // {\"material\":\"alpha\"}\n",
    "uniform float g_Xj[3]; // {\"material\":\"power\",\"default\":\"1 2 3\"}\n",
    "uniform float g_Xk; // {\"material\":\"exponent\",\"default\":-0.0}\n",
    "uniform float g_UserAlpha; // {\"material\":\"alpha\",\"default\":1}\n",
    "uniform sampler2D g_Texture2; // {\"material\":\"t2\",\"combo\":\"T2\",\"default\":\"util/white\"}\n",
    "uniform sampler2D g_Texture7; // {\"default\":\"_rt_FullFrameBuffer\"}\n",
    "uniform sampler2D g_Texture3; // {\"default\":\"_rt_imageLayerComposite_-1_a\"}\n",
    "uniform vec4 g_Texture2Resolution;\n", "uniform vec4 g_Texture7Resolution;\n", "uniform vec2 g_TexelSize;\n",
    "uniform float g_AudioSpectrum16Left[16];\n", "uniform float g_AudioSpectrum64Right[64];\n",
    "uniform vec2 g_PointerPosition;\n", "uniform float g_Daytime;\n", "uniform float g_Frametime;\n",
    "uniform mat4 g_ModelViewProjectionMatrixInverse;\n", "uniform mat3 g_EffectTextureProjectionMatrix;\n",
    "// [COMBO] {\"material\":\"m\",\"combo\":\"MODE\",\"type\":\"options\",\"default\":2147483648}\n",
    "// [COMBO] {\"combo\":\"EXTRA\",\"default\":-1e99}\n",
    "#if MODE == 2147483647\nuniform float g_Dead; // {\"material\":\"gain\",\"default\":5}\n#endif\n",
    "#define HELPER2(x) ((x) * (x))\n", "float unused_fn(float x) { return pow(x, 2.0) / (x - x); }\n",
};

/* uses of the declarations above, in front of the last statement of main */
static const char *const uses[] = {
    "\tgl_FragColor.r += g_Xa * 0.0;\n", "\tgl_FragColor.g *= g_Xb > 0.0 ? 1.0 : 1.0;\n", "\tgl_FragColor.b += float(g_Xc) * 0.0;\n",
    "\tif (g_Xe) gl_FragColor.a *= 1.0;\n", "\tgl_FragColor += g_Xf * 0.0;\n", "\tgl_FragColor.rg += g_Xh * 0.0;\n",
    "\tgl_FragColor.r += pow(abs(g_Xi), g_Xk) * 0.0;\n", "\tgl_FragColor.r += g_Xj[2] * 0.0;\n",
    "\tgl_FragColor += texSample2D(g_Texture2, vec2(0.5)) * 0.0;\n", "\tgl_FragColor += texSample2D(g_Texture7, vec2(0.5)) * 0.0;\n",
    "\tgl_FragColor.r += g_AudioSpectrum16Left[3] * 0.0 + g_Daytime * 0.0 + g_PointerPosition.x * 0.0;\n",
    "\tgl_FragColor.r += g_Texture2Resolution.x * 0.0 + g_TexelSize.x * 0.0;\n",
    "\tgl_FragColor.r += sin(g_Time * g_Xa * 3.0) * 0.0;\n",
};

static char *gentle (const char *src, gboolean frag)
{
    GString *s = g_string_new (src);
    int n = 1 + (int) rnd (5);
    for (int i = 0; i < n; i++)
        g_string_prepend (s, decls[rnd (G_N_ELEMENTS (decls))]);
    if (frag) {
        /* the declarations in use: the uniforms are active, the constants baked */
        const char *end = strrchr (s->str, '}');
        gsize at = end ? (gsize) (end - s->str) : s->len;
        for (guint i = 0; i < G_N_ELEMENTS (decls) && i < 12; i++)
            if (!strstr (s->str, decls[i]) && decls[i][0] == 'u')
                g_string_prepend (s, decls[i]), at += strlen (decls[i]);
        if (!strstr (s->str, "uniform float g_Time;"))
            g_string_prepend (s, "uniform float g_Time;\n"), at += 22;
        n = 1 + (int) rnd (6);
        for (int i = 0; i < n; i++)
            g_string_insert (s, (gssize) at, uses[rnd (G_N_ELEMENTS (uses))]);
    }
    return g_string_free (s, FALSE);
}

/* a line start of s, by chance */
static gsize line_at (const char *s)
{
    gsize n = strlen (s), at = n ? rnd ((guint32) n) : 0;
    while (at > 0 && s[at - 1] != '\n')
        at--;
    return at;
}

static char *mutate (const char *src, int rounds)
{
    GString *s = g_string_new (src);
    for (int r = 0; r < rounds; r++) {
        guint op = rnd (16);
        if (op < 6) {
            const char *sn = snip ();
            g_string_insert (s, (gssize) (rnd (3) ? line_at (s->str) : rnd ((guint32) s->len + 1)), sn);
        } else if (op < 8 && s->len > 2) {
            /* a token gives way to a snippet */
            gsize a = rnd ((guint32) s->len), b = a;
            while (b < s->len && (g_ascii_isalnum (s->str[b]) || s->str[b] == '_' || s->str[b] == '.'))
                b++;
            if (b == a)
                b = a + 1;
            g_string_erase (s, (gssize) a, (gssize) (b - a));
            g_string_insert (s, (gssize) a, snip ());
        } else if (op == 8 && s->len > 1) {
            g_string_truncate (s, rnd ((guint32) s->len));
        } else if (op == 9 && s->len > 1) {
            gsize a = rnd ((guint32) s->len), l = MIN (rnd (40) + 1, s->len - a);
            g_string_erase (s, (gssize) a, (gssize) l);
        } else if (op == 10 && s->len > 1) {
            /* one line many times */
            gsize a = line_at (s->str), b = a;
            while (b < s->len && s->str[b] != '\n')
                b++;
            char *line = g_strndup (s->str + a, b - a + 1);
            int times = 1 << rnd (9);
            for (int i = 0; i < times && s->len < 400000; i++)
                g_string_insert (s, (gssize) a, line);
            g_free (line);
        } else if (op == 11) {
            char *l = long_line ((int) rnd (8), 1 << (4 + rnd (9)));
            g_string_insert (s, (gssize) line_at (s->str), l);
            g_free (l);
        } else if (op == 12 && s->len > 1) {
            s->str[rnd ((guint32) s->len)] = (char) (1 + rnd (255));
        } else if (op == 13 && s->len > 1) {
            /* two spans trade places */
            gsize a = rnd ((guint32) s->len), b = rnd ((guint32) s->len);
            char c = s->str[a];
            s->str[a] = s->str[b];
            s->str[b] = c;
        } else if (op == 14) {
            g_string_append (s, snip ());
        } else {
            g_string_prepend (s, snip ());
        }
    }
    return g_string_free (s, FALSE);
}

/* optional external corpus: *.frag with the *.vert of the same name */
static GPtrArray *corpus_v, *corpus_f;

static void load_corpus (void)
{
    const char *dir = g_getenv ("WALPE_FX_HOSTILE_CORPUS");
    corpus_v = g_ptr_array_new_with_free_func (g_free);
    corpus_f = g_ptr_array_new_with_free_func (g_free);
    GDir *d = dir && *dir ? g_dir_open (dir, 0, NULL) : NULL;
    if (!d)
        return;
    const char *n;
    while ((n = g_dir_read_name (d)) != NULL) {
        if (!g_str_has_suffix (n, ".frag"))
            continue;
        char *fp = g_build_filename (dir, n, NULL);
        char *vp = g_strdup (fp);
        memcpy (vp + strlen (vp) - 4, "vert", 4);
        char *f = NULL, *v = NULL;
        if (g_file_get_contents (fp, &f, NULL, NULL) && g_file_get_contents (vp, &v, NULL, NULL) &&
            strlen (f) < 200000 && strlen (v) < 200000) {
            /* dumps are complete sources: the body is what follows "#line 1" */
            const char *fb = strstr (f, "#line 1\n"), *vb = strstr (v, "#line 1\n");
            g_ptr_array_add (corpus_f, g_strdup (fb ? fb + 8 : f));
            g_ptr_array_add (corpus_v, g_strdup (vb ? vb + 8 : v));
        }
        g_free (f);
        g_free (v);
        g_free (fp);
        g_free (vp);
    }
    g_dir_close (d);
    printf ("corpus: %u shader pairs from %s\n", corpus_f->len, dir);
}

static void pick (const char **v, const char **f)
{
    guint n = N_TPL + corpus_f->len, k = rnd (n);
    if (k < N_TPL) {
        *v = tpl_vert[k];
        *f = tpl_frag[k];
    } else {
        *v = corpus_v->pdata[k - N_TPL];
        *f = corpus_f->pdata[k - N_TPL];
    }
}

/* ---------------------------------------------------------------- files */

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

static const char *COMMON_H =
    "#define M_PI_2 (2.0 * 3.14159265)\n"
    "vec2 helper(float x) { return vec2(x, x * 0.5); }\n";

/* --------------------------------------------------------------- no GL */

static WalpeFxTime scan (const char *src)
{
    WalpeFxTime t = { 0 };
    walpe_fx_time_scan (src, NULL, NULL, &t);
    return t;
}

static void time_scan_checks (void)
{
    /* brackets 100 000 deep behind a g_Time: factor () / product () went
     * one pair of C stack frames down per bracket */
    char *l = long_line (0, 100000);
    char *src = g_strconcat ("uniform float g_Time;\nvoid main() {\n", l, "}\n", NULL);
    gint64 t0 = g_get_monotonic_time ();
    scan (src);
    double dt = (g_get_monotonic_time () - t0) / 1e6;
    result (dt < 20.0, "time scan: brackets nested 100 000 deep (no stack overflow, back in time)");
    g_free (src);
    g_free (l);
    /* the same through vecN ( ... ) arguments and calls */
    l = long_line (7, 30000);
    char *l2 = long_line (6, 60000);
    src = g_strconcat ("void main() {\n\tfloat a = g_Time * ", l + 13, "\tfloat b = 2.0 * ", l2 + 12, "}\n", NULL);
    t0 = g_get_monotonic_time ();
    scan (src);
    dt = (g_get_monotonic_time () - t0) / 1e6;
    result (dt < 20.0, "time scan: constructors and calls nested 30 000 / 60 000 deep");
    g_free (src);
    g_free (l);
    g_free (l2);
    /* one product of 60 000 g_Time: every g_Time used to walk the whole
     * expression (minutes for a shader of a few hundred KiB) */
    l = long_line (1, 60000);
    src = g_strconcat ("void main() {\n", l, "}\n", NULL);
    t0 = g_get_monotonic_time ();
    WalpeFxTime t = scan (src);
    dt = (g_get_monotonic_time () - t0) / 1e6;
    char *m = g_strdup_printf ("time scan: a product of 60 000 g_Time factors is back in time (%.2f s, "
                               "uses_time %d derived %d)", dt, t.uses_time, t.derived);
    result (dt < 10.0 && t.uses_time, m);
    g_free (m);
    g_free (src);
    g_free (l);
    {
        /* the source repairs on the same kind of text: lines of 4 000
         * nested brackets (just under the 8192 characters a line may have
         * to be repaired at all). Their cost per line is quadratic in its
         * length; the line limit and FX_MAX_SOURCE bound the total. */
        GString *fb = g_string_new ("varying vec2 v_TexCoord;\nuniform float g_Time;\nuniform sampler2D g_Texture0;\n"
                                    "void main() {\n");
        char *ln = long_line (0, 4000), *ln2 = long_line (4, 1500), *ln3 = long_line (7, 600);
        for (int i = 0; i < 4; i++) {
            g_string_append (fb, ln);
            g_string_append (fb, ln2);
            g_string_append (fb, ln3);
        }
        g_string_append (fb, "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord) * ll;\n}\n");
        GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        char *vsrc = fx_build_source (tpl_vert[0], combos, TRUE), *fsrc = fx_build_source (fb->str, combos, FALSE);
        g_hash_table_unref (combos);
        t0 = g_get_monotonic_time ();
        fx_prepass (&vsrc, &fsrc, "hostile");
        dt = (g_get_monotonic_time () - t0) / 1e6;
        m = g_strdup_printf ("source repairs: 12 lines of deeply nested brackets, %u bytes, back in %.2f s",
                             (guint) fb->len, dt);
        result (dt < 120.0, m);
        g_free (m);
        g_free (vsrc);
        g_free (fsrc);
        g_free (ln); g_free (ln2); g_free (ln3);
        g_string_free (fb, TRUE);
    }
    /* what the scan is for still holds */
    t = scan ("uniform float g_Time;\nvoid main() { float a = sin(g_Time * 3.0 * 0.5); }\n");
    result (t.uses_time && t.derived && fabs (t.rate - 1.5) < 1e-9, "time scan: g_Time * 3.0 * 0.5 has rate 1.5");
    t = scan ("void main() { float a = frac((g_Time + 2.0) * ((4.0))); }\n");
    result (t.uses_time && t.derived && fabs (t.rate - 4.0) < 1e-9, "time scan: (g_Time + 2.0) * ((4.0)) has rate 4");
}

static char *expand (const char *body, const char *sdir)
{
    GString *o = g_string_new (NULL);
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_expand_into (o, body, sdir, tmpdir, seen, 0);
    g_hash_table_unref (seen);
    return g_string_free (o, FALSE);
}

static void include_checks (void)
{
    char *sdir = g_build_filename (tmpdir, "shaders", NULL);
    /* bomb.h: 64 KiB of text and 400 lines that include it again, each
     * under another spelling of its name ("./bomb.h", ".//bomb.h", ...):
     * "once per path" did not see them as one file - 400 * 400 copies */
    GString *b = g_string_new (NULL);
    for (int i = 0; i < 400; i++) {
        g_string_append (b, "#include \".");
        for (int k = 0; k <= i; k++)
            g_string_append_c (b, '/');
        g_string_append (b, "bomb.h\"\n");
    }
    for (int i = 0; i < 1024; i++)
        g_string_append (b, "// 0123456789012345678901234567890123456789012345678901234567\n");
    put ("shaders/bomb.h", b->str);
    gsize one = b->len;
    g_string_free (b, TRUE);
    char *o = expand ("#include \"bomb.h\"\nvoid main() {}\n", sdir);
    gsize len = strlen (o);
    char *m = g_strdup_printf ("includes: one header under 400 spellings is expanded once "
                               "(%" G_GSIZE_FORMAT " bytes from a header of %" G_GSIZE_FORMAT ")", len, one);
    result (len < 3 * one, m);
    g_free (m);
    g_free (o);
    /* many different headers, each large: the total stays bounded */
    GString *big = g_string_new (NULL);
    while (big->len < 900 * 1024)
        g_string_append (big, "// 0123456789012345678901234567890123456789012345678901234567\n");
    GString *top = g_string_new (NULL);
    for (int i = 0; i < 40; i++) {
        char *n = g_strdup_printf ("shaders/big%d.h", i);
        put (n, big->str);
        g_free (n);
        g_string_append_printf (top, "#include \"big%d.h\"\n", i);
    }
    g_string_append (top, "void main() {}\n");
    o = expand (top->str, sdir);
    len = strlen (o);
    m = g_strdup_printf ("includes: 40 headers of 900 KiB stop at the size limit (%" G_GSIZE_FORMAT " bytes)", len);
    result (len <= FX_MAX_SOURCE + 1024 * 1024, m);
    g_free (m);
    g_free (o);
    g_string_free (top, TRUE);
    g_string_free (big, TRUE);
    /* a shader file far beyond any shader is not read at all */
    char *huge = g_build_filename (tmpdir, "shaders", "huge.frag", NULL);
    FILE *fp = fopen (huge, "wb");
    if (fp) {
        static char blk[65536];
        memset (blk, ' ', sizeof (blk));
        blk[sizeof (blk) - 1] = '\n';
        for (int i = 0; i < 1024; i++)
            fwrite (blk, 1, sizeof (blk), fp);
        fclose (fp);
    }
    char *r = fx_read_shader (huge);
    result (r == NULL, "a 64 MiB shader file is refused");
    g_free (r);
    g_unlink (huge);
    g_free (huge);
    /* includes that include each other, and themselves */
    put ("shaders/a.h", "#include \"b.h\"\n#include \"a.h\"\nfloat fa() { return 1.0; }\n");
    put ("shaders/b.h", "#include \"a.h\"\n#include \"b.h\"\n#include \"./a.h\"\nfloat fb() { return 2.0; }\n");
    o = expand ("#include \"a.h\"\n#include \"b.h\"\nvoid main() {}\n", sdir);
    result (strlen (o) < 4096 && strstr (o, "fa()") && strstr (o, "fb()"), "includes: a.h <-> b.h end");
    g_free (o);
    g_free (sdir);
}

/* Combo names come from the wallpaper's JSON and go into the source as
 * "#define <name> <value>" lines. A name with a line break carried any
 * text into the source, of any length. */
static void combo_checks (void)
{
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    JsonNode *root = wpj_parse_data ("{\"MODE\":2,\"_x9\":\"3\",\"A B\":1,\"\":1,\"9LIVES\":1,"
                                     "\"N 1\\nfloat injected_text;\\n#define M\":1,\"K\\u00e4\":1,\"ok\":true}", -1);
    GString *big = g_string_new ("BIG 1\n");
    while (big->len < 8 * 1024 * 1024)
        g_string_append (big, "float filler_text_from_a_combo_name;\n");
    GString *longname = g_string_new (NULL);
    for (int i = 0; i < 4096; i++)
        g_string_append_c (longname, 'L');
    if (root && JSON_NODE_HOLDS_OBJECT (root)) {
        json_object_set_int_member (json_node_get_object (root), big->str, 1);
        json_object_set_int_member (json_node_get_object (root), longname->str, 1);
        fx_combos_overlay (combos, json_node_get_object (root));
    }
    fx_collect_combo_defaults ("// [COMBO] {\"combo\":\"FROM_SHADER\",\"default\":4}\n"
                               "// [COMBO] {\"combo\":\"X\\n#define Y\",\"default\":1}\n", combos);
    const char *body = "void main() {}\n";
    char *src = fx_build_source (body, combos, FALSE);
    gsize len = strlen (src);
    result (strstr (src, "#define MODE 2\n") && strstr (src, "#define _x9 3\n") &&
            strstr (src, "#define ok 1\n") && strstr (src, "#define FROM_SHADER 4\n"),
            "combos: names that are identifiers are defined");
    result (!strstr (src, "injected_text") && !strstr (src, "filler_text") && !strstr (src, "#define A B") &&
            !strstr (src, "9LIVES") && !strstr (src, "#define  1") && !strstr (src, "#define Y") &&
            !strstr (src, "LLLL") && !strstr (src, "#define K"),
            "combos: a name that is no identifier does not reach the source");
    char *m = g_strdup_printf ("combos: the source does not grow with such names (%" G_GSIZE_FORMAT " bytes)", len);
    result (len < 64 * 1024, m);
    g_free (m);
    g_free (src);
    g_string_free (big, TRUE);
    g_string_free (longname, TRUE);
    if (root)
        json_node_unref (root);
    g_hash_table_unref (combos);
}

/* ---- annotation scans: the combos a shader declares, the combo and the
 * default texture of a sampler (fx_collect_combo_defaults,
 * fx_sampler_combo, fx_sampler_default). They used to look for the next
 * '{' / ';' / line end with strchr () from every "[COMBO]" / "sampler2D"
 * of the text: without that character the text was walked to its end once
 * per word. The scans now walk the text once; what they find must be what
 * the old code found, which is kept here as the reference. */
static void ref_combo_defaults (const char *src, GHashTable *combos)
{
    for (const char *p = src; (p = strstr (p, "[COMBO]")) != NULL; p += 7) {
        const char *b1 = strchr (p, '{');
        const char *eol = strchr (p, '\n');
        if (!b1 || (eol && b1 > eol))
            continue;
        const char *b2 = eol ? g_strrstr_len (b1, eol - b1, "}") : strrchr (b1, '}');
        if (!b2)
            continue;
        char *json = g_strndup (b1, (gsize) (b2 - b1 + 1));
        JsonNode *root = wpj_parse_data (json, -1);
        g_free (json);
        if (!root)
            continue;
        if (JSON_NODE_HOLDS_OBJECT (root)) {
            JsonObject *o = json_node_get_object (root);
            const char *name = wpj_str (o, "combo", NULL);
            if (name && *name && !g_hash_table_contains (combos, name))
                g_hash_table_insert (combos, g_strdup (name),
                                     GINT_TO_POINTER (walpe_d2i (wpj_num (o, "default", 0))));
        }
        json_node_unref (root);
    }
}

static char *ref_sampler_str (const char *src, int unit, const char *key)
{
    char uname[16];
    snprintf (uname, sizeof (uname), "g_Texture%d", unit);
    for (const char *p = src; (p = strstr (p, "sampler2D")) != NULL; p += 9) {
        const char *semi = strchr (p, ';');
        const char *eol = strchr (p, '\n');
        if (!semi || (eol && semi > eol))
            continue;
        const char *nm = fx_skip_ws (p + 9, semi);
        if (strncmp (nm, uname, strlen (uname)) || g_ascii_isalnum (nm[strlen (uname)]))
            continue;
        const char *c = g_strstr_len (semi, eol ? eol - semi : -1, key);
        if (!c)
            continue;
        const char *q1 = strchr (c + strlen (key), '"');
        const char *q2 = q1 ? strchr (q1 + 1, '"') : NULL;
        if (q1 && q2 && (!eol || q2 < eol))
            return g_strndup (q1 + 1, (gsize) (q2 - q1 - 1));
    }
    return NULL;
}

static gboolean same_combos (GHashTable *a, GHashTable *b)
{
    if (g_hash_table_size (a) != g_hash_table_size (b))
        return FALSE;
    GHashTableIter it;
    gpointer k, v, v2;
    g_hash_table_iter_init (&it, a);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (!g_hash_table_lookup_extended (b, k, NULL, &v2) || v != v2)
            return FALSE;
    return TRUE;
}

/* TRUE when the scans and the reference agree on text */
static gboolean anno_same (const char *text)
{
    gboolean same = TRUE;
    GHashTable *a = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *b = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_collect_combo_defaults (text, a);
    ref_combo_defaults (text, b);
    same &= same_combos (a, b);
    g_hash_table_unref (a);
    g_hash_table_unref (b);
    for (int u = 0; u < FX_MAX_BINDS && same; u++) {
        char *x = fx_sampler_combo (text, u), *y = ref_sampler_str (text, u, "\"combo\"");
        same &= !g_strcmp0 (x, y);
        g_free (x);
        g_free (y);
        x = fx_sampler_default (text, u);
        y = ref_sampler_str (text, u, "\"default\"");
        same &= !g_strcmp0 (x, y);
        g_free (x);
        g_free (y);
    }
    return same;
}

static double cpu_now (void)
{
    return (double) clock () / CLOCKS_PER_SEC;
}

static void annotation_checks (void)
{
    static const char *const tok[] = {
        "sampler2D", "sampler2D ", "uniform sampler2D ", "g_Texture0", "g_Texture1", " g_Texture2", "g_Texture1x", "g_Texture7 ",
        ";", "; ", ";;", "\n", "\n", "\r\n", " ", "\t", "//", "// ", "\"combo\"", "\"default\"", "\"combo\":", "\"default\":",
        "\"", "\"\"", "\"A\"", "\"util/white\"", ":", ",", "{", "}", "{}", "}{", "[COMBO]", "// [COMBO] ", "[COMBO]{",
        "{\"combo\":\"A\",\"default\":1}", "{\"combo\":\"B\",\"default\":2,\"o\":{\"x\":[1,2]}}", "{\"combo\":\"C\"}",
        "{\"material\":\"m\",\"combo\":\"D\",\"default\":\"e\"}", "{\"combo\":7}", "{\"combo\":\"A\",\"default\":9}", "x", "combo", "default",
    };
    guint n_text = 0, bad = 0;
    char *first_bad = NULL;
    /* the seeds and mutations of them */
    for (guint32 seed = 1; seed <= 300; seed++) {
        rs = seed * 2246822519u + 77u;
        const char *v, *f;
        pick (&v, &f);
        char *m = seed <= N_TPL * 2 ? g_strdup (seed & 1 ? f : v) : mutate (rnd (2) ? f : v, 1 + (int) rnd (5));
        /* (the reference walks a long text once per word it finds) */
        if (strlen (m) <= 32768) {
            n_text++;
            if (!anno_same (m) && !bad++)
                first_bad = g_strndup (m, 200);
        }
        g_free (m);
    }
    /* lines made of the words the scans look for */
    for (guint32 seed = 1; seed <= 4000; seed++) {
        rs = seed * 2654435761u + 99991u;
        GString *s = g_string_new (NULL);
        guint nt = 1 + rnd (seed % 7 ? 40 : 400);
        for (guint i = 0; i < nt; i++)
            g_string_append (s, tok[rnd (G_N_ELEMENTS (tok))]);
        n_text++;
        if (!anno_same (s->str) && !bad++)
            first_bad = g_strndup (s->str, 200);
        g_string_free (s, TRUE);
    }
    char *m = g_strdup_printf ("annotation scans: the same answers as the scans they replace on %u texts (%u differ%s%s)",
                               n_text, bad, first_bad ? ", first: " : "", first_bad ? first_bad : "");
    result (bad == 0, m);
    g_free (m);
    g_free (first_bad);

    /* texts of FX_MAX_SOURCE bytes that have the words and not what the
     * scans look for behind them. (Under AddressSanitizer every strstr ()
     * measures its whole argument, so any loop over the matches of a word
     * is quadratic there: small texts in such builds.) */
#if defined (__SANITIZE_ADDRESS__)
    const gsize big = 64 * 1024;
#else
    const gsize big = FX_MAX_SOURCE;
#endif
    static const char *const unit[] = {
        "sampler2D\n", "sampler2D g_Texture1\n", "uniform sampler2D g_Texture1; // \"combo\"\n",
        "sampler2D g_Texture1;", "sampler2D g_Texture1;\"combo\"", "sampler2D ", "// [COMBO]\n", "[COMBO]", "[COMBO] {",
        "// [COMBO] {\"combo\":\"A\"\n",
    };
    double t0 = cpu_now ();
    for (guint k = 0; k < G_N_ELEMENTS (unit); k++) {
        GString *s = g_string_new (NULL);
        while (s->len < big)
            g_string_append (s, unit[k]);
        GHashTable *c = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        fx_collect_combo_defaults (s->str, c);
        g_hash_table_unref (c);
        for (int u = 1; u < FX_MAX_BINDS; u++) {
            g_free (fx_sampler_combo (s->str, u));
            g_free (fx_sampler_default (s->str, u));
        }
        g_string_free (s, TRUE);
    }
    double dt = cpu_now () - t0;
    m = g_strdup_printf ("annotation scans: %u texts of %" G_GSIZE_FORMAT " KiB without what is looked for, "
                         "every sampler unit: %.2f s of CPU", (guint) G_N_ELEMENTS (unit), big >> 10, dt);
    result (dt < 20.0, m);
    g_free (m);

    /* an annotation is short: megabytes between the braces are not parsed
     * once per "[COMBO]" in front of them */
    {
        GString *s = g_string_new ("// ");
        while (s->len < big / 4)
            g_string_append (s, "[COMBO] {\"a\":1,");
        g_string_append (s, "\"combo\":\"LATE\",\"default\":3}\n// [COMBO] {\"combo\":\"SHORT\",\"default\":5}\n");
        GHashTable *c = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        t0 = cpu_now ();
        fx_collect_combo_defaults (s->str, c);
        dt = cpu_now () - t0;
        m = g_strdup_printf ("annotation scans: one line of %" G_GSIZE_FORMAT " KiB of \"[COMBO] {\": %.2f s of "
                             "CPU, the short annotation behind it is read", big >> 12, dt);
        result (dt < 20.0 && GPOINTER_TO_INT (g_hash_table_lookup (c, "SHORT")) == 5, m);
        g_free (m);
        g_hash_table_unref (c);
        g_string_free (s, TRUE);
    }
}

/* ---- the size limit of the source repairs (FX_MAX_REPAIR_SOURCE): a
 * source beyond it is compiled as written. The repairs go through a source
 * once per declaration they look at; megabytes of declarations took
 * minutes of stage 1 for one pass. */
static gboolean try_never (const char *src, char *log, gsize logsize, gpointer user)
{
    (void) src;
    (*(int *) user)++;
    g_strlcpy (log, "0:3(1): error: a\n0:4(1): error: b\n", logsize);
    return FALSE;
}

static char *built_from (const char *head, const char *unit, gsize bytes, const char *tail, gboolean vertex)
{
    GString *b = g_string_new (head);
    for (guint i = 0; b->len < bytes; i++)
        g_string_append_printf (b, unit, i, i);
    g_string_append (b, tail);
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    char *s = fx_build_source (b->str, combos, vertex);
    g_hash_table_unref (combos);
    g_string_free (b, TRUE);
    return s;
}

/* what the repairs print, one "<level> <text>" line each */
static GString *size_said;

static void size_log (const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer user)
{
    (void) domain; (void) user;
    if (size_said)
        g_string_append_printf (size_said, "%d %s\n", (int) (level & G_LOG_LEVEL_MASK), msg);
}

static void repair_size_checks (void)
{
    static const char *VMAIN = "attribute vec3 a_Position;\nvoid main() {\n\tgl_Position = vec4(a_Position, 1.0);\n}\n";
    static const char *FMAIN = "void main() {\n\tvec3 k = 1.0;\n\tgl_FragColor = vec4(k, 1.0);\n}\n";
    /* a source of ordinary size is repaired as before */
    char *v = built_from ("", "// %u %u\n", 1000, VMAIN, TRUE), *f = built_from ("", "// %u %u\n", 1000, FMAIN, FALSE);
    fx_prepass (&v, &f, "small");
    result (strstr (f, "vec3 k = vec3(1.0);") != NULL, "repair size: a source of ordinary size is repaired");
    g_free (v);
    g_free (f);
    /* one beyond the limit is left as written, by stage 1 and by stage 2 */
    v = built_from ("", "// %u %u\n", 1000, VMAIN, TRUE);
    f = built_from ("", "// filler line %u %u\n", FX_MAX_REPAIR_SOURCE, FMAIN, FALSE);
    char *before = g_strdup (f);
    fx_prepass (&v, &f, "large");
    int tries = 0, budget = 48;
    gboolean changed = fx_repair_by_lines (&f, "0:3(1): error: a\n", try_never, &tries, &budget);
    result (!strcmp (f, before) && !changed && tries == 0,
            "repair size: a source beyond the limit is compiled as written");
    g_free (before);
    g_free (v);
    g_free (f);
    /* OpenGL ES. The body of a source has to be rewritten there
     * (fx_essl.h), and the rewriting walks the symbol lists the repairs
     * walk. A source beyond the limit is not compiled on OpenGL ES: the
     * pass builder is told so, one line says why, the texts stay as they
     * were and megabytes of names cost nothing. GLSL 3.30 is as above. */
    {
        static const char *EMAIN = "void main() {\n\tfloat k = 1;\n\tgl_FragColor = vec4(k);\n}\n";
        GLogFunc old_log = g_log_set_default_handler (size_log, NULL);
        char *m;
        fx_set_glsl_es (300);
        v = built_from ("", "// %u %u\n", 1000, VMAIN, TRUE);
        f = built_from ("", "// %u %u\n", 1000, EMAIN, FALSE);
        size_said = g_string_new (NULL);
        gboolean go = fx_prepass (&v, &f, "small");
        result (go && strstr (f, "float k = 1.0;") != NULL && !strstr (size_said->str, "not compiled"),
                "repair size, GLSL ES: a source of ordinary size is rewritten and compiled");
        g_free (f);
        f = built_from ("", "// filler line %u %u\n", FX_MAX_REPAIR_SOURCE, EMAIN, FALSE);
        char *v0 = g_strdup (v), *f0 = g_strdup (f);
        g_string_truncate (size_said, 0);
        go = fx_prepass (&v, &f, "large");
        m = g_strdup_printf ("%d fx: shader large: a source of more than %d KiB is not compiled on OpenGL ES "
                             "(no HLSL-semantics repairs, no GLSL ES rewriting)\n",
                             (int) G_LOG_LEVEL_WARNING, FX_MAX_REPAIR_SOURCE / 1024);
        result (!go && !strcmp (size_said->str, m) && !strcmp (v, v0) && !strcmp (f, f0),
                "repair size, GLSL ES: a source beyond the limit is not compiled, and one line says so");
        g_free (m);
        /* the builder that shares and specialises programs (fx_specialize
         * on, fx_quality = fast) asks the same question before anything
         * else: no program, no state on the pass, and the line once - not
         * a second time from the generic program it falls back to. No GL
         * call is made on the way (there is no context here). */
        {
            EffPass ps;
            memset (&ps, 0, sizeof ps);
            const int spec = g_atomic_int_get (&fxs_spec_on);
            g_atomic_int_set (&fxs_spec_on, 1);
            g_string_truncate (size_said, 0);
            GLuint prog = fxs_build_program (&ps, &v, &f, "large", "test", NULL);
            g_atomic_int_set (&fxs_spec_on, spec);
            m = g_strdup_printf ("%d fx: shader large: a source of more than %d KiB is not compiled on OpenGL ES "
                                 "(no HLSL-semantics repairs, no GLSL ES rewriting)\n",
                                 (int) G_LOG_LEVEL_WARNING, FX_MAX_REPAIR_SOURCE / 1024);
            result (!prog && !ps.spec && !strcmp (size_said->str, m) && !strcmp (v, v0) && !strcmp (f, f0),
                    "repair size, GLSL ES: the specialising builder does not compile it either, and says so once");
            g_free (m);
        }
        /* the same pair as GLSL 3.30: compiled as written, the line it always printed */
        fx_set_glsl_es (0);
        g_free (v);
        g_free (f);
        v = built_from ("", "// %u %u\n", 1000, VMAIN, TRUE);
        f = built_from ("", "// filler line %u %u\n", FX_MAX_REPAIR_SOURCE, EMAIN, FALSE);
        g_free (v0);
        g_free (f0);
        v0 = g_strdup (v);
        f0 = g_strdup (f);
        g_string_truncate (size_said, 0);
        go = fx_prepass (&v, &f, "large");
        m = g_strdup_printf ("%d fx: shader large: a source of more than %d KiB is compiled as written "
                             "(no HLSL-semantics repairs)\n", (int) G_LOG_LEVEL_MESSAGE,
                             FX_MAX_REPAIR_SOURCE / 1024);
        result (go && !strcmp (size_said->str, m) && !strcmp (v, v0) && !strcmp (f, f0),
                "repair size, GLSL 3.30: the same source is compiled as written, with the line it always printed");
        g_free (m);
        g_free (v0);
        g_free (f0);
        g_free (v);
        g_free (f);
        /* names by the megabyte on OpenGL ES: declared, then used */
        fx_set_glsl_es (300);
        GString *uses = g_string_new ("void main() {\n\tfloat x = 1;\n");
        for (guint i = 0; uses->len < FX_MAX_SOURCE / 2; i++)
            g_string_append_printf (uses, "\tx = x + g%u * 2;\n", i);
        g_string_append (uses, "\tgl_FragColor = vec4(x);\n}\n");
        v = built_from ("", "// %u %u\n", 1000, VMAIN, TRUE);
        f = built_from ("", "float g%u = 1; // %u\n", FX_MAX_SOURCE / 2, uses->str, FALSE);
        g_string_free (uses, TRUE);
        double e0 = (double) clock () / CLOCKS_PER_SEC;
        go = fx_prepass (&v, &f, "names");
        double de = (double) clock () / CLOCKS_PER_SEC - e0;
        m = g_strdup_printf ("repair size, GLSL ES: %d MiB of names declared and used: %.2f s of CPU, not compiled",
                             FX_MAX_SOURCE >> 20, de);
        result (!go && de < 5.0, m);
        g_free (m);
        g_free (v);
        g_free (f);
        fx_set_glsl_es (0);
        g_string_free (size_said, TRUE);
        size_said = NULL;
        g_log_set_default_handler (old_log, NULL);
    }
    /* declarations by the megabyte, in both stages: every interface
     * repair looked at each of them against all the others */
    static const char *const unit[] = {
        "varying vec4 v%u; // %u\n", "uniform float u%u; // %u\n", "varying vec2 w%u;\nuniform vec3 w%ux;\n",
    };
    double t0 = (double) clock () / CLOCKS_PER_SEC;
    for (guint k = 0; k < G_N_ELEMENTS (unit); k++) {
        v = built_from ("", unit[k], FX_MAX_SOURCE, VMAIN, TRUE);
        f = built_from ("", unit[k], FX_MAX_SOURCE, "void main() {\n\tw0 = vec2(1.0);\n\tgl_FragColor = vec4(1.0);\n}\n", FALSE);
        fx_prepass (&v, &f, "bomb");
        tries = 0;
        budget = 48;
        char lg[256];
        gboolean ok = try_never (f, lg, sizeof lg, &tries);
        for (int round = 0; !ok && round < 16 && fx_repair_by_lines (&f, lg, try_never, &tries, &budget); round++)
            ok = try_never (f, lg, sizeof lg, &tries);
        fx_varying_repair (&v, &f, TRUE);
        g_free (v);
        g_free (f);
    }
    double dt = (double) clock () / CLOCKS_PER_SEC - t0;
    char *m = g_strdup_printf ("repair size: %u pairs of %d MiB of declarations through both repair stages: %.2f s of CPU",
                               (guint) G_N_ELEMENTS (unit), FX_MAX_SOURCE >> 20, dt);
    result (dt < 30.0, m);
    g_free (m);
#if !defined (__SANITIZE_ADDRESS__)
    /* ... and what such text costs right at the limit (not in sanitizer
     * builds: every strstr () measures its whole argument there) */
    t0 = (double) clock () / CLOCKS_PER_SEC;
    v = built_from ("", unit[0], FX_MAX_REPAIR_SOURCE - 4096, VMAIN, TRUE);
    f = built_from ("", unit[0], FX_MAX_REPAIR_SOURCE - 4096, "void main() {\n\tv0 = vec4(1.0);\n\tgl_FragColor = v1;\n}\n", FALSE);
    fx_prepass (&v, &f, "limit");
    dt = (double) clock () / CLOCKS_PER_SEC - t0;
    m = g_strdup_printf ("repair size: two sources of declarations at the limit (%d KiB): %.2f s of CPU",
                         FX_MAX_REPAIR_SOURCE >> 10, dt);
    result (strlen (v) <= FX_MAX_REPAIR_SOURCE && dt < 30.0, m);
    g_free (m);
    g_free (v);
    g_free (f);
#endif
}

/* fx_fuse.h, the recogniser of a pure displacement: the final statement is
 * compared without white space, the coordinate is then cut out of the text
 * itself. A name of the statement in two parts (two macros, or a comment
 * in between, blanked before the recogniser sees the text) passed the
 * comparison; the name was then not found in the text. */
static void fuse_checks (void)
{
    static const char *const head = "uniform sampler2D g_Texture0;\n"
                                    "uniform vec4 g_Texture0Resolution;\n"
                                    "varying vec4 v_TexCoord;\n";
    static const char *const two = "#define g_Tex\n#define ture0 g_Texture0\n";
    static const struct { const char *defs, *stmt, *expr; } c[] = {
        /* as an effect writes it: a displacement */
        { "", "gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy + vec2(0.01, 0.0));",
          "return ( v_TexCoord__q.xy + vec2(0.01, 0.0));" },
        { "", "gl_FragColor\n\t=\ttexSample2D (\n g_Texture0 ,v_TexCoord.xy) ;", "return (v_TexCoord__q.xy);" },
        /* the name of the input in two parts: valid GLSL, not recognised */
        { two, "gl_FragColor = texSample2D(g_Tex ture0, v_TexCoord.xy);", NULL },
        { two, "gl_FragColor = texSample2D(g_Tex ture0, v_TexCoord.xy + g_Texture0Resolution.zw);", NULL },
        { two, "gl_FragColor = texSample2D(g_Tex ture0, v_TexCoord.xy + min(g_Texture0Resolution.zw, vec2(1.0)));",
          NULL },
        { "", "gl_FragColor = texSample2D(g_Tex/* */ture0, v_TexCoord.xy);", NULL },
        { "", "gl_FragColor = texSample2D(g_Tex\nture0, v_TexCoord.xy);", NULL },
        /* other names in two parts */
        { "", "gl_FragColor = tex Sample2D(g_Texture0, v_TexCoord.xy);", NULL },
        { "", "gl_FragColor = texSample2D(g_Texture0 Resolution, v_TexCoord.xy);", NULL },
    };
    guint n_ok = 0, n_res = 0;
    GString *bad = g_string_new (NULL);
    for (guint i = 0; i < G_N_ELEMENTS (c); i++) {
        char *body = g_strconcat (head, c[i].defs, "void main() {\n\t", c[i].stmt, "\n}\n", NULL);
        GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        char *fsrc = fx_build_source (body, combos, FALSE);
        g_hash_table_unref (combos);
        char *r = fxf_resolve (fsrc, FALSE);
        if (r) {
            n_res++;
            GArray *g = fxf_globals (r);
            char *fn = fxf_warp_function (r, g, 1);
            gboolean ok = c[i].expr ? fn && strstr (fn, "vec2 _wf1(vec2 _wpx)") && strstr (fn, c[i].expr) : fn == NULL;
            if (ok)
                n_ok++;
            else
                g_string_append_printf (bad, " %u", i);
            g_free (fn);
            g_array_unref (g);
            g_free (r);
        }
        g_free (fsrc);
        g_free (body);
    }
    char *m = g_strdup_printf ("displacement recogniser: %u of %u final statements resolved, %u as expected "
                               "(a name in two parts is no displacement)%s%s",
                               n_res, (guint) G_N_ELEMENTS (c), n_ok, bad->len ? "; wrong:" : "", bad->str);
    result (n_res == G_N_ELEMENTS (c) && n_ok == n_res, m);
    g_free (m);
    g_string_free (bad, TRUE);
}

/* The size of a statement, macros expanded (fx_srcrepair.h): the drivers'
 * compilers walk an expression by recursion, and one statement may be as
 * long as the source. */
static char *long_sum (guint terms)
{
    GString *s = g_string_new ("uniform sampler2D g_Texture0;\nvarying vec4 v_TexCoord;\nvoid main() {\n"
                               "\tvec4 c = texSample2D(g_Texture0, v_TexCoord.xy);\n\tc.r = c.g");
    for (guint i = 1; i < terms; i++)
        g_string_append (s, "+c.g");
    g_string_append (s, ";\n\tgl_FragColor = c;\n}\n");
    return g_string_free (s, FALSE);
}

static void statement_checks (void)
{
    gint64 st = -1;
    gboolean ok = fx_expansion_sizes ("float a;\nvoid f() {\n\ta = a + 1.0;\n\ta = a\n\t\t* 2.0\n\t\t* a;\n}\n", &st);
    char *m = g_strdup_printf ("statement size: the longest of a small text has %" G_GINT64_FORMAT " tokens (7)", st);
    result (ok && st == 7, m);
    g_free (m);

    char *sum = long_sum (20000);
    ok = fx_expansion_sizes (sum, &st);
    m = g_strdup_printf ("statement size: a sum of 20000 terms is one statement of %" G_GINT64_FORMAT " tokens, "
                         "beyond the limit of %d", st, FX_MAX_STATEMENT);
    result (ok && st > FX_MAX_STATEMENT, m);
    g_free (m);
    g_free (sum);

    /* the same out of 300 bytes of macros */
    ok = fx_expansion_sizes ("#define A (x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x)\n"
                             "#define B (A+A+A+A+A+A+A+A+A+A+A+A+A+A+A+A)\n"
                             "#define C (B+B+B+B+B+B+B+B+B+B+B+B+B+B+B+B)\n"
                             "float f(float x) {\n\treturn C + C;\n}\n", &st);
    m = g_strdup_printf ("statement size: three levels of macros in one statement count %" G_GINT64_FORMAT " tokens, "
                         "beyond the limit", st);
    result (ok && st > FX_MAX_STATEMENT, m);
    g_free (m);

    /* long sources of short statements, and a table, stay below it */
    GString *s = g_string_new ("void main() {\n\tvec4 c = vec4(0.0);\n");
    for (int i = 0; i < 20000; i++)
        g_string_append_printf (s, "\tc.r = c.r * 1.0001 + sin(c.g * %d.0);\n", i);
    g_string_append (s, "\tfloat tab[1024] = float[1024](0.0");
    for (int i = 1; i < 1024; i++)
        g_string_append_printf (s, ", %d.5", i);
    g_string_append (s, ");\n\tgl_FragColor = c * tab[3];\n}\n");
    ok = fx_expansion_sizes (s->str, &st);
    m = g_strdup_printf ("statement size: 20000 short statements and a table of 1024 numbers: the longest has %"
                         G_GINT64_FORMAT " tokens, within the limit", st);
    result (ok && st > 2000 && st <= FX_MAX_STATEMENT, m);
    g_free (m);
    g_string_free (s, TRUE);
}

/* every text stage of fx_build_pass and of the specialiser on one pair */
static void text_pipeline (const char *vraw, const char *fraw)
{
    char *sdir = g_build_filename (tmpdir, "shaders", NULL);
    char *body[2];
    for (int k = 0; k < 2; k++) {
        char *h = fx_hoist_decls (k ? fraw : vraw);
        body[k] = expand (h, sdir);
        g_free (h);
    }
    g_free (sdir);
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_collect_combo_defaults (body[0], combos);
    fx_collect_combo_defaults (body[1], combos);
    for (int u = 0; u < FX_MAX_BINDS; u++) {
        g_free (fx_sampler_default (body[1], u));
        char *c = fx_sampler_combo (body[1], u);
        if (c)
            g_hash_table_insert (combos, c, GINT_TO_POINTER (1));
    }
    char *vsrc = fx_build_source (body[0], combos, TRUE);
    char *fsrc = fx_build_source (body[1], combos, FALSE);
    g_hash_table_unref (combos);
    WalpeFxTime t = { 0 };
    walpe_fx_time_scan (body[1], NULL, NULL, &t);
    walpe_fx_time_scan (body[0], NULL, NULL, &t);
    fx_fragment_may_discard (body[1]);
    static const char *const keys[] = { "material", "default", "combo" };
    static const char *const names[] = { "g_Speed", "g_Texture1", "g_X", "g_UserAlpha", "g_Bounds" };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        for (guint k = 0; k < G_N_ELEMENTS (keys); k++) {
            g_free (fx_shader_anno_t (body[1], names[i], keys[k], NULL));
            g_free (fx_uniform_anno (body[1], body[0], names[i], keys[k], GL_FLOAT));
        }
    fx_prepass (&vsrc, &fsrc, "hostile");
    char *a = fxs_active_lines (fsrc);
    g_free (a);
    a = fxs_scan_text (fsrc);
    g_free (a);
    a = fx_active_code (vsrc);
    g_free (a);
    guint *map = NULL;
    a = fxs_active_map (fsrc, 0, &map);
    g_free (a);
    g_free (map);
    for (int k = 0; k < 2; k++) {
        char *r = fxf_resolve (k ? fsrc : vsrc, !k);
        if (r) {
            GArray *g = fxf_globals (r);
            const char *hd = NULL, *bd = NULL, *en = NULL;
            fxf_main_span (r, &hd, &bd, &en);
            if (k)
                g_free (fxf_warp_function (r, g, 0));
            g_array_unref (g);
            g_free (r);
        }
    }
    g_free (fxs_cost_count_source (fsrc));
    g_free (vsrc);
    g_free (fsrc);
    g_free (body[0]);
    g_free (body[1]);
}

/* the seeds that carry this test's key statements reach the hoists: every
 * entry of fxs_hoists[] applies to the seed made for it, as written */
static void seed_checks (void)
{
    char *sdir = g_build_filename (tmpdir, "shaders", NULL);
    guint want = 0, got = 0;
    for (guint k = 0; k < N_TPL; k++) {
        int hi = seed_hoist[k];
        if (hi < 0 || hi >= (int) G_N_ELEMENTS (fxs_hoists))
            continue;
        want++;
        char *body[2];
        for (int i = 0; i < 2; i++) {
            char *h = fx_hoist_decls (i ? tpl_frag[k] : tpl_vert[k]);
            body[i] = expand (h, sdir);
            g_free (h);
        }
        GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        fx_collect_combo_defaults (body[0], combos);
        fx_collect_combo_defaults (body[1], combos);
        char *vsrc = fx_build_source (body[0], combos, TRUE), *fsrc = fx_build_source (body[1], combos, FALSE);
        g_hash_table_unref (combos);
        fx_prepass (&vsrc, &fsrc, "seed");
        char *fact = fx_active_code (fsrc), *vact = fx_active_code (vsrc);
        if (fxs_rw_hoist (&fxs_hoists[hi], &vsrc, &fsrc, fact, vact) && strstr (fsrc, "_we_hoist0") &&
            strstr (vsrc, "_we_hoist0"))
            got++;
        g_free (fact); g_free (vact);
        g_free (vsrc); g_free (fsrc);
        g_free (body[0]); g_free (body[1]);
    }
    g_free (sdir);
    char *m = g_strdup_printf ("seeds: the hoists of fx_rewrite.h apply to the seeds made for them (%u of %u)",
                               got, want);
    result (got == want, m);
    g_free (m);
}

static void text_fuzz (guint count, guint32 first)
{
    {
        /* a ']' without its '[' in front of an operator, at the start of
         * a line: the operand used to start one byte before the line
         * (found by this fuzz; under ASan a heap-buffer-overflow in the
         * expression typer that reads the operand) */
        static const char *const lines[] = { "]+x", "]]]*y.xy", " ] - z", "] % 2.0", "a[0]] / b" };
        gboolean inside = TRUE;
        for (guint i = 0; i < G_N_ELEMENTS (lines); i++) {
            const char *op = strpbrk (lines[i], "+*-%/");
            inside &= fx_operand_start (lines[i], op) >= lines[i];
        }
        result (inside, "source repair: the operand in front of an operator starts inside its line");
    }
    gint64 t0 = g_get_monotonic_time ();
    double worst = 0;
    guint32 worst_seed = 0;
    for (guint32 seed = first; seed < first + count; seed++) {
        rs = seed * 2654435761u + 12345u;
        const char *v, *f;
        pick (&v, &f);
        char *mv = rnd (3) ? g_strdup (v) : mutate (v, 1 + (int) rnd (4));
        char *mf = mutate (f, 1 + (int) rnd (5));
        gint64 s0 = g_get_monotonic_time ();
        text_pipeline (mv, mf);
        double dt = (g_get_monotonic_time () - s0) / 1e6;
        if (dt > worst) {
            worst = dt;
            worst_seed = seed;
        }
        g_free (mv);
        g_free (mf);
    }
    char *m = g_strdup_printf ("text stages: %u mutated shader pairs (seeds %u..%u) in %.1f s, slowest %.2f s "
                               "(seed %u)", count, first, first + count - 1,
                               (g_get_monotonic_time () - t0) / 1e6, worst, worst_seed);
    result (worst < 30.0, m);
    g_free (m);
}

/* ---------------------------------------------------------------- GL */

#define W 40
#define H 24

static GLuint input_tex (void)
{
    static guint8 px[W * H * 4];
    guint32 s = 12345;
    for (int i = 0; i < W * H * 4; i++) {
        s = s * 1664525u + 1013904223u;
        px[i] = (guint8) (s >> 24);
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

static const char *const jvals[] = {
    "1", "0", "-1", "0.5", "1e308", "-1e308", "1e999", "2147483648", "-2147483649", "4294967296", "9007199254740993",
    "true", "false", "null", "\"\"", "\"1 2 3\"", "\"nan nan nan\"", "\"inf -inf 1e999 x\"", "\"1e30 1e30 1e30 1e30\"",
    "\"a\"", "[]", "[1,2,3]", "{}", "{\"user\":\"opt\",\"value\":\"0.5 0.5\"}", "{\"user\":7,\"value\":null}",
    "{\"script\":\"export function update(v){return v}\",\"value\":1}", "{\"animation\":{},\"value\":\"1 1\"}",
    "{\"user\":{\"name\":\"opt\",\"condition\":\"1\"},\"value\":true}", "[[[[[[[[[[[[1]]]]]]]]]]]]",
    "\"_rt_FullFrameBuffer\"", "\"_rt_imageLayerComposite_1_a\"", "\"_alias_x\"", "\"../../../etc/passwd\"", "\"util/white\"",
    "\"previous\"", "\"framebuffer\"", "\"rgba8888\"", "\"rg88\"", "\"r8\"", "\"rgba16f\"", "\"nonsense\"",
};
static const char *jv (void) { return jvals[rnd (G_N_ELEMENTS (jvals))]; }

/* global-scope text for the members of a fused run (fx_fuse.h renames
 * what a member declares and merges the members into one program) */
static const char *const fuse_snips[] = {
    "float _wf1(vec2 p) { return p.x; }\n", "vec2 _wv1() { return vec2(0.0); }\n", "float _we_fuv;\n", "#define _wq v_TexCoord\n",
    "struct S { vec2 a; float b[3]; };\n", "const vec2 K = vec2(1.0, 2.0);\n", "vec2 helper2(vec2 x) { return x * 1.0; }\n",
    "uniform float g_Time;\n", "#define uv2(x) ((x) + 0.0)\n", "#define offset2 offset\n", "float main2() { return 0.0; }\n",
    "float gl_FragColor2;\n", "float g_Texture0x;\n", "const float flowPhase2 = 0.0, flowPhase3 = 1.0;\n",
    "float arr[4];\n", "vec4 v_TexCoord2;\n", "float aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;\n",
    "#if MODE == 1\nfloat only_mode1;\n#else\nfloat only_else;\n#endif\n", "#undef MODE\n#define MODE 3\n", "#define helper(x) vec2(x)\n",
    "float f1(float a) { return a; } float f1(vec2 a) { return a.x; }\n", "layout(std140) uniform Blk { float q; };\n",
    "float helper;\n", "void main2() { }\n", "float time;\n", "float offset;\n", "vec2 moved;\n", "vec4 sines;\n", "#pragma debug(on)\n", "#extension GL_ARB_gpu_shader5 : enable\n",
};

/* an effect of the family fx_fuse.h fuses: one pass, no targets, a pure
 * displacement, every constant a number; returns the instance JSON */
static char *make_family_effect (int idx)
{
    char name[32];
    g_snprintf (name, sizeof (name), "e%d", idx);
    GString *f = g_string_new (tpl_frag[1]), *v = g_string_new (tpl_vert[1]);
    int n = (int) rnd (4);
    for (int i = 0; i < n; i++) {
        /* behind the first line (the #include): at global scope */
        const char *nl = strchr (f->str, '\n');
        g_string_insert (f, nl ? (gssize) (nl - f->str) + 1 : 0, fuse_snips[rnd (G_N_ELEMENTS (fuse_snips))]);
    }
    if (rnd (5) == 0) {
        const char *nl = strchr (v->str, '\n');
        g_string_insert (v, nl ? (gssize) (nl - v->str) + 1 : 0, fuse_snips[rnd (G_N_ELEMENTS (fuse_snips))]);
    }
    char *mf = rnd (8) ? g_strdup (f->str) : mutate (f->str, 1);
    char *vp = g_strdup_printf ("shaders/effects/%s_0.vert", name), *fp = g_strdup_printf ("shaders/effects/%s_0.frag", name);
    put (vp, v->str);
    put (fp, mf);
    g_free (vp); g_free (fp); g_free (mf);
    g_string_free (f, TRUE);
    g_string_free (v, TRUE);
    char *mp = g_strdup_printf ("materials/effects/%s_0.json", name);
    char *mj = g_strdup_printf ("{\"passes\":[{\"shader\":\"effects/%s_0\"}]}", name);
    put (mp, mj);
    g_free (mp); g_free (mj);
    char *ep = g_strdup_printf ("effects/%s/effect.json", name);
    char *ej = g_strdup_printf ("{\"passes\":[{\"material\":\"materials/effects/%s_0.json\"}]}", name);
    put (ep, ej);
    g_free (ep); g_free (ej);
    return g_strdup_printf ("{\"file\":\"effects/%s/effect.json\",\"name\":\"%s\",\"visible\":true,\"passes\":[{"
                            "\"constantshadervalues\":{\"speed\":%u.5,\"strength\":0.0%u,\"scale\":\"%u %u\","
                            "\"bounds\":\"0.%u 1\"},\"combos\":{\"MODE\":%u}}]}",
                            name, name, rnd (9), rnd (9), 1 + rnd (3), 1 + rnd (3), rnd (9), rnd (3));
}

/* one effect instance with its files; returns the instance JSON */
static char *make_effect (int idx)
{
    const char *v, *f;
    char name[32];
    g_snprintf (name, sizeof (name), "e%d", idx);
    int npass = 1 + (int) rnd (3);
    GString *ej = g_string_new ("{");
    if (rnd (3) == 0)
        g_string_append_printf (ej, "\"fbos\":[{\"name\":\"_rt_a\",\"scale\":%s,\"format\":%s},{\"name\":\"_rt_b\",\"scale\":%d},"
                                "%s,{\"name\":%s}],", rnd (2) ? jv () : "2", rnd (2) ? jv () : "\"rgba8888\"",
                                1 << rnd (5), jv (), jv ());
    else
        g_string_append (ej, "\"fbos\":[{\"name\":\"_rt_a\",\"scale\":2},{\"name\":\"_rt_b\",\"scale\":1}],");
    g_string_append (ej, "\"passes\":[");
    GString *ip = g_string_new ("[");
    for (int p = 0; p < npass; p++) {
        pick (&v, &f);
        /* a third as written, a third with further declarations in use, a third broken */
        guint how = rnd (6);
        char *mv = how < 4 ? g_strdup (v) : how == 4 ? gentle (v, FALSE) : mutate (v, 1 + (int) rnd (3));
        char *mf = how < 2 ? g_strdup (f) : how < 4 ? gentle (f, TRUE) : mutate (f, 1 + (int) rnd (3));
        char *vp = g_strdup_printf ("shaders/effects/%s_%d.vert", name, p);
        char *fp = g_strdup_printf ("shaders/effects/%s_%d.frag", name, p);
        put (vp, mv);
        put (fp, mf);
        g_free (vp); g_free (fp); g_free (mv); g_free (mf);
        char *mp = g_strdup_printf ("materials/effects/%s_%d.json", name, p);
        GString *mj = g_string_new (NULL);
        g_string_append_printf (mj, "{\"passes\":[{\"shader\":\"effects/%s_%d\"", name, p);
        if (rnd (3) == 0)
            g_string_append_printf (mj, ",\"combos\":{\"MODE\":%s,\"MASK\":%s,\"X Y\":%s}", jv (), jv (), jv ());
        if (rnd (3) == 0)
            g_string_append_printf (mj, ",\"textures\":[%s,%s,%s,null,%s]", jv (), jv (), jv (), jv ());
        if (rnd (4) == 0)
            g_string_append_printf (mj, ",\"target\":%s,\"bind\":[{\"name\":%s,\"index\":%s}]", jv (), jv (), jv ());
        g_string_append (mj, "}]}");
        put (mp, rnd (12) ? mj->str : "{\"passes\":[{\"shader\":7}]}");
        g_string_free (mj, TRUE);
        g_free (mp);
        if (p)
            g_string_append_c (ej, ',');
        if (rnd (9) == 0) {
            g_string_append_printf (ej, "{\"command\":\"%s\",\"target\":\"%s\",\"source\":\"%s\"}", rnd (2) ? "copy" : "swap",
                                    rnd (2) ? "_rt_a" : "nowhere", rnd (2) ? "_rt_b" : "previous");
        } else {
            g_string_append_printf (ej, "{\"material\":\"materials/effects/%s_%d.json\"", name, p);
            if (p + 1 < npass || rnd (4) == 0)
                g_string_append_printf (ej, ",\"target\":\"%s\"", rnd (6) ? (p & 1 ? "_rt_b" : "_rt_a") : "_rt_none");
            if (p > 0 || rnd (4) == 0) {
                g_string_append_printf (ej, ",\"bind\":[{\"name\":\"%s\",\"index\":0}", rnd (6) ? (p & 1 ? "_rt_a" : "_rt_b") : "previous");
                if (rnd (3) == 0)
                    g_string_append_printf (ej, ",{\"name\":%s,\"index\":%s},{\"name\":\"previous\",\"index\":%d},7",
                                            rnd (2) ? jv () : "\"_rt_a\"", jv (), (int) rnd (12) - 2);
                g_string_append_c (ej, ']');
            }
            g_string_append_c (ej, '}');
        }
        if (p)
            g_string_append_c (ip, ',');
        g_string_append (ip, "{");
        gboolean any = FALSE;
        if (rnd (2)) {
            g_string_append_printf (ip, "\"constantshadervalues\":{\"speed\":%s,\"gain\":%s,\"alpha\":%s,\"bounds\":%s,"
                                    "\"power\":%s,\"tint\":%s,\"scale\":%s,\"exponent\":%s,\"n\":%s,\"b\":%s,\"pulsespeed\":%s}",
                                    jv (), jv (), rnd (2) ? "1" : jv (), rnd (2) ? "\"0 0\"" : jv (), jv (), jv (), jv (),
                                    jv (), jv (), jv (), jv ());
            any = TRUE;
        }
        if (rnd (4) == 0) {
            g_string_append_printf (ip, "%s\"combos\":{\"MODE\":%s,\"MASK\":%s}", any ? "," : "", jv (), jv ());
            any = TRUE;
        }
        if (rnd (4) == 0) {
            g_string_append_printf (ip, "%s\"textures\":[null,%s,%s]", any ? "," : "", jv (), jv ());
            any = TRUE;
        }
        if (rnd (6) == 0)
            g_string_append_printf (ip, "%s\"usertextures\":[null,{\"name\":\"$mediaThumbnail\",\"type\":\"system\"},%s]",
                                    any ? "," : "", jv ());
        g_string_append (ip, "}");
    }
    g_string_append (ej, "]}");
    g_string_append (ip, "]");
    char *ep = g_strdup_printf ("effects/%s/effect.json", name);
    put (ep, ej->str);
    g_free (ep);
    char *inst = g_strdup_printf ("{\"file\":\"effects/%s/effect.json\",\"name\":\"%s\",\"visible\":%s,\"passes\":%s}",
                                  name, name, rnd (10) ? "true" : jv (), rnd (10) ? ip->str : jv ());
    g_string_free (ej, TRUE);
    g_string_free (ip, TRUE);
    return inst;
}

static void clear_effect_files (void)
{
    static const char *const sub[][2] = { { "effects", NULL }, { "materials", NULL }, { "shaders", "effects" } };
    for (guint i = 0; i < G_N_ELEMENTS (sub); i++) {
        char *d = g_build_filename (tmpdir, sub[i][0], sub[i][1], NULL);
        rm_rf (d);
        g_free (d);
    }
}

/* the seeds as written: each builds and draws; the message says how many
 * of the passes that carry this test's key statements run a hoisted
 * program (three with the table of version 0.12.1) */
static void gl_seed_checks (GLuint in)
{
    g_atomic_int_set (&fxs_share_on, 0);
    g_atomic_int_set (&fxs_spec_on, 1);
    g_atomic_int_set (&fxs_verify_on, 0);
    g_atomic_int_set (&fxs_quality, 0);
    fx_program_stats (NULL, NULL, NULL, NULL, NULL, NULL, TRUE);
    guint bad = 0, hoisted = 0, want = 0;
    for (guint k = 0; k < N_TPL; k++) {
        want += seed_hoist[k] >= 0;
        put ("shaders/effects/s_0.vert", tpl_vert[k]);
        put ("shaders/effects/s_0.frag", tpl_frag[k]);
        put ("materials/effects/s_0.json", "{\"passes\":[{\"shader\":\"effects/s_0\"}]}");
        put ("effects/s/effect.json", "{\"passes\":[{\"material\":\"materials/effects/s_0.json\"}]}");
        JsonNode *root = wpj_parse_data ("{\"effects\":[{\"file\":\"effects/s/effect.json\",\"name\":\"s\","
                                         "\"visible\":true,\"passes\":[{}]}]}", -1);
        JsonObject *up = json_object_new ();
        struct FxSet *set = root ? fx_build_for_object (tmpdir, wpj_arr (json_node_get_object (root), "effects"),
                                                        "seed", up) : NULL;
        json_object_unref (up);
        if (root)
            json_node_unref (root);
        if (!set || set->n < 1 || fx_failed_count (set)) {
            bad++;
        } else {
            fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
            for (int d = 0; d < 2; d++)
                fx_run_for_object (set, in, W, H, W, H, d * 0.37, 1.0f, FALSE);
            for (int c = 0; c < set->n; c++)
                for (int i = 0; set->chains[c] && i < set->chains[c]->n_passes; i++) {
                    const EffPass *ps = &set->chains[c]->passes[i];
                    if (ps->spec && (ps->spec->rewrites & (FXS_RW_PULSE | FXS_RW_SHAKE)))
                        hoisted++;
                }
        }
        if (set)
            fx_free_for_object (set);
        clear_effect_files ();
    }
    int fallback = 0;
    fx_program_stats (NULL, NULL, NULL, &fallback, NULL, NULL, TRUE);
    char *m = g_strdup_printf ("seeds: %u of %u build and draw as written; hoisted programs in use: %u of %u, "
                               "specialised builds that fell back: %d", (guint) N_TPL - bad, (guint) N_TPL,
                               hoisted, want, fallback);
    result (bad == 0, m);
    g_free (m);
}

/* a statement beyond FX_MAX_STATEMENT does not reach the driver: Mesa's
 * compiler ran out of stack on a sum of this kind (the process ended
 * inside glCompileShader) */
static void gl_statement_check (GLuint in)
{
    g_atomic_int_set (&fxs_share_on, 0);
    g_atomic_int_set (&fxs_spec_on, 0);
    g_atomic_int_set (&fxs_verify_on, 0);
    g_atomic_int_set (&fxs_quality, 0);
    char *sum = long_sum (20000);
    put ("shaders/effects/s_0.vert", tpl_vert[0]);
    put ("shaders/effects/s_0.frag", sum);
    g_free (sum);
    put ("materials/effects/s_0.json", "{\"passes\":[{\"shader\":\"effects/s_0\"}]}");
    put ("effects/s/effect.json", "{\"passes\":[{\"material\":\"materials/effects/s_0.json\"}]}");
    JsonNode *root = wpj_parse_data ("{\"effects\":[{\"file\":\"effects/s/effect.json\",\"name\":\"s\","
                                     "\"visible\":true,\"passes\":[{}]}]}", -1);
    JsonObject *up = json_object_new ();
    struct FxSet *set = root ? fx_build_for_object (tmpdir, wpj_arr (json_node_get_object (root), "effects"),
                                                    "statement", up) : NULL;
    json_object_unref (up);
    if (root)
        json_node_unref (root);
    int failed = set ? fx_failed_count (set) : -1;
    if (set) {
        fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
        fx_run_for_object (set, in, W, H, W, H, 0.0, 1.0f, FALSE);
        fx_free_for_object (set);
    }
    clear_effect_files ();
    result (!set || failed == 1, "a sum of 20000 terms in one statement: the effect is not compiled, the others go on");
}

static guint n_built, n_null, n_failed_chains, n_draws;

/* The pass table of a chain has the size of the effect's pass list
 * (fx_chain_new): a list of exactly the most passes an effect may have is
 * built and drawn with its last entry a real pass, a longer one, an empty
 * one and one that is no list are refused, entries that are no objects
 * end the build, and an instance that names more passes than the effect
 * has changes nothing. (Under AddressSanitizer a pass written or read
 * outside the table ends the test.) */
static struct FxSet *pass_list_set (const char *list, const char *inst_passes)
{
    put ("shaders/effects/s_0.vert", tpl_vert[0]);
    put ("shaders/effects/s_0.frag", tpl_frag[0]);
    put ("materials/effects/s_0.json", "{\"passes\":[{\"shader\":\"effects/s_0\"}]}");
    char *ej = g_strdup_printf ("{\"fbos\":[{\"name\":\"_rt_a\",\"scale\":1},{\"name\":\"_rt_b\",\"scale\":2}],"
                                "\"passes\":%s}", list);
    put ("effects/s/effect.json", ej);
    g_free (ej);
    char *ij = g_strdup_printf ("{\"effects\":[{\"file\":\"effects/s/effect.json\",\"name\":\"s\","
                                "\"visible\":true,\"passes\":%s}]}", inst_passes);
    JsonNode *root = wpj_parse_data (ij, -1);
    g_free (ij);
    JsonObject *up = json_object_new ();
    struct FxSet *set = root ? fx_build_for_object (tmpdir, wpj_arr (json_node_get_object (root), "effects"),
                                                    "passes", up) : NULL;
    json_object_unref (up);
    if (root)
        json_node_unref (root);
    return set;
}

/* n entries: copies between the two targets, `odd` in place of entry 5,
 * the material pass last */
static char *pass_list (guint n, const char *odd)
{
    GString *l = g_string_new ("[");
    for (guint i = 0; i < n; i++) {
        if (i)
            g_string_append_c (l, ',');
        if (odd && i == 5)
            g_string_append (l, odd);
        else if (i + 1 == n)
            g_string_append (l, "{\"material\":\"materials/effects/s_0.json\"}");
        else
            g_string_append_printf (l, "{\"command\":\"copy\",\"target\":\"%s\",\"source\":\"%s\"}",
                                    i & 1 ? "_rt_a" : "_rt_b", i & 1 ? "_rt_b" : "_rt_a");
    }
    g_string_append_c (l, ']');
    return g_string_free (l, FALSE);
}

static void gl_pass_count_checks (GLuint in)
{
    g_atomic_int_set (&fxs_share_on, 0);
    g_atomic_int_set (&fxs_spec_on, 0);
    g_atomic_int_set (&fxs_verify_on, 0);
    g_atomic_int_set (&fxs_quality, 0);
    guint bad = 0;
    static const guint counts[] = { 1, 2, FX_MAX_PASSES - 1, FX_MAX_PASSES };
    for (guint k = 0; k < G_N_ELEMENTS (counts); k++) {
        char *l = pass_list (counts[k], NULL);
        struct FxSet *set = pass_list_set (l, "[{}]");
        g_free (l);
        gboolean ok = set && set->n == 1 && set->chains[0] && !fx_failed_count (set) &&
                      set->chains[0]->n_passes == (int) counts[k] &&
                      set->chains[0]->passes[counts[k] - 1].program != 0;
        for (guint i = 0; ok && i + 1 < counts[k]; i++)
            ok = set->chains[0]->passes[i].is_copy && !set->chains[0]->passes[i].program;
        if (ok) {
            fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
            for (int d = 0; d < 3; d++)
                fx_run_for_object (set, in, W, H, W, H, d * 0.37, 1.0f, d == 2);
            fx_free_targets_for_object (set);
        }
        bad += !ok;
        if (set)
            fx_free_for_object (set);
        clear_effect_files ();
    }
    char *m = g_strdup_printf ("pass table: effects of 1, 2, %d and %d passes build with the last one a program, and draw",
                               FX_MAX_PASSES - 1, FX_MAX_PASSES);
    result (bad == 0, m);
    g_free (m);

    bad = 0;
    char *over = pass_list (FX_MAX_PASSES + 1, NULL), *far = pass_list (2000, NULL);
    char *odd1 = pass_list (FX_MAX_PASSES, "7"), *odd2 = pass_list (FX_MAX_PASSES, "null");
    char *odd3 = pass_list (FX_MAX_PASSES, "[{\"command\":\"copy\"}]");
    const char *const refused[] = { over, far, "[]", "{}", "7", "\"copy\"", "null", odd1, odd2, odd3 };
    for (guint k = 0; k < G_N_ELEMENTS (refused); k++) {
        struct FxSet *set = pass_list_set (refused[k], "[{}]");
        /* no chain, counted as failed: the layer is drawn without the effect */
        gboolean ok = set && set->n == 1 && !set->chains[0] && fx_failed_count (set) == 1;
        if (ok) {
            fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
            ok = fx_run_for_object (set, in, W, H, W, H, 0.1, 1.0f, FALSE) == in;
        }
        if (!ok)
            printf ("  pass list %u of the refused ones was not refused\n", k);
        bad += !ok;
        if (set)
            fx_free_for_object (set);
        clear_effect_files ();
    }
    g_free (over); g_free (far); g_free (odd1); g_free (odd2); g_free (odd3);
    m = g_strdup_printf ("pass table: %d and 2000 passes, no passes, no list, entries that are no objects: "
                         "no chain, the layer's own picture", FX_MAX_PASSES + 1);
    result (bad == 0, m);
    g_free (m);

    /* the instance names 40 passes, the effect has 3 */
    GString *ip = g_string_new ("[");
    for (int i = 0; i < 40; i++)
        g_string_append_printf (ip, "%s{\"constantshadervalues\":{\"gain\":%d}}", i ? "," : "", i);
    g_string_append_c (ip, ']');
    char *l = pass_list (3, NULL);
    struct FxSet *set = pass_list_set (l, ip->str);
    gboolean ok = set && set->chains[0] && set->chains[0]->n_passes == 3 && !fx_failed_count (set);
    if (ok) {
        fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
        fx_run_for_object (set, in, W, H, W, H, 0.1, 1.0f, FALSE);
        /* what scripts ask for: passes that do not exist */
        float fv[4] = { 0.5f, 0, 0, 0 }, gv[4] = { 0, 0, 0, 0 };
        for (int ps = -2; ps < 45; ps++) {
            fx_set_constant (set, 0, ps, "gain", fv, 1);
            fx_get_constant (set, 0, ps, "gain", gv);
        }
        fx_run_for_object (set, in, W, H, W, H, 0.2, 1.0f, FALSE);
    }
    result (ok, "pass table: an instance with 40 pass entries for an effect of 3, and script constants "
                "of passes that do not exist");
    if (set)
        fx_free_for_object (set);
    clear_effect_files ();
    g_free (l);
    g_string_free (ip, TRUE);
    fx_targets_release ();
}

static void gl_one (guint32 seed, GLuint in)
{
    rs = seed * 2654435761u + 777u;
    /* half of the fx_quality = fast seeds: a run of displacement effects (fx_fuse.h) */
    gboolean family = seed % 6 >= 4 && (seed / 6) % 2 == 0;
    int neff = family ? 2 + (int) rnd (5) : 1 + (int) rnd (3);
    GString *effects = g_string_new ("{\"effects\":[");
    for (int i = 0; i < neff; i++) {
        char *inst = family && rnd (7) ? make_family_effect (i) : make_effect (i);
        g_string_append_printf (effects, "%s%s", i ? "," : "", inst);
        g_free (inst);
    }
    g_string_append (effects, "]}");
    /* the modes: version 0.12's path, sharing, baking unchecked / compared, fast */
    guint mode = seed % 6;
    g_atomic_int_set (&fxs_share_on, mode == 1);
    g_atomic_int_set (&fxs_spec_on, mode >= 2);
    g_atomic_int_set (&fxs_verify_on, mode == 3 || mode == 5);
    g_atomic_int_set (&fxs_quality, mode >= 4);
    JsonNode *root = wpj_parse_data (effects->str, -1);
    g_string_free (effects, TRUE);
    if (!root)
        return;
    JsonObject *up = json_object_new ();
    json_object_set_string_member (up, "opt", "1e999 nan");
    struct FxSet *set = fx_build_for_object (tmpdir, wpj_arr (json_node_get_object (root), "effects"), "hostile", up);
    json_object_unref (up);
    json_node_unref (root);
    if (!set) {
        n_null++;
        return;
    }
    n_built++;
    n_failed_chains += (guint) fx_failed_count (set);
    static const int sizes[][2] = { { W, H }, { 1, 1 }, { 3, 257 }, { W, H }, { 64, 2 } };
    /* with the comparison on, a specialised program takes over behind draw 32 */
    int nd = family ? 40 : mode == 3 || mode == 5 ? 10 : 4;
    for (int d = 0; d < nd; d++) {
        int w = sizes[d % 5][0], h = sizes[d % 5][1];
        fx_set_frame_info (w, h, d == 2 ? NAN : 0.5f, d == 3 ? INFINITY : 0.5f, 1.0f / 30);
        if (rnd (4) == 0)
            fx_set_layer_geometry (0, 0, (float) w, (float) h, rnd (2));
        /* what scripts do between frames */
        if (rnd (3) == 0) {
            float fv[4] = { (float) rnd (5), d == 1 ? 1e30f : 0.5f, -1.0f, 0 };
            fx_set_constant (set, (int) rnd (4) - 1, (int) rnd (4) - 2, rnd (2) ? "gain" : "alpha", fv, 1 + (int) rnd (4));
            fx_set_enabled (set, (int) rnd (4) - 1, rnd (2));
            float gv[4];
            fx_get_constant (set, (int) rnd (4) - 1, (int) rnd (4) - 2, "bounds", gv);
            fx_get_enabled (set, (int) rnd (5) - 1);
            fx_find_effect (set, "e1");
            fx_effect_name (set, (int) rnd (5) - 1);
        }
        fx_run_for_object (set, in, w, h, w, h, d == 4 ? 1e9 : d * 0.37, d == 2 ? 7.9f : 1.0f, FALSE);
        n_draws++;
    }
    fx_free_for_object (set);
}

static void gl_fuzz (guint count, guint32 first)
{
    GLuint in = input_tex ();
    gl_seed_checks (in);
    gl_statement_check (in);
    gl_pass_count_checks (in);
    gint64 t0 = g_get_monotonic_time ();
    guint leaks = 0;
    guint32 first_leak = 0;
    for (guint32 seed = first; seed < first + count; seed++) {
        gl_one (seed, in);
        if (g_getenv ("WALPE_FX_HOSTILE_VERBOSE"))
            fprintf (stderr, "seed %u done\n", seed);
        /* the set is freed: nothing of it may be left in this thread's caches */
        if (((fxs_progs && g_hash_table_size (fxs_progs)) || (fxs_srcs && g_hash_table_size (fxs_srcs))) &&
            !leaks++)
            first_leak = seed;
        /* each seed has its own effect files */
        char *e = g_build_filename (tmpdir, "effects", NULL), *m = g_build_filename (tmpdir, "materials", NULL);
        char *s = g_build_filename (tmpdir, "shaders", "effects", NULL);
        rm_rf (e);
        rm_rf (m);
        rm_rf (s);
        g_free (e); g_free (m); g_free (s);
    }
    glDeleteTextures (1, &in);
    fx_targets_release ();
    int fgroups = 0, fmembers = 0, verified = 0, rejected = 0, baked = 0, shared = 0, identity = 0;
    fx_fuse_stats (&fgroups, &fmembers);
    fx_verify_stats (&verified, &rejected);
    fx_program_stats (NULL, &shared, NULL, NULL, &baked, &identity, FALSE);
    char *m = g_strdup_printf ("executor: %u hostile effect sets (seeds %u..%u): %u built (%u chains failed inside), "
                               "%u refused whole, %u draws, %.1f s; programs shared %d, constants baked %d, "
                               "compared %d + %d rejected, identity passes %d, fused runs %d (%d members)",
                               count, first, first + count - 1, n_built, n_failed_chains, n_null, n_draws,
                               (g_get_monotonic_time () - t0) / 1e6, shared, baked, verified, rejected, identity,
                               fgroups, fmembers);
    result (TRUE, m);
    g_free (m);
    m = g_strdup_printf ("executor: program and source caches are empty after every set is freed "
                         "(%u seeds left something, first %u)", leaks, first_leak);
    result (!leaks, m);
    g_free (m);
    gint64 bytes = 0;
    int cnt = 0;
    fx_targets_stats (&bytes, &cnt);
    m = g_strdup_printf ("executor: no effect target is left (%d, %" G_GINT64_FORMAT " bytes)", cnt, bytes);
    result (cnt == 0, m);
    g_free (m);
}

static gboolean gl_run (EGLDisplay dpy, guint count, guint32 first)
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
    fxs_enabled ();
    gl_fuzz (count, first);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

int main (int argc, char **argv)
{
    g_unsetenv ("WALPE_FX_SHARE");
    g_unsetenv ("WALPE_FX_SPEC");
    g_unsetenv ("WALPE_FX_QUALITY");
    g_unsetenv ("WALPE_FX_SPEC_VERIFY");
    g_unsetenv ("WALPE_FX_COST");
    const char *only = argc > 1 && strcmp (argv[1], "all") ? argv[1] : NULL;
    guint count = argc > 2 ? (guint) atoi (argv[2]) : 0;
    guint32 first = argc > 3 ? (guint32) atoi (argv[3]) : 1;
    if (!g_getenv ("WALPE_FX_HOSTILE_VERBOSE"))
        g_log_set_default_handler (quiet_log, NULL);
    tmpdir = g_dir_make_tmp ("walpe-fxhostile-XXXXXX", NULL);
    if (!tmpdir) {
        printf ("skip: no temporary directory\n");
        return 0;
    }
    put ("shaders/common.h", COMMON_H);
    put ("shaders/self.h", "#include \"self.h\"\n#include \"./self.h\"\nfloat self() { return 0.0; }\n");
    seeds_init ();
    load_corpus ();
    if (!only || !strcmp (only, "scan"))
        time_scan_checks ();
    if (!only || !strcmp (only, "include"))
        include_checks ();
    if (!only || !strcmp (only, "combos"))
        combo_checks ();
    if (!only || !strcmp (only, "annotations"))
        annotation_checks ();
    if (!only || !strcmp (only, "repairsize"))
        repair_size_checks ();
    if (!only || !strcmp (only, "fuse"))
        fuse_checks ();
    if (!only || !strcmp (only, "statement"))
        statement_checks ();
    if (!only || !strcmp (only, "text")) {
        seed_checks ();
        text_fuzz (count ? count : 600, first);
    }
    if (!only || !strcmp (only, "gl")) {
        int ran = 0;
        if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
            ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL),
                           count ? count : 120, first);
        if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
            epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
            EGLDeviceEXT dev[8];
            EGLint nd = 0;
            if (eglQueryDevicesEXT (8, dev, &nd))
                for (EGLint i = 0; i < nd && !ran; i++)
                    ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL),
                                   count ? count : 120, first);
        }
        if (!ran)
            printf ("skip GL checks (no EGL device / surfaceless platform)\n");
    }
    rm_rf (tmpdir);
    g_free (tmpdir);
    g_ptr_array_unref (corpus_v);
    g_ptr_array_unref (corpus_f);
    seeds_free ();
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
