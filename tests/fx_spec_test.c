/* Effect program specialisation check (src/engine/fx_specialize.h,
 * fx_rewrite.h).
 *
 *   fx-spec-test
 *
 * 1. No GL: declarations that are / are not baked, the constant text GLSL
 *    reads back to the same 32 bits, line count and annotations kept,
 *    statement replacement that leaves #if lines alone, the frame-constant
 *    check (a run that reads a varying is refused), kernel weights of the
 *    bilinear blur.
 * 2. With an EGL device or Mesa's surfaceless platform (skipped without):
 *    a small effect written to a temporary project, through the public
 *    executor API. Specialised and generic programs must draw the same
 *    picture (maximum difference 1 of 255); a baked constant is still
 *    readable and writable by scripts (the write relinks the pass once);
 *    script-bound constants stay uniforms; identical passes share one
 *    program and still draw their own constants; an opacity-1 pass behind
 *    another effect is skipped without changing a pixel.
 * 3. With the Wallpaper Engine assets as well (WALPE_WE_ASSETS or the
 *    configured / detected install; skipped without): WE's own pulse,
 *    shake, water waves, opacity, blur, shine and god rays effects, each
 *    drawn generic, specialised (must match within 1 of 255) and with
 *    fx_quality = fast (PSNR reported, must stay above 30 dB). On a
 *    machine with a GPU this is the check of everything the
 *    specialisation rewrites on the real driver.
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
    printf ("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? "  [" : "", detail ? detail : "");
    if (detail)
        printf ("]\n");
    if (!ok)
        fails++;
}

#define HEAD "#version 330\n#define MASK 0\n#define GLSL 1\n#define frac fract\n" \
             "#define saturate(x) clamp(x, 0.0, 1.0)\n#define mul(x, y) ((y) * (x))\n" \
             "#define CAST3(x) (vec3(x))\n#define CAST4(x) (vec4(x))\n#define texSample2D texture\n"
#define VS_HEAD HEAD "#define varying out\n#define attribute in\n#line 1\n"
#define FS_HEAD HEAD "#define varying in\nout vec4 _we_fragColor;\n#define gl_FragColor _we_fragColor\n#line 1\n"

/* ------------------------------------------------------------- no GL */

static int count_lines (const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

static void decl (const char *line, int want_type, const char *want_name)
{
    const char *nb = NULL, *ne = NULL;
    gsize nl = 0;
    int t = fxs_decl_at (line, &nb, &nl, &ne);
    gboolean ok = t == want_type &&
                  (!want_type || (nl == strlen (want_name) && !strncmp (nb, want_name, nl)));
    char *w = g_strdup_printf ("declaration: %s", line);
    result (ok, w, NULL);
    g_free (w);
}

static void source_checks (void)
{
    decl ("uniform float g_Speed; // {\"material\":\"speed\"}", FXS_T_FLOAT, "g_Speed");
    decl ("\tuniform vec2 g_Scale;", FXS_T_VEC2, "g_Scale");
    decl ("uniform float3 g_Color ;", FXS_T_VEC3, "g_Color");
    decl ("uniform lowp vec4 g_Rect;", FXS_T_VEC4, "g_Rect");
    decl ("uniform int g_Count;", FXS_T_INT, "g_Count");
    decl ("uniform bool g_On;", FXS_T_BOOL, "g_On");
    decl ("uniform float g_AudioSpectrum16Left[16];", 0, NULL);
    decl ("uniform float g_A, g_B;", 0, NULL);
    decl ("uniform float g_Init = 1.0;", 0, NULL);
    decl ("uniform sampler2D g_Texture0;", 0, NULL);
    decl ("uniform mat4 g_ModelViewProjectionMatrix;", 0, NULL);
    decl ("varying float g_Speed;", 0, NULL);
    decl ("uniform floatx g_Speed;", 0, NULL);

    /* a float survives print + parse bit for bit */
    static const float vals[] = { 0.1f, 1.0f, -3.0f, 0.60000002f, 1e-7f, 123456.789f, 5.4200001f,
                                  0.33333334f, 16777216.0f, 1.57079632679f, 0.0f, 2.5e10f };
    gboolean same = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS (vals); i++) {
        GString *o = g_string_new (NULL);
        fxs_put_float (o, vals[i]);
        float back = (float) g_ascii_strtod (o->str, NULL);
        same = same && !memcmp (&back, &vals[i], sizeof (float)) && strpbrk (o->str, ".eE") != NULL;
        g_string_free (o, TRUE);
    }
    result (same, "constants print to text that reads back to the same float, with a decimal point", NULL);

    /* baking: declaration replaced in place, the rest of the file untouched */
    struct FxSpec sp = { 0 };
    FxsConst c[3] = { { "g_Speed", "speed", FXS_T_FLOAT, 1, 0, 1, 1, { 2.5f, 0, 0, 1 }, 0, { 0 }, 0 },
                      { "g_Tint", "tint", FXS_T_VEC3, 3, 0, 1, 1, { 1, 0.5f, 0.25f, 1 }, 0, { 0 }, 0 },
                      { "g_Free", "free", FXS_T_FLOAT, 1, 1, 0, 1, { 7, 0, 0, 1 }, 0, { 0 }, 0 } };
    sp.c = c;
    sp.n = 3;
    const char *src = FS_HEAD
        "uniform float g_Speed; // {\"material\":\"speed\",\"default\":1}\n"
        "#if MASK\nuniform vec3 g_Tint; // {\"material\":\"tint\"}\n#else\nuniform vec3 g_Tint;\n#endif\n"
        "uniform float g_Free;\nvoid main() { gl_FragColor = vec4(g_Tint * g_Speed, g_Free); }\n";
    char *b = fxs_bake_source (src, &sp);
    result (strstr (b, "const float g_Speed = 2.5; // {\"material\":\"speed\",\"default\":1}\n") != NULL,
            "baking keeps the annotation behind the declaration", NULL);
    result (strstr (b, "const vec3 g_Tint = vec3(1.0, 0.5, 0.25); // {\"material\":\"tint\"}") != NULL &&
            strstr (b, "#else\nconst vec3 g_Tint = vec3(1.0, 0.5, 0.25);\n#endif") != NULL,
            "baking rewrites the declaration in both #if branches", NULL);
    result (strstr (b, "uniform float g_Free;") != NULL, "a constant marked dynamic stays a uniform", NULL);
    result (count_lines (b) == count_lines (src), "baking keeps the line count", NULL);
    g_free (b);
    {
        /* constants named in a pow() are left to the GPU */
        struct FxSpec hp = { 0 };
        FxsConst hc[4] = { { "g_Gain", "gain", FXS_T_FLOAT, 1, 0, 0, 1, { 2, 0, 0, 1 }, 0, { 0 }, 0 },
                           { "g_Exp", "exp", FXS_T_FLOAT, 1, 0, 0, 1, { 1, 0, 0, 1 }, 0, { 0 }, 0 },
                           { "g_Tint", "tint", FXS_T_VEC3, 3, 0, 0, 1, { 1, 1, 1, 1 }, 0, { 0 }, 0 },
                           { "g_Off", "off", FXS_T_FLOAT, 1, 0, 0, 1, { 3, 0, 0, 1 }, 0, { 0 }, 0 } };
        hp.c = hc;
        hp.n = 4;
        const char *hs = FS_HEAD
            "uniform float g_Gain;\nuniform float g_Exp;\nuniform vec3 g_Tint;\nuniform float g_Off;\n"
            "float mypow(float x) { return x; }\n"
            "void main() {\n\tvec3 c = vec3(0.5);\n\t// pow(c, g_Tint) in a comment\n"
            "#if MASK\n\tc = pow(c, g_Tint);\n#endif\n"
            "\tc = pow (max(c * g_Gain, vec3(0.0)),\n\t\tvec3(g_Exp)) * g_Tint;\n"
            "\tgl_FragColor = vec4(c, mypow(g_Off));\n}\n";
        fxs_hazards (&hp, VS_HEAD "void main() { }\n", hs);
        result (hc[0].hazard && hc[1].hazard && !hc[2].hazard && !hc[3].hazard && !fxs_bakeable (&hc[0]) &&
                fxs_bakeable (&hc[2]),
                "constants named inside a pow() stay uniforms (not in a comment, a dead #if branch or another function)",
                NULL);
    }

    /* statement replacement across a preprocessor line */
    char *r = g_strdup (FS_HEAD "void main() {\n#if MASK\n\tfloat a = 2.0;\n#else\n\tfloat a = 1.0; // one\n"
                                "\tfloat b = a * 2.0;\n#endif\n\tfloat c = b + 1.0;\n\tgl_FragColor = vec4(c);\n}\n");
    int nl = count_lines (r);
    gboolean ok = fxs_replace (&r, "floatb=a*2.0;floatc=b+1.0;", "float c = _k;");
    result (ok && strstr (r, "float c = _k;\n#endif\n") != NULL && count_lines (r) == nl,
            "replacement keeps #endif and the line count", r + strlen (FS_HEAD));
    g_free (r);
    /* a dead branch between two compiled statements goes with them */
    r = g_strdup (FS_HEAD "void main() {\n\tfloat a = 1.0;\n#if MASK\n\tfloat b = a * 3.0;\n#else\n"
                          "\tfloat b = a * 2.0;\n#endif\n\tfloat c = b + 1.0;\n\tgl_FragColor = vec4(c);\n}\n");
    nl = count_lines (r);
    ok = fxs_replace (&r, "floata=1.0;floatb=a*2.0;floatc=b+1.0;", "float c = _k;");
    result (ok && strstr (r, "float c = _k;\n#if MASK\n\n#else\n\n#endif\n") != NULL && count_lines (r) == nl,
            "replacement spans a dead #if branch, directives stay", r + strlen (FS_HEAD));
    g_free (r);
    r = g_strdup (FS_HEAD "void main() {\n\tfloat a = 1.0;\n\tfloat a2 = 1.0;\n}\n");
    result (!fxs_replace (&r, "=1.0;", "x") && strstr (r, "float a2 = 1.0;") != NULL,
            "a pattern that occurs twice replaces nothing", NULL);
    g_free (r);

    /* frame-constant check */
    const char *fl = "uniform float g_Time;\nconst float g_K = 2.0;\nuniform sampler2D g_Texture1;\n"
                     "varying vec2 v_TexCoord;\n";
    GPtrArray *un = g_ptr_array_new_with_free_func (g_free), *mac = g_ptr_array_new_with_free_func (g_free);
    ok = fxs_hoist_check ("x = sin(g_Time * g_K) * 0.5 + M_X; float n = textureLod(g_Texture1, vec2(g_Time), 0.0).r; x += n;",
                          "x", fl, "", "#define M_X 3.0\n", un, mac);
    result (ok && un->len == 3 && mac->len == 1, "a run of uniforms, constants, macros and own locals is frame-constant", NULL);
    g_ptr_array_set_size (un, 0);
    g_ptr_array_set_size (mac, 0);
    result (!fxs_hoist_check ("x = sin(g_Time) * v_TexCoord.x;", "x", fl, "", "", un, mac),
            "a run that reads a varying is not", NULL);
    result (!fxs_hoist_check ("x = sin(g_Time) * outside;", "x", fl, "", "", un, mac),
            "a run that reads a local from outside is not", NULL);
    result (!fxs_hoist_check ("x = gl_FragCoord.x;", "x", fl, "", "", un, mac),
            "a run that reads gl_FragCoord is not", NULL);
    g_ptr_array_unref (un);
    g_ptr_array_unref (mac);
    GString *t = g_string_new ("float n = texSample2D(g_Texture1, vec2(g_Time, 1.0) * g_K).r;");
    ok = fxs_hoist_tex (t) && !strcmp (t->str, "float n = textureLod(g_Texture1, vec2(g_Time, 1.0) * g_K, 0.0).r;");
    result (ok, "a texture read in a hoisted run reads the base level", t->str);
    g_string_free (t, TRUE);

    /* bilinear blur: the weights of each replacement in fxs_rw_blur () sum
     * to one (centre + twice the three pairs; the four reads; the two
     * halves). That each pair's weight and offset follow from the kernel
     * it replaces is checked against a real install, see fx_keys_test.c */
    double s13 = 0.171834 + 2 * (0.275763 + 0.114722 + 0.023597);
    double s7 = 0.202817 + 0.404486 + 0.321393 + 0.071303;
    ok = fabs (s13 - 1.0) < 2e-5 && fabs (s7 - 1.0) < 2e-5 && 2 * 0.5 == 1.0;
    result (ok, "blur kernels as bilinear reads: the merged weights sum to one", NULL);
}

