/* fx_essl_test.c - the GLSL ES output of the effect shader builder
 * (src/engine/fx_essl.h).
 *
 *   fx-essl-test
 *
 * The shaders below are written for this test in Wallpaper Engine's
 * dialect, one per construct GLSL 3.30 takes and GLSL ES does not.
 *
 * 1. The head: "#version 330" and nothing else new for desktop OpenGL;
 *    version line, default precisions and the dropped texture names for
 *    GLSL ES 3.00 / 3.10 / 3.20.
 * 2. Dialect conversion, the text expected for each construct: integer
 *    literals and integer expressions where a float is wanted
 *    (initialisers, operators, comparisons, ?:, compound assignments,
 *    built-in and user function arguments, return values, array
 *    constructors, macros that stand for integers), what must stay an
 *    integer (indices, int overloads, texelFetch / textureSize, integer
 *    division), statements over several lines.
 * 3. What GLSL ES is stricter about: uniform initialisers, global
 *    initialisers that are no constant expressions, "const" locals,
 *    reserved words as names, precision statements, gl_FragData[0],
 *    names that are no macros in an #if.
 *    Lines behind the backslash of a "#define", an #if the model cannot
 *    evaluate; the work limits (joined statements, macro bodies).
 * 4. A shader that is valid GLSL ES already comes out unchanged.
 * 5. The lexical promotion (the fallback's candidate) on its own.
 * 6. Stage 2 with a scripted "driver": conversions the source model
 *    cannot see (inside a function-like macro, next to an untyped call)
 *    are found from the line the log names.
 * 7. With an EGL display that makes an OpenGL ES 3.x context (Mesa: any
 *    driver, llvmpipe included): every shader of parts 2 and 3 builds
 *    through fx_build_program () as the engine builds it, and the ones
 *    the fallback can reach build with stage 1 switched off
 *    (WALPE_FX_NO_PREPASS=1). Skipped without such a context.
 * `meson test -C build fx-essl`
 *
 * The engine file is included so the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#include <epoxy/egl.h>
#include <stdio.h>

static int fails, checks;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    printf ("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, !ok && detail ? "\n      " : "",
            !ok && detail ? detail : "");
    if (!ok)
        fails++;
}

#define PRE "uniform float g_Time;\nuniform vec4 g_Texture0Resolution;\n" \
            "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
#define MAIN(stmts) PRE "void main() {\n" stmts "    gl_FragColor = vec4(0.0);\n}\n"
#define VS_BODY "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n" \
                "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvoid main() {\n" \
                "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n" \
                "    v_TexCoord = a_TexCoord * 1;\n}\n"

static char *built (const char *body, gboolean vertex)
{
    GHashTable *cb = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    char *src = fx_build_source (body, cb, vertex);
    g_hash_table_unref (cb);
    return src;
}

/* body as the engine makes it GLSL ES: built, repaired (stage 1), then
 * the GLSL ES pass; *n = lines that pass changed */
static char *es_src (const char *body, int *n)
{
    char *src = built (body, FALSE);
    fx_prepass_src (&src);
    int k = fx_essl_src (&src);
    if (n)
        *n = k;
    return src;
}

