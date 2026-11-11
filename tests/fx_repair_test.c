/* Shader repair unit check (src/engine/fx_srcrepair.h).
 *
 *   fx-repair-test [dir]
 *
 * 1. Source inspection (stage 1): every HLSL-ism is repaired from the
 *    source alone - no driver, no log. Valid GLSL must stay untouched.
 * 2. Error line numbers are read from Mesa, NVIDIA, glslang, AMD and
 *    unknown log wordings alike; warnings are skipped.
 * 3. Error-driven fallback (stage 2) with a scripted "driver" that only
 *    accepts the right rewrite and words its log like Mesa, NVIDIA,
 *    glslang, AMD or nobody we know: the same repair every time. A log
 *    without line numbers must end the fallback, not loop it.
 * 4. Varying interface: a fragment input narrower than the vertex output
 *    (3219908811's glow) is widened from the two sources; the lenient
 *    form used after a failed link takes declarations in dead branches.
 * 5. The transform effect in Vertex mode: which vertex shaders are drawn
 *    with the layer's quad, and fx_layer_affine() against the shader's
 *    own formula (offset in pixels, pivot, order of rotate/offset/scale).
 * 6. With an EGL device or Mesa's surfaceless platform: real compiles and
 *    links through fx_build_program() - default, stage 2 alone
 *    (WALPE_FX_NO_PREPASS=1) with the log as the driver wrote it and
 *    re-worded like NVIDIA / glslang (WALPE_FX_LOG_AS), and stage 1
 *    alone with the log withheld (WALPE_FX_LOG_AS=none: a driver whose
 *    wording we cannot read). Skipped without GL.
 * 7. With a directory of sources dumped by WALPE_FX_DUMP_SRC=dir
 *    (NNNN-name.vert / .frag pairs): every pass must build with the log
 *    withheld, and with the fallback alone.
 *
 * fx-repair-test --changes dir: list every line source inspection
 * rewrites in such a dump (review aid, no GL).
 *
 * The engine file is included so the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#include <epoxy/egl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <unistd.h>

static int fails, checks;

#define HEAD "#version 330\n#define GLSL 1\n#define frac fract\n#define lerp mix\n" \
             "#define mul(x, y) ((y) * (x))\n#define CAST2(x) (vec2(x))\n" \
             "#define texSample2D texture\n#define MASK 1\n#define RESOLUTION 32\n#line 1\n"

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    printf ("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, !ok && detail ? "\n      " : "",
            !ok && detail ? detail : "");
    if (!ok)
        fails++;
}

/* stage 1: body in, `want` must appear (NULL: nothing may change) */
static void st (const char *what, const char *body, const char *want)
{
    char *src = g_strconcat (HEAD, body, NULL), *orig = g_strdup (src);
    int n = fx_prepass_src (&src);
    gboolean ok = want ? strstr (src, want) != NULL : (n == 0 && !strcmp (src, orig));
    char *d = ok ? NULL
                 : g_strdup_printf ("want %s\n      got  %s", want ? want : "(unchanged)",
                                    strstr (src, "#line 1\n") + 8);
    result (ok, what, d);
    g_free (d);
    g_free (src);
    g_free (orig);
}

static char *line_of (const char *src, int ln)
{
    const char *p = strstr (src, "\n#line 1\n") + 9;
    for (int i = 1; i < ln && p; i++) {
        p = strchr (p, '\n');
        if (p)
            p++;
    }
    const char *e = p ? strchr (p, '\n') : NULL;
    return p ? g_strndup (p, e ? (gsize) (e - p) : strlen (p)) : g_strdup ("");
}

/* scripted driver: accepts the shader only when line `line` reads `want`;
 * otherwise complains about that line in its own words */
typedef struct { int line; const char *want, *fmt; int calls; int errline; } Fake;

static gboolean fake_try (const char *src, char *log, gsize n, gpointer user)
{
    Fake *f = user;
    f->calls++;
    char *l = line_of (src, f->line);
    gboolean ok = !strcmp (l, f->want);
    g_free (l);
    if (!ok)
        g_snprintf (log, n, f->fmt, f->errline ? f->errline : f->line);
    return ok;
}

static const struct { const char *name, *fmt; } wordings[] = {
    { "mesa", "0:%d(7): error: wording this code never saw\n" },
    { "nvidia", "0(%d) : error C9999: wording this code never saw\n" },
    { "glslang", "ERROR: 0:%d: 'x' : wording this code never saw\nERROR: 1 compilation errors.\n" },
    { "amd", "Fragment shader failed to compile with the following errors:\n"
             "ERROR: 0:%d: error(#160) wording this code never saw\n" },
    { "unknown", "Zeile 0:%d: Fehler, den niemand kennt\n" },
};

/* stage 2: decls on body line 1, the statement on body line 3. The
 * driver complains about line 3 until line `chk` reads `want`. */
static void fb2 (const char *what, const char *decls, const char *code, const char *want, int chk)
{
    for (guint w = 0; w < G_N_ELEMENTS (wordings); w++) {
        char *src = g_strdup_printf (HEAD "%s\nvec2 f(float a) {\n%s\n    return vec2(a);\n}\n",
                                     decls, code);
        Fake fk = { chk, want, wordings[w].fmt, 0, 3 };
        char log[512];
        int budget = 48, rounds = 0;
        gboolean ok = fake_try (src, log, sizeof (log), &fk);
        while (!ok && rounds < 16 && fx_repair_by_lines (&src, log, fake_try, &fk, &budget)) {
            rounds++;
            ok = fake_try (src, log, sizeof (log), &fk);
        }
        char *got = line_of (src, chk);
        char *name = g_strdup_printf ("fallback %-8s %s", wordings[w].name, what);
        char *d = ok ? NULL : g_strdup_printf ("want %s\n      got  %s", want, got);
        result (ok, name, d);
        g_free (d);
        g_free (name);
        g_free (got);
        g_free (src);
    }
}

static void fb (const char *what, const char *decls, const char *code, const char *want)
{
    fb2 (what, decls, code, want, 3);
}

static void log_lines (const char *what, const char *log, int n, int first)
{
    GArray *a = g_array_new (FALSE, FALSE, sizeof (int));
    fx_log_error_lines (log, a);
    gboolean ok = (int) a->len == n && (n == 0 || g_array_index (a, int, 0) == first);
    char *d = ok ? NULL : g_strdup_printf ("want %d line(s), first %d; got %u, first %d", n, first,
                                           a->len, a->len ? g_array_index (a, int, 0) : 0);
    result (ok, what, d);
    g_free (d);
    g_array_unref (a);
}

static void static_checks (void)
{
    /* the constructs of the inventory's workshop shaders */
    st ("init float -> vec2", "uniform float g_Strength;\nvoid main() {\n"
        "    vec2 strength = g_Strength * g_Strength * 60;\n}\n",
        "    vec2 strength = vec2(g_Strength * g_Strength * 60);");
    st ("init vec4 -> vec3 (swizzle)", "void main() {\n    vec4 tex = vec4(1.0);\n"
        "    vec3 c = tex.rgba;\n}\n", "    vec3 c = vec3(tex.rgba);");
    st ("for-init float -> int", "uniform float u_Min;\nuniform float u_Max;\nvoid main() {\n"
        "    for (int i = u_Min; i < u_Max; i++) {\n    }\n}\n",
        "    for (int i = int(u_Min); i < u_Max; i++) {");
    st ("for-init negated float local", "uniform float u_it;\nvoid main() {\n"
        "    float iterations = int(u_it);\n    for (int i = -iterations; i <= iterations; i++){\n"
        "    }\n}\n", "    for (int i = int(-iterations); i <= iterations; i++){");
    st ("return float -> vec2", "vec2 f(float a) {\n    return a * 2.0;\n}\n",
        "    return vec2(a * 2.0);");
    st ("compound vec2 /= vec4", "uniform vec4 g_Texture0Resolution;\nvoid main() {\n"
        "    vec2 strength = vec2(1.0);\n    strength /= g_Texture0Resolution;\n}\n",
        "    strength /= vec2(g_Texture0Resolution);");
    st ("compound float += vec2", "void main() {\n    float x = 1.0; vec2 uv = vec2(0.5);\n"
        "    x += uv;\n}\n", "    x += float(uv);");
    st ("two compounds on a line, int *= float", "void main() {\n    int barLeft = 1; int barRight = 1;\n"
        "    float isLeft = 0.0; float isRight = 1.0;\n"
        "    barLeft *= isLeft; barRight *= isRight;\n}\n",
        "    barLeft *= int(isLeft); barRight *= int(isRight);");
    st ("plain assignment float -> vec2", "void main() {\n    vec2 uv;\n    uv = 0.5;\n}\n",
        "    uv = vec2(0.5);");
    st ("assignment after else", "void main() {\n    vec3 c; float k = 1.0;\n    if (k > 0.5) c = vec3(k);\n"
        "    else c = k;\n}\n", "    else c = vec3(k);");
    st ("bool used as a number", "void main() {\n    float depth = 0.5; float lim = 0.2;\n"
        "    depth *= (depth < lim) * 6.0;\n}\n", "    depth *= float(depth < lim) * 6.0;");
    st ("compound with a bare comparison", "void main() {\n    float x = 0.0; float a = 1.0;\n"
        "    x += a < 0.5;\n}\n", "    x += float(a < 0.5);");
    st ("float modulo, then int initializer", "void main() {\n    float frequency = 3.5;\n"
        "    int barFreq1 = frequency % RESOLUTION;\n}\n",
        "    int barFreq1 = int(mod(float(frequency), float(RESOLUTION)));");
    st ("mix(vec4, vec3, float): truncate", "void main() {\n    vec4 albedo = vec4(1.0); vec3 n = vec3(0.0);\n"
        "    float mask = 0.5;\n    albedo.rgb = mix(albedo, n, mask);\n}\n",
        "    albedo.rgb = mix(vec3(albedo), n, mask);");
    st ("max(int, vec3) inside a constructor: splat", "out vec4 o;\nvoid main() {\n"
        "    vec4 albedo = vec4(1.0);\n    o = vec4(max(0, albedo.rgb), albedo.a);\n}\n",
        "    o = vec4(max(vec3(0), albedo.rgb), albedo.a);");
    st ("pow(vec3, float): splat", "void main() {\n    vec3 c = vec3(0.5);\n    c = pow(c, 2.2);\n}\n",
        "    c = pow(c, vec3(2.2));");
    st ("texSample2D with a vec4 coordinate", "uniform sampler2D g_Texture0;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n    vec4 a = texSample2D(g_Texture0, v_TexCoord);\n}\n",
        "    vec4 a = texSample2D(g_Texture0, vec2(v_TexCoord));");
    st ("uint and int mixed: int throughout", "void main() {\n    uint a = 3u;\n    int b = 2;\n"
        "    int c = a * b;\n}\n", "    int a = 3;");
    st ("stray #endif", "void main() {\n}\n#endif\n", "// (stray) #endif");
    st ("stray #else", "#else\nvoid main() {\n}\n", "// (stray) #else");
    st ("uniform in #if", "uniform vec4 g_Texture0Resolution;\n#if g_Texture0Resolution.y <= 640\n"
        "#define K 1\n#else\n#define K 2\n#endif\nvoid main() {\n}\n",
        "#if 0 // was: #if g_Texture0Resolution.y <= 640");
    st ("macro and uniform types", "#define SCALE 2.5\nuniform vec3 u_Color;\nvoid main() {\n"
        "    vec2 a = SCALE;\n    float l = u_Color;\n}\n", "    vec2 a = vec2(SCALE);");
    st ("local shadows a uniform", "uniform vec2 scale;\nvoid main() {\n    float scale = 2.0;\n"
        "    vec2 a = scale;\n}\n", "    vec2 a = vec2(scale);");
    st ("parameter type, block scope ends", "float g(vec3 v) {\n    float r = v;\n    return r;\n}\n"
        "void main() {\n    float v = 1.0;\n    float q = v;\n}\n", "    float r = float(v);");
    st ("code in a dead #if branch is not typed", "uniform float g_T;\nvoid main() {\n#if MASK == 0\n"
        "    vec2 a = g_T;\n#else\n    vec2 a = vec2(g_T);\n#endif\n}\n", NULL);
    st ("active #if branch is repaired", "uniform float g_T;\nvoid main() {\n#if MASK == 1\n"
        "    vec2 a = g_T;\n#endif\n}\n", "    vec2 a = vec2(g_T);");

    /* valid GLSL: nothing may change */
    st ("valid: stock-style fragment shader",
        "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nuniform float g_Time;\n"
        "uniform float g_Alpha;\nuniform vec3 g_Color;\nuniform vec4 g_Texture0Resolution;\n"
        "uniform mat4 g_M;\nvarying vec4 v_TexCoord;\nout vec4 o;\n"
        "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "vec3 tone(vec3 c, float k)\n{\n    return mix(c, vec3(lum(c)), k);\n}\n"
        "void main() {\n"
        "    vec4 albedo = texSample2D(g_Texture0, v_TexCoord.xy);\n"
        "    float mask = texSample2D(g_Texture1, v_TexCoord.zw).g;\n"
        "    vec2 uv = (v_TexCoord.xy - 0.5) * 2.0;\n"
        "    uv *= 2.0; uv.x *= g_Texture0Resolution.x / g_Texture0Resolution.y;\n"
        "    float d = length(uv) - 0.5, e = smoothstep(0.0, 1.0, d);\n"
        "    float t = frac(g_Time * 0.1) > 0.5 ? 1.0 : 0.0;\n"
        "    bool on = (d < 0.5);\n"
        "    float s = on ? 1.0 : 0.0;\n"
        "    vec3 c = clamp(albedo.rgb * g_Color, 0.0, 1.0);\n"
        "    c = max(c, 0.0); c = pow(c, vec3(2.2)); c = mix(c, tone(c, 0.5), mask);\n"
        "    c += step(0.5, c) * float(d < 0.2) + CAST2(e).xyx * t * s;\n"
        "    vec4 p = mul(vec4(uv, 0.0, 1.0), g_M);\n"
        "    int i = 0; float f = 1; f *= 2; i += 1;\n"
        "    for (int k = 0; k < 4; k++) { f += float(k) * 0.25; }\n"
        "    albedo.rgb = c * p.x; albedo.a *= g_Alpha * f;\n"
        "    o = albedo;\n}\n", NULL);
    st ("valid: uint hash without int mixing",
        "uint hash(uint x) {\n    x ^= x >> 16;\n    x *= 0x7feb352du;\n    return x;\n}\n"
        "void main() {\n    uint h = hash(7u);\n    float r = float(h) / 4294967295.0;\n}\n", NULL);
    st ("valid: ternary on vectors in arithmetic",
        "void main() {\n    vec3 a = vec3(1.0), b = vec3(0.5), col = vec3(0.2); float k = 0.3;\n"
        "    col = col * (k < 0.5 ? a : b);\n}\n", NULL);
    st ("valid: balanced conditionals", "#if MASK\n#define A 1\n#elif RESOLUTION == 16\n#define A 2\n"
        "#else\n#define A 3\n#endif\n#ifdef NOPE\n#endif\nvoid main() {\n}\n", NULL);
}