/* ---------------------------------------------------------------- GL */

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

static GLuint input_tex (void)
{
    static guint8 px[W * H * 4];
    guint32 s = 12345;
    for (int i = 0; i < W * H * 4; i++) {
        s = s * 1664525u + 1013904223u;
        /* smooth-ish colour with noise, alpha between 64 and 255 */
        int x = (i / 4) % W, y = (i / 4) / W;
        int base = (i % 4) == 3 ? 64 + (x * 3) % 192 : (x * 4 + y * 5 + (i % 4) * 40) % 256;
        px[i] = (guint8) CLAMP (base + (int) ((s >> 24) % 32) - 16, 0, 255);
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

static gpointer build_set (const char *dir, const char *effects_json)
{
    /* wpj_parse_data takes objects only */
    char *wrapped = g_strdup_printf ("{\"effects\":%s}", effects_json);
    JsonNode *root = wpj_parse_data (wrapped, -1);
    g_free (wrapped);
    if (!root)
        return NULL;
    gpointer set = fx_build_for_object (dir, wpj_arr (json_node_get_object (root), "effects"), "test", NULL);
    json_node_unref (root);
    return set;
}

static void read_tex (GLuint tex, guint8 *out)
{
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

/* draw the set over the input at time t; pixels out */
static gboolean draw (gpointer set, GLuint in, double t, guint8 *out)
{
    fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
    GLuint o = fx_run_for_object (set, in, W, H, W, H, t, 1.0f, FALSE);
    read_tex (o, out);
    return o != in;
}

static int max_diff (const guint8 *a, const guint8 *b, double *psnr)
{
    int m = 0;
    double se = 0;
    for (int i = 0; i < W * H * 4; i++) {
        int d = abs ((int) a[i] - (int) b[i]);
        m = MAX (m, d);
        se += (double) d * d;
    }
    if (psnr)
        *psnr = se > 0 ? 10.0 * log10 (255.0 * 255.0 * W * H * 4 / se) : 99.0;
    return m;
}

/* effects_json drawn at two times with (spec, fast) settings */
static gboolean render (const char *dir, const char *effects_json, gboolean spec, gboolean fast, GLuint in,
                        guint8 *out0, guint8 *out1, guint *rewrites)
{
    g_atomic_int_set (&fxs_spec_on, spec);
    g_atomic_int_set (&fxs_quality, fast);
    struct FxSet *set = build_set (dir, effects_json);
    g_atomic_int_set (&fxs_spec_on, 1);
    g_atomic_int_set (&fxs_quality, 0);
    if (!set)
        return FALSE;
    if (rewrites) {
        *rewrites = 0;
        for (int i = 0; i < set->n; i++)
            for (int p = 0; set->chains[i] && p < set->chains[i]->n_passes; p++)
                if (set->chains[i]->passes[p].spec)
                    *rewrites |= set->chains[i]->passes[p].spec->rewrites;
    }
    draw (set, in, 0.7, out0);
    draw (set, in, 3.1, out1);
    gboolean ok = fx_failed_count (set) == 0;
    fx_free_for_object (set);
    return ok;
}

/* effects_json built with the verification on (the default): 32 draws
 * with the generic programs, the specialised candidates compared behind
 * six of them; pictures of the draws after that, at the two times of
 * render(). n_spec / n_pass: passes that draw with baked constants by
 * then / all passes. */
static gboolean render_verified (const char *dir, const char *effects_json, GLuint in, guint8 *out0,
                                 guint8 *out1, int *n_spec, int *n_pass)
{
    static guint8 first[W * H * 4];
    int was = g_atomic_int_get (&fxs_verify_on);
    g_atomic_int_set (&fxs_verify_on, 1);
    struct FxSet *set = build_set (dir, effects_json);
    if (!set) {
        g_atomic_int_set (&fxs_verify_on, was);
        return FALSE;
    }
    for (int i = 0; i < 32; i++)
        draw (set, in, 0.11 * i, first);
    draw (set, in, 0.7, out0);
    draw (set, in, 3.1, out1);
    *n_spec = *n_pass = 0;
    for (int i = 0; i < set->n; i++)
        for (int p = 0; set->chains[i] && p < set->chains[i]->n_passes; p++) {
            struct FxSpec *sp = set->chains[i]->passes[p].spec;
            if (set->chains[i]->passes[p].is_copy)
                continue;
            *n_pass += 1;
            *n_spec += sp && sp->n_baked > 0;
        }
    gboolean ok = fx_failed_count (set) == 0;
    fx_free_for_object (set);
    g_atomic_int_set (&fxs_verify_on, was);
    return ok;
}

static const char *gain_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec2 v_TexCoord;\nvoid main() {\n"
    "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n";
static const char *gain_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"1 1 1\"}\n"
    "uniform float g_Exp; // {\"material\":\"exponent\",\"default\":1}\n"
    "uniform float g_Unused; // {\"material\":\"unused\",\"default\":3}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb = mix(c.rgb, c.rgb * g_Gain, g_Exp) * g_Tint;\n\tgl_FragColor = c;\n}\n";

/* frame-constant work in the fragment shader: a pulse value made from the
 * time, a pair of thresholds and a noise read, the same for every pixel.
 * The hoist recogniser (fxs_hoists, src/engine/fx_rewrite.h) finds such a
 * run by the keys of the start of its first statement and of its last
 * statement (fx_keys.h) and carries the result out under the name its
 * table has for it (pulse). The statements are this test's own: own_keys ()
 * registers their keys in the engine's slots */
#define WAVE_FIRST "pulse = 0.5 + 0.5 * sin("
#define WAVE_LAST "pulse = pow(max(pulse, 0.0), g_Curve);"
static const char *wave_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nuniform float g_Time;\nattribute vec3 a_Position;\n"
    "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvoid main() {\n"
    "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n";
static const char *wave_frag =
    "uniform float g_Time;\n"
    "uniform vec2 g_Gate; // {\"material\":\"bounds\",\"default\":\"0 1\"}\n"
    "uniform float g_Curve; // {\"material\":\"power\",\"default\":1}\n"
    "uniform float g_Rate; // {\"material\":\"speed\",\"default\":3}\n"
    "uniform float g_Shift; // {\"material\":\"phase\",\"default\":0}\n"
    "uniform float g_Depth; // {\"material\":\"amount\",\"default\":1}\n"
    "uniform float g_GrainRate; // {\"material\":\"noisespeed\",\"default\":0.5}\n"
    "uniform float g_GrainDepth; // {\"material\":\"noiseamount\",\"default\":0}\n"
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture1; // {\"default\":\"masks/noise\"}\nvarying vec2 v_TexCoord;\n\n"
    "void main() {\n    float pulse = 1.0;\n"
    "    " WAVE_FIRST "g_Shift + g_Rate * g_Time);\n"
    "    pulse = smoothstep(g_Gate.x, g_Gate.y, pulse);\n"
    "    float grain = g_GrainDepth * texSample2D(g_Texture1, g_GrainRate * g_Time * vec2(0.25, 0.125)).r;\n"
    "    pulse = pulse * g_Depth + grain;\n"
    "    " WAVE_LAST "\n"
    "    vec4 c = texSample2D(g_Texture0, v_TexCoord);\n    gl_FragColor = vec4(c.rgb * pulse, c.a);\n}\n";

/* an effect the identity recogniser accepts: one multiply of the alpha by
 * the user's value. fxs_identity (src/engine/fx_rewrite.h) knows the main
 * function of such a fragment shader by its key (fx_keys.h) and looks for
 * the two plain statements in the vertex code. The main function is this
 * test's own: own_keys () registers its key in the engine's slot */
#define OP_MAIN "void main() {\n  vec4 px = texSample2D(g_Texture0, v_TexCoord.xy);\n" \
                "  px.a = px.a * g_UserAlpha;\n  gl_FragColor = px;\n}\n"
static const char *op_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec4 v_TexCoord;\n\nvoid main() {\n  v_TexCoord.zw = vec2(0.0);\n  v_TexCoord.xy = a_TexCoord;\n"
    "  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n}\n";
static const char *op_frag =
    "uniform float g_UserAlpha; // {\"material\":\"alpha\",\"default\":1}\n"
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\nvarying vec4 v_TexCoord;\n\n" OP_MAIN;

/* the keys of this test's own statements, in the engine's slots */
static void own_keys (void)
{
    fxk_test_register (FXK_PULSE_FIRST, WAVE_FIRST);
    fxk_test_register (FXK_PULSE_LAST, WAVE_LAST);
    fxk_test_register (FXK_OPACITY_MAIN, OP_MAIN);
}

/* two displacement effects of different make: a sine wave along x with a
 * mask, and a time-driven offset scaled by a second texture */
static const char *wv_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nuniform vec4 g_Texture1Resolution;\nuniform float g_Angle; // {\"material\":\"angle\",\"default\":0}\n"
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\nvarying vec2 v_Dir;\n"
    "void main() {\n    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "\tv_TexCoord = a_TexCoord.xyxy;\n\tv_Dir = vec2(cos(g_Angle), sin(g_Angle));\n}\n";
static const char *wv_frag =
    "varying vec4 v_TexCoord;\nvarying vec2 v_Dir;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture1; // {\"default\":\"masks/noise\"}\nuniform float g_Time;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.02}\n"
    "uniform float g_Freq; // {\"material\":\"freq\",\"default\":9}\n"
    "float wave(float x) { return sin(x); }\n"
    "void main() {\n\tfloat weight = texSample2D(g_Texture1, v_TexCoord.zw).r;\n"
    "\tvec2 at = v_TexCoord.xy;\n"
    "\tat += v_Dir * wave(dot(at, v_Dir.yx) * g_Freq + g_Time) * g_Amp * weight;\n"
    "\tgl_FragColor = texSample2D(g_Texture0, at);\n}\n";
static const char *sh_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec2 v_TexCoord;\nvoid main() {\n"
    "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n";
static const char *sh_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture1; // {\"default\":\"masks/noise\"}\nuniform float g_Time;\n"
    "uniform vec2 g_Amp; // {\"material\":\"amp\",\"default\":\"0.01 0.02\"}\n"
    "void main() {\n\tvec2 flow = texSample2D(g_Texture1, v_TexCoord * 0.5).rg - vec2(0.5);\n"
    "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord + flow * g_Amp * sin(g_Time * 2.0));\n}\n";
/* not a pure displacement: mixes two reads of the input */
static const char *mx_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nuniform float g_Time;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.02}\n"
    "void main() {\n\tvec4 a = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tgl_FragColor = mix(a, texSample2D(g_Texture0, v_TexCoord + vec2(g_Amp * sin(g_Time))), 0.5);\n}\n";

/* leans on what the GPU does with a division by zero: (x + 0.25) / 0 is
 * +inf at run time, clamped to 1, so with the default the picture passes
 * unchanged. A compiler that sees the zero as a constant may make
 * anything of it. */
static const char *hz_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Div; // {\"material\":\"div\",\"default\":0}\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tfloat t = clamp((c.r + 0.25) / g_Div, 0.0, 1.0);\n\tc.rgb *= t * g_Gain;\n\tgl_FragColor = c;\n}\n";

/* a displacement whose constant sits in a pow(): a uniform in its own
 * program, a constant in a fused pass */
static const char *pw_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nuniform float g_Time;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.02}\n"
    "uniform float g_Exp; // {\"material\":\"exp\",\"default\":2}\n"
    "void main() {\n\tfloat w = pow(abs(sin(v_TexCoord.y * 11.0 + g_Time)), g_Exp);\n"
    "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord + vec2(g_Amp * w, 0.0));\n}\n";

/* a smooth picture: what displacement effects are used on */
static GLuint smooth_tex (void)
{
    static guint8 px[W * H * 4];
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

/* passes drawn by one frame of effects_json built with (spec, fast) */
static guint passes_of (const char *dir, const char *effects_json, gboolean fast, GLuint in, guint8 *out,
                        int *groups)
{
    g_atomic_int_set (&fxs_quality, fast);
    struct FxSet *set = build_set (dir, effects_json);
    g_atomic_int_set (&fxs_quality, 0);
    if (!set)
        return 0;
    if (groups)
        *groups = set->fuse ? set->fuse->n : 0;
    guint r0 = 0, r1 = 0;
    draw (set, in, 0.4, out);       /* warm: targets, uniforms */
    fx_pass_stats (&r0, NULL, FALSE);
    draw (set, in, 1.9, out);
    fx_pass_stats (&r1, NULL, FALSE);
    fx_free_for_object (set);
    return r1 - r0;
}

static void gl_fuse_checks (void)
{
    static guint8 a[W * H * 4], b[W * H * 4];
    put_effect ("wv", wv_vert, wv_frag);
    put_effect ("sh", sh_vert, sh_frag);
    put_effect ("mx", sh_vert, mx_frag);
    GLuint in = smooth_tex ();
    const char *run3 = "[{\"file\":\"effects/wv/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":0.03,\"angle\":0.4}}]},"
                       "{\"file\":\"effects/sh/effect.json\"},"
                       "{\"file\":\"effects/wv/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":0.02,\"freq\":14,\"angle\":2.0}}]}]";
    int groups = -1;
    double psnr = 0;
    guint seq = passes_of (tmpdir, run3, FALSE, in, a, NULL), fus = passes_of (tmpdir, run3, TRUE, in, b, &groups);
    int md = max_diff (a, b, &psnr);
    char *d = g_strdup_printf ("passes %u -> %u, groups %d, max difference %d, PSNR %.1f dB", seq, fus, groups, md, psnr);
    result (seq == 3 && fus == 1 && groups == 1 && psnr >= 30.0,
            "fast: a run of three displacement effects is one pass, picture close to the sequential one", d);
    g_free (d);
    /* an effect that is no pure displacement splits the run */
    const char *split = "[{\"file\":\"effects/wv/effect.json\"},{\"file\":\"effects/sh/effect.json\"},"
                        "{\"file\":\"effects/mx/effect.json\"},"
                        "{\"file\":\"effects/sh/effect.json\"},{\"file\":\"effects/wv/effect.json\"}]";
    seq = passes_of (tmpdir, split, FALSE, in, a, NULL);
    fus = passes_of (tmpdir, split, TRUE, in, b, &groups);
    md = max_diff (a, b, &psnr);
    d = g_strdup_printf ("passes %u -> %u, groups %d, max difference %d, PSNR %.1f dB", seq, fus, groups, md, psnr);
    result (seq == 5 && fus == 3 && groups == 2 && psnr >= 30.0,
            "fast: an effect that mixes two reads of its input stays its own pass between two fused runs", d);
    g_free (d);
    /* script-bound constant: that effect is not fused */
    const char *bound = "[{\"file\":\"effects/wv/effect.json\"},"
                        "{\"file\":\"effects/sh/effect.json\",\"passes\":[{\"constantshadervalues\":"
                        "{\"amp\":{\"script\":\"x\",\"value\":\"0.01 0.02\"}}}]},"
                        "{\"file\":\"effects/wv/effect.json\"}]";
    fus = passes_of (tmpdir, bound, TRUE, in, b, &groups);
    result (fus == 3 && groups == 0, "fast: an effect with a script-bound constant is not fused", NULL);
    /* switched off, written to: back to single passes, same picture as sequential */
    struct FxSet *ref = build_set (tmpdir, run3);
    g_atomic_int_set (&fxs_quality, 1);     /* stays on while `set` is alive, as the setting does */
    struct FxSet *set = build_set (tmpdir, run3);
    guint r0 = 0, r1 = 0;
    fx_set_enabled (set, 1, FALSE);
    fx_set_enabled (ref, 1, FALSE);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r0, NULL, FALSE);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r1, NULL, FALSE);
    draw (ref, in, 1.9, a);
    md = max_diff (a, b, NULL);
    d = g_strdup_printf ("passes %u, max difference %d", r1 - r0, md);
    result (r1 - r0 == 2 && md <= 1, "fast: a member switched off ends the fusion (the others run one by one)", d);
    g_free (d);
    fx_set_enabled (set, 1, TRUE);
    fx_set_enabled (ref, 1, TRUE);
    float nv = 0.05f;
    fx_set_constant (set, 0, 0, "amp", &nv, 1);
    fx_set_constant (ref, 0, 0, "amp", &nv, 1);
    fx_pass_stats (&r0, NULL, FALSE);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r1, NULL, FALSE);
    draw (ref, in, 1.9, a);
    md = max_diff (a, b, NULL);
    d = g_strdup_printf ("passes %u, max difference %d", r1 - r0, md);
    result (r1 - r0 == 3 && md <= 1,
            "fast: the frame in which a script writes a member's constant draws the run effect by effect", d);
    g_free (d);
    fx_pass_stats (&r0, NULL, FALSE);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r1, NULL, FALSE);
    draw (ref, in, 1.9, a);
    md = max_diff (a, b, &psnr);
    d = g_strdup_printf ("passes %u, groups %d, max difference %d, PSNR %.1f dB", r1 - r0, set->fuse ? set->fuse->n : 0,
                         md, psnr);
    result (r1 - r0 == 2 && set->fuse && set->fuse->n == 1 && psnr >= 30.0,
            "fast: ... after it that effect is its own pass and the two others are fused again", d);
    g_free (d);
    fx_free_for_object (set);
    g_atomic_int_set (&fxs_quality, 0);
    fx_free_for_object (ref);
    /* a member whose constant is named in a pow() */
    put_effect ("pw", sh_vert, pw_frag);
    const char *pwrun = "[{\"file\":\"effects/pw/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":0.03}}]},"
                        "{\"file\":\"effects/wv/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":0.02}}]}]";
    ref = build_set (tmpdir, pwrun);
    g_atomic_int_set (&fxs_quality, 1);
    set = build_set (tmpdir, pwrun);
    EffPass *pp = set ? &set->chains[0]->passes[0] : NULL;
    draw (set, in, 1.9, b);
    fx_pass_stats (&r0, NULL, FALSE);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r1, NULL, FALSE);
    draw (ref, in, 1.9, a);
    md = max_diff (a, b, &psnr);
    d = g_strdup_printf ("passes %u, groups %d, max difference %d, PSNR %.1f dB", r1 - r0, set->fuse ? set->fuse->n : 0, md, psnr);
    result (pp && glGetUniformLocation (pp->program, "g_Exp") >= 0 && glGetUniformLocation (pp->program, "g_Amp") < 0 &&
            r1 - r0 == 1 && set->fuse && set->fuse->n == 1 && psnr >= 30.0,
            "fast: an effect whose constant is named in a pow() keeps it a uniform but is fused all the same", d);
    g_free (d);
    nv = 3.0f;
    fx_set_constant (set, 0, 0, "exp", &nv, 1);
    fx_set_constant (ref, 0, 0, "exp", &nv, 1);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r0, NULL, FALSE);
    draw (set, in, 1.9, b);
    fx_pass_stats (&r1, NULL, FALSE);
    draw (ref, in, 1.9, a);
    md = max_diff (a, b, NULL);
    d = g_strdup_printf ("passes %u, groups %d, max difference %d", r1 - r0, set->fuse ? set->fuse->n : 0, md);
    result (r1 - r0 == 2 && set->fuse && set->fuse->n == 0 && md <= 1,
            "fast: ... until a script writes that constant: then the effect leaves the run", d);
    g_free (d);
    fx_free_for_object (set);
    g_atomic_int_set (&fxs_quality, 0);
    fx_free_for_object (ref);
    glDeleteTextures (1, &in);
}

