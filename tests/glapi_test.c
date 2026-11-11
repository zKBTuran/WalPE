/* glapi_test.c - which OpenGL API renders (src/common/glapi.c): no EGL, no
 * GL, no driver.
 *
 *  1. the preference: names, WALPE_GL_API, the config key (written only
 *     when it is not "auto");
 *  2. the ladder of each preference;
 *  3. the walk over it with made-up EGL capability sets - a desktop
 *     driver, drivers with OpenGL ES only above an old desktop OpenGL
 *     (the Mali / V3D case), an EGL without the OpenGL API, OpenGL ES 3.0
 *     only, a window config that is no ES 3 config: which rung is taken,
 *     which are tried at all, what the log sentence says;
 *  4. the fallback table for extension lists as such drivers give them:
 *     what is native, there by extension, replaced, off;
 *  5. what the daemon decides with (walpe_gl_api_choose_for): a saved gl /
 *     gles the machine has no context for runs as auto would and says so,
 *     WALPE_GL_API stays "that API or nothing", and with auto, or where
 *     the saved API is there, nothing differs from the plain walk.
 * `meson test -C build glapi` */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include "common/glapi.h"
#include "common/config.h"

static int checks, fails;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, !ok && detail ? ": " : "",
            !ok && detail ? detail : "");
}

/* a driver as EGL shows it: the highest desktop OpenGL and OpenGL ES
 * context versions it makes (major * 10 + minor, 0 = none) */
typedef struct {
    int max_gl, max_es;
    int tries;              /* how often it was asked */
    int tried_gl, tried_es;
} FakeEgl;

static gboolean fake_try (const WalpeGlRung *r, gpointer user)
{
    FakeEgl *f = user;
    f->tries++;
    if (r->es)
        f->tried_es++;
    else
        f->tried_gl++;
    return r->major * 10 + r->minor <= (r->es ? f->max_es : f->max_gl);
}

static void pref_checks (void)
{
    WalpeGlApiPref p = WALPE_GL_API_GL;
    result (walpe_gl_api_parse ("auto", &p) && p == WALPE_GL_API_AUTO, "parse auto", NULL);
    result (walpe_gl_api_parse ("gl", &p) && p == WALPE_GL_API_GL, "parse gl", NULL);
    result (walpe_gl_api_parse ("GLES", &p) && p == WALPE_GL_API_GLES, "parse GLES (any case)", NULL);
    result (walpe_gl_api_parse (" opengl-es \n", &p) && p == WALPE_GL_API_GLES,
            "parse opengl-es with blanks around", NULL);
    p = WALPE_GL_API_GL;
    result (!walpe_gl_api_parse ("vulkan", &p) && p == WALPE_GL_API_GL &&
            !walpe_gl_api_parse ("", &p) && !walpe_gl_api_parse (NULL, &p) &&
            !walpe_gl_api_parse ("gles2", &p), "other words are no preference and change nothing", NULL);
    result (!strcmp (walpe_gl_api_name (WALPE_GL_API_AUTO), "auto") &&
            !strcmp (walpe_gl_api_name (WALPE_GL_API_GL), "gl") &&
            !strcmp (walpe_gl_api_name (WALPE_GL_API_GLES), "gles"), "names", NULL);
    result (walpe_gl_api_env_override (NULL) == -1 && walpe_gl_api_env_override ("") == -1 &&
            walpe_gl_api_env_override ("1") == -1 &&
            walpe_gl_api_env_override ("gles") == WALPE_GL_API_GLES &&
            walpe_gl_api_env_override ("gl") == WALPE_GL_API_GL &&
            walpe_gl_api_env_override ("auto") == WALPE_GL_API_AUTO,
            "WALPE_GL_API: the three names override, anything else leaves it to the setting", NULL);
    result (walpe_gl_essl_version (30) == 300 && walpe_gl_essl_version (31) == 310 &&
            walpe_gl_essl_version (32) == 320 && walpe_gl_essl_version (33) == 320,
            "GLSL ES version of an OpenGL ES version", NULL);
}