static void fallback_checks (void)
{
    log_lines ("log lines: mesa", "0:109(12): error: x\n0:120(3): error: y\n", 2, 109);
    log_lines ("log lines: nvidia", "0(109) : error C7011: implicit cast from \"float\" to \"vec2\"\n",
               1, 109);
    log_lines ("log lines: nvidia warning skipped",
               "0(12) : warning C7050: \"uv\" might be used before being initialized\n", 0, 0);
    log_lines ("log lines: glslang", "ERROR: 0:42: 'x' : undeclared identifier\n"
               "ERROR: 1 compilation errors.  No code generated.\n", 1, 42);
    log_lines ("log lines: amd", "Vertex shader failed to compile with the following errors:\n"
               "ERROR: 0:7: error(#143) Undeclared identifier: foo\nERROR: error(#273) 1 compilation errors.\n",
               1, 7);
    log_lines ("log lines: none in a link log", "error: vertex shader output type mismatch\n", 0, 0);

    /* u(), g(), h() ... are unknown to the inferencer: only the declared
     * types on the line are known, the driver is the judge */
    fb ("initializer", "", "    vec2 uv = u(a);", "    vec2 uv = vec2(u(a));");
    fb ("return", "", "    return u(a);", "    return vec2(u(a));");
    fb ("compound", "float x;", "    x += u(a);", "    x += float(u(a));");
    fb ("plain assignment", "vec2 uv;", "    uv = u(a);", "    uv = vec2(u(a));");
    fb ("modulo", "", "    float m = h(a) % 3;", "    float m = mod(float(h(a)), float(3));");
    fb ("modulo + initializer", "", "    int m = h(a) % 3;", "    int m = int(mod(float(h(a)), float(3)));");
    fb ("call arguments", "vec3 v;", "    v = mix(p(), q(), w());",
        "    v = mix(vec3(p()), vec3(q()), vec3(w()));");
    fb ("bool arithmetic", "float d;", "    d = (u(a) < 0.5) * 6.0;", "    d = float(u(a) < 0.5) * 6.0;");
    fb ("stray #endif", "", "#endif", "// (stray) #endif");
    fb ("uniform in #if", "", "#if u_x.y <= 640", "#if 0 // was: #if u_x.y <= 640");
    fb2 ("uint -> int in the body", "uint q;", "    int c = q * u(a);", "int q;", 1);
    fb ("const local with a run-time value", "uniform sampler2D t;",
        "    const vec4 b = texSample2D(t, vec2(a)); // base", "    vec4 b = texSample2D(t, vec2(a)); // base");
    {
        /* not offered at file scope (a global needs a constant either way) */
        char *src = g_strdup (HEAD "const vec2 g = u();\nvoid main() {\n}\n");
        char **ll = g_strsplit (src, "\n", -1);
        guint nl = g_strv_length (ll), base = fx_body_base (ll, nl);
        GPtrArray *c = fx_candidates (ll, nl, base, base);
        gboolean none = TRUE;
        for (guint i = 0; i < c->len; i++) {
            FxCand *cd = c->pdata[i];
            none &= !cd->text || strncmp (cd->text, "vec2 g", 6) != 0;
        }
        result (none, "fallback: a file-scope const keeps its const", NULL);
        g_ptr_array_unref (c);
        g_strfreev (ll);
        g_free (src);
    }

    /* a log that names no line: nothing to try, and no endless loop */
    {
        char *src = g_strdup (HEAD "void main() {\n    vec2 uv = u();\n}\n");
        Fake fk = { 2, "never", "error: no line anywhere\n", 0, 0 };
        int budget = 48;
        gboolean ch = fx_repair_by_lines (&src, "error: no line anywhere\n", fake_try, &fk, &budget);
        result (!ch && fk.calls == 0, "fallback: log without line numbers ends it", NULL);
        g_free (src);
    }
    /* the driver keeps naming the line: bounded */
    {
        char *src = g_strdup (HEAD "void main() {\n    vec2 uv = u();\n}\n");
        Fake fk = { 2, "never", "0(%d) : error C1: no\n", 0, 0 };
        char log[128];
        int budget = 48, rounds = 0;
        fake_try (src, log, sizeof (log), &fk);
        while (rounds < 16 && fx_repair_by_lines (&src, log, fake_try, &fk, &budget))
            rounds++;
        result (rounds == 0 && fk.calls <= 49, "fallback: unrepairable line gives up", NULL);
        g_free (src);
    }
}

#define VS_HEAD "#version 330\n#define varying out\n#define attribute in\n#define mul(x, y) ((y) * (x))\n#line 1\n"
#define FS_HEAD "#version 330\n#define varying in\nout vec4 _we_fragColor;\n" \
                "#define gl_FragColor _we_fragColor\n#define texSample2D texture\n#define MASK 1\n#line 1\n"
#define VS_BODY "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n" \
                "attribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\nvoid main() {\n" \
                "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n" \
                "    v_TexCoord.xy = a_TexCoord;\n}\n"

static void varying_checks (void)
{
    /* 3219908811: vertex "varying vec4 v_TexCoord", fragment "varying vec2" */
    char *vs = g_strdup (VS_HEAD VS_BODY);
    char *fs = g_strdup (FS_HEAD "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
                         "void main() {\n    vec2 p = v_TexCoord;\n"
                         "    gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy) + p.xyxy;\n}\n");
    gboolean ch = fx_varying_repair (&vs, &fs, FALSE);
    result (ch && strstr (fs, "varying vec4 v_TexCoord;") && strstr (fs, "vec2 p = v_TexCoord.xy;") &&
            strstr (fs, "texSample2D(g_Texture0, v_TexCoord.xy)") && !strstr (vs, ".xy.xy"),
            "varying: fragment vec2 input of a vec4 output widened", ch ? fs : "(unchanged)");
    result (!fx_varying_repair (&vs, &fs, FALSE), "varying: second run finds nothing", NULL);
    g_free (vs);
    g_free (fs);

    /* the wider declaration sits in a branch that is not compiled */
    vs = g_strdup (VS_HEAD "#if NOPE\nvarying vec4 v_TexCoord;\n#else\nvarying vec2 v_TexCoord;\n#endif\n"
                   "void main() {\n}\n");
    fs = g_strdup (FS_HEAD "varying vec2 v_TexCoord;\nvoid main() {\n}\n");
    result (!fx_varying_repair (&vs, &fs, FALSE), "varying: dead-branch declaration ignored (strict)", NULL);
    result (fx_varying_repair (&vs, &fs, TRUE) && strstr (fs, "varying vec4 v_TexCoord;"),
            "varying: taken after a failed link (lenient)", NULL);
    g_free (vs);
    g_free (fs);

    /* same widths, ints, arrays: untouched */
    vs = g_strdup (VS_HEAD "varying vec2 v_A;\nflat varying int v_I;\nvarying vec3 v_B, v_C;\nvoid main() {\n}\n");
    fs = g_strdup (FS_HEAD "varying vec2 v_A;\nflat varying int v_I;\nvarying vec3 v_B, v_C;\nvoid main() {\n}\n");
    result (!fx_varying_repair (&vs, &fs, FALSE) && !fx_varying_repair (&vs, &fs, TRUE),
            "varying: matching interface untouched", NULL);
    g_free (vs);
    g_free (fs);
}