static int count_lines (const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

/* want must appear in the result, not (if given) must not; the number of
 * lines must be the body's (compile logs name lines) */
static void es (const char *what, const char *body, const char *want, const char *not)
{
    char *before = built (body, FALSE);
    char *src = es_src (body, NULL);
    gboolean ok = strstr (src, want) != NULL && (!not || !strstr (src, not)) &&
                  count_lines (src) == count_lines (before);
    const char *at = strstr (src, "#line 1\n");
    char *d = ok ? NULL : g_strdup_printf ("want: %s\n      got:\n%s", want, at ? at + 8 : src);
    result (ok, what, d);
    g_free (d);
    g_free (src);
    g_free (before);
}

/* ------------------------------------------------------------ 1. the head */

static void head_checks (void)
{
    static const char *body = "void main() {\n}\n";
    fx_set_glsl_es (0);
    GHashTable *cb = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert (cb, g_strdup ("MASK"), GINT_TO_POINTER (1));
    char *v = fx_build_source (body, cb, TRUE), *f = fx_build_source (body, cb, FALSE);
    char *want_v = g_strconcat ("#version 330\n#define MASK 1\n", fx_prelude_common,
                                "#define varying out\n#define attribute in\n#line 1\n", body, NULL);
    result (!strcmp (v, want_v), "GLSL 3.30, vertex: the text as it always was", v);
    result (g_str_has_prefix (f, "#version 330\n#define MASK 1\n#define GLSL 1\n") &&
            !strstr (f, "precision") && !strstr (f, " es\n") && !strstr (f, "texture2DLod") &&
            strstr (f, "out vec4 _we_fragColor;\n#define gl_FragColor _we_fragColor\n") &&
            g_str_has_suffix (f, "#line 1\nvoid main() {\n}\n"),
            "GLSL 3.30, fragment: nothing of the GLSL ES head", f);
    g_free (v);
    g_free (f);
    g_free (want_v);
    static const int vers[] = { 300, 310, 320 };
    for (guint i = 0; i < G_N_ELEMENTS (vers); i++) {
        fx_set_glsl_es (vers[i]);
        v = fx_build_source (body, cb, TRUE);
        f = fx_build_source (body, cb, FALSE);
        char *head = g_strdup_printf ("#version %d es\nprecision highp float;\nprecision highp int;\n"
                                      "precision highp sampler2D;\nprecision highp sampler3D;\n"
                                      "precision highp samplerCube;\nprecision highp sampler2DShadow;\n"
                                      "precision highp sampler2DArray;\n#define MASK 1\n#define GLSL 1\n",
                                      vers[i]);
        char *what = g_strdup_printf ("GLSL ES %d: version line, precisions, then combos and prelude",
                                      vers[i]);
        result (g_str_has_prefix (v, head) && g_str_has_prefix (f, head), what, f);
        g_free (what);
        what = g_strdup_printf ("GLSL ES %d: the same helper names, the dropped texture names, "
                                "\"#line 1\" in front of the body", vers[i]);
        result (strstr (f, "#define texSample2D texture\n") && strstr (f, "#define lerp mix\n") &&
                strstr (f, "#define texture2DLod textureLod\n") &&
                strstr (f, "out vec4 _we_fragColor;\n") && strstr (v, "#define varying out\n") &&
                g_str_has_suffix (v, "#line 1\nvoid main() {\n}\n") &&
                /* the precision words are defined away only behind the
                 * statements that use them */
                strstr (f, "#define highp\n") > strstr (f, "precision highp sampler2DArray;\n"),
                what, f);
        g_free (what);
        g_free (head);
        g_free (v);
        g_free (f);
    }
    g_hash_table_unref (cb);
    fx_set_glsl_es (300);
}

/* ------------------------------------------------- 2. dialect conversion */

static void conversion_checks (void)
{
    es ("float x = 1", MAIN ("    float a = 1;\n"), "    float a = 1.0;\n", NULL);
    es ("float * 2", MAIN ("    float b = g_Time * 2;\n"), "    float b = g_Time * 2.0;\n", NULL);
    es ("vec2 * 2 - 1", MAIN ("    vec2 c = v_TexCoord * 2 - 1;\n"),
        "    vec2 c = v_TexCoord * 2.0 - 1.0;\n", NULL);
    es ("an integer division stays one: float g = 1 / 2", MAIN ("    float g = 1 / 2;\n"),
        "    float g = float(1 / 2);\n", "1.0 / 2.0");
    es ("through unary minus and parentheses", MAIN ("    float h = -1;\n    float k = (2);\n"),
        "    float h = -1.0;\n    float k = (2.0);\n", NULL);
    es ("HLSL's 1f", MAIN ("    float q = 1f;\n"), "    float q = 1.0f;\n", NULL);
    es ("an octal literal keeps its value", MAIN ("    float o = 010;\n    float p = g_Time * 017;\n"),
        "    float o = float(010);\n    float p = g_Time * float(017);\n", "010.0");

    es ("max (float, 0), pow (float, 2)",
        MAIN ("    float a = 0.5;\n    float d = max(a, 0);\n    float e = pow(a, 2);\n"),
        "    float d = max(a, 0.0);\n    float e = pow(a, 2.0);\n", NULL);
    es ("max / clamp of integers keep their integer overload",
        MAIN ("    int n = max(3, 4);\n    int m = clamp(n, 0, 5);\n"),
        "    int n = max(3, 4);\n    int m = clamp(n, 0, 5);\n", NULL);
    es ("smoothstep / step / mix with integer edges",
        MAIN ("    float s = smoothstep(0, 1, g_Time) + step(0, g_Time) + mix(0, 1, g_Time);\n"),
        "    float s = smoothstep(0.0, 1.0, g_Time) + step(0.0, g_Time) + mix(0.0, 1.0, g_Time);\n",
        NULL);
    es ("texture bias and lod are floats",
        MAIN ("    vec4 t = texSample2D(g_Texture0, v_TexCoord, 1) + "
              "texSample2DLod(g_Texture0, v_TexCoord, 0);\n"),
        "texSample2D(g_Texture0, v_TexCoord, 1.0) + texSample2DLod(g_Texture0, v_TexCoord, 0.0);\n",
        NULL);
    es ("textureSize and texelFetch keep their integers",
        MAIN ("    ivec2 ts = textureSize(g_Texture0, 0);\n"
              "    vec4 tf = texelFetch(g_Texture0, ivec2(1, 2), 0);\n"),
        "    ivec2 ts = textureSize(g_Texture0, 0);\n"
        "    vec4 tf = texelFetch(g_Texture0, ivec2(1, 2), 0);\n", NULL);
    es ("the prelude's HLSL helpers: saturate, frac, fmod",
        MAIN ("    float a = 2.0;\n    float sat = saturate(1) + frac(3) + fmod(a, 2);\n"),
        "    float sat = saturate(1.0) + frac(3.0) + fmod(a, 2.0);\n", NULL);

    es ("comparison, assignment, compound assignment",
        MAIN ("    float a = 0.5;\n    if (a > 0) a = 2;\n    a *= 3;\n"),
        "    if (a > 0.0) a = 2.0;\n    a *= 3.0;\n", NULL);
    es ("the arms of ?:",
        MAIN ("    float a = 0.5;\n    bool cond = a > 0;\n    float f = cond ? 1 : a;\n"),
        "    bool cond = a > 0.0;\n    float f = cond ? 1.0 : a;\n", NULL);
    es ("a loop counter in float arithmetic and in a comparison with a float",
        MAIN ("    float a = 0.5;\n    for (int i = 0; i < 4; i++) a += g_Time * i;\n"
              "    for (int j = 0; j < a; j++) {\n        a -= 1;\n    }\n"),
        "    for (int i = 0; i < 4; i++) a += g_Time * float(i);\n"
        "    for (int j = 0; float(j) < a; j++) {\n        a -= 1.0;\n    }\n", NULL);
    es ("a component of an integer vector in front of a blank: the constructor closes at the name",
        MAIN ("    float a = 0.5;\n    ivec2 n = ivec2(1, 2);\n    if (n.x < a) a += n.y * a;\n"),
        "    if (float(n.x) < a) a += float(n.y) * a;\n", NULL);
    /* the same two, a name and a field, with everything that can stand
     * behind them before the next token: blanks, a tab, a comment, the end
     * of the line */
    es ("a name and a field in front of blanks, a tab and a comment",
        MAIN ("    float a = 0.5;\n    int j = 2;\n    ivec2 n = ivec2(1, 2);\n"
              "    float w = a * j   + n.y\t/ a;\n"
              "    float v = a + j /* steps */ + n.x /* first */ * a;\n"),
        "    float w = a * float(j)   + float(n.y)\t/ a;\n"
        "    float v = a + float(j) /* steps */ + float(n.x) /* first */ * a;\n", NULL);
    es ("a name and a field at the end of a line of a longer statement",
        MAIN ("    float a = 0.5;\n    int j = 2;\n    ivec2 n = ivec2(1, 2);\n"
              "    float z = a * j\n        + n.x\n        * a;\n"),
        "    float z = a * float(j)         + float(n.x)         * a;\n\n\n", NULL);
    es ("a name at the end of a line in front of a comment, a field at the end of a condition's line",
        MAIN ("    float a = 0.5;\n    int j = 2;\n    ivec2 n = ivec2(1, 2);\n"
              "    float y = a - j // steps\n        - 1;\n    if (n.x\n        < a) a -= j;\n"),
        "    float y = a - float(j)                  - 1.0;\n\n"
        "    if (float(n.x)         < a) a -= float(j);\n\n", NULL);
    es ("while with a non-constant bound: only the float side changes",
        MAIN ("    float a = 4.0;\n    int n = 0;\n    while (a > 1 && n < 8) { a = a / 2; n++; }\n"),
        "    while (a > 1.0 && n < 8) { a = a / 2.0; n++; }\n", NULL);

    es ("arguments of the shader's own functions, return values",
        PRE "float sq(float x) { return x * x; }\nfloat one() { return 1; }\n"
            "vec3 tint(vec3 c, float k) {\n    return c * k;\n}\nvoid main() {\n"
            "    float u = sq(3);\n    vec3 v = tint(vec3(1, 0, 0), 2);\n"
            "    gl_FragColor = vec4(v * u * one(), 1);\n}\n",
        "float one() { return 1.0; }\n", NULL);
    es ("... the calls",
        PRE "float sq(float x) { return x * x; }\nfloat one() { return 1; }\n"
            "vec3 tint(vec3 c, float k) {\n    return c * k;\n}\nvoid main() {\n"
            "    float u = sq(3);\n    vec3 v = tint(vec3(1, 0, 0), 2);\n"
            "    gl_FragColor = vec4(v * u * one(), 1);\n}\n",
        "    float u = sq(3.0);\n    vec3 v = tint(vec3(1, 0, 0), 2.0);\n"
        "    gl_FragColor = vec4(v * u * one(), 1);\n", NULL);

    es ("a macro that stands for an integer, a constant",
        PRE "#define SAMPLES 8\nconst float K = 2;\nvoid main() {\n    float m = SAMPLES;\n"
            "    float s2 = g_Time / SAMPLES;\n    gl_FragColor = vec4(m * s2 * K);\n}\n",
        "const float K = 2.0;\nvoid main() {\n    float m = float(SAMPLES);\n"
        "    float s2 = g_Time / float(SAMPLES);\n", NULL);
    /* "g_Time * RATIO" is (g_Time * 16) / 9: RATIO is no operand, and
     * float(RATIO) would be float(16 / 9) = 1.0 */
    es ("a macro whose body is several operands is not converted as one",
        PRE "#define RATIO 16 / 9\n#define WIDE (16 / 9)\n#define ALIAS RATIO\nvoid main() {\n"
            "    float w = g_Time * RATIO;\n    float v = g_Time * WIDE;\n    float u = g_Time * ALIAS;\n"
            "    gl_FragColor = vec4(w + v + u);\n}\n",
        "    float w = g_Time * RATIO;\n    float v = g_Time * float(WIDE);\n    float u = g_Time * ALIAS;\n",
        "float(RATIO)");
    es ("array constructor elements; indices stay integers",
        MAIN ("    float w[3] = float[3](1, 2, 3);\n    int idx = 1;\n    float v0 = w[idx + 1];\n"),
        "    float w[3] = float[3](1.0, 2.0, 3.0);\n    int idx = 1;\n    float v0 = w[idx + 1];\n",
        NULL);
    es ("float % (stage 1 makes it mod ()), int % stays",
        MAIN ("    float a = 5.0;\n    int n = 7;\n    float fm = a % 2;\n    int r = n % 3;\n"),
        "    float fm = mod(float(a), float(2));\n    int r = n % 3;\n", NULL);
    es ("derivatives and constructors need nothing",
        MAIN ("    float dd = ddx(g_Time) + fwidth(v_TexCoord.x);\n    vec3 col = vec3(1, 0, 0) * 2;\n"),
        "    float dd = ddx(g_Time) + fwidth(v_TexCoord.x);\n    vec3 col = vec3(1, 0, 0) * 2.0;\n",
        NULL);

    /* a statement over two lines is rewritten whole, on its first line;
     * the second stays as an empty line */
    {
        const char *body = MAIN ("    float a = 0.5;\n    float z = a *\n        2 + 1;\n");
        char *src = es_src (body, NULL), *before = built (body, FALSE);
        result (strstr (src, "    float z = a *         2.0 + 1.0;\n\n    gl_FragColor") != NULL &&
                count_lines (src) == count_lines (before),
                "a statement over two lines: converted, the line count kept", src);
        g_free (src);
        g_free (before);
    }
}

/* ------------------------------------------- 3. what GLSL ES is strict on */

static void strict_checks (void)
{
    es ("uniform initialiser with an annotation: the initialiser goes",
        PRE "uniform float u_Amount = 0.5; // {\"material\":\"amount\",\"default\":0.5}\n"
            "uniform float u_Fixed = 2;\nvoid main() {\n    gl_FragColor = vec4(u_Amount * u_Fixed);\n}\n",
        "uniform float u_Amount; // {\"material\":\"amount\",\"default\":0.5}\n", NULL);
    es ("uniform initialiser without one: a constant",
        PRE "uniform float u_Amount = 0.5; // {\"material\":\"amount\",\"default\":0.5}\n"
            "uniform float u_Fixed = 2;\nvoid main() {\n    gl_FragColor = vec4(u_Amount * u_Fixed);\n}\n",
        "const   float u_Fixed = 2.0;\n", "uniform float u_Fixed");
    es ("a global initialised from a uniform: assigned at the start of main ()",
        PRE "vec2 texel = 1.0 / g_Texture0Resolution.xy;\nfloat gain = 2.0 * 3.0;\nvoid main() {\n"
            "    gl_FragColor = vec4(texel, gain, 1.0);\n}\n",
        "vec2 texel;\nfloat gain = 2.0 * 3.0;\nvoid main() { texel = 1.0 / g_Texture0Resolution.xy;\n",
        NULL);
    es ("... behind the '{' of main (), not behind the next '{' after a prototype of it",
        PRE "void main();\nvec2 texel = 1.0 / g_Texture0Resolution.xy;\nfloat half_of(float x) {\n"
            "    return x * 0.5;\n}\nvoid main() {\n    gl_FragColor = vec4(texel, half_of(1.0), 1.0);\n}\n",
        "void main();\nvec2 texel;\nfloat half_of(float x) {\n    return x * 0.5;\n}\n"
        "void main() { texel = 1.0 / g_Texture0Resolution.xy;\n", NULL);
    es ("... also when main ()'s header is a statement joined from two lines",
        PRE "vec2 texel = 1.0 / g_Texture0Resolution.xy;\nvoid main (\n) { float one = 1;\n"
            "    gl_FragColor = vec4(texel, one, 1.0);\n}\n",
        "vec2 texel;\nvoid main ( ) { texel = 1.0 / g_Texture0Resolution.xy; float one = 1.0;\n\n", "#error");
    es ("a local const that is no constant expression loses the const; a real one keeps it",
        MAIN ("    const vec4 base = texSample2D(g_Texture0, v_TexCoord);\n    const float two = 2;\n"),
        "          vec4 base = texSample2D(g_Texture0, v_TexCoord);\n    const float two = 2.0;\n",
        NULL);
    es ("reserved words as variable names get a prefix, everywhere they are used",
        MAIN ("    vec4 sample = texSample2D(g_Texture0, v_TexCoord);\n    float smooth = 0.5;\n"
              "    gl_FragColor = sample * smooth * 2;\n"),
        "    vec4 _we_sample = texSample2D(g_Texture0, v_TexCoord);\n    float _we_smooth = 0.5;\n"
        "    gl_FragColor = _we_sample * _we_smooth * 2.0;\n", NULL);
    es ("a reserved word as a sampler parameter and as a struct member",
        PRE "struct Tap { float filter; vec2 at; };\n"
            "vec4 pick(sampler2D input, Tap t) {\n    return texSample2D(input, t.at) * t.filter;\n}\n"
            "void main() {\n    Tap tp;\n    tp.filter = 2;\n    tp.at = v_TexCoord;\n"
            "    gl_FragColor = pick(g_Texture0, tp);\n}\n",
        "struct Tap { float _we_filter; vec2 at; };\n"
        "vec4 pick(sampler2D _we_input, Tap t) {\n    return texSample2D(_we_input, t.at) * t._we_filter;\n}\n"
        "void main() {\n    Tap tp;\n    tp._we_filter = 2;\n", NULL);
    es ("a reserved word as a later name of a declaration (\"vec4 a = ..., sample;\")",
        MAIN ("    vec4 acc = vec4(0.0), sample;\n    float wsum = 0.0, filter = 2.0;\n"
              "    for (int i = 0; i < 2; ++i) {\n        sample = texSample2D(g_Texture0, v_TexCoord);\n"
              "        acc += sample * sample.a * filter;\n        wsum += sample.a;\n    }\n"
              "    gl_FragColor = acc / max(0.001, wsum);\n"),
        "    vec4 acc = vec4(0.0), _we_sample;\n    float wsum = 0.0, _we_filter = 2.0;\n"
        "    for (int i = 0; i < 2; ++i) {\n        _we_sample = texSample2D(g_Texture0, v_TexCoord);\n"
        "        acc += _we_sample * _we_sample.a * _we_filter;\n", NULL);
    es ("... but not the later name of a uniform declaration, nor an argument behind a comma",
        PRE "uniform float gain, filter;\nfloat two(float a, float b) {\n    return a + b;\n}\n"
            "void main() {\n    float t = two(gain, filter);\n    gl_FragColor = vec4(t);\n}\n",
        "uniform float gain, filter;\nfloat two(float a, float b) {\n    return a + b;\n}\n"
        "void main() {\n    float t = two(gain, filter);\n", "_we_filter");
    es ("... but not a uniform's name (it is looked up from outside), nor \"smoothstep\"",
        PRE "uniform float filter;\nvoid main() {\n    gl_FragColor = vec4(smoothstep(0.0, 1.0, filter));\n}\n",
        "uniform float filter;\nvoid main() {\n    gl_FragColor = vec4(smoothstep(0.0, 1.0, filter));\n",
        "_we_filter");
    es ("the shader's own precision statement is blanked",
        PRE "precision mediump float;\nvoid main() {\n    gl_FragColor = vec4(1.0);\n}\n",
        "varying vec2 v_TexCoord;\n                        \nvoid main() {\n", "precision mediump");
    /* ... in place: a block comment that the line opens or closes stays
     * what it was */
    es ("a precision statement on a line that opens a block comment",
        PRE "precision mediump float; /* the weights\n   float w = 1; */\nvoid main() {\n"
            "    gl_FragColor = vec4(1.0);\n}\n",
        "                         /* the weights\n   float w = 1; */\nvoid main() {\n", "//precision");
    es ("a precision statement behind the end of a block comment",
        PRE "/* high precision\n   everywhere */ precision highp float; precision highp int;\nvoid main() {\n"
            "    gl_FragColor = vec4(1.0);\n}\n",
        "/* high precision\n   everywhere */                                            \nvoid main() {\n",
        "*/ precision");   /* (not "precision highp": the GLSL ES head itself has those words) */
    /* GLSL 3.30 drivers (and Wallpaper Engine's preprocessor) read a name
     * that is no macro as 0 in an #if; GLSL ES makes it an error. A combo
     * that is off is simply not defined. */
    es ("a name that is no macro in an #if / #elif is written as the 0 it stands for",
        PRE "#define SOFT 0\n#if GLOW || SOFT == 0\nfloat edge = 0.25;\n#endif\n"
            "#if RINGS && defined(SOFT)\nfloat ring = 0.5;\n#elif !RINGS\nfloat ring = 0.75;\n#endif\n"
            "void main() {\n    gl_FragColor = vec4(edge + ring);\n}\n",
        "#define SOFT 0\n#if 0 || SOFT == 0\nfloat edge = 0.25;\n#endif\n"
        "#if 0 && defined(SOFT)\nfloat ring = 0.5;\n#elif !0\nfloat ring = 0.75;\n#endif\n", NULL);
    es ("... not the operand of defined, a macro, a name the compiler defines, a function-like use",
        PRE "#define ON 1\n#define PICK(x) x\n"
            "#if defined(GLOW) || defined SOFT || ON || GL_ES || __VERSION__ > 100 || PICK(1)\n"
            "float e2 = 1.0;\n#endif\nvoid main() {\n    gl_FragColor = vec4(e2);\n}\n",
        "#if defined(GLOW) || defined SOFT || ON || GL_ES || __VERSION__ > 100 || PICK(1)\nfloat e2 = 1.0;\n",
        NULL);
    es ("gl_FragData[0]",
        PRE "void main() {\n    gl_FragData[0] = vec4(1, 0, 0, 1);\n}\n",
        "    _we_fragColor = vec4(1, 0, 0, 1);\n", "gl_FragData");
}

/* ------------------------------- 3b. lines that are not what they look like */

static void macro_line_checks (void)
{
    /* the lines behind the backslash of a "#define" come through the pass
     * like code at file scope; the last one here looks like a global with
     * an initialiser that is no constant expression */
    es ("the last line of a #define over several lines is no global of the file",
        PRE "#define SETUP \\\n    float sa = v_TexCoord.x; \\\n    float sb = v_TexCoord.y;\n"
            "void main() {\n    SETUP\n    gl_FragColor = vec4(sa, sb, 0.0, 1.0);\n}\n",
        "    float sb = v_TexCoord.y;\nvoid main() {\n    SETUP\n", "void main() { sb");
    /* the model walks both branches of an #if it cannot evaluate: moving
     * both initialisers to main () would assign both */
    es ("a global initialiser inside an #if the model cannot evaluate stays where it is",
        PRE "#define PICK(x) x\n#if PICK(1)\nfloat k = g_Time * 2.0;\n#else\nfloat k = g_Time * 3.0;\n"
            "#endif\nvoid main() {\n    gl_FragColor = vec4(k);\n}\n",
        "float k = g_Time * 2.0;\n#else\nfloat k = g_Time * 3.0;\n#endif\nvoid main() {\n",
        "void main() { k");
}

/* ------------------------------------------------- 3c. the work limits */

static void limit_checks (void)
{
    /* Joined statements. A line that ends inside a statement is parsed
     * again with each of the lines behind it; one source may have
     * FXE_JOIN_WORK bytes parsed that way. Behind 3 such lines the
     * statement over two lines at the end is joined as always; behind 800
     * of them (which use the budget up) it is converted line by line -
     * and the single-line statement behind it still is converted. */
    for (int big = 0; big < 2; big++) {
        char *wide = g_strnfill (400, 'w');
        GString *b = g_string_new (PRE "void main() {\n    float a = 0.5;\n    float ");
        g_string_append_printf (b, "%s = 0.25;\n    float z = a +\n", wide);
        for (int i = 0; i < (big ? 800 : 3); i++)
            g_string_append_printf (b, "        %s +\n", wide);
        g_string_append (b, "        1;\n    float y = a *\n        2;\n    float w = a * 3;\n"
                            "    gl_FragColor = vec4(z + y + w);\n}\n");
        char *src = es_src (b->str, NULL), *before = built (b->str, FALSE);
        const char *want = big ? "    float y = a *\n        2;\n    float w = a * 3.0;\n"
                               : "    float y = a *         2.0;\n\n    float w = a * 3.0;\n";
        result (strstr (src, want) != NULL && count_lines (src) == count_lines (before),
                big ? "join budget used up: lines are converted one by one, the line count kept"
                    : "join budget not used up: a statement over two lines is joined", NULL);
        g_free (src);
        g_free (before);
        g_string_free (b, TRUE);
        g_free (wide);
    }
    /* Constant expressions. The check reads the bodies of the macros an
     * initialiser names, FXE_CONST_WORK bytes of them per source: A is 200
     * times B, B is 7885 bytes. Past the budget a macro is "not known to
     * be constant" and the global is assigned in main () - which means
     * the same; an initialiser checked before that keeps its place. */
    {
        GString *b = g_string_new (PRE "#define SMALL (1.0+1.0)\nfloat h = SMALL * 2.0;\n#define B (1.0");
        for (int i = 0; i < 1970; i++)
            g_string_append (b, "+1.0");
        g_string_append (b, ")\n#define A (B");
        for (int i = 0; i < 199; i++)
            g_string_append (b, "+B");
        g_string_append (b, ")\nfloat g = A;\nvoid main() {\n    gl_FragColor = vec4(h + g);\n}\n");
        char *src = es_src (b->str, NULL);
        result (strstr (src, "\nfloat h = SMALL * 2.0;\n") != NULL &&
                strstr (src, "\nfloat g;\nvoid main() { g = A;\n") != NULL,
                "constant-expression budget: a small macro is read, 1.5 MB of macro bodies are not", NULL);
        g_free (src);
        g_string_free (b, TRUE);
    }
}

/* ---------------------------------------------------- 4. valid GLSL ES */

static void untouched_checks (void)
{
    static const char *body =
        PRE "uniform float u_Scale; // {\"material\":\"scale\",\"default\":1}\n"
        "const float PI = 3.14159;\n"
        "float wave(vec2 p, float t) {\n    return 0.5 + 0.5 * sin(p.x * 20.0 + t * PI);\n}\n"
        "void main() {\n    vec4 c = texSample2D(g_Texture0, v_TexCoord);\n    int n = 3;\n"
        "    for (int i = 0; i < n; i++) {\n        c.rgb *= wave(v_TexCoord * float(i + 1), g_Time);\n    }\n"
        "    float lod = texSample2DLod(g_Texture0, v_TexCoord, 0.0).a;\n"
        "    gl_FragColor = c * u_Scale * step(0.0, lod) / max(g_Texture0Resolution.x, 1.0);\n}\n";
    int n = -1;
    char *before = built (body, FALSE);
    fx_prepass_src (&before);
    char *src = es_src (body, &n);
    result (n == 0 && !strcmp (src, before), "a shader that is valid GLSL ES is not touched", src);
    g_free (src);
    g_free (before);
    /* the vertex stage of every test pass */
    char *v = built (VS_BODY, TRUE);
    fx_prepass_src (&v);
    fx_essl_src (&v);
    result (strstr (v, "    v_TexCoord = a_TexCoord * 1.0;\n") != NULL, "vertex stage: the same pass", v);
    g_free (v);
}

/* ------------------------------------------------ 5. lexical promotion */

static void promote (const char *what, const char *line, const char *want)
{
    gboolean b = FALSE;
    char *code = fx_code_view (line, &b);
    char *r = fxe_promote (line, code);
    gboolean ok = want ? r && !strcmp (r, want) : r == NULL;
    result (ok, what, r ? r : "(no change)");
    g_free (r);
    g_free (code);
}

static void promote_checks (void)
{
    promote ("promotion: literals next to float-looking code; not indices, not integer constructors",
             "x = y * 2 + arr[3] + float(int(5)) + ivec2(1, 2).x;",
             "x = y * 2.0 + arr[3] + float(int(5)) + ivec2(1, 2).x;");
    promote ("promotion: a for header is left alone", "for (int i = 0; i < 4; i++) s += 1;",
             "for (int i = 0; i < 4; i++) s += 1.0;");
    promote ("promotion: an integer declaration up to its ';'", "int k = 3; float z = 4;",
             "int k = 3; float z = 4.0;");
    promote ("promotion: not an operand of %", "a = b % 2;", NULL);
    promote ("promotion: not an operand of a shift", "a = b << 2;", NULL);
    promote ("promotion: && is no integer operator", "if (a > 1 && b < 2)", "if (a > 1.0 && b < 2.0)");
    promote ("promotion: an object-like macro's body", "#define N 4", "#define N 4.0");
    promote ("promotion: a function-like macro's body, not its parameters",
             "#define F(x) ((x) / 3)", "#define F(x) ((x) / 3.0)");
    promote ("promotion: other directives are not code", "#if N > 2", NULL);
    promote ("promotion: case labels", "case 2:", NULL);
    promote ("promotion: float, exponent, hex and unsigned literals are none of its business",
             "x = 1.5 + 2e3 + 0x10 + 3u;", NULL);
    promote ("promotion: digits inside names", "v2 = tex2 * 2;", "v2 = tex2 * 2.0;");
    promote ("promotion: texelFetch's integers", "c = texelFetch(t, ivec2(3, 4), 0) * 2;",
             "c = texelFetch(t, ivec2(3, 4), 0) * 2.0;");
    promote ("promotion: comments are not code", "x = y; // times 2", NULL);
    /* an integer division between literals has to stay one: GLSL ES would
     * compile "1.0 / 2.0" and the picture would differ from GLSL 3.30's */
    promote ("promotion: 1 / 2 is an integer division, the line is left", "x = y + 1 / 2;", NULL);
    promote ("promotion: so is 3 * 5 / 2", "x = y - 3 * 5 / 2;", NULL);
    promote ("promotion: and (3 + 4) / 2", "x = f(y) * ((3 + 4) / 2);", NULL);
    promote ("promotion: return 7 / 2", "return 7 / 2;", NULL);
    promote ("promotion: #define THIRD (1 / 3)", "#define THIRD (1 / 3)", NULL);
    promote ("promotion: x * 6 / 4 is (x * 6) / 4, the compiler judges it", "a = x * 6 / 4;",
             "a = x * 6.0 / 4.0;");
    promote ("promotion: x * -3 / 4 likewise", "a = x * -3 / 4;", "a = x * -3.0 / 4.0;");
    promote ("promotion: an octal literal keeps its value", "a = y * 010;", NULL);
    promote ("promotion: 1.0 / 2, f(1) / 2 and y / 2 / 3 are no integer divisions of literals",
             "a = 1.0 / 2 + f(1) / 2 + y / 2 / 3;", "a = 1.0 / 2.0 + f(1.0) / 2.0 + y / 2.0 / 3.0;");
}

/* ------------------------------------------------ 6. stage 2, scripted */

/* a "driver" that accepts the source once `need` is in it and otherwise
 * names body line `line` in Mesa's wording */
typedef struct { const char *need; int line; int calls; } Fake;

static gboolean fake_try (const char *src, char *log, gsize logsize, gpointer user)
{
    Fake *f = user;
    f->calls++;
    if (strstr (src, f->need)) {
        log[0] = 0;
        return TRUE;
    }
    g_snprintf (log, logsize, "0:%d(12): error: could not implicitly convert operands\n", f->line);
    return FALSE;
}

static void stage2 (const char *what, const char *body, int line, const char *need)
{
    char *src = es_src (body, NULL);
    Fake fk = { need, line, 0 };
    char log[256];
    int budget = 48, rounds = 0;
    gboolean ok = fake_try (src, log, sizeof log, &fk);
    gboolean seen_by_stage1 = ok;
    while (!ok && rounds < 16 && fx_repair_by_lines (&src, log, fake_try, &fk, &budget)) {
        rounds++;
        ok = fake_try (src, log, sizeof log, &fk);
    }
    result (ok && !seen_by_stage1 && rounds == 1, what, src);
    g_free (src);
}

static void stage2_checks (void)
{
    /* body line 7 uses the macro of line 5 */
    stage2 ("fallback: the integer inside a function-like macro of the shader",
            PRE "#define TWICE(x) ((x) * 2)\nvoid main() {\n    float tw = TWICE(g_Time);\n"
                "    gl_FragColor = vec4(tw);\n}\n",
            7, "#define TWICE(x) ((x) * 2.0)\n");
    stage2 ("fallback: an integer literal next to a call the source model cannot type",
            PRE "#define GAIN(x) ((x) + 0.5)\nvoid main() {\n    float t2 = GAIN(g_Time) * 2;\n"
                "    gl_FragColor = vec4(t2);\n}\n",
            7, "    float t2 = GAIN(g_Time) * 2.0;\n");
    /* GLSL 3.30: the same sources get no GLSL ES candidate */
    fx_set_glsl_es (0);
    {
        char *src = built (PRE "#define TWICE(x) ((x) * 2)\nvoid main() {\n    float tw = TWICE(g_Time);\n"
                               "    gl_FragColor = vec4(tw);\n}\n", FALSE);
        char *orig = g_strdup (src);
        fx_prepass_src (&src);
        char **lines = g_strsplit (src, "\n", -1);
        guint nl = g_strv_length (lines), base = fx_body_base (lines, nl);
        GPtrArray *c = fx_candidates (lines, nl, base, base + 6);
        gboolean none = TRUE;
        for (guint i = 0; i < c->len; i++) {
            FxCand *cd = c->pdata[i];
            none &= cd->at1 == 0 && (!cd->text || !strstr (cd->text, "2.0"));
        }
        result (none && !strcmp (src, orig), "GLSL 3.30: no GLSL ES candidate, no GLSL ES rewrite", src);
        g_ptr_array_unref (c);
        g_strfreev (lines);
        g_free (src);
        g_free (orig);
    }
    fx_set_glsl_es (300);
}

/* ------------------------------------------------------------ 7. with GL */

static int n_critical;

static void count_log (const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer user)
{
    (void) domain;
    (void) user;
    if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) {
        n_critical++;
        printf ("      (log) %s\n", msg);
    }
}