static void ladder_checks (void)
{
    WalpeGlRung l[WALPE_GL_LADDER_MAX];
    int n = walpe_gl_api_ladder (WALPE_GL_API_AUTO, l);
    result (n == 3 && !l[0].es && l[0].major == 3 && l[0].minor == 3 && l[1].es && l[1].major == 3 &&
            l[1].minor == 2 && l[2].es && l[2].major == 3 && l[2].minor == 1,
            "auto: OpenGL 3.3, OpenGL ES 3.2, OpenGL ES 3.1", NULL);
    n = walpe_gl_api_ladder (WALPE_GL_API_GL, l);
    result (n == 1 && !l[0].es && l[0].major == 3 && l[0].minor == 3, "gl: OpenGL 3.3 alone", NULL);
    n = walpe_gl_api_ladder (WALPE_GL_API_GLES, l);
    result (n == 2 && l[0].es && l[0].minor == 2 && l[1].es && l[1].minor == 1,
            "gles: OpenGL ES 3.2, then 3.1", NULL);
    char buf[32];
    WalpeGlRung r = { TRUE, 3, 1 };
    result (!strcmp (walpe_gl_rung_name (&r, buf, sizeof buf), "OpenGL ES 3.1"), "rung name", buf);
}

static void choose_checks (void)
{
    static const WalpeGlEglFacts both = { "OpenGL OpenGL_ES", FALSE, FALSE, FALSE };
    WalpeGlApiChoice c;

    /* a desktop driver (NVIDIA, radeonsi, iris, llvmpipe): everything */
    FakeEgl desk = { 46, 32, 0, 0, 0 };
    gboolean ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, &both, fake_try, &desk, &c);
    result (ok && c.rung == 0 && !c.got.es && c.got.major == 3 && c.got.minor == 3 && !c.fell_back &&
            !c.forced && desk.tries == 1 && desk.tried_es == 0,
            "desktop driver, auto: OpenGL 3.3 with the first and only request", c.tried);
    result (!strcmp (c.tried, "OpenGL 3.3: ok") && !strcmp (c.why, "OpenGL 3.3"),
            "desktop driver, auto: what the log says", c.why);
    desk = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_GLES, &both, fake_try, &desk, &c);
    result (ok && c.got.es && c.got.minor == 2 && c.forced && !c.fell_back && desk.tried_gl == 0 &&
            strstr (c.why, "gl_api = gles") != NULL,
            "desktop driver, gles: OpenGL ES 3.2, desktop OpenGL never asked for", c.why);
    desk = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_GL, &both, fake_try, &desk, &c);
    result (ok && !c.got.es && c.forced && desk.tries == 1, "desktop driver, gl: OpenGL 3.3", c.why);

    /* Mali with panfrost: desktop OpenGL 3.1, OpenGL ES 3.1 */
    FakeEgl mali = { 31, 31, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, &both, fake_try, &mali, &c);
    result (ok && c.rung == 2 && c.got.es && c.got.major == 3 && c.got.minor == 1 && c.fell_back &&
            !c.forced && mali.tries == 3,
            "OpenGL 3.1 + OpenGL ES 3.1, auto: falls to OpenGL ES 3.1", c.tried);
    result (!strcmp (c.tried, "OpenGL 3.3: refused; OpenGL ES 3.2: refused; OpenGL ES 3.1: ok") &&
            strstr (c.why, "OpenGL ES 3.1") && strstr (c.why, "no desktop OpenGL 3.3"),
            "... and the log names the path and the reason", c.why);
    mali = (FakeEgl) { 31, 31, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_GL, &both, fake_try, &mali, &c);
    result (!ok && c.rung == -1 && strstr (c.why, "OpenGL 3.3") && strstr (c.why, "gl_api = gl"),
            "the same driver, gl: no context, and the sentence says what was asked for", c.why);

    /* Mali with panthor / a newer Adreno: OpenGL ES 3.2, desktop 3.1 */
    FakeEgl es32 = { 31, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, &both, fake_try, &es32, &c);
    result (ok && c.rung == 1 && c.got.es && c.got.minor == 2 && c.fell_back && es32.tries == 2,
            "OpenGL 3.1 + OpenGL ES 3.2, auto: OpenGL ES 3.2", c.tried);

    /* an EGL that offers OpenGL ES alone */
    static const WalpeGlEglFacts es_only = { "OpenGL_ES", FALSE, FALSE, FALSE };
    FakeEgl eso = { 46, 32, 0, 0, 0 };      /* (would even make one: must not be asked) */
    ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, &es_only, fake_try, &eso, &c);
    result (ok && c.got.es && c.got.minor == 2 && eso.tried_gl == 0 && c.fell_back &&
            strstr (c.tried, "OpenGL 3.3: not among EGL's client APIs") != NULL,
            "EGL without the OpenGL API: the desktop rung is skipped, not tried", c.tried);
    eso = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_GL, &es_only, fake_try, &eso, &c);
    result (!ok && eso.tries == 0, "... and gl finds nothing there", c.why);
    /* "OpenGL_ES" must not count as "OpenGL" */
    static const WalpeGlEglFacts order = { "OpenGL_ES OpenVG", FALSE, FALSE, FALSE };
    eso = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_GL, &order, fake_try, &eso, &c);
    result (!ok && eso.tries == 0, "client API names are whole words", c.tried);

    /* OpenGL ES 3.0 only: below the minimum */
    FakeEgl es30 = { 21, 30, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, &both, fake_try, &es30, &c);
    result (!ok && c.rung == -1 && es30.tries == 3 && strstr (c.why, "OpenGL ES 3.1 or later"),
            "OpenGL 2.1 + OpenGL ES 3.0: nothing usable, the sentence names the minimum", c.why);

    /* the window config is no OpenGL ES 3 config (and the caller says so) */
    static const WalpeGlEglFacts gl_conf = { "OpenGL OpenGL_ES", TRUE, TRUE, FALSE };
    FakeEgl d2 = { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_GLES, &gl_conf, fake_try, &d2, &c);
    result (!ok && d2.tries == 0 && strstr (c.tried, "the window config is not for this API"),
            "a config without the ES 3 bit rules the ES rungs out", c.tried);
    d2 = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, &gl_conf, fake_try, &d2, &c);
    result (ok && !c.got.es, "... and leaves desktop OpenGL alone", c.tried);

    /* nothing known about EGL: every rung is tried */
    FakeEgl d3 = { 0, 31, 0, 0, 0 };
    ok = walpe_gl_api_choose (WALPE_GL_API_AUTO, NULL, fake_try, &d3, &c);
    result (ok && c.got.es && c.got.minor == 1 && d3.tries == 3, "no EGL facts: all rungs tried", c.tried);
}