/* ---- the transform effect's Vertex mode (scenenode_effects.c)
 *
 * A vertex shader of this test's own with the shape of the transform
 * effect's: a function that turns a point about the centre, then offsets
 * and scales it, applied to the position (MODE 1) or to the texture
 * coordinate (MODE 0). fx_vertex_traits () knows the transform effect by
 * the keys of three of its statements and the local copy of the position
 * by two more (fx_keys.h); xf_keys () registers the keys of the
 * statements below in those slots. The uniform names are the ones the
 * engine asks the linked program for (fx_vertex_setup). */
#define XF_ROT "v = turnAbout(v - vec2(0.5), -g_Direction);"
#define XF_RET "return vec2(0.5) + g_Scale * (v + g_Offset);"
#define XF_APPLY "position.xy = placeFx(position.xy);"
#define XF_LOCAL "vec3 position = vec3(a_Position);"
#define XF_MVP "mul(vec4(position.xyz, 1.0), g_ModelViewProjectionMatrix)"
#define XF_BODY(extra) \
    "varying vec2 v_TexCoord;\nvec2 placeFx(vec2 v) {\n\t" XF_ROT "\n\t" XF_RET "\n}\n" \
    "void main() {\n\t" XF_LOCAL "\n" \
    "#if MODE == 1\n\t" XF_APPLY "\n#endif\n" extra \
    "\tgl_Position = " XF_MVP ";\n\tv_TexCoord = a_TexCoord;\n" \
    "#if MODE == 0\n\tv_TexCoord = placeFx(v_TexCoord);\n#endif\n}\n"
#define XF_VERT(mode, extra) "#version 330\n#define MODE " mode "\n#line 1\n" \
    "uniform mat4 g_ModelViewProjectionMatrix;\nuniform vec2 g_Offset;\nuniform vec2 g_Scale;\n" \
    "uniform float g_Direction;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n" \
    XF_BODY (extra)

static void xf_keys (void)
{
    fxk_test_register (FXK_XF_ROT_NEG, XF_ROT);
    fxk_test_register (FXK_XF_RETURN, XF_RET);
    fxk_test_register (FXK_XF_APPLY, XF_APPLY);
    fxk_test_register (FXK_POS_LOCAL, XF_LOCAL);
    fxk_test_register (FXK_MVP_LOCAL, XF_MVP);
}

static void traits (const char *what, const char *vsrc, gboolean moves, int sign)
{
    EffPass p;
    memset (&p, 0, sizeof (p));
    fx_vertex_traits (&p, vsrc);
    char *d = g_strdup_printf ("moves_verts %d (want %d), xf_sign %d (want %d)", p.moves_verts, moves,
                               p.xf_sign, sign);
    result (p.moves_verts == moves && p.xf_sign == sign, what, d);
    g_free (d);
}

/* placeFx () of the shader above, evaluated directly */
static void apply_fx (const float off[2], const float sc[2], float ang, float x, float y, float *ox,
                      float *oy)
{
    float vx = x - 0.5f, vy = y - 0.5f, r = -ang, cs = cosf (r), sn = sinf (r);
    float rx = vx * cs - vy * sn, ry = vx * sn + vy * cs;
    *ox = (rx + off[0]) * sc[0] + 0.5f;
    *oy = (ry + off[1]) * sc[1] + 0.5f;
}

static EffectChain *xf_chain (float ox, float oy, float sx, float sy, float ang)
{
    EffectChain *ch = g_new0 (EffectChain, 1);
    EffPass *p = &ch->passes[0];
    ch->n_passes = 1;
    ch->enabled = TRUE;
    ch->vertex_affine = TRUE;
    p->consts = g_new0 (FxConst, 3);
    p->n_consts = 3;
    p->consts[0].v[0] = ox;
    p->consts[0].v[1] = oy;
    p->consts[1].v[0] = sx;
    p->consts[1].v[1] = sy;
    p->consts[2].v[0] = ang;
    p->xf[0] = 0;
    p->xf[1] = 1;
    p->xf[2] = 2;
    p->xf_sign = -1;
    return ch;
}

static void transform_checks (void)
{
    traits ("vertex traits: transform, Vertex mode", XF_VERT ("1", ""), TRUE, -1);
    traits ("vertex traits: transform, UV mode (plain copy of a_Position)", XF_VERT ("0", ""), FALSE, 0);
    traits ("vertex traits: other shader that moves the position (skew)",
            XF_VERT ("0", "\tposition.x += 100.0 * a_TexCoord.y;\n"), TRUE, 0);
    traits ("vertex traits: plain MVP pass",
            "#version 330\n#line 1\nuniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
            "void main() {\n\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n}\n",
            FALSE, 0);
    traits ("vertex traits: unit-quad pass of an internal target",
            "#version 330\n#line 1\nattribute vec3 a_Position;\nvoid main() {\n"
            "\tgl_Position = vec4(a_Position, 1.0);\n}\n", FALSE, 0);

    /* fx_layer_affine(): one effect, then two in a row, against placeFx() */
    struct FxSet set;
    memset (&set, 0, sizeof (set));
    EffectChain *chains[3] = { NULL, xf_chain (-500, 0, 1, 1, 0), xf_chain (12, -160, 1.5f, 0.75f, 0.67f) };
    set.chains = chains;
    set.n = 3;
    float m[6];
    const float pts[4][2] = { { -233, 74.5f }, { 233, -74.5f }, { 0, 0 }, { 17, -3 } };
    set.fold_from = -1;
    result (!fx_layer_affine (&set, m) && m[0] == 1 && m[3] == 1 && m[4] == 0 && m[5] == 0,
            "layer affine: nothing folded = identity", NULL);
    set.fold_from = 2;
    gboolean ok = fx_layer_affine (&set, m);
    for (int i = 0; i < 4 && ok; i++) {
        float ex, ey;
        apply_fx (chains[2]->passes[0].consts[0].v, chains[2]->passes[0].consts[1].v, 0.67f,
                  pts[i][0], pts[i][1], &ex, &ey);
        float gx = m[0] * pts[i][0] + m[2] * pts[i][1] + m[4];
        float gy = m[1] * pts[i][0] + m[3] * pts[i][1] + m[5];
        ok = fabsf (gx - ex) < 1e-2f && fabsf (gy - ey) < 1e-2f;
    }
    result (ok, "layer affine: offset + scale + angle = placeFx()", NULL);
    set.fold_from = 1;
    ok = fx_layer_affine (&set, m);
    for (int i = 0; i < 4 && ok; i++) {
        float ax, ay, ex, ey;
        apply_fx (chains[1]->passes[0].consts[0].v, chains[1]->passes[0].consts[1].v, 0, pts[i][0],
                  pts[i][1], &ax, &ay);
        apply_fx (chains[2]->passes[0].consts[0].v, chains[2]->passes[0].consts[1].v, 0.67f, ax, ay,
                  &ex, &ey);
        float gx = m[0] * pts[i][0] + m[2] * pts[i][1] + m[4];
        float gy = m[1] * pts[i][0] + m[3] * pts[i][1] + m[5];
        ok = fabsf (gx - ex) < 1e-2f && fabsf (gy - ey) < 1e-2f;
    }
    result (ok, "layer affine: two effects, the earlier one first", NULL);
    /* "-500 0": the layer moves 500 scene pixels to the left, nothing else */
    set.n = 2;
    ok = fx_layer_affine (&set, m) && m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 &&
         fabsf (m[4] + 500) < 1e-3f && fabsf (m[5]) < 1e-3f;
    result (ok, "layer affine: offset is in layer pixels", NULL);
    /* a disabled effect is not applied */
    chains[1]->enabled = FALSE;
    result (!fx_layer_affine (&set, m), "layer affine: disabled effect skipped", NULL);
    for (int i = 1; i < 3; i++) {
        g_free (chains[i]->passes[0].consts);
        g_free (chains[i]);
    }
}

/* ---- real compiler */

static void gl_run (EGLDisplay dpy, const char *dir);

static void gl_checks (const char *dir)
{
    int ran = 0;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd; i++, ran++)
                gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL), dir);
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL),
                dir);
        ran++;
    }
    if (!ran)
        printf ("skip GL checks (no EGL device / surfaceless platform)\n");
}

static const struct { const char *name, *noprepass, *logas; } modes[] = {
    { "source inspection + fallback (default)", NULL, NULL },
    { "source inspection alone, log withheld", NULL, "none" },
    { "fallback alone, driver's own log", "1", NULL },
    { "fallback alone, log worded like NVIDIA", "1", "nvidia" },
    { "fallback alone, log worded like glslang", "1", "glslang" },
};

static void set_mode (guint m)
{
    if (modes[m].noprepass)
        g_setenv ("WALPE_FX_NO_PREPASS", modes[m].noprepass, TRUE);
    else
        g_unsetenv ("WALPE_FX_NO_PREPASS");
    if (modes[m].logas)
        g_setenv ("WALPE_FX_LOG_AS", modes[m].logas, TRUE);
    else
        g_unsetenv ("WALPE_FX_LOG_AS");
}

static gboolean build (const char *vbody, const char *fbody)
{
    char *vs = g_strconcat (VS_HEAD, vbody, NULL), *fs = g_strconcat (FS_HEAD, fbody, NULL);
    GLuint prog = fx_build_program (&vs, &fs, "test", "test");
    if (prog)
        glDeleteProgram (prog);
    g_free (vs);
    g_free (fs);
    return prog != 0;
}

/* The transform effect in Vertex mode through the real executor: as a
 * pass in layer units (content moves by pixels inside the layer's buffer,
 * what leaves it is clipped), and left to the caller when it is the last
 * effect (fx_layer_affine). */
static void gl_transform_check (void)
{
    char *vs = g_strdup (
        "#version 330\n#define MODE 1\n#define varying out\n"
        "#define attribute in\n#define mul(a, b) ((b) * (a))\n#line 1\n"
        "vec2 turnAbout(vec2 p, float a) { float c = cos(a), s = sin(a);\n"
        "  return vec2(c * p.x - s * p.y, s * p.x + c * p.y); }\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\nuniform vec2 g_Offset;\nuniform vec2 g_Scale;\n"
        "uniform float g_Direction;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
        XF_BODY (""));
    char *fs = g_strdup (FS_HEAD "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
                         "void main() {\n    gl_FragColor = texSample2D(g_Texture0, v_TexCoord);\n}\n");
    EffectChain *ch = g_new0 (EffectChain, 1);
    EffPass *p = &ch->passes[0];
    ch->n_passes = 1;
    ch->enabled = TRUE;
    ch->cacheable = -1;
    fx_vertex_traits (p, vs);
    p->program = fx_build_program (&vs, &fs, "transform", "test");
    if (!p->program) {
        result (FALSE, "transform pass: program builds", NULL);
        g_free (vs);
        g_free (fs);
        g_free (ch);
        return;
    }
    fx_cache_uniforms (p);
    fx_vertex_setup (p);
    ch->vertex_affine = p->xf_sign != 0 && p->xf[0] >= 0 && p->xf[1] >= 0 && p->xf[2] >= 0;
    result (p->moves_verts && ch->vertex_affine && p->mvp_loc >= 0,
            "transform pass: recognised (moves vertices, three constants found)", NULL);
    if (ch->vertex_affine) {
        FxConst *off = &p->consts[p->xf[0]], *sc = &p->consts[p->xf[1]];
        off->v[0] = 2;
        off->v[1] = 1;
        sc->v[0] = sc->v[1] = 1;
        off->dirty = sc->dirty = TRUE;
    }
    /* 8x4 layer: left half white, right half black, opaque; row 0 = top */
    guint8 in[4 * 8 * 4], out[4 * 8 * 4];
    for (int i = 0; i < 32; i++) {
        guint8 v = (i % 8) < 4 ? 255 : 0;
        in[i * 4] = in[i * 4 + 1] = in[i * 4 + 2] = v;
        in[i * 4 + 3] = 255;
    }
    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 8, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, in);
    const float lay[4] = { 0, 0, 8, 4 };
    GLuint o = fx_chain_run (ch, tex, 8, 4, 8, 4, 0.0, 1.0f, lay, TRUE);
    glBindTexture (GL_TEXTURE_2D, o);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