static gboolean build (const char *fbody)
{
    char *vs = built (VS_BODY, TRUE), *fs = built (fbody, FALSE);
    GLuint p = fx_build_program (&vs, &fs, "test/essl", "fx-essl-test");
    if (p)
        glDeleteProgram (p);
    g_free (vs);
    g_free (fs);
    return p != 0;
}

static void gl_checks (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_ES_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip the GL part (no OpenGL ES 3 config)\n");
        return;
    }
    EGLContext ctx = EGL_NO_CONTEXT;
    for (int minor = 2; minor >= 1 && ctx == EGL_NO_CONTEXT; minor--) {
        const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE };
        ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    }
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip the GL part (no surfaceless OpenGL ES 3.1 context)\n");
        return;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    fx_set_glsl_es (-1);                /* what the context is */
    n_critical = 0;
    GLogFunc old_log = g_log_set_default_handler (count_log, NULL);
    walpe_glutil_init ();
    result (walpe_gl_feat_es () && fx_essl () == walpe_gl_essl_version (epoxy_gl_version ()) &&
            n_critical == 0, "OpenGL ES context: the built-in programs compile, effect shaders follow "
            "its GLSL ES version", NULL);
    static const struct { const char *name, *body; gboolean stage2; } sh[] = {
        { "initialisers and operators",
          MAIN ("    float a = 1;\n    float b = g_Time * 2;\n    vec2 c = v_TexCoord * 2 - 1;\n"
                "    float g = 1 / 2;\n    float h = -1;\n    gl_FragColor = vec4(a + b + g + h, c, 1);\n"),
          TRUE },
        { "built-ins",
          MAIN ("    float a = 0.5;\n    float d = max(a, 0) + pow(a, 2) + smoothstep(0, 1, g_Time);\n"
                "    int n = clamp(max(3, 4), 0, 5);\n"
                "    vec4 t = texSample2D(g_Texture0, v_TexCoord, 1) + texSample2DLod(g_Texture0, v_TexCoord, 0);\n"
                "    ivec2 ts = textureSize(g_Texture0, 0);\n"
                "    gl_FragColor = t * d * saturate(1) + texelFetch(g_Texture0, ts / 2 + n, 0) * fmod(a, 2);\n"),
          TRUE },
        { "control flow",
          MAIN ("    float a = 0.5;\n    if (a > 0) a = 2;\n    a *= 3;\n    bool cond = a > 0;\n"
                "    float f = cond ? 1 : a;\n    for (int i = 0; i < 4; i++) a += g_Time * i;\n"
                "    for (int j = 0; j < a; j++) {\n        a -= 1;\n    }\n"
                "    int n = 0;\n    while (a > 1 && n < 8) { a = a / 2; n++; }\n"
                "    gl_FragColor = vec4(a * f);\n"),
          FALSE },
        { "names and fields in front of blanks, comments and line ends",
          MAIN ("    float a = 0.5;\n    int j = 2;\n    ivec2 n = ivec2(1, 2);\n"
                "    float w = a * j   + n.y\t/ a;\n"
                "    float v = a + j /* steps */ + n.x /* first */ * a;\n"
                "    float z = a * j\n        + n.x\n        * a;\n"
                "    float y = a - j // steps\n        - 1;\n    if (n.x\n        < a) a -= j;\n"
                "    if (n.x < a) a += n.y * a;\n"
                "    gl_FragColor = vec4(w + v + z + y + a);\n"),
          FALSE },
        { "functions, macros, arrays",
          PRE "#define SAMPLES 8\nconst float K = 2;\nfloat sq(float x) { return x * x; }\n"
              "float one() { return 1; }\nvec3 tint(vec3 c, float k) {\n    return c * k;\n}\n"
              "void main() {\n    float w[3] = float[3](1, 2, 3);\n    int idx = 1;\n"
              "    float u = sq(3) + w[idx + 1] + g_Time / SAMPLES;\n    vec3 v = tint(vec3(1, 0, 0), 2);\n"
              "    float z = u *\n        2 + 1;\n    int r = idx % 3;\n    float fm = u % 2;\n"
              "    gl_FragColor = vec4(v * z * one() * K * fm, r);\n}\n",
          FALSE },
        { "GLSL ES strictness",
          PRE "uniform float u_Amount = 0.5; // {\"material\":\"amount\",\"default\":0.5}\n"
              "uniform float u_Fixed = 2;\nvec2 texel = 1.0 / g_Texture0Resolution.xy;\n"
              "precision mediump float;\nvoid main() {\n"
              "    const vec4 base = texSample2D(g_Texture0, v_TexCoord + texel);\n"
              "    vec4 sample = base * u_Amount;\n    float smooth = u_Fixed;\n"
              "    gl_FragData[0] = sample * smooth * 2;\n}\n",
          FALSE },
        { "a reserved word as a later declarator",
          MAIN ("    vec4 acc = vec4(0.0), sample;\n    float wsum = 0.0, filter = 2.0;\n"
                "    for (int i = 0; i < 2; ++i) {\n        sample = texSample2D(g_Texture0, v_TexCoord);\n"
                "        acc += sample * sample.a * filter;\n        wsum += sample.a;\n    }\n"
                "    gl_FragColor = acc / max(0.001, wsum);\n"),
          FALSE },
        { "names that are no macros in #if / #elif",
          PRE "#define SOFT 0\n#if GLOW || SOFT == 0\nfloat edge = 0.25;\n#endif\n"
              "#if RINGS && defined(SOFT)\nfloat ring = 0.5;\n#elif !RINGS\nfloat ring = 0.75;\n#endif\n"
              "void main() {\n#if SOFT == 0\n    gl_FragColor = vec4(edge + ring);\n#endif\n}\n",
          FALSE },
        { "a macro's integer and an untyped call (stage 2 in both runs)",
          PRE "#define TWICE(x) ((x) * 2)\n#define GAIN(x) ((x) + 0.5)\nvoid main() {\n"
              "    float tw = TWICE(g_Time);\n    float t2 = GAIN(g_Time) * 2;\n"
              "    gl_FragColor = vec4(tw * t2);\n}\n",
          TRUE },
    };
    for (int pass = 0; pass < 2; pass++) {
        if (pass)
            g_setenv ("WALPE_FX_NO_PREPASS", "1", TRUE);
        for (guint i = 0; i < G_N_ELEMENTS (sh); i++) {
            if (pass && !sh[i].stage2)
                continue;
            n_critical = 0;
            gboolean ok = build (sh[i].body);
            char *what = g_strdup_printf ("OpenGL ES, %s: %s builds", pass ? "the fallback alone"
                                          : "as the engine builds it", sh[i].name);
            result (ok, what, NULL);
            g_free (what);
        }
    }
    g_unsetenv ("WALPE_FX_NO_PREPASS");
    g_log_set_default_handler (old_log, NULL);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
}

int main (void)
{
    fx_set_glsl_es (300);
    head_checks ();
    conversion_checks ();
    strict_checks ();
    macro_line_checks ();
    limit_checks ();
    untouched_checks ();
    promote_checks ();
    stage2_checks ();
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        gl_checks (dpy);
        if (dpy != EGL_NO_DISPLAY)
            eglTerminate (dpy);
    } else {
        printf ("skip the GL part (no EGL surfaceless platform)\n");
    }
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