/* The saved setting against WALPE_GL_API. `strict` FALSE = the preference
 * is the saved one: gl / gles without a context must still give a running
 * daemon (walpe-ctl, which would set another value, needs one). */
static void saved_setting_checks (void)
{
    static const WalpeGlEglFacts both = { "OpenGL OpenGL_ES", FALSE, FALSE, FALSE };
    static const WalpeGlEglFacts es_only = { "OpenGL_ES", FALSE, FALSE, FALSE };
    static const WalpeGlEglFacts no_es_conf = { "OpenGL OpenGL_ES", TRUE, TRUE, FALSE };
    WalpeGlApiChoice c;

    /* saved gl where only OpenGL ES can be had: desktop OpenGL 3.1 + ES 3.1 */
    FakeEgl mali = { 31, 31, 0, 0, 0 };
    gboolean ok = walpe_gl_api_choose_for (WALPE_GL_API_GL, FALSE, &both, fake_try, &mali, &c);
    result (ok && c.got.es && c.got.major == 3 && c.got.minor == 1 && c.setting_fallback &&
            !c.forced && c.fell_back && c.rung == 2 && mali.tried_gl == 1 && mali.tried_es == 2,
            "saved gl, OpenGL 3.1 + OpenGL ES 3.1: runs on OpenGL ES 3.1, no rung asked twice", c.tried);
    result (!strcmp (c.tried, "OpenGL 3.3: refused; OpenGL ES 3.2: refused; OpenGL ES 3.1: ok") &&
            g_str_has_prefix (c.why, "OpenGL ES 3.1: ") && strstr (c.why, "saved gl_api = gl cannot") &&
            strstr (c.why, "no OpenGL 3.3 context") && strstr (c.why, "auto is used for this run") &&
            strstr (c.why, "stays as saved"),
            "... and the note names what runs, what was saved and that auto is used", c.why);
    /* ... and where EGL has no OpenGL API at all */
    FakeEgl eso = { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GL, FALSE, &es_only, fake_try, &eso, &c);
    result (ok && c.got.es && c.got.minor == 2 && c.setting_fallback && !c.forced && c.rung == 1 &&
            eso.tried_gl == 0 && eso.tried_es == 1 &&
            !strcmp (c.tried, "OpenGL 3.3: not among EGL's client APIs; OpenGL ES 3.2: ok"),
            "saved gl, EGL with OpenGL ES alone: OpenGL ES 3.2, desktop OpenGL not asked for", c.tried);

    /* saved gles where no window config is an OpenGL ES 3 config */
    FakeEgl desk = { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GLES, FALSE, &no_es_conf, fake_try, &desk, &c);
    result (ok && !c.got.es && c.got.major == 3 && c.got.minor == 3 && c.setting_fallback &&
            !c.forced && !c.fell_back && c.rung == 0 && desk.tried_es == 0 && desk.tried_gl == 1,
            "saved gles, no OpenGL ES 3 config: runs on OpenGL 3.3", c.tried);
    result (g_str_has_prefix (c.why, "OpenGL 3.3: ") && strstr (c.why, "saved gl_api = gles cannot") &&
            strstr (c.why, "no OpenGL ES 3.1 or later context") &&
            strstr (c.why, "auto is used for this run") &&
            g_str_has_suffix (c.tried, "; OpenGL 3.3: ok") &&
            g_str_has_prefix (c.tried, "OpenGL ES 3.2: the window config is not for this API; "),
            "... and the note and the list of rungs say so", c.why);
    /* ... and where the driver stops at OpenGL ES 3.0 */
    FakeEgl es30 = { 46, 30, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GLES, FALSE, &both, fake_try, &es30, &c);
    result (ok && !c.got.es && c.setting_fallback && es30.tried_es == 2 && es30.tried_gl == 1 &&
            !strcmp (c.tried, "OpenGL ES 3.2: refused; OpenGL ES 3.1: refused; OpenGL 3.3: ok"),
            "saved gles, OpenGL 4.6 + OpenGL ES 3.0: runs on OpenGL 3.3", c.tried);

    /* WALPE_GL_API: that API or nothing, in both cases */
    mali = (FakeEgl) { 31, 31, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GL, TRUE, &both, fake_try, &mali, &c);
    result (!ok && c.rung == -1 && !c.setting_fallback && mali.tried_gl == 1 && mali.tried_es == 0 &&
            strstr (c.why, "OpenGL 3.3 is needed (gl_api = gl)"),
            "WALPE_GL_API=gl on the OpenGL ES machine: no context, OpenGL ES not asked for", c.why);
    desk = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GLES, TRUE, &no_es_conf, fake_try, &desk, &c);
    result (!ok && c.rung == -1 && !c.setting_fallback && desk.tries == 0 &&
            strstr (c.why, "(gl_api = gles)"),
            "WALPE_GL_API=gles without an OpenGL ES 3 config: no context, nothing asked for", c.why);

    /* the default, and a saved API that is there: the plain walk */
    WalpeGlApiChoice plain;
    desk = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_AUTO, FALSE, &both, fake_try, &desk, &c);
    FakeEgl desk2 = { 46, 32, 0, 0, 0 };
    gboolean ok2 = walpe_gl_api_choose (WALPE_GL_API_AUTO, &both, fake_try, &desk2, &plain);
    result (ok && ok2 && !c.got.es && c.rung == 0 && desk.tries == 1 && desk.tried_es == 0 &&
            !c.setting_fallback && !c.forced && !strcmp (c.tried, "OpenGL 3.3: ok") &&
            !strcmp (c.why, "OpenGL 3.3") && !strcmp (c.tried, plain.tried) && !strcmp (c.why, plain.why),
            "auto on a desktop driver: one request, OpenGL 3.3, the sentence of the plain walk", c.why);
    desk = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GLES, FALSE, &both, fake_try, &desk, &c);
    result (ok && c.got.es && c.got.minor == 2 && c.forced && !c.setting_fallback &&
            desk.tried_gl == 0 && desk.tries == 1 && strstr (c.why, "asked for (gl_api = gles)"),
            "saved gles where OpenGL ES 3.2 is there: taken, desktop OpenGL not asked for", c.why);
    desk = (FakeEgl) { 46, 32, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GL, FALSE, &both, fake_try, &desk, &c);
    result (ok && !c.got.es && c.forced && !c.setting_fallback && desk.tries == 1 &&
            !strcmp (c.why, "OpenGL 3.3 (gl_api = gl)"),
            "saved gl where OpenGL 3.3 is there: taken", c.why);
    mali = (FakeEgl) { 31, 31, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_AUTO, FALSE, &both, fake_try, &mali, &c);
    result (ok && c.got.es && c.fell_back && !c.setting_fallback && mali.tries == 3 &&
            strstr (c.why, "no desktop OpenGL 3.3"),
            "auto on the OpenGL ES machine: the fallback of auto, not of a saved setting", c.why);

    /* neither API: the daemon cannot start, and the sentence says both were tried */
    FakeEgl none = { 21, 30, 0, 0, 0 };
    ok = walpe_gl_api_choose_for (WALPE_GL_API_GL, FALSE, &both, fake_try, &none, &c);
    result (!ok && c.rung == -1 && !c.setting_fallback && none.tries == 3 &&
            strstr (c.why, "no usable OpenGL context") && strstr (c.why, "gl_api = gl is saved") &&
            !strcmp (c.tried, "OpenGL 3.3: refused; OpenGL ES 3.2: refused; OpenGL ES 3.1: refused"),
            "saved gl, OpenGL 2.1 + OpenGL ES 3.0: nothing usable after both were tried", c.why);
}