#define PX(x, y) (&out[((y) * 8 + (x)) * 4])
    /* offset (2, 1) = two pixels right, one pixel up (towards row 0):
     * columns 0-1 and the bottom row are empty, old column 0 (white) is at
     * column 2, old column 4 (black) at column 6 */
    gboolean ok = PX (0, 0)[3] == 0 && PX (1, 2)[3] == 0 && PX (2, 3)[3] == 0 && PX (7, 3)[3] == 0 &&
                  PX (2, 0)[3] == 255 && PX (2, 0)[0] == 255 && PX (5, 2)[0] == 255 &&
                  PX (6, 0)[3] == 255 && PX (6, 0)[0] == 0 && PX (7, 2)[0] == 0;
    char *d = g_strdup_printf ("row0 alpha %d %d %d %d %d %d %d %d, row3 alpha %d %d, (2,0) r %d, (6,0) r %d",
                               PX (0, 0)[3], PX (1, 0)[3], PX (2, 0)[3], PX (3, 0)[3], PX (4, 0)[3],
                               PX (5, 0)[3], PX (6, 0)[3], PX (7, 0)[3], PX (2, 3)[3], PX (7, 3)[3],
                               PX (2, 0)[0], PX (6, 0)[0]);
    result (ok, "transform pass mid-chain: moves by layer pixels, y up, clipped to the buffer", d);
    g_free (d);
#undef PX
    /* last effect: not run, handed to the caller */
    struct FxSet set;
    memset (&set, 0, sizeof (set));
    EffectChain *chains[1] = { ch };
    set.chains = chains;
    set.n = 1;
    set.cacheable = -1;
    set.fold_from = set.key_fold = -1;
    float m[6];
    fx_set_layer_geometry (0, 0, 8, 4, TRUE);
    GLuint r1 = fx_run_for_object (&set, tex, 8, 4, 8, 4, 0.0, 1.0f, FALSE);
    gboolean folded = r1 == tex && fx_layer_affine (&set, m) && fabsf (m[4] - 2) < 1e-4f &&
                      fabsf (m[5] - 1) < 1e-4f && m[0] == 1 && m[3] == 1;
    result (folded, "transform as last effect: left to the caller as an affine map", NULL);
    fx_set_layer_geometry (0, 0, 8, 4, FALSE);
    GLuint r2 = fx_run_for_object (&set, tex, 8, 4, 8, 4, 0.0, 1.0f, FALSE);
    result (r2 != tex && !fx_layer_affine (&set, m), "transform without folding: drawn as a pass", NULL);
    glDeleteTextures (1, &tex);
    fx_chain_free (ch);
    g_free (vs);
    g_free (fs);
}

/* ---- render targets (scenenode_effects.c): formats, clears, the pool */
static EffectChain *target_test_chain (const char *fbody, gboolean partial)
{
    char *vs = g_strdup (VS_HEAD "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
                         "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvoid main() {\n"
                         "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
                         "    v_TexCoord = a_TexCoord;\n}\n");
    char *fs = g_strconcat (FS_HEAD, fbody, NULL);
    EffectChain *ch = g_new0 (EffectChain, 1);
    EffPass *p = &ch->passes[0];
    ch->n_passes = 1;
    ch->enabled = TRUE;
    ch->cacheable = -1;
    ch->feedback = -1;
    fx_vertex_traits (p, vs);
    if (fx_fragment_may_discard (fbody))
        p->partial = TRUE;
    p->program = fx_build_program (&vs, &fs, "target", "test");
    g_free (vs);
    g_free (fs);
    if (!p->program || p->partial != partial) {
        if (p->program)
            fx_chain_free (ch);
        else
            g_free (ch);
        return NULL;
    }
    fx_cache_uniforms (p);
    fx_vertex_setup (p);
    /* identity matrix, as the executor leaves it for the unit quad */
    static const float ident[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    glUseProgram (p->program);
    if (p->mvp_loc >= 0)
        glUniformMatrix4fv (p->mvp_loc, 1, GL_FALSE, ident);
    return ch;
}

static GLuint target_test_tex (int w, int h, guint8 r, guint8 g, guint8 b, guint8 a)
{
    guint8 *px = g_malloc ((gsize) w * h * 4);
    for (int i = 0; i < w * h; i++) {
        px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = a;
    }
    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    g_free (px);
    return tex;
}

static void target_read (GLuint tex, guint8 *out)
{
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

static void gl_target_checks (void)
{
    /* fbos[].format names */
    result (fx_rt_format_from_name ("rg88") == FX_FMT_RG8 && fx_rt_format_from_name ("r8") == FX_FMT_R8 &&
            fx_rt_format_from_name ("RG1616F") == FX_FMT_RG16F &&
            fx_rt_format_from_name ("r16f") == FX_FMT_R16F &&
            fx_rt_format_from_name ("rgba_backbuffer") == FX_FMT_RGBA8 &&
            fx_rt_format_from_name ("rgba8888") == FX_FMT_RGBA8 &&
            fx_rt_format_from_name (NULL) == FX_FMT_RGBA8 &&
            fx_rt_format_from_name ("(capture)") == FX_FMT_RGBA8 &&
            fx_rt_format_from_name ("no-such-format") == FX_FMT_RGBA8,
            "target formats: declared names map to their format, the rest to RGBA8", NULL);
    result (fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);"
                                   "v_TexCoord=a_TexCoord;}") &&
            fx_vertex_plain_cover ("voidmain(){vec3position=vec3(a_Position);"
                                   "gl_Position=mul(vec4(position.xyz,1.0),g_ModelViewProjectionMatrix);}") &&
            fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);"
                                   "v_Screen=gl_Position.xyw;if(gl_Position.x==0.0)v_A=1.0;}") &&
            !fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);"
                                    "gl_Position.xy*=0.5;}") &&
            !fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);"
                                    "gl_Position+=vec4(0.1,0.0,0.0,0.0);}") &&
            !fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);"
                                    "gl_Position.x=0.0;}") &&
            !fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix)*0.5;}") &&
            !fx_vertex_plain_cover ("voidmain(){gl_Position=mul(vec4(a_Position,1.0),g_OtherMatrix);}") &&
            !fx_vertex_plain_cover ("voidmain(){v_TexCoord=a_TexCoord;}"),
            "clears: only a vertex shader that writes gl_Position once, as the plain product, counts as full cover "
            "(reads of it are fine)",
            NULL);
    result (fx_fragment_may_discard ("void main() { if (a < 0.5) discard; }") &&
            fx_fragment_may_discard ("void main() { clip(a - 0.5); }") &&
            !fx_fragment_may_discard ("void main() { gl_FragColor = vec4(1.0); }"),
            "clears: a shader that can drop fragments is recognised", NULL);

    /* a declared two-channel target is made as one, and counted as one */
    gint64 b0 = 0, b1 = 0;
    int n0 = 0, n1 = 0;
    fx_targets_stats (&b0, &n0);
    GLuint fbo = 0, tex = 0;
    FxRtFormat got = fx_make_rt_fmt (&fbo, &tex, 16, 8, FX_FMT_RG8);
    GLint ifmt = 0;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    fx_targets_stats (&b1, &n1);
    result (got == FX_FMT_RG8 && ifmt == GL_RG8 && b1 - b0 == 16 * 8 * 2 && n1 - n0 == 1 &&
            glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "target formats: rg88 target is GL_RG8, complete, 2 bytes per pixel in the stats", NULL);
    fx_drop_rt (&fbo, &tex, 16, 8);
    fx_targets_stats (&b1, &n1);
    result (b1 == b0 && n1 == n0 && !fbo && !tex, "target formats: dropping it returns the bytes", NULL);

    /* the pool shares by size and format only */
    GLuint f1 = 0, t1 = 0, f2 = 0, t2 = 0, f3 = 0, t3 = 0;
    int e1 = fx_pool_get_fmt (16, 8, FX_FMT_RGBA8, &f1, &t1);
    fx_pool_put (e1);
    int e2 = fx_pool_get_fmt (16, 8, FX_FMT_RG8, &f2, &t2);
    int e3 = fx_pool_get_fmt (16, 8, FX_FMT_RGBA8, &f3, &t3);
    result (t3 == t1 && t2 != t1 && e2 != e3, "pool: a free target is reused for the same size and format only",
            NULL);
    fx_pool_put (e2);
    fx_pool_put (e3);

    /* compose capture: no alpha channel, shared by size, held over runs */
    guint cf = 0, ct = 0, cf2 = 0, ct2 = 0, cf3 = 0, ct3 = 0;
    gboolean opaque = FALSE, opaque2 = FALSE;
    int c1 = fx_capture_get (16, 8, &cf, &ct, &opaque);
    fx_pool_begin_run ();   /* what a layer's effect run does first */
    int c2 = fx_capture_get (16, 8, &cf2, &ct2, &opaque2);
    result (c1 != c2 && ct != ct2, "capture: stays lent while a run starts (a second one gets its own)", NULL);
    fx_capture_put (c2);
    fx_capture_put (c1);
    int c3 = fx_capture_get (16, 8, &cf3, &ct3, NULL);
    result (ct3 == ct || ct3 == ct2, "capture: handed back, the next compose layer of that size reuses it", NULL);
    if (opaque) {
        /* transparent source pixels: the capture must read alpha 1 */
        GLuint src = target_test_tex (16, 8, 10, 20, 30, 0), sf = 0;
        guint8 px[16 * 8 * 4];
        glGenFramebuffers (1, &sf);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, sf);
        glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, cf3);
        glBlitFramebuffer (0, 0, 16, 8, 0, 0, 16, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        target_read (ct3, px);
        result (px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 255 && px[16 * 8 * 4 - 1] == 255,
                "capture: RGB8 copy of transparent pixels reads alpha 1", NULL);
        glDeleteFramebuffers (1, &sf);
        glDeleteTextures (1, &src);
    } else {
        printf ("note capture: RGB8 is not renderable here, RGBA8 + alpha write is used\n");
    }
    fx_capture_put (c3);

    /* clears: a pass that writes every pixel is not cleared and still
     * leaves nothing of what the (reused) target held; one that discards
     * is cleared, so the dropped pixels are transparent black */
    EffectChain *copy = target_test_chain ("uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
                                           "void main() {\n    gl_FragColor = texSample2D(g_Texture0, v_TexCoord);\n}\n",
                                           FALSE);
    EffectChain *holes = target_test_chain ("uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
                                            "void main() {\n    if (v_TexCoord.x < 0.5) discard;\n"
                                            "    gl_FragColor = texSample2D(g_Texture0, v_TexCoord);\n}\n",
                                            TRUE);
    result (copy && holes, "clears: test passes build (plain = full cover, discard = partial)", NULL);
    if (copy && holes) {
        GLuint red = target_test_tex (8, 4, 255, 0, 0, 255), blue = target_test_tex (8, 4, 0, 0, 255, 128);
        guint8 px[8 * 4 * 4];
        struct FxSet set;
        EffectChain *chains[1];
        memset (&set, 0, sizeof (set));
        set.chains = chains;
        set.n = 1;
        set.cacheable = -1;
        set.fold_from = set.key_fold = -1;
        /* run 1 fills a pool target with red, run 2 (the same target) with
         * half-transparent blue: nothing red may be left */
        chains[0] = copy;
        GLuint o1 = fx_run_for_object (&set, red, 8, 4, 8, 4, 0.0, 1.0f, FALSE);
        target_read (o1, px);
        gboolean ok1 = px[0] == 255 && px[2] == 0 && px[3] == 255;
        GLuint o2 = fx_run_for_object (&set, blue, 8, 4, 8, 4, 0.0, 1.0f, FALSE);
        target_read (o2, px);
        gboolean ok2 = TRUE;
        for (int i = 0; i < 32; i++)
            ok2 &= px[i * 4] == 0 && px[i * 4 + 2] == 255 && px[i * 4 + 3] == 128;
        result (ok1 && ok2 && o1 == o2, "clears: uncleared full-cover pass replaces every pixel of a reused target",
                NULL);
        /* the discarding pass on the same target: left half must be 0,0,0,0 */
        chains[0] = holes;
        set.cacheable = -1;
        GLuint o3 = fx_run_for_object (&set, red, 8, 4, 8, 4, 0.0, 1.0f, FALSE);
        target_read (o3, px);
        gboolean ok3 = o3 == o2;
        for (int i = 0; i < 32; i++) {
            if (i % 8 < 4)
                ok3 &= px[i * 4] == 0 && px[i * 4 + 1] == 0 && px[i * 4 + 2] == 0 && px[i * 4 + 3] == 0;
            else
                ok3 &= px[i * 4] == 255 && px[i * 4 + 3] == 255;
        }
        result (ok3, "clears: a discarding pass still gets its target cleared", NULL);
        /* state left for the caller after a run: unit 0 active, no FBO */
        GLint unit = 0, dfb = -1;
        glGetIntegerv (GL_ACTIVE_TEXTURE, &unit);
        glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
        result (unit == GL_TEXTURE0 && dfb == 0, "state: a run ends with texture unit 0 active and framebuffer 0 bound",
                NULL);
        /* a scene drawn into its own framebuffer: runs return to that one */
        GLuint sfb = 0;
        glGenFramebuffers (1, &sfb);
        fx_set_scene_target (sfb);
        chains[0] = copy;
        set.cacheable = -1;
        GLuint o4 = fx_run_for_object (&set, red, 8, 4, 8, 4, 0.0, 1.0f, FALSE);
        glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
        target_read (o4, px);
        result ((GLuint) dfb == sfb && px[0] == 255 && px[3] == 255,
                "state: with a scene target set, a run ends with that framebuffer bound", NULL);
        fx_set_scene_target (0);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers (1, &sfb);

        /* a cached static set owns its result only; the cache survives a
         * pool flush and a refresh makes no new target */
        chains[0] = copy;
        set.cacheable = -1;
        set.cached = FALSE;
        fx_set_cache_enabled (TRUE);
        fx_targets_release ();
        gint64 by0 = 0, by1 = 0, by2 = 0;
        int cn0 = 0, cn1 = 0, cn2 = 0;
        fx_targets_stats (&by0, &cn0);
        GLuint s1 = fx_run_for_object (&set, red, 8, 4, 8, 4, 0.0, 1.0f, TRUE);
        fx_targets_release ();
        fx_targets_stats (&by1, &cn1);
        GLuint s2 = fx_run_for_object (&set, red, 8, 4, 8, 4, 0.0, 1.0f, TRUE);
        target_read (s2, px);
        fx_invalidate (&set);
        GLuint s3 = fx_run_for_object (&set, red, 8, 4, 8, 4, 0.0, 1.0f, TRUE);
        fx_targets_stats (&by2, &cn2);
        result (s1 == s2 && s3 == s1 && cn1 - cn0 == 1 && cn2 == cn1 && px[0] == 255 && px[3] == 255,
                "pool: a cached set keeps one target (its result) and refreshes into it", NULL);
        fx_free_targets_for_object (&set);
        glDeleteTextures (1, &red);
        glDeleteTextures (1, &blue);
    }
    if (copy)
        fx_chain_free (copy);
    if (holes)
        fx_chain_free (holes);
    fx_targets_release ();
}