static void pstats (int *compiled, int *shared, int *respec, int *fallback)
{
    fx_program_stats (compiled, shared, respec, fallback, NULL, NULL, FALSE);
}

static void gl_spec_checks (void)
{
    static guint8 a0[W * H * 4], a1[W * H * 4], b0[W * H * 4], b1[W * H * 4];
    GError *err = NULL;
    tmpdir = g_dir_make_tmp ("walpe-fxspec-XXXXXX", &err);
    if (!tmpdir) {
        result (FALSE, "temporary project", err ? err->message : NULL);
        return;
    }
    put_effect ("gain", gain_vert, gain_frag);
    put_effect ("wave", wave_vert, wave_frag);
    put_effect ("opacity", op_vert, op_frag);
    {
        /* a 4x4 noise picture for the wave effect's second sampler */
        guint8 n[16 * 4];
        for (int i = 0; i < 64; i++)
            n[i] = (guint8) ((i * 73 + 41) % 251);
        GdkPixbuf *pb = gdk_pixbuf_new_from_data (n, GDK_COLORSPACE_RGB, TRUE, 8, 4, 4, 16, NULL, NULL);
        char *p = g_build_filename (tmpdir, "materials/masks/noise.png", NULL), *d = g_path_get_dirname (p);
        g_mkdir_with_parents (d, 0700);
        gdk_pixbuf_save (pb, p, "png", NULL, NULL);
        g_object_unref (pb);
        g_free (d);
        g_free (p);
    }
    GLuint in = input_tex ();
    const char *gain = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                       "{\"gain\":0.5,\"tint\":\"1 0.5 0.25\",\"exponent\":1.0}}]}]";
    int c0, s0, r0, f0, c1, s1, r1, f1;

    /* --- baked, readable, same picture */
    pstats (&c0, &s0, &r0, &f0);
    struct FxSet *set = build_set (tmpdir, gain);
    result (set && fx_failed_count (set) == 0, "specialised pass builds", NULL);
    if (!set)
        return;
    EffPass *p = &set->chains[0]->passes[0];
    result (p->spec && p->spec->n_baked == 4 && glGetUniformLocation (p->program, "g_Gain") < 0 &&
            glGetUniformLocation (p->program, "g_Tint") < 0,
            "constants of the instance and annotation defaults are baked (no such uniforms)", NULL);
    float v[4] = { 0 };
    int n = fx_get_constant (set, 0, 0, "gain", v);
    result (n == 1 && v[0] == 0.5f, "a baked constant reads back by its material key", NULL);
    n = fx_get_constant (set, 0, 0, "tint", v);
    result (n == 3 && v[0] == 1.0f && v[1] == 0.5f && v[2] == 0.25f, "a baked vector reads back", NULL);
    result (fx_get_constant (set, 0, 0, "unused", v) == 0,
            "a constant the shader never uses has no entry (as with the generic program)", NULL);
    draw (set, in, 0.7, a0);
    render (tmpdir, gain, FALSE, FALSE, in, b0, b1, NULL);
    int md = max_diff (a0, b0, NULL);
    char *d = g_strdup_printf ("max difference %d", md);
    result (md <= 1, "specialised and generic programs draw the same picture", d);
    g_free (d);

    /* --- a script writes a baked constant */
    float nv = 0.25f;
    pstats (&c0, &s0, &r0, &f0);
    result (fx_set_constant (set, 0, 0, "gain", &nv, 1), "a baked constant can be set", NULL);
    draw (set, in, 0.7, a0);
    pstats (&c1, &s1, &r1, &f1);
    p = &set->chains[0]->passes[0];
    result (r1 == r0 + 1 && glGetUniformLocation (p->program, "g_Gain") >= 0 &&
            glGetUniformLocation (p->program, "g_Tint") < 0,
            "the write relinks the pass once: that constant is a uniform, the others stay baked", NULL);
    n = fx_get_constant (set, 0, 0, "gain", v);
    result (n == 1 && v[0] == 0.25f, "the new value reads back", NULL);
    g_atomic_int_set (&fxs_spec_on, 0);
    struct FxSet *gen = build_set (tmpdir, gain);
    g_atomic_int_set (&fxs_spec_on, 1);
    fx_set_constant (gen, 0, 0, "gain", &nv, 1);
    draw (gen, in, 0.7, b0);
    md = max_diff (a0, b0, NULL);
    d = g_strdup_printf ("max difference %d", md);
    result (md <= 1, "after the relink the picture matches the generic program with that value", d);
    g_free (d);
    nv = 0.75f;
    fx_set_constant (set, 0, 0, "gain", &nv, 1);
    fx_set_constant (gen, 0, 0, "gain", &nv, 1);
    draw (set, in, 0.7, a0);
    draw (gen, in, 0.7, b0);
    pstats (&c0, &s0, &r0, &f0);
    result (r0 == r1 && max_diff (a0, b0, NULL) <= 1, "further writes are plain uniform uploads", NULL);
    fx_free_for_object (gen);
    fx_free_for_object (set);

    /* --- script-bound constants are never baked */
    const char *bound = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                        "{\"gain\":{\"script\":\"export function update(v){return v;}\",\"value\":0.5},"
                        "\"tint\":{\"animation\":{},\"value\":\"1 0.5 0.25\"}}}]}]";
    set = build_set (tmpdir, bound);
    p = set ? &set->chains[0]->passes[0] : NULL;
    result (p && glGetUniformLocation (p->program, "g_Gain") >= 0 && glGetUniformLocation (p->program, "g_Tint") >= 0 &&
            glGetUniformLocation (p->program, "g_Exp") < 0,
            "constants with a script or an animation stay uniforms, the rest is baked", NULL);
    fx_free_for_object (set);

    /* --- sharing */
    const char *two = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":{\"gain\":0.5}}]},"
                      "{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":{\"gain\":0.5}}]},"
                      "{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":{\"gain\":0.6}}]}]";
    pstats (&c0, &s0, &r0, &f0);
    set = build_set (tmpdir, two);
    pstats (&c1, &s1, &r1, &f1);
    result (set && c1 - c0 == 2 && s1 - s0 == 1 &&
            set->chains[0]->passes[0].program == set->chains[1]->passes[0].program &&
            set->chains[0]->passes[0].program != set->chains[2]->passes[0].program,
            "identical passes share one program, a different constant gets its own", NULL);
    GLuint shared_prog = set ? set->chains[0]->passes[0].program : 0;
    fx_free_for_object (set);
    result (!glIsProgram (shared_prog), "a shared program is deleted with its last user", NULL);
    /* same text, different uniform values: each pass draws its own */
    const char *sa = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                     "{\"gain\":{\"script\":\"x\",\"value\":0.5}}}]}]";
    const char *sb = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                     "{\"gain\":{\"script\":\"x\",\"value\":1.0}}}]}]";
    struct FxSet *A = build_set (tmpdir, sa), *B = build_set (tmpdir, sb);
    gboolean same_prog = A && B && A->chains[0]->passes[0].program == B->chains[0]->passes[0].program;
    draw (A, in, 0.7, a0);
    draw (B, in, 0.7, b0);
    draw (A, in, 0.7, a1);
    draw (B, in, 0.7, b1);
    result (same_prog && max_diff (a0, a1, NULL) == 0 && max_diff (b0, b1, NULL) == 0 && max_diff (a0, b0, NULL) > 20,
            "two passes on one program keep their own uniform values", NULL);
    fx_free_for_object (A);
    fx_free_for_object (B);

    /* --- frame-constant run moved to the vertex shader */
    const char *wave = "[{\"file\":\"effects/wave/effect.json\",\"passes\":[{\"constantshadervalues\":"
                       "{\"speed\":2.0,\"amount\":0.8,\"noiseamount\":0.3,\"power\":1.4}}]}]";
    guint rw = 0;
    gboolean ok = render (tmpdir, wave, TRUE, FALSE, in, a0, a1, &rw) && render (tmpdir, wave, FALSE, FALSE, in, b0, b1, NULL);
    int m0 = max_diff (a0, b0, NULL), m1 = max_diff (a1, b1, NULL);
    d = g_strdup_printf ("rewrite bits %u, max difference %d and %d, pictures at the two times differ by %d", rw, m0, m1,
                         max_diff (a0, a1, NULL));
    result (ok && (rw & FXS_RW_PULSE) && m0 <= 1 && m1 <= 1 && max_diff (a0, a1, NULL) > 4,
            "frame-constant work moved to the vertex shader: same picture", d);
    g_free (d);

    /* --- identity pass behind another effect */
    const char *chain = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":{\"gain\":0.5}}]},"
                        "{\"file\":\"effects/opacity/effect.json\",\"passes\":[{\"constantshadervalues\":{\"alpha\":1.0}}]}]";
    guint run0 = 0, run1 = 0;
    set = build_set (tmpdir, chain);
    fx_pass_stats (&run0, NULL, FALSE);
    draw (set, in, 0.7, a0);
    fx_pass_stats (&run1, NULL, FALSE);
    gboolean ident = set && set->chains[1]->passes[0].spec && set->chains[1]->passes[0].spec->identity;
    g_atomic_int_set (&fxs_spec_on, 0);
    gen = build_set (tmpdir, chain);
    g_atomic_int_set (&fxs_spec_on, 1);
    guint g0 = 0, g1 = 0;
    fx_pass_stats (&g0, NULL, FALSE);
    draw (gen, in, 0.7, b0);
    fx_pass_stats (&g1, NULL, FALSE);
    d = g_strdup_printf ("passes drawn %u (generic %u), max difference %d", run1 - run0, g1 - g0, max_diff (a0, b0, NULL));
    result (ident && run1 - run0 == 1 && g1 - g0 == 2 && max_diff (a0, b0, NULL) <= 1,
            "opacity 1 without a mask behind another effect: not drawn, same picture", d);
    g_free (d);
    /* a script changes the baked alpha: the chain is no identity any more */
    nv = 0.5f;
    fx_set_constant (set, 1, 0, "alpha", &nv, 1);
    fx_set_constant (gen, 1, 0, "alpha", &nv, 1);
    fx_pass_stats (&run0, NULL, FALSE);
    draw (set, in, 0.7, a0);
    fx_pass_stats (&run1, NULL, FALSE);
    draw (gen, in, 0.7, b0);
    result (run1 - run0 == 2 && max_diff (a0, b0, NULL) <= 1,
            "a script lowering that opacity brings the pass back", NULL);
    fx_free_for_object (set);
    fx_free_for_object (gen);
    glDeleteTextures (1, &in);
}

/* WE's own effects, when the install is there */
/* constants the executor sets on its own when neither the instance nor an
 * annotation gives a value (g_Alpha, g_Color4), an int, and ordinary ones */
static const char *al_frag =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float g_Alpha;\nuniform vec4 g_Color4;\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"1 1 1\"}\n"
    "uniform int g_Mode; // {\"material\":\"mode\",\"default\":0}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb = c.rgb * g_Gain * g_Tint * g_Color4.rgb;\n\tif (g_Mode == 1)\n\t\tc.rgb = c.bgr;\n"
    "\tc.a *= g_Alpha * g_Color4.a;\n\tgl_FragColor = c;\n}\n";

/* fx_specialize off (the default): passes with the same text share one
 * program and nothing else changes - byte for byte what one program per
 * pass draws */
static void gl_share_checks (void)
{
    static guint8 a[W * H * 4], b[W * H * 4];
    GLuint in = input_tex ();
    put_effect ("al", gain_vert, al_frag);
    const char *three = "[{\"file\":\"effects/al/effect.json\",\"passes\":[{\"constantshadervalues\":"
                        "{\"gain\":0.9,\"tint\":\"1 0.5 0.25\",\"mode\":1}}]},"
                        "{\"file\":\"effects/al/effect.json\",\"passes\":[{\"constantshadervalues\":{\"gain\":1.4}}]},"
                        "{\"file\":\"effects/al/effect.json\",\"passes\":[{\"constantshadervalues\":"
                        "{\"gain\":0.8,\"tint\":\"0.2 1 1\"}}]}]";
    int c0, c1, c2, s1, s2;
    g_atomic_int_set (&fxs_spec_on, 0);
    /* the default: no sharing. Every pass has its own program, built by
     * the 0.12 code, and nothing of fx_specialize.h is attached */
    result (g_atomic_int_get (&fxs_share_on) == 0 && !fxs_sharing (),
            "default: program sharing is off (WALPE_FX_SHARE=1 turns it on)", NULL);
    pstats (&c0, NULL, NULL, NULL);
    struct FxSet *base = build_set (tmpdir, three);
    pstats (&c1, &s1, NULL, NULL);
    if (base) {
        EffPass *b0 = &base->chains[0]->passes[0], *b1 = &base->chains[1]->passes[0],
                *b2 = &base->chains[2]->passes[0];
        result (b0->program && b0->program != b1->program && b1->program != b2->program &&
                b0->program != b2->program && !b0->spec && !b1->spec && !b2->spec,
                "default: three passes of one text have three programs and no specialisation state", NULL);
    }
    g_atomic_int_set (&fxs_share_on, 1);
    struct FxSet *shr = build_set (tmpdir, three);
    pstats (&c2, &s2, NULL, NULL);
    g_atomic_int_set (&fxs_share_on, 0);    /* (what is built from here on: the default again) */
    if (!base || !shr) {
        result (FALSE, "share: passes build", NULL);
        g_atomic_int_set (&fxs_spec_on, 1);
        return;
    }
    EffPass *p0 = &shr->chains[0]->passes[0], *p1 = &shr->chains[1]->passes[0], *p2 = &shr->chains[2]->passes[0];
    char *d = g_strdup_printf ("programs compiled: %d one per pass, %d shared", c1 - c0, c2 - c1);
    result (c1 - c0 == 3 && c2 - c1 == 1 && s2 - s1 == 2 && p0->program == p1->program && p1->program == p2->program &&
            p0->spec && p0->spec->n == 0 && !p0->spec->probation && glGetUniformLocation (p0->program, "g_Gain") >= 0,
            "share: three passes of one text compile one program, with every constant a uniform", d);
    g_free (d);
    int worst = 0;
    for (int i = 0; i < 3; i++) {
        draw (base, in, 0.3 * i, a);
        draw (shr, in, 0.3 * i, b);
        worst = MAX (worst, max_diff (a, b, NULL));
    }
    d = g_strdup_printf ("max difference %d", worst);
    result (worst == 0, "share: the picture is byte-identical to one program per pass (the executor's own defaults "
            "for constants without a value included)", d);
    g_free (d);
    /* script writes: a constant with a value, one without, and back */
    float gv = 0.55f, av[4] = { 0.5f, 0, 0, 1 }, back[4] = { 0, 0, 0, 1 };
    fx_set_constant (base, 1, 0, "gain", &gv, 1);
    fx_set_constant (shr, 1, 0, "gain", &gv, 1);
    fx_set_constant (base, 2, 0, "g_Alpha", av, 1);
    fx_set_constant (shr, 2, 0, "g_Alpha", av, 1);
    draw (base, in, 1.0, a);
    draw (shr, in, 1.0, b);
    worst = max_diff (a, b, NULL);
    draw (base, in, 1.1, a);
    draw (shr, in, 1.1, b);
    worst = MAX (worst, max_diff (a, b, NULL));
    fx_set_constant (base, 2, 0, "g_Alpha", back, 1);
    fx_set_constant (shr, 2, 0, "g_Alpha", back, 1);
    for (int i = 0; i < 2; i++) {
        draw (base, in, 1.2 + 0.1 * i, a);
        draw (shr, in, 1.2 + 0.1 * i, b);
        worst = MAX (worst, max_diff (a, b, NULL));
    }
    d = g_strdup_printf ("max difference %d", worst);
    result (worst == 0, "share: ... and stays so when a script writes constants of one of the passes", d);
    g_free (d);
    /* a pass switched off and on again */
    fx_set_enabled (base, 0, FALSE);
    fx_set_enabled (shr, 0, FALSE);
    draw (base, in, 1.5, a);
    draw (shr, in, 1.5, b);
    worst = max_diff (a, b, NULL);
    fx_set_enabled (base, 0, TRUE);
    fx_set_enabled (shr, 0, TRUE);
    draw (base, in, 1.6, a);
    draw (shr, in, 1.6, b);
    worst = MAX (worst, max_diff (a, b, NULL));
    result (worst == 0, "share: ... and when one of them is switched off and on", NULL);
    /* the same three with the constants baked (unchecked): the ones
     * without a value stay uniforms and are put back the same way */
    g_atomic_int_set (&fxs_spec_on, 1);
    struct FxSet *spc = build_set (tmpdir, three);
    draw (spc, in, 0.0, b);
    fx_set_constant (spc, 1, 0, "gain", &gv, 1);
    fx_set_constant (spc, 2, 0, "g_Alpha", av, 1);
    draw (spc, in, 1.0, b);
    fx_set_constant (spc, 2, 0, "g_Alpha", back, 1);     /* as base and shr above */
    draw (spc, in, 1.6, b);
    draw (spc, in, 1.6, b);
    d = g_strdup_printf ("max difference %d", max_diff (a, b, NULL));
    result (spc && max_diff (a, b, NULL) <= 1, "share: specialised passes that share a program restore such constants too", d);
    g_free (d);
    if (spc)
        fx_free_for_object (spc);
    fx_free_for_object (base);
    fx_free_for_object (shr);
    glDeleteTextures (1, &in);
}

/* the default path: generic program first, the specialised one behind
 * the first draw once it has drawn the same */
static void gl_verify_checks (void)
{
    static guint8 a0[W * H * 4], a1[W * H * 4], b0[W * H * 4], b1[W * H * 4];
    GLuint in = input_tex ();
    const char *gain = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                       "{\"gain\":0.6,\"tint\":\"1 0.5 0.25\",\"exponent\":1.5}}]}]";
    int v0 = 0, x0 = 0, v1 = 0, x1 = 0, c0, c1, r0, r1;
    char *d;
    render (tmpdir, gain, FALSE, FALSE, in, b0, b1, NULL);      /* the generic pictures */
    /* the default: six comparisons, behind draws 1, 2, 4, 8, 16 and 32 */
    {
        const char *slow = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                           "{\"gain\":0.61,\"tint\":\"1 0.5 0.25\",\"exponent\":1.5}}]}]";
        struct FxSet *ps = build_set (tmpdir, slow);
        fx_verify_stats (&v0, &x0);
        gboolean early = FALSE;
        for (int i = 0; ps && i < 31; i++) {
            draw (ps, in, 0.1 * i, a0);
            early |= !ps->chains[0]->passes[0].spec->probation;
        }
        fx_verify_stats (&v1, &x1);
        gboolean waiting = ps && !early && v1 == v0 && ps->chains[0]->passes[0].spec->passed == 5;
        if (ps)
            draw (ps, in, 3.1, a0);
        fx_verify_stats (&v1, &x1);
        result (waiting && v1 == v0 + 1 && !ps->chains[0]->passes[0].spec->probation &&
                ps->chains[0]->passes[0].spec->n_baked == 4,
                "verify: a pass draws with the generic program until six comparisons (draws 1, 2, 4, ... 32) agree", NULL);
        if (ps)
            fx_free_for_object (ps);
    }
    g_atomic_int_set (&fxs_verify_n, 1);    /* the mechanics below: one comparison */
    fx_verify_stats (&v0, &x0);
    guint ser0 = fx_picture_serial ();
    struct FxSet *set = build_set (tmpdir, gain);
    if (!set) {
        result (FALSE, "verify: pass builds", NULL);
        return;
    }
    EffPass *p = &set->chains[0]->passes[0];
    result (p->spec && p->spec->probation && p->spec->cand && p->spec->n_baked == 0 &&
            glGetUniformLocation (p->program, "g_Gain") >= 0,
            "verify: a pass starts with the generic program, the specialised one is only a candidate", NULL);
    draw (set, in, 0.7, a0);
    p = &set->chains[0]->passes[0];
    fx_verify_stats (&v1, &x1);
    d = g_strdup_printf ("max difference of the first frame %d; compared %d, rejected %d", max_diff (a0, b0, NULL),
                         v1 - v0 + x1 - x0, x1 - x0);
    result (max_diff (a0, b0, NULL) == 0 && v1 == v0 + 1 && x1 == x0 && !p->spec->probation && !p->spec->cand &&
            p->spec->n_baked == 4 && glGetUniformLocation (p->program, "g_Gain") < 0,
            "verify: the first frame is the generic program's; behind it the candidate drew the same and took over", d);
    g_free (d);
    result (fx_picture_serial () != ser0, "verify: the switch bumps fx_picture_serial", NULL);
    draw (set, in, 0.7, a0);
    draw (set, in, 3.1, a1);
    result (max_diff (a0, b0, NULL) <= 1 && max_diff (a1, b1, NULL) <= 1,
            "verify: the frames after it match the generic program", NULL);
    /* the verdict is the program text's: another pass with it is not compared again */
    struct FxSet *again = build_set (tmpdir, gain);
    EffPass *q = again ? &again->chains[0]->passes[0] : NULL;
    fx_verify_stats (&v0, &x0);
    result (q && q->spec && !q->spec->probation && q->spec->n_baked == 4 && q->program == p->program && v0 == v1,
            "verify: a second pass with the same text starts with the compared program", NULL);
    if (again)
        fx_free_for_object (again);

    /* a script writes a baked constant: generic for that draw, then baked again without it */
    float nv = 0.25f;
    pstats (&c0, NULL, &r0, NULL);
    fx_verify_stats (&v0, &x0);
    fx_set_constant (set, 0, 0, "gain", &nv, 1);
    draw (set, in, 0.7, a0);
    p = &set->chains[0]->passes[0];
    pstats (&c1, NULL, &r1, NULL);
    fx_verify_stats (&v1, &x1);
    g_atomic_int_set (&fxs_spec_on, 0);
    struct FxSet *gen = build_set (tmpdir, gain);
    g_atomic_int_set (&fxs_spec_on, 1);
    fx_set_constant (gen, 0, 0, "gain", &nv, 1);
    draw (gen, in, 0.7, b0);
    d = g_strdup_printf ("max difference %d, baked %d, relinks %d, compiled %d, compared %d",
                         max_diff (a0, b0, NULL), p->spec->n_baked, r1 - r0, c1 - c0, v1 - v0 + x1 - x0);
    result (max_diff (a0, b0, NULL) == 0 && r1 == r0 + 1 && v1 == v0 + 1 && p->spec->n_baked == 3 &&
            glGetUniformLocation (p->program, "g_Gain") >= 0 && glGetUniformLocation (p->program, "g_Tint") < 0,
            "verify: a write to a baked constant draws that frame with the generic program, then the rest is "
            "baked again (compared first)", d);
    g_free (d);
    float v[4] = { 0 };
    result (fx_get_constant (set, 0, 0, "gain", v) == 1 && v[0] == 0.25f && fx_get_constant (set, 0, 0, "tint", v) == 3 &&
            v[1] == 0.5f, "verify: written and baked constants read back", NULL);
    draw (set, in, 0.7, a0);
    result (max_diff (a0, b0, NULL) <= 1, "verify: the frame after the write matches the generic program", NULL);
    nv = 0.75f;
    fx_set_constant (set, 0, 0, "gain", &nv, 1);
    fx_set_constant (gen, 0, 0, "gain", &nv, 1);
    pstats (NULL, NULL, &r0, NULL);
    draw (set, in, 0.7, a0);
    draw (gen, in, 0.7, b0);
    pstats (NULL, NULL, &r1, NULL);
    result (r1 == r0 && max_diff (a0, b0, NULL) <= 1, "verify: further writes are plain uniform uploads", NULL);
    fx_free_for_object (set);

    /* a write between the build and the first draw (a script's init) */
    const char *gain2 = "[{\"file\":\"effects/gain/effect.json\",\"passes\":[{\"constantshadervalues\":"
                        "{\"gain\":0.9,\"tint\":\"1 0.5 0.25\",\"exponent\":1.5}}]}]";
    set = build_set (tmpdir, gain2);
    fx_set_constant (set, 0, 0, "gain", &nv, 1);
    draw (set, in, 0.7, a0);
    p = &set->chains[0]->passes[0];
    d = g_strdup_printf ("max difference %d, baked %d", max_diff (a0, b0, NULL), p->spec->n_baked);
    result (max_diff (a0, b0, NULL) == 0 && p->spec->n_baked == 3 && glGetUniformLocation (p->program, "g_Gain") >= 0,
            "verify: a constant written before the first draw stays a uniform, the others are baked", d);
    g_free (d);
    draw (set, in, 0.7, a0);
    result (max_diff (a0, b0, NULL) <= 1, "verify: ... and the picture is the generic program's", NULL);
    fx_free_for_object (set);
    fx_free_for_object (gen);

    /* a shader that leans on a division by zero */
    put_effect ("hz", gain_vert, hz_frag);
    const char *hz = "[{\"file\":\"effects/hz/effect.json\",\"passes\":[{\"constantshadervalues\":{\"gain\":0.8}}]}]";
    render (tmpdir, hz, FALSE, FALSE, in, b0, b1, NULL);
    fx_verify_stats (&v0, &x0);
    set = build_set (tmpdir, hz);
    draw (set, in, 0.7, a0);
    draw (set, in, 0.7, a0);
    draw (set, in, 3.1, a1);
    fx_verify_stats (&v1, &x1);
    p = &set->chains[0]->passes[0];
    gboolean rejected = x1 > x0;
    d = g_strdup_printf ("max difference %d / %d; this driver's compiler %s", max_diff (a0, b0, NULL), max_diff (a1, b1, NULL),
                         rejected ? "does not give what the GPU computes: the pass keeps the generic program"
                                  : "gives what the GPU computes: the constants are baked");
    result (max_diff (a0, b0, NULL) <= 1 && max_diff (a1, b1, NULL) <= 1 &&
            (rejected ? p->spec->n_baked == 0 && glGetUniformLocation (p->program, "g_Div") >= 0 : p->spec->n_baked > 0),
            "verify: a constant zero the shader divides by - the picture is the generic program's either way", d);
    g_free (d);
    pstats (&c0, NULL, NULL, NULL);
    again = build_set (tmpdir, hz);
    pstats (&c1, NULL, NULL, NULL);
    q = again ? &again->chains[0]->passes[0] : NULL;
    result (q && (!rejected || (c1 == c0 && !q->spec->probation && q->spec->n_baked == 0)),
            "verify: a rejected text is not compiled or compared a second time", NULL);
    if (again)
        fx_free_for_object (again);
    fx_free_for_object (set);

    /* fusion (fx_quality fast) waits for the members' programs */
    GLuint smooth = smooth_tex ();
    const char *run3 = "[{\"file\":\"effects/wv/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":0.031,\"angle\":0.4}}]},"
                       "{\"file\":\"effects/sh/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":\"0.011 0.02\"}}]},"
                       "{\"file\":\"effects/wv/effect.json\",\"passes\":[{\"constantshadervalues\":{\"amp\":0.021,\"freq\":14,\"angle\":2.0}}]}]";
    g_atomic_int_set (&fxs_spec_on, 0);
    struct FxSet *ref = build_set (tmpdir, run3);
    g_atomic_int_set (&fxs_spec_on, 1);
    g_atomic_int_set (&fxs_quality, 1);
    set = build_set (tmpdir, run3);
    guint p0 = 0, p1 = 0, p2 = 0, p3 = 0;
    fx_pass_stats (&p0, NULL, FALSE);
    draw (set, smooth, 0.4, a0);
    fx_pass_stats (&p1, NULL, FALSE);
    draw (ref, smooth, 0.4, b0);
    int first = max_diff (a0, b0, NULL);
    fx_pass_stats (&p2, NULL, FALSE);
    draw (set, smooth, 1.9, a0);
    fx_pass_stats (&p3, NULL, FALSE);
    draw (ref, smooth, 1.9, b0);
    double psnr = 0;
    int md = max_diff (a0, b0, &psnr);
    d = g_strdup_printf ("passes %u then %u, groups %d, first frame max difference %d, second %d (PSNR %.1f dB)",
                         p1 - p0, p3 - p2, set->fuse ? set->fuse->n : 0, first, md, psnr);
    result (p1 - p0 == 3 && first == 0 && p3 - p2 == 1 && set->fuse && set->fuse->n == 1 && psnr >= 30.0,
            "verify + fast: the first frame draws the run effect by effect (generic), the next one as one pass", d);
    g_free (d);
    fx_free_for_object (set);
    g_atomic_int_set (&fxs_quality, 0);
    fx_free_for_object (ref);
    glDeleteTextures (1, &smooth);
    glDeleteTextures (1, &in);
    g_atomic_int_set (&fxs_verify_n, 6);
}

static void gl_we_checks (void)
{
    const char *we = walpe_we_assets_dir ();
    if (!we) {
        printf ("skip WE effect checks (no Wallpaper Engine assets: set WALPE_WE_ASSETS)\n");
        return;
    }
    /* hazard: values with which the generic program divides by zero; the
     * unchecked specialisation need not draw the same there (it does not
     * on llvmpipe), the verified one must */
    static const struct { const char *name, *json; guint exact_rw, fast_rw; } fx[] = {
        { "pulse (thresholds 0 0: a step, by a division by zero)",
          "[{\"file\":\"effects/pulse/effect.json\",\"passes\":[{\"combos\":{\"PULSECOLOR\":1},\"constantshadervalues\":"
          "{\"speed\":0.02,\"phase\":6.282,\"amount\":0.09,\"bounds\":\"0 0\",\"noisespeed\":0.02,\"noiseamount\":0.25,"
          "\"power\":1.29}}]}]", 0, 0 },
        { "pulse (noise on)", "[{\"file\":\"effects/pulse/effect.json\",\"passes\":[{\"constantshadervalues\":"
          "{\"speed\":1.97,\"amount\":1.21,\"noiseamount\":0.5,\"power\":1.39,\"tinthigh\":\"1 0.65 0\"}}]}]", FXS_RW_PULSE, 0 },
        { "pulse (defaults)", "[{\"file\":\"effects/pulse/effect.json\"}]", FXS_RW_PULSE, 0 },
        { "shake", "[{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"constantshadervalues\":"
          "{\"speed\":1.3,\"strength\":0.2,\"friction\":\"1.15 1.2\"},\"textures\":[null,\"util/noise\"]}]}]", FXS_RW_SHAKE, 0 },
        { "shake (noise)", "[{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"NOISE\":1},\"constantshadervalues\":"
          "{\"speed\":1.3,\"strength\":0.2},\"textures\":[null,\"util/noise\"]}]}]", FXS_RW_SHAKE, 0 },
        { "water waves", "[{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"constantshadervalues\":"
          "{\"speed\":2,\"scale\":5,\"strength\":0.15,\"exponent\":1.0}}]}]", 0, 0 },
        { "opacity + tint", "[{\"file\":\"effects/opacity/effect.json\",\"passes\":[{\"constantshadervalues\":{\"alpha\":0.6}}]},"
          "{\"file\":\"effects/tint/effect.json\",\"passes\":[{\"constantshadervalues\":{\"alpha\":0.5}}]}]", 0, 0 },
        { "blur", "[{\"file\":\"effects/blur/effect.json\"}]", 0, FXS_RW_BLUR },
        { "blur (7x7)", "[{\"file\":\"effects/blur/effect.json\",\"passes\":[{},{\"combos\":{\"KERNEL\":1}},"
          "{\"combos\":{\"KERNEL\":1,\"VERTICAL\":1}},{}]}]", 0, FXS_RW_BLUR },
        { "precise blur", "[{\"file\":\"effects/blurprecise/effect.json\"}]", 0, 0 },
        { "water waves x2 + shake + water ripple (a run)",
          "[{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"constantshadervalues\":{\"speed\":2,\"scale\":5,\"strength\":0.15}}]},"
          "{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"combos\":{\"DUALWAVES\":1},\"constantshadervalues\":"
          "{\"speed\":3,\"scale\":9,\"strength\":0.1,\"direction\":1.2}}]},"
          "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"constantshadervalues\":{\"speed\":1.3,\"strength\":0.12},"
          "\"textures\":[null,\"util/noise\"]}]},"
          "{\"file\":\"effects/waterripple/effect.json\",\"passes\":[{\"constantshadervalues\":{\"ripplestrength\":0.1},"
          "\"textures\":[null,null,\"util/noise\"]}]}]", FXS_RW_SHAKE, 0 },
        { "shine (15 samples)", "[{\"file\":\"effects/shine/effect.json\",\"passes\":[{},{\"combos\":{\"SAMPLES\":2}},{},{},{}]}]",
          0, FXS_RW_CAST },
        { "god rays (50 samples)", "[{\"file\":\"effects/godrays/effect.json\",\"passes\":[{},{\"combos\":{\"SAMPLES\":1}},{},{},{}]}]",
          0, FXS_RW_CAST },
    };
    static guint8 g0[W * H * 4], g1[W * H * 4], s0[W * H * 4], s1[W * H * 4], f0[W * H * 4], f1[W * H * 4];
    GLuint noisy = input_tex (), smooth = smooth_tex ();
    for (guint i = 0; i < G_N_ELEMENTS (fx); i++) {
        guint rw = 0, rwf = 0;
        double p0 = 0, p1 = 0;
        /* a fused run samples the layer once instead of once per effect:
         * compared on a smooth picture (per-pixel noise survives one
         * resampling and is smeared by four) */
        GLuint in = strstr (fx[i].name, "a run") ? smooth : noisy;
        if (!render (tmpdir, fx[i].json, FALSE, FALSE, in, g0, g1, NULL)) {
            printf ("skip WE %s (does not build here)\n", fx[i].name);
            continue;
        }
        gboolean hazard = strstr (fx[i].name, "division by zero") != NULL;
        gboolean ok = render (tmpdir, fx[i].json, TRUE, FALSE, in, s0, s1, &rw);
        int m0 = max_diff (g0, s0, NULL), m1 = max_diff (g1, s1, NULL);
        char *w = g_strdup_printf ("WE %s: specialised (unchecked) = generic", fx[i].name);
        /* a WE version whose statements differ simply gets no rewrite */
        char *d = g_strdup_printf ("max difference %d / %d, rewrite bits %u%s", m0, m1, rw,
                                   (rw & fx[i].exact_rw) == fx[i].exact_rw ? "" : " (expected rewrite not applied)");
        if (hazard)
            printf ("note  WE %s: unchecked specialisation, %s\n", fx[i].name, d);
        else
            result (ok && m0 <= 1 && m1 <= 1, w, d);
        g_free (w);
        g_free (d);
        /* the default: whatever this driver's compiler does with the
         * baked constants, the picture is the generic program's */
        int nspec = 0, npass = 0;
        ok = render_verified (tmpdir, fx[i].json, in, s0, s1, &nspec, &npass);
        m0 = max_diff (g0, s0, NULL);
        m1 = max_diff (g1, s1, NULL);
        w = g_strdup_printf ("WE %s: verified specialisation = generic", fx[i].name);
        d = g_strdup_printf ("max difference %d / %d, %d of %d passes draw with baked constants", m0, m1, nspec, npass);
        result (ok && m0 <= 1 && m1 <= 1, w, d);
        g_free (w);
        g_free (d);
        if (hazard)
            continue;
        ok = render (tmpdir, fx[i].json, TRUE, TRUE, in, f0, f1, &rwf);
        m0 = max_diff (g0, f0, &p0);
        m1 = max_diff (g1, f1, &p1);
        w = g_strdup_printf ("WE %s: fx_quality fast", fx[i].name);
        d = g_strdup_printf ("max difference %d / %d, PSNR %.1f / %.1f dB, rewrite bits %u%s", m0, m1, p0, p1, rwf,
                             (rwf & fx[i].fast_rw) == fx[i].fast_rw ? "" : " (expected rewrite not applied)");
        gboolean fused = strstr (fx[i].name, "a run") != NULL;
        result (ok && p0 >= 30.0 && p1 >= 30.0 && ((rwf & (FXS_RW_BLUR | FXS_RW_CAST)) || fused || (m0 <= 1 && m1 <= 1)), w, d);
        g_free (w);
        g_free (d);
    }
    glDeleteTextures (1, &noisy);
    glDeleteTextures (1, &smooth);
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
    fxs_enabled ();
    g_atomic_int_set (&fxs_spec_on, 1);
    g_atomic_int_set (&fxs_quality, 0);
    /* the checks of the baking itself use the specialised programs from
     * the first frame; the verification has its own */
    g_atomic_int_set (&fxs_verify_on, 0);
    gl_spec_checks ();
    if (tmpdir) {
        gl_share_checks ();
        gl_fuse_checks ();
        gl_we_checks ();
        g_atomic_int_set (&fxs_verify_on, 1);
        gl_verify_checks ();
    }
    fx_targets_release ();
    if (tmpdir) {
        rm_rf (tmpdir);
        g_clear_pointer (&tmpdir, g_free);
    }
    /* everything was freed: the caches of this thread are empty and gone */
    result (!fxs_progs && !fxs_srcs, "program and source caches are empty after the last pass is freed", NULL);
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
    own_keys ();
    source_checks ();
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