static const WalpeGlfItem *plan_of (gboolean es, int version, const char *ext)
{
    static WalpeGlfItem items[WALPE_GLF_N];
    WalpeGlFacts f = { es, version, ext, NULL, NULL };
    walpe_gl_plan (&f, items);
    return items;
}

static void plan_checks (void)
{
    /* desktop OpenGL 4.6 with the usual extensions */
    const WalpeGlfItem *p = plan_of (FALSE, 46, "GL_EXT_texture_compression_s3tc "
                                     "GL_EXT_texture_filter_anisotropic GL_KHR_debug "
                                     "GL_ARB_buffer_storage GL_ARB_pipeline_statistics_query");
    gboolean all = TRUE;
    for (int i = 0; i < WALPE_GLF_N; i++)
        all &= p[i].name != NULL && (p[i].how == WALPE_GLF_NATIVE || p[i].how == WALPE_GLF_EXTENSION) &&
               p[i].feature == (WalpeGlFeature) i;
    char *d = walpe_gl_plan_describe (p);
    result (all && !*d, "desktop OpenGL 4.6: every item native or by extension, nothing to report", d);
    g_free (d);
    /* desktop OpenGL 3.3 without S3TC (a software or embedded driver) */
    p = plan_of (FALSE, 33, "");
    result (p[WALPE_GLF_S3TC].how == WALPE_GLF_REPLACED && p[WALPE_GLF_BGRA_UPLOAD].how == WALPE_GLF_NATIVE &&
            p[WALPE_GLF_SHADER_DIALECT].how == WALPE_GLF_NATIVE &&
            p[WALPE_GLF_TIMER_QUERY].how == WALPE_GLF_NATIVE &&
            p[WALPE_GLF_PERSISTENT_MAP].how == WALPE_GLF_OFF &&
            p[WALPE_GLF_FX_FUSION].how == WALPE_GLF_NATIVE,
            "desktop OpenGL 3.3, no extensions: S3TC decoded, persistent mapping off, the rest native",
            NULL);

    /* Mesa's OpenGL ES 3.2 on a desktop GPU or llvmpipe */
    p = plan_of (TRUE, 32, "GL_EXT_texture_format_BGRA8888 GL_EXT_texture_compression_s3tc "
                 "GL_EXT_texture_filter_anisotropic GL_EXT_disjoint_timer_query GL_KHR_debug "
                 "GL_EXT_texture_norm16 GL_EXT_color_buffer_float");
    result (p[WALPE_GLF_SHADER_DIALECT].how == WALPE_GLF_REPLACED &&
            strstr (p[WALPE_GLF_SHADER_DIALECT].via, "3.20") &&
            p[WALPE_GLF_BGRA_UPLOAD].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_S3TC].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_ANISOTROPIC].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_FLOAT_TARGETS].how == WALPE_GLF_NATIVE &&
            p[WALPE_GLF_NORM16].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_TEX_LEVEL_QUERY].how == WALPE_GLF_NATIVE &&
            p[WALPE_GLF_TIMER_QUERY].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_DEBUG_OUTPUT].how == WALPE_GLF_NATIVE,
            "OpenGL ES 3.2 with Mesa's extensions: formats and timers through extensions", NULL);
    result (p[WALPE_GLF_LOD_BIAS].how == WALPE_GLF_OFF && p[WALPE_GLF_SAMPLE_COUNTS].how == WALPE_GLF_OFF &&
            p[WALPE_GLF_PIPELINE_STATS].how == WALPE_GLF_OFF &&
            p[WALPE_GLF_PERSISTENT_MAP].how == WALPE_GLF_OFF &&
            p[WALPE_GLF_FX_FUSION].how == WALPE_GLF_OFF &&
            p[WALPE_GLF_TEX_READBACK].how == WALPE_GLF_REPLACED &&
            p[WALPE_GLF_POINT_SIZE_ENABLE].how == WALPE_GLF_REPLACED,
            "... and what OpenGL ES never has is off or replaced, each with its effect named", NULL);
    gboolean named = TRUE;
    for (int i = 0; i < WALPE_GLF_N; i++)
        if (p[i].how == WALPE_GLF_OFF)
            named &= p[i].effect != NULL && *p[i].effect && p[i].via == NULL;
    d = walpe_gl_plan_describe (p);
    result (named && strstr (d, "texture LOD bias: off") && strstr (d, "effect shader dialect: GLSL ES 3.20 output") &&
            !strstr (d, "BGRA"), "the log line lists what is replaced or off, not what an extension gives", d);
    g_free (d);

    /* a Mali-class OpenGL ES 3.1: no S3TC, half-float targets by extension */
    p = plan_of (TRUE, 31, "GL_EXT_texture_format_BGRA8888 GL_EXT_color_buffer_half_float "
                 "GL_KHR_debug GL_OES_texture_float_linear");
    result (strstr (p[WALPE_GLF_SHADER_DIALECT].via, "3.10") &&
            p[WALPE_GLF_BGRA_UPLOAD].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_S3TC].how == WALPE_GLF_REPLACED &&
            p[WALPE_GLF_ANISOTROPIC].how == WALPE_GLF_OFF &&
            p[WALPE_GLF_FLOAT_TARGETS].how == WALPE_GLF_EXTENSION &&
            !strcmp (p[WALPE_GLF_FLOAT_TARGETS].via, "GL_EXT_color_buffer_half_float") &&
            p[WALPE_GLF_NORM16].how == WALPE_GLF_REPLACED &&
            p[WALPE_GLF_TIMER_QUERY].how == WALPE_GLF_REPLACED &&
            p[WALPE_GLF_DEBUG_OUTPUT].how == WALPE_GLF_EXTENSION &&
            p[WALPE_GLF_TEX_LEVEL_QUERY].how == WALPE_GLF_NATIVE,
            "OpenGL ES 3.1 of an ARM GPU: S3TC decoded, no anisotropy, timers by wall clock", NULL);

    /* OpenGL ES 3.1 with no extension at all: every fallback at once */
    p = plan_of (TRUE, 31, NULL);
    result (p[WALPE_GLF_BGRA_UPLOAD].how == WALPE_GLF_REPLACED &&
            strstr (p[WALPE_GLF_BGRA_UPLOAD].via, "CPU") &&
            p[WALPE_GLF_FLOAT_TARGETS].how == WALPE_GLF_REPLACED &&
            p[WALPE_GLF_DEBUG_OUTPUT].how == WALPE_GLF_OFF,
            "OpenGL ES 3.1 bare: BGRA reordered on the CPU, float targets become RGBA8", NULL);
    /* an extension name that only starts like the one asked for */
    p = plan_of (TRUE, 31, "GL_EXT_texture_format_BGRA8888_extra GL_EXT_disjoint_timer_query2");
    result (p[WALPE_GLF_BGRA_UPLOAD].how == WALPE_GLF_REPLACED &&
            p[WALPE_GLF_TIMER_QUERY].how == WALPE_GLF_REPLACED, "extension names are whole words", NULL);
    /* OpenGL ES 3.0 (never chosen; the table still says why) */
    p = plan_of (TRUE, 30, "");
    result (p[WALPE_GLF_TEX_LEVEL_QUERY].how == WALPE_GLF_OFF &&
            strstr (p[WALPE_GLF_SHADER_DIALECT].via, "3.00"), "OpenGL ES 3.0: level queries off", NULL);
    result (!strcmp (walpe_glf_how_name (WALPE_GLF_NATIVE), "native") &&
            !strcmp (walpe_glf_how_name (WALPE_GLF_EXTENSION), "extension") &&
            !strcmp (walpe_glf_how_name (WALPE_GLF_REPLACED), "replaced") &&
            !strcmp (walpe_glf_how_name (WALPE_GLF_OFF), "off"), "state names for status", NULL);
}