static void dir_checks (const char *dir, const char *mode)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d) {
        result (FALSE, "source dump directory readable", dir);
        return;
    }
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    const char *n;
    while ((n = g_dir_read_name (d)) != NULL)
        if (g_str_has_suffix (n, ".vert"))
            g_ptr_array_add (names, g_strndup (n, strlen (n) - 5));
    g_dir_close (d);
    g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
    int built = 0, failed = 0;
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < names->len; i++) {
        char *vp = g_strdup_printf ("%s/%s.vert", dir, (char *) names->pdata[i]);
        char *fp = g_strdup_printf ("%s/%s.frag", dir, (char *) names->pdata[i]);
        char *vs = NULL, *fs = NULL;
        if (g_file_get_contents (vp, &vs, NULL, NULL) && g_file_get_contents (fp, &fs, NULL, NULL)) {
            /* identical pairs (one per effect instance) compile once */
            char *key = g_strconcat (vs, "\1", fs, NULL);
            if (g_hash_table_add (seen, key)) {
                GLuint prog = fx_build_program (&vs, &fs, names->pdata[i], names->pdata[i]);
                if (prog) {
                    glDeleteProgram (prog);
                    built++;
                } else {
                    failed++;
                    printf ("FAIL dumped pass %s does not build\n", (char *) names->pdata[i]);
                }
            }
        }
        g_free (vs);
        g_free (fs);
        g_free (vp);
        g_free (fp);
    }
    g_hash_table_unref (seen);
    char *d2 = g_strdup_printf ("%u dumped passes, %d distinct: %d built, %d failed", names->len,
                                built + failed, built, failed);
    char *nm = g_strdup_printf ("dumped passes build: %s", mode);
    printf ("     %s\n", d2);
    result (failed == 0 && built > 0, nm, d2);
    g_free (nm);
    g_free (d2);
    g_ptr_array_unref (names);
}

/* an effect with two passes that both have no "target" (WE ping-pongs
 * the layer; its builtin refraction is built this way): the second pass
 * must read what the first one drew, not the buffer it is drawing into.
 * Pass a writes (0.25, 0.5, 0.75, 1) * input alpha, pass b adds 0.5 red. */