/* the config key, in a private XDG_CONFIG_HOME */
static void config_checks (void)
{
    char *dir = g_dir_make_tmp ("walpe-glapi-XXXXXX", NULL);
    if (!dir) {
        printf ("skip the config part (no temporary directory)\n");
        return;
    }
    g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
    char *cdir = g_build_filename (dir, "walpe", NULL), *file = g_build_filename (cdir, "config.json", NULL);
    g_mkdir_with_parents (cdir, 0700);
    WalpeConfig *c = walpe_config_load ();
    result (c && c->gl_api == WALPE_GL_API_AUTO, "config: auto unless chosen", NULL);
    walpe_config_save (c);
    char *txt = NULL;
    g_file_get_contents (file, &txt, NULL, NULL);
    result (txt && !strstr (txt, "gl_api"), "config: auto is not written (a default config stays as it was)", NULL);
    g_free (txt);
    c->gl_api = WALPE_GL_API_GLES;
    walpe_config_save (c);
    walpe_config_free (c);
    txt = NULL;
    g_file_get_contents (file, &txt, NULL, NULL);
    result (txt && strstr (txt, "\"gl_api\"") && strstr (txt, "\"gles\""), "config: gles is saved by name", NULL);
    g_free (txt);
    c = walpe_config_load ();
    result (c && c->gl_api == WALPE_GL_API_GLES, "config: and read back", NULL);
    walpe_config_free (c);
    g_file_set_contents (file, "{ \"config_version\" : 2, \"gl_api\" : \"direct3d\" }", -1, NULL);
    c = walpe_config_load ();
    result (c && c->gl_api == WALPE_GL_API_AUTO, "config: an unknown word reads as auto", NULL);
    walpe_config_free (c);
    g_file_set_contents (file, "{ \"config_version\" : 2, \"gl_api\" : 7 }", -1, NULL);
    c = walpe_config_load ();
    result (c && c->gl_api == WALPE_GL_API_AUTO, "config: a number reads as auto", NULL);
    walpe_config_free (c);
    g_unlink (file);
    g_rmdir (cdir);
    g_rmdir (dir);
    g_free (file);
    g_free (cdir);
    g_free (dir);
}

int main (void)
{
    pref_checks ();
    ladder_checks ();
    choose_checks ();
    saved_setting_checks ();
    plan_checks ();
    config_checks ();
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