static void put_file (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

static void gl_pingpong_check (void)
{
    char *dir = g_dir_make_tmp ("walpe-fxtest-XXXXXX", NULL);
    if (!dir)
        return;
    const char *vs = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
                     "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvoid main() {\n"
                     "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
                     "\tv_TexCoord = a_TexCoord;\n}\n";
    put_file (dir, "effects/pp/effect.json",
              "{\"passes\":[{\"material\":\"materials/effects/pp_a.json\"},"
              "{\"material\":\"materials/effects/pp_b.json\"}]}");
    put_file (dir, "materials/effects/pp_a.json", "{\"passes\":[{\"shader\":\"effects/pp_a\",\"blending\":\"normal\"}]}");
    put_file (dir, "materials/effects/pp_b.json", "{\"passes\":[{\"shader\":\"effects/pp_b\",\"blending\":\"normal\"}]}");
    put_file (dir, "shaders/effects/pp_a.vert", vs);
    put_file (dir, "shaders/effects/pp_b.vert", vs);
    put_file (dir, "shaders/effects/pp_a.frag",
              "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() {\n"
              "\tgl_FragColor = vec4(0.25, 0.5, 0.75, 1.0) * texSample2D(g_Texture0, v_TexCoord).a;\n}\n");
    put_file (dir, "shaders/effects/pp_b.frag",
              "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() {\n"
              "\tgl_FragColor = texSample2D(g_Texture0, v_TexCoord) + vec4(0.5, 0.0, 0.0, 0.0);\n}\n");
    JsonNode *en = wpj_parse_data ("{\"e\":[{\"file\":\"effects/pp/effect.json\",\"id\":1,\"name\":\"pp\","
                                   "\"visible\":true,\"passes\":[{},{}]}]}", -1);
    gpointer set = en ? fx_build_for_object (dir, wpj_arr (json_node_get_object (en), "e"), "pingpong", NULL)
                      : NULL;
    guint8 px[4] = { 0 };
    if (set) {
        guint8 white[16 * 16 * 4];
        memset (white, 255, sizeof white);
        GLuint in = 0, fb = 0;
        glGenTextures (1, &in);
        glBindTexture (GL_TEXTURE_2D, in);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        fx_set_frame_info (16, 16, 0.5f, 0.5f, 1.0f / 30);
        GLuint out = fx_run_for_object (set, in, 16, 16, 16, 16, 1.0, 1.0f, FALSE);
        glGenFramebuffers (1, &fb);
        glBindFramebuffer (GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
        glReadPixels (8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers (1, &fb);
        fx_free_for_object (set);
        fx_targets_release ();
        glDeleteTextures (1, &in);
    }
    char *d = g_strdup_printf ("built %d, centre pixel %u,%u,%u,%u (want 191,128,191,255)", set != NULL,
                               px[0], px[1], px[2], px[3]);
    result (set && abs (px[0] - 191) <= 1 && abs (px[1] - 128) <= 1 && abs (px[2] - 191) <= 1 && px[3] == 255,
            "two passes without a target: the second reads the first one's picture", d);
    g_free (d);
    if (en)
        json_node_unref (en);
    const char *files[] = { "effects/pp/effect.json", "materials/effects/pp_a.json", "materials/effects/pp_b.json",
                            "shaders/effects/pp_a.vert", "shaders/effects/pp_b.vert", "shaders/effects/pp_a.frag",
                            "shaders/effects/pp_b.frag", "effects/pp", "effects", "materials/effects", "materials",
                            "shaders/effects", "shaders", "" };
    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        char *p = g_build_filename (dir, files[i], NULL);
        g_remove (p);
        g_free (p);
    }
    g_free (dir);
}

/* one uniform name with another type in each stage: the program links and
 * each stage gets the value of its own declaration */
static void gl_uniform_split_check (void)
{
    char *dir = g_dir_make_tmp ("walpe-fxtest-XXXXXX", NULL);
    if (!dir)
        return;
    put_file (dir, "effects/us/effect.json", "{\"passes\":[{\"material\":\"materials/effects/us.json\"}]}");
    put_file (dir, "materials/effects/us.json", "{\"passes\":[{\"shader\":\"effects/us\",\"blending\":\"normal\"}]}");
    put_file (dir, "shaders/effects/us.vert",
              "uniform mat4 g_ModelViewProjectionMatrix;\n"
              "uniform vec2 u_res; // {\"material\":\"res\",\"default\":\"0.25 0.5\"}\n"
              "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_Res;\nvoid main() {\n"
              "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_Res = u_res;\n}\n");
    put_file (dir, "shaders/effects/us.frag",
              "uniform sampler2D g_Texture0;\nuniform float u_res; // {\"material\":\"res\",\"default\":0.75}\n"
              "varying vec2 v_Res;\nvoid main() {\n\tgl_FragColor = vec4(v_Res, u_res, 1.0);\n}\n");
    JsonNode *en = wpj_parse_data ("{\"e\":[{\"file\":\"effects/us/effect.json\",\"id\":1,\"name\":\"us\","
                                   "\"visible\":true,\"passes\":[{}]}]}", -1);
    gpointer set = en ? fx_build_for_object (dir, wpj_arr (json_node_get_object (en), "e"), "unisplit", NULL)
                      : NULL;
    guint8 px[4] = { 0 };
    if (set) {
        guint8 white[16 * 16 * 4];
        memset (white, 255, sizeof white);
        GLuint in = 0, fb = 0;
        glGenTextures (1, &in);
        glBindTexture (GL_TEXTURE_2D, in);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        fx_set_frame_info (16, 16, 0.5f, 0.5f, 1.0f / 30);
        GLuint out = fx_run_for_object (set, in, 16, 16, 16, 16, 1.0, 1.0f, FALSE);
        glGenFramebuffers (1, &fb);
        glBindFramebuffer (GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
        glReadPixels (8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers (1, &fb);
        fx_free_for_object (set);
        fx_targets_release ();
        glDeleteTextures (1, &in);
    }
    char *d = g_strdup_printf ("built %d, centre pixel %u,%u,%u,%u (want 64,128,191,255)", set != NULL,
                               px[0], px[1], px[2], px[3]);
    result (set && abs (px[0] - 64) <= 1 && abs (px[1] - 128) <= 1 && abs (px[2] - 191) <= 1 && px[3] == 255,
            "uniform declared vec2 in the vertex and float in the fragment shader: links, each gets its default", d);
    g_free (d);
    if (en)
        json_node_unref (en);
    const char *files[] = { "effects/us/effect.json", "materials/effects/us.json", "shaders/effects/us.vert",
                            "shaders/effects/us.frag", "effects/us", "effects", "materials/effects", "materials",
                            "shaders/effects", "shaders", "" };
    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        char *p = g_build_filename (dir, files[i], NULL);
        g_remove (p);
        g_free (p);
    }
    g_free (dir);
}

static void gl_run (EGLDisplay dpy, const char *dir)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip GL checks on one EGL display (no usable GL config)\n");
        return;
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
        return;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    static const char *fbodies[] = {
        /* scalar/vector conversions, modulo, bool arithmetic */
        "uniform float g_Time;\nuniform float speed;\nvarying vec4 v_TexCoord;\n"
        "vec2 f(float a) {\n    return a * 2.0;\n}\n"
        "void main() {\n    vec2 uv = g_Time * speed;\n    float x = 1.0;\n    x += uv;\n    uv = 0.5;\n"
        "    int i = 7;\n    float m = i % 3;\n    x *= (m < 2.0) * 6.0;\n"
        "    gl_FragColor = vec4(uv + f(m), x, 1.0) + v_TexCoord;\n}\n",
        /* uint/int, mixed-size builtin arguments, vec4 texture coordinate */
        "uniform sampler2D g_Texture0;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n    uint a = 3u;\n    int b = 2;\n    int c = a * b;\n"
        "    vec4 t = texSample2D(g_Texture0, v_TexCoord);\n"
        "    vec3 v = mix(vec3(0.0), vec3(1.0), vec4(0.5));\n"
        "    gl_FragColor = vec4(max(0, v), float(c)) + t;\n}\n",
        /* preprocessor: stray #endif, uniform in #if */
        "uniform vec4 g_Texture0Resolution;\nvarying vec4 v_TexCoord;\n"
        "#if g_Texture0Resolution.y <= 640\n#define K 0.5\n#else\n#define K 1.0\n#endif\n"
        "void main() {\n    gl_FragColor = v_TexCoord * K;\n}\n#endif\n",
        /* 3219908811's glow: fragment reads a vec2 of the vertex's vec4 */
        "uniform sampler2D g_Texture0;\nuniform float u_Min;\nuniform float u_Max;\n"
        "uniform float g_Spectrum[64];\nvarying vec2 v_TexCoord;\n"
        "float freq(float x) {\n    float l = 0.1;\n"
        "    for (int i = u_Min; i < u_Max; i++) {\n        l += g_Spectrum[i];\n    }\n    return l;\n}\n"
        "void main() {\n    vec4 albedo = texSample2D(g_Texture0, v_TexCoord.xy);\n"
        "    vec2 p = v_TexCoord;\n    p = (p - 0.5) * 2.0;\n    albedo.rgb *= freq(p.x);\n"
        "    gl_FragColor = vec4(max(0, albedo.bgr), albedo.a);\n}\n",
    };
    for (guint m = 0; m < G_N_ELEMENTS (modes); m++) {
        set_mode (m);
        for (guint i = 0; i < G_N_ELEMENTS (fbodies); i++) {
            /* body 3's link error names no line: without stage 1 only
             * the lenient interface repair after the failed link helps */
            gboolean ok = build (VS_BODY, fbodies[i]);
            char *name = g_strdup_printf ("real driver, body %u: %s", i, modes[m].name);
            result (ok, name, NULL);
            g_free (name);
        }
    }
    /* valid shaders stay valid in every mode */
    for (guint m = 0; m < G_N_ELEMENTS (modes); m++) {
        set_mode (m);
        gboolean ok = build (VS_BODY, "uniform sampler2D g_Texture0;\nvarying vec4 v_TexCoord;\n"
                             "void main() {\n    vec4 a = texSample2D(g_Texture0, v_TexCoord.xy);\n"
                             "    float k = (a.r < 0.5) ? 1.0 : 0.0;\n    a.rgb = mix(a.rgb, vec3(k), 0.5);\n"
                             "    gl_FragColor = a;\n}\n");
        char *name = g_strdup_printf ("real driver, valid shader: %s", modes[m].name);
        result (ok, name, NULL);
        g_free (name);
    }
    set_mode (0);
    gl_transform_check ();
    gl_pingpong_check ();
    gl_uniform_split_check ();
    gl_target_checks ();
    if (dir) {
        /* the log withheld: source inspection must do; then the fallback
         * alone, with the driver's log and with it worded like NVIDIA's */
        for (guint m = 1; m <= 3; m++) {
            set_mode (m);
            dir_checks (dir, modes[m].name);
        }
    }
    set_mode (0);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
}

/* fx-repair-test --changes dir: what source inspection rewrites in a dump
 * (every changed line, once, with its count) - to review that nothing but
 * HLSL-isms is touched. No GL. */
static int list_changes (const char *dir)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d)
        return 2;
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    const char *n;
    while ((n = g_dir_read_name (d)) != NULL)
        if (g_str_has_suffix (n, ".vert"))
            g_ptr_array_add (names, g_strndup (n, strlen (n) - 5));
    g_dir_close (d);
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *lines = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    int pairs = 0, changed = 0;
    for (guint i = 0; i < names->len; i++) {
        char *vp = g_strdup_printf ("%s/%s.vert", dir, (char *) names->pdata[i]);
        char *fp = g_strdup_printf ("%s/%s.frag", dir, (char *) names->pdata[i]);
        char *src[2] = { NULL, NULL };
        if (g_file_get_contents (vp, &src[0], NULL, NULL) && g_file_get_contents (fp, &src[1], NULL, NULL)) {
            char *key = g_strconcat (src[0], "\1", src[1], NULL);
            if (g_hash_table_add (seen, key)) {
                char *old[2] = { g_strdup (src[0]), g_strdup (src[1]) };
                fx_prepass (&src[0], &src[1], names->pdata[i]);
                pairs++;
                gboolean ch = FALSE;
                for (int k = 0; k < 2; k++) {
                    char **a = g_strsplit (old[k], "\n", -1), **b = g_strsplit (src[k], "\n", -1);
                    for (guint l = 0; a[l] && b[l]; l++)
                        if (strcmp (a[l], b[l])) {
                            char *e = g_strdup_printf ("%s  ->  %s", g_strstrip (a[l]), g_strstrip (b[l]));
                            g_hash_table_insert (lines, e, GINT_TO_POINTER (
                                GPOINTER_TO_INT (g_hash_table_lookup (lines, e)) + 1));
                            ch = TRUE;
                        }
                    if (g_strv_length (a) != g_strv_length (b))
                        printf ("LINE COUNT CHANGED in %s\n", (char *) names->pdata[i]);
                    g_strfreev (a);
                    g_strfreev (b);
                    g_free (old[k]);
                }
                changed += ch;
            }
        }
        g_free (src[0]);
        g_free (src[1]);
        g_free (vp);
        g_free (fp);
    }
    GList *ks = g_list_sort (g_hash_table_get_keys (lines), (GCompareFunc) g_strcmp0);
    for (GList *k = ks; k; k = k->next)
        printf ("%4d  %s\n", GPOINTER_TO_INT (g_hash_table_lookup (lines, k->data)), (char *) k->data);
    g_list_free (ks);
    printf ("%u dumped passes, %d distinct, %d with repairs, %u distinct rewritten lines\n", names->len,
            pairs, changed, g_hash_table_size (lines));
    g_hash_table_unref (seen);
    g_hash_table_unref (lines);
    g_ptr_array_unref (names);
    return 0;
}

/* 8. Hostile and merely unusual shader text must
 *    neither crash nor stall the source models; uniform annotations with
 *    nested objects; arguments of the shader's own functions; trailing
 *    commas in a wallpaper's JSON files. */
static void survives (const char *what, const char *body, double max_s)
{
    char *src = g_strconcat (HEAD, body, NULL);
    gint64 t0 = g_get_monotonic_time ();
    (void) fx_prepass_src (&src);
    double s = (g_get_monotonic_time () - t0) / 1e6;
    char *d = g_strdup_printf ("took %.2f s (limit %.1f s)", s, max_s);
    result (s <= max_s, what, d);
    g_free (d);
    g_free (src);
}

static char *nested (const char *pre, const char *open, int n, const char *mid, const char *close,
                     const char *post)
{
    GString *s = g_string_new (pre);
    for (int i = 0; i < n; i++)
        g_string_append (s, open);
    g_string_append (s, mid);
    for (int i = 0; i < n; i++)
        g_string_append (s, close);
    g_string_append (s, post);
    return g_string_free (s, FALSE);
}

static void anno (const char *what, const char *body, const char *uname, const char *key,
                  const char *want)
{
    char *got = fx_shader_anno_t (body, uname, key, NULL);
    char *d = g_strdup_printf ("want %s, got %s", want ? want : "(none)", got ? got : "(none)");
    result (!g_strcmp0 (got, want), what, d);
    g_free (d);
    g_free (got);
}

static void audit_checks (void)
{
    /* #if arithmetic: INT64_MIN / -1 and % -1 trap on x86 */
    survives ("#if INT64_MIN / -1 does not trap",
              "#if (-9223372036854775807 - 1) / -1\nfloat a;\n#endif\n"
              "#if (-9223372036854775807 - 1) % -1\nfloat b;\n#endif\nvoid main () {}\n", 5);
    {
        FxPp x = { "(-9223372036854775807 - 1) / -1 == (-9223372036854775807 - 1)", NULL, 0, FALSE, 0, 0, FALSE };
        x.macros = g_hash_table_new (g_str_hash, g_str_equal);
        gint64 v = fx_pp_expr (&x, 1);
        result (v == 1 && !x.bad, "#if INT64_MIN / -1 wraps like cpp", NULL);
        FxPp y = { "7 / -1 == -7 && 7 % -1 == 0 && -(-3) == 3 && 1 + 2 * 3 == 7", x.macros, 0, FALSE, 0, 0, FALSE };
        v = fx_pp_expr (&y, 1);
        result (v == 1 && !y.bad, "#if arithmetic unchanged", NULL);
        g_hash_table_unref (x.macros);
    }
    /* nesting: one line of 200000 '(' or '!' used to overflow the stack */
    char *b = nested ("#if ", "(", 200000, "1", ")", "\nfloat a;\n#endif\nvoid main () {}\n");
    survives ("#if with 200000 nested parentheses", b, 20);
    g_free (b);
    b = nested ("#if ", "!", 200000, "1", "", "\nfloat a;\n#endif\nvoid main () {}\n");
    survives ("#if with 200000 unary operators", b, 20);
    g_free (b);
    b = nested ("void main () {\n\tfloat a = ", "(", 200000, "1.0", ")", ";\n}\n");
    survives ("statement with 200000 nested parentheses", b, 20);
    g_free (b);
    b = nested ("void main () {\n\tfloat a = ", "-", 200000, "1.0", "", ";\n}\n");
    survives ("statement with 200000 unary minus", b, 20);
    g_free (b);
    b = nested ("void main () {\n\tfloat a = ", "true ? 1.0 : ", 100000, "2.0", "", ";\n}\n");
    survives ("statement with a 100000-term ?: chain", b, 20);
    g_free (b);
    st ("64 nested parentheses still evaluate",
        "#if ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((1"
        "))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))\n"
        "void main () { vec3 a = 1.0; }\n#endif\n", "vec3 a = vec3(1.0);");
    /* macros that name each other: 8 uses over 8 levels = 16.7 million
     * expansions (minutes); 16 uses over 6 levels in the typer */
    {
        GString *s = g_string_new (NULL);
        for (int i = 0; i < 9; i++) {
            g_string_append_printf (s, "#define A%d (", i);
            for (int k = 0; k < 8; k++)
                g_string_append_printf (s, "%sA%d", k ? "+" : "", i + 1);
            g_string_append (s, ")\n");
        }
        g_string_append (s, "#define A9 1\n#if A0\nfloat a;\n#endif\nvoid main () {}\n");
        survives ("#if over macros with fan-out 8, 9 levels", s->str, 5);
        result (!fx_expansion_ok (s->str), "fan-out 8 over 9 levels: expansion size refused", NULL);
        g_string_assign (s, "");
        for (int i = 0; i < 6; i++) {
            g_string_append_printf (s, "#define B%d (", i);
            for (int k = 0; k < 16; k++)
                g_string_append_printf (s, "%sB%d", k ? "+" : "", i + 1);
            g_string_append (s, ")\n");
        }
        g_string_append (s, "#define B6 1.0\nvoid main () {\n\tvec3 v = B0;\n\tfloat f = B0 + B0;\n}\n");
        survives ("statements over macros with fan-out 16, 6 levels", s->str, 5);
        result (!fx_expansion_ok (s->str), "fan-out 16 over 6 levels: expansion size refused", NULL);
        /* function-like: F(F(F(...))) with 10 uses of the parameter, 8 deep;
         * a chain of 300 macros in front of a bomb; 70 parameters */
        g_string_assign (s, "#define F(x) x x x x x x x x x x\nfloat a = F(F(F(F(F(F(F(F(1.0))))))));\n");
        result (!fx_expansion_ok (s->str), "nested function-like macro: expansion size refused", NULL);
        g_string_assign (s, "#define F(x, y) (x + y)\n#define G(a) F(a, F(a, 1.0))\n"
                            "void main () { float v = G(G(G(2.0))) + F(1.0, 2.0); }\n");
        result (fx_expansion_ok (s->str), "ordinary function-like macros: accepted", NULL);
        g_string_assign (s, "");
        for (int i = 0; i < 300; i++)
            g_string_append_printf (s, "#define C%d C%d\n", i, i + 1);
        g_string_append (s, "#define C300 1\nfloat a = C0;\n");
        result (!fx_expansion_ok (s->str), "300 macros deep: refused", NULL);
        g_string_assign (s, "#define SELF SELF + 1\n#define P Q\n#define Q P\nfloat a = SELF + P;\n");
        result (fx_expansion_ok (s->str), "self-referencing macros: accepted (not expanded in themselves)", NULL);
        g_string_free (s, TRUE);
        char *big = g_strconcat (HEAD, "void main () {\n", NULL);
        GString *b2 = g_string_new (big);
        for (int i = 0; i < 2000; i++)
            g_string_append (b2, "\tgl_FragColor += lerp(vec4(1.0), vec4(0.5), frac(0.25)) * mul(vec4(1.0), mat4(1.0));\n");
        g_string_append (b2, "}\n");
        result (fx_expansion_ok (b2->str), "2000 lines using the prelude's macros: accepted", NULL);
        g_string_free (b2, TRUE);
        g_free (big);
    }
    st ("#if INT64_MIN / -1: the driver gets the value, not the division",
        "#if (-9223372036854775807 - 1) / -1\nvoid main () {}\n#endif\n", "#if 1 // was: #if (-9223372036854775807 - 1) / -1");
    st ("#elif INT64_MIN % -1 through a macro",
        "#define MINV (-9223372036854775807 - 1)\n#if 0\n#elif MINV % -1\n#else\nvoid main () {}\n#endif\n",
        "#elif 0 // was: #elif MINV % -1");
    st ("ordinary division in #if stays", "#if 8 / 2 == 4 && 7 % -1 == 0\nvoid main () {}\n#endif\n", NULL);
    st ("small macro chains still type",
        "#define K2 (K1 + K1)\n#define K1 0.5\nvoid main () { vec3 a = K2; }\n", "vec3 a = vec3(K2);");
    /* a Latin-1 / GBK comment byte: GRegex returns NULL for such lines */
    survives ("int/uint mixing next to a non-UTF-8 comment",
              "void main () {\n\tuint a = 3u; // caf\xe9\n\tint b = 2;\n\tint c = a + b; /* \xb0\xa1 */\n}\n", 5);
    {
        char *tmp = g_build_filename (g_get_tmp_dir (), "walpe-fxtest-XXXXXX", NULL);
        int fd = g_mkstemp (tmp);
        const char raw[] = "// caf\xe9 \xb0\xa1\nuniform float a; // \xff\nvoid main () {}\n";
        gboolean ok = fd >= 0 && write (fd, raw, sizeof raw - 1) == (gssize) sizeof raw - 1;
        if (fd >= 0)
            close (fd);
        char *got = ok ? fx_read_shader (tmp) : NULL;
        result (got && g_utf8_validate (got, -1, NULL) && strlen (got) == sizeof raw - 1 &&
                strstr (got, "uniform float a;") && strstr (got, "void main () {}"),
                "shader file with non-UTF-8 bytes is read as UTF-8, same length", got);
        g_free (got);
        g_unlink (tmp);
        g_free (tmp);
    }
    /* uniform annotations with a nested object */
    const char *ab =
        "uniform vec2 g_Point0; // {\"material\":\"point0\",\"label\":\"first point\",\"default\":\"0.25 0.75\",\"require\":{\"DIRECT\":0}}\n"
        "uniform float g_Speed; // {\"material\":\"rayspeed\",\"default\":0.2,\"range\":[0.1, 1.0]}\n"
        "uniform vec3 g_Col; // {\"label\":\"a}b{\",\"default\":\"1 0 0\",\"options\":{\"x\":{\"y\":1}},\"material\":\"col\"}\n"
        "uniform float g_Open; // {\"material\":\"open\",\"require\":{\"A\":1}\n";
    anno ("annotation with a nested object: material", ab, "g_Point0", "material", "point0");
    anno ("annotation with a nested object: default", ab, "g_Point0", "default", "0.25 0.75");
    anno ("flat annotation unchanged", ab, "g_Speed", "default", "0.2");
    anno ("braces inside a string, key after two nested objects", ab, "g_Col", "material", "col");
    anno ("annotation that never closes: none", ab, "g_Open", "material", NULL);
    /* arguments of the shader's own functions (a helper that takes a vec2) */
    st ("vec4 handed to a vec2 parameter",
        "vec2 turn2(vec2 v, float r)\n{\n\treturn v * r;\n}\nvarying vec4 v_TexCoord;\n"
        "uniform float u_dir;\nvoid main () {\n"
        "\tvec2 c = turn2(v_TexCoord, -u_dir + 1.57) * 2.0;\n}\n",
        "vec2 c = turn2(vec2(v_TexCoord), -u_dir + 1.57) * 2.0;");
    st ("scalar handed to a vec3 parameter, call inside an expression",
        "float lum(vec3 c) { return dot(c, vec3(0.3, 0.6, 0.1)); }\n"
        "void main () {\n\tfloat g = 0.5;\n\tgl_FragColor = vec4(lum(g) + lum(vec3(g)));\n}\n",
        "vec4(lum(vec3(g)) + lum(vec3(g)))");
    st ("matching arguments stay", "vec2 rot(vec2 v, float r) { return v * r; }\nvarying vec4 v_T;\n"
        "void main () {\n\tvec2 c = rot(v_T.xy, 1) + rot(vec2(1.0), 2.0);\n}\n", NULL);
    st ("out parameters stay", "void split(vec4 v, out vec2 a, inout float b) { a = v.xy; b = v.z; }\n"
        "void main () {\n\tvec3 a;\n\tvec2 b;\n\tsplit(vec4(1.0), a, b);\n}\n", NULL);
    st ("overloads with different parameters stay",
        "float f(vec2 v) { return v.x; }\nfloat f(vec3 v) { return v.x; }\nvarying vec4 v_T;\n"
        "void main () {\n\tfloat a = f(v_T);\n}\n", NULL);
    st ("prototype and definition agree: repaired",
        "float f(vec2 v);\nvarying vec4 v_T;\nvoid main () {\n\tfloat a = f(v_T);\n}\n"
        "float f(vec2 v) { return v.x; }\n", "float a = f(vec2(v_T));");
    st ("header over two lines: not touched",
        "float f(vec2 v,\n\tfloat r) { return v.x * r; }\nvarying vec4 v_T;\n"
        "void main () {\n\tfloat a = f(v_T, 1.0);\n}\n", NULL);
    /* g_Time for the shaders: the scene time until a fold; folds by whole
     * hours at an unseen moment (frame gap) past the soft limit, or at
     * the hard limit; never below FX_TIME_LO_S; float steps under 1 ms */
    {
        fx_time_fold = 0;
        fx_time_gap = FALSE;
        result (fx_shader_time (0.0) == 0.0f && fx_shader_time (4199.9) == (float) 4199.9 &&
                fx_shader_time (16383.5) == (float) 16383.5 && fx_time_fold == 0,
                "g_Time: the scene time itself while it is shown without a break (4.5 h)", NULL);
        float s1 = fx_shader_time (16384.0), s2 = fx_shader_time (16384.5);
        result (s1 == 1984.0f && s2 == 1984.5f && fx_time_fold == 14400.0,
                "g_Time: hard limit folds by whole hours, continues from there", NULL);
        fx_time_fold = 0;
        fx_time_gap = TRUE;
        s1 = fx_shader_time (4100.0);
        s2 = fx_shader_time (5000.0);
        result (s1 == 4100.0f && s2 == 1400.0f, "g_Time: folded at a frame gap once past the soft limit", NULL);
        fx_time_gap = FALSE;
        result (fx_shader_time (5000.25) == 1400.25f && fx_shader_time (7799.0) == 4199.0f,
                "g_Time: continuous after the fold", NULL);
        fx_time_gap = TRUE;
        s1 = fx_shader_time (8640005.0);   /* 100 days later */
        result (s1 >= (float) FX_TIME_LO_S && s1 < (float) (FX_TIME_LO_S + FX_TIME_PERIOD_S) && s1 == 3605.0f,
                "g_Time: 100 days on, still in [600, 4200)", NULL);
        result (fx_shader_time (12.0) == 12.0f && fx_time_fold == 0,
                "g_Time: another scene starts from its own clock", NULL);
        fx_time_gap = FALSE;
        float worst = 0;
        for (double t = 1.0; t < FX_TIME_HARD_S; t += 37.77) {
            float f = (float) t;
            worst = MAX (worst, nextafterf (f, INFINITY) - f);
        }
        char *d = g_strdup_printf ("largest float step below the hard limit: %.4f ms", worst * 1000.0);
        result (worst <= 0.001f, "g_Time: float steps stay below 1 ms", d);
        g_free (d);
        fx_time_fold = 0;
    }
    st ("selector on a declared name is dropped",
        "varying vec4 v_Size.xy; // xy = size\nvarying vec3 v_T;\nvoid main () {\n\tvec2 a = v_Size.xy + v_T.xy;\n"
        "\tvec3 b = v_Size;\n}\n", "varying vec4 v_Size; // xy = size");
    st ("the name declared that way is typed for the later lines",
        "varying vec4 v_Size.xy;\nvoid main () {\n\tvec3 b = v_Size;\n}\n", "vec3 b = vec3(v_Size);");
    st ("number as the condition of ?:",
        "void main () {\n\tfloat outside = step(0.5, 0.25);\n\tvec4 a = vec4(1.0);\n\tvec4 b = vec4(0.0);\n"
        "\tvec4 f = outside ? a : b;\n}\n", "vec4 f = (outside) != 0.0 ? a : b;");
    st ("int condition inside a call, after a comparison ternary",
        "void main () {\n\tint k = 2;\n\tfloat x = 0.5;\n"
        "\tfloat y = max(x < 1.0 ? 1.0 : 2.0, k ? 3.0 : 4.0);\n}\n", "max(x < 1.0 ? 1.0 : 2.0, (k) != 0 ? 3.0 : 4.0)");
    st ("bool conditions, comparisons and && chains stay",
        "varying vec4 v_T;\nuniform float u_n;\nvoid main () {\n\tbool o = v_T.x > 0.5;\n"
        "\tfloat n = v_T.x < 0.0 && v_T.y < 0.0 ? u_n : 0.0;\n\tn = o ? n : (u_n == 1.0) ? 2.0 : 3.0;\n"
        "\tn = sign(v_T.x) == sign(v_T.y) ? n : -1e5;\n}\n", NULL);
    /* float vectors of different sizes in + - * / (HLSL truncates) */
    st ("vec3 - vec2: the wider operand is truncated",
        "varying vec3 v_TexCoord;\nuniform float u_offset;\nuniform float u_scale;\nvoid main () {\n"
        "\tfloat scale = pow(length(abs(v_TexCoord - CAST2(u_offset)) * 1.0), 3.0) * u_scale;\n}\n",
        "abs(vec2(v_TexCoord) - CAST2(u_offset))");
    st ("vec2 - (float * vec4 + float)",
        "varying vec4 v_TexCoord;\nuniform float u_r;\nuniform sampler2D g_Texture0;\nvoid main () {\n"
        "\tvec4 timer = vec4(1.0);\n\tfloat pointer = 0.5;\n"
        "\tvec4 r = texSample2D(g_Texture0, v_TexCoord.xy - (u_r * timer + pointer));\n}\n",
        "v_TexCoord.xy - vec2((u_r * timer + pointer))");
    st ("vec4 * vec4 + vec2: both factors end up truncated",
        "void main () {\n\tvec4 a = vec4(1.0);\n\tvec4 b = vec4(2.0);\n\tvec2 c = vec2(3.0);\n"
        "\tvec2 d = a * b + c;\n}\n", "vec2 d = vec2(a) * vec2(b) + c;");
    st ("selectors after a call or subscript belong to the operand",
        "uniform mat4 m;\nuniform vec3 eye;\nvec4 f4(float x) { return vec4(x); }\nvoid main () {\n"
        "\tvec3 p = vec3(1.0);\n\tvec4 q[2];\n\tvec3 d = p - mul(vec4(eye, 1.0), m).xyz;\n"
        "\tvec2 e = p.xy - f4(1.0).zw + q[1].xy;\n}\n", NULL);
    st ("equal sizes, scalars, matrices, exponents and unary signs stay",
        "uniform mat4 m;\nvoid main () {\n\tvec4 a = vec4(1.0);\n\tvec3 b = a.xyz * 2.0 + vec3(1e-5) - -a.xyz;\n"
        "\tvec4 c = m * a + a / 3.0;\n\tfloat f = a.x * -a.y;\n\tf += a.z; f++;\n}\n", NULL);
    /* a fragment input the shader writes to */
    {
        char *src = g_strconcat (HEAD, "varying vec2 v_TexCoord; // uv\r\nvarying vec4 v_Keep;\r\n"
                                 "uniform float u_s;\r\nvoid main()\r\n{\r\n"
                                 "\tv_TexCoord.y += u_s; // shift\r\n"
                                 "\tgl_FragColor = vec4(v_TexCoord, 0.0, 1.0) + v_Keep;\r\n}\r\n", NULL);
        int n = fx_input_write_repair (&src);
        gboolean ok = n == 1 && strstr (src, "varying vec2 v_TexCoord; vec2 _weWv_TexCoord; // uv") &&
                      strstr (src, "{ _weWv_TexCoord = v_TexCoord;") &&
                      strstr (src, "\t_weWv_TexCoord.y += u_s;") &&
                      strstr (src, "vec4(_weWv_TexCoord, 0.0, 1.0) + v_Keep;") && !strstr (src, "_weWv_Keep");
        result (ok, "fragment input written to: global copy, uses renamed, copied in main ()",
                strstr (src, "#line 1\n") + 8);
        g_free (src);
        src = g_strconcat (HEAD, "varying vec2 v_TexCoord;\nvoid main () {\n\tvec2 uv = v_TexCoord;\n"
                           "\tuv.x += 0.5;\n\tif (v_TexCoord.x == uv.x) discard;\n}\n", NULL);
        char *orig = g_strdup (src);
        n = fx_input_write_repair (&src);
        result (n == 0 && !strcmp (src, orig), "fragment input only read: untouched", NULL);
        g_free (orig);
        g_free (src);
    }
    /* one uniform name, another type in each stage */
    {
        char *vs = g_strconcat (HEAD, "uniform vec2 u_res; // {\"default\":\"512 512\"}\nuniform float u_same;\n"
                                "#if 0\nuniform vec3 u_dead;\n#endif\nuniform float u_dead;\n"
                                "void main () {\n\tgl_Position = vec4(u_res, u_same, u_dead);\n}\n", NULL);
        char *fs = g_strconcat (HEAD, "uniform float u_res; // {\"default\":512}\nuniform float u_same;\n"
                                "uniform float u_dead;\nuniform float u_resolution;\n#define res u_res\n"
                                "void main () {\n\tgl_FragColor = vec4(res * u_same, u_res, u_dead, u_resolution);\n}\n",
                                NULL);
        int n = fx_uniform_split_repair (vs, &fs);
        gboolean ok = n == 1 && strstr (fs, "uniform float _weFu_res; // {\"default\":512}") &&
                      strstr (fs, "#define res _weFu_res\n") && strstr (fs, "vec4(res * u_same, _weFu_res, u_dead, u_resolution)") &&
                      strstr (fs, "uniform float u_resolution;") && !strstr (fs, "_weFu_same") && !strstr (fs, "_weFu_dead");
        result (ok, "uniform of another type in the vertex shader: the fragment one renamed, others untouched",
                strstr (fs, "#line 1\n") + 8);
        {
            /* annotation of the declaration that has the uniform's type (WE's Clouds) */
            const char *fb = "#if P == 0\n#else\nuniform vec4 g_S; // {\"material\":\"speed\",\"default\":\"1 2 3 4\"}\n#endif\n"
                             "uniform float g_Only; // {\"default\":7}\n";
            const char *vb = "#if P == 0\nuniform  vec2 g_S; // {\"material\":\"speed\",\"default\":\"5 6\"}\n#endif\n"
                             "uniform vec3 g_V;\n";
            char *a = fx_uniform_anno (fb, vb, "g_S", "default", GL_FLOAT_VEC2);
            char *b = fx_uniform_anno (fb, vb, "g_S", "default", GL_FLOAT_VEC4);
            char *c = fx_uniform_anno (fb, vb, "g_S", "default", GL_FLOAT_VEC3);
            char *d = fx_uniform_anno (fb, vb, "g_Only", "default", GL_FLOAT_VEC2);
            char *e = fx_uniform_anno (fb, NULL, "g_S", "material", GL_FLOAT_VEC2);
            char *f = fx_uniform_anno (fb, vb, "g_V", "default", GL_FLOAT_VEC3);
            result (!g_strcmp0 (a, "5 6") && !g_strcmp0 (b, "1 2 3 4") && !g_strcmp0 (c, "1 2 3 4") &&
                    !g_strcmp0 (d, "7") && !g_strcmp0 (e, "speed") && !f,
                    "annotation comes from the declaration of the uniform's type, else from any as before", NULL);
            g_free (a); g_free (b); g_free (c); g_free (d); g_free (e); g_free (f);
        }
        g_free (vs);
        g_free (fs);
    }
    /* trailing commas in a wallpaper's JSON file (WE's own reader takes them) */
    {
        char *tmp = g_build_filename (g_get_tmp_dir (), "walpe-fxtest-XXXXXX", NULL);
        int fd = g_mkstemp (tmp);
        const char js[] = "{\"a\":[1,2, ],\"s\":\"x,]\\\",}\",\"b\":{\"c\":3,\n},}\n";
        gboolean ok = fd >= 0 && write (fd, js, sizeof js - 1) == (gssize) sizeof js - 1;
        if (fd >= 0)
            close (fd);
        JsonNode *n = ok ? wpj_parse_file (tmp) : NULL;
        JsonObject *o = n ? json_node_get_object (n) : NULL;
        result (o && wpj_arr_len (wpj_arr (o, "a")) == 2 && !g_strcmp0 (wpj_str (o, "s", NULL), "x,]\",}") &&
                wpj_num (wpj_obj (o, "b"), "c", 0) == 3,
                "JSON file with trailing commas is read; commas inside strings stay", NULL);
        if (n)
            json_node_unref (n);
        g_unlink (tmp);
        g_free (tmp);
    }
}

int main (int argc, char **argv)
{
    if (argc > 2 && !strcmp (argv[1], "--changes"))
        return list_changes (argv[2]);
    xf_keys ();
    static_checks ();
    audit_checks ();
    fallback_checks ();
    varying_checks ();
    transform_checks ();
    gl_checks (argc > 1 ? argv[1] : NULL);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
