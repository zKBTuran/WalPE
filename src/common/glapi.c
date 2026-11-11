/* glapi.c - the OpenGL / OpenGL ES choice and the ES fallback table (see
 * glapi.h). No EGL, no GL: strings and numbers in, decisions out. */
#include "glapi.h"
#include <string.h>

gboolean walpe_gl_api_parse (const char *s, WalpeGlApiPref *out)
{
    static const struct { const char *nm; WalpeGlApiPref p; } tab[] = {
        { "auto", WALPE_GL_API_AUTO },
        { "gl", WALPE_GL_API_GL }, { "opengl", WALPE_GL_API_GL }, { "desktop", WALPE_GL_API_GL },
        { "gles", WALPE_GL_API_GLES }, { "es", WALPE_GL_API_GLES },
        { "opengles", WALPE_GL_API_GLES }, { "opengl-es", WALPE_GL_API_GLES },
        { "opengl_es", WALPE_GL_API_GLES },
    };
    if (!s)
        return FALSE;
    while (*s == ' ' || *s == '\t')
        s++;
    gsize n = strlen (s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;
    for (guint i = 0; i < G_N_ELEMENTS (tab); i++)
        if (strlen (tab[i].nm) == n && !g_ascii_strncasecmp (tab[i].nm, s, n)) {
            if (out)
                *out = tab[i].p;
            return TRUE;
        }
    return FALSE;
}

const char *walpe_gl_api_name (WalpeGlApiPref p)
{
    return p == WALPE_GL_API_GL ? "gl" : p == WALPE_GL_API_GLES ? "gles" : "auto";
}

int walpe_gl_api_env_override (const char *value)
{
    WalpeGlApiPref p;
    if (!value || !*value || !walpe_gl_api_parse (value, &p))
        return -1;
    return (int) p;
}

int walpe_gl_api_ladder (WalpeGlApiPref pref, WalpeGlRung out[WALPE_GL_LADDER_MAX])
{
    static const WalpeGlRung gl = { FALSE, WALPE_GL_MIN_GL_MAJOR, WALPE_GL_MIN_GL_MINOR };
    static const WalpeGlRung es32 = { TRUE, 3, 2 };
    static const WalpeGlRung es31 = { TRUE, WALPE_GL_MIN_ES_MAJOR, WALPE_GL_MIN_ES_MINOR };
    int n = 0;
    if (pref != WALPE_GL_API_GLES)
        out[n++] = gl;
    if (pref != WALPE_GL_API_GL) {
        out[n++] = es32;
        out[n++] = es31;
    }
    return n;
}

const char *walpe_gl_rung_name (const WalpeGlRung *r, char *buf, gsize n)
{
    g_snprintf (buf, n, "%s %d.%d", r->es ? "OpenGL ES" : "OpenGL", r->major, r->minor);
    return buf;
}

/* `word` is one of the space-separated words of `list` */
static gboolean has_word (const char *list, const char *word)
{
    gsize n = strlen (word);
    if (!list || !n)
        return FALSE;
    for (const char *p = list; (p = strstr (p, word)) != NULL; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return TRUE;
    return FALSE;
}

/* NULL when the rung may be tried, else why not */
static const char *rung_ruled_out (const WalpeGlRung *r, const WalpeGlEglFacts *egl)
{
    if (!egl)
        return NULL;
    if (egl->client_apis && !has_word (egl->client_apis, r->es ? "OpenGL_ES" : "OpenGL"))
        return "not among EGL's client APIs";
    if (egl->config_known && !(r->es ? egl->config_es3 : egl->config_gl))
        return "the window config is not for this API";
    return NULL;
}

gboolean walpe_gl_api_choose (WalpeGlApiPref pref, const WalpeGlEglFacts *egl,
                              WalpeGlTryFn try_rung, gpointer user, WalpeGlApiChoice *out)
{
    WalpeGlRung ladder[WALPE_GL_LADDER_MAX];
    int n = walpe_gl_api_ladder (pref, ladder);
    GString *tried = g_string_new (NULL);
    char name[32];
    memset (out, 0, sizeof (*out));
    out->rung = -1;
    out->forced = pref != WALPE_GL_API_AUTO;
    gboolean gl_missed = FALSE;
    for (int i = 0; i < n && out->rung < 0; i++) {
        const char *no = rung_ruled_out (&ladder[i], egl);
        gboolean ok = !no && try_rung && try_rung (&ladder[i], user);
        g_string_append_printf (tried, "%s%s: %s", tried->len ? "; " : "",
                                walpe_gl_rung_name (&ladder[i], name, sizeof name),
                                ok ? "ok" : no ? no : "refused");
        if (ok) {
            out->rung = i;
            out->got = ladder[i];
        } else if (!ladder[i].es) {
            gl_missed = TRUE;
        }
    }
    g_strlcpy (out->tried, tried->str, sizeof out->tried);
    g_string_free (tried, TRUE);
    if (out->rung < 0) {
        g_snprintf (out->why, sizeof out->why,
                    "no usable OpenGL context: %s is needed (gl_api = %s)",
                    pref == WALPE_GL_API_GL ? "OpenGL 3.3"
                    : pref == WALPE_GL_API_GLES ? "OpenGL ES 3.1 or later"
                    : "OpenGL 3.3, or OpenGL ES 3.1 or later",
                    walpe_gl_api_name (pref));
        return FALSE;
    }
    walpe_gl_rung_name (&out->got, name, sizeof name);
    out->fell_back = pref == WALPE_GL_API_AUTO && out->got.es && gl_missed;
    if (!out->got.es)
        g_snprintf (out->why, sizeof out->why, "%s%s", name,
                    out->forced ? " (gl_api = gl)" : "");
    else if (out->forced)
        g_snprintf (out->why, sizeof out->why, "%s, asked for (gl_api = gles)", name);
    else
        g_snprintf (out->why, sizeof out->why,
                    "%s: this driver has no desktop OpenGL %d.%d context", name,
                    WALPE_GL_MIN_GL_MAJOR, WALPE_GL_MIN_GL_MINOR);
    return TRUE;
}

gboolean walpe_gl_api_choose_for (WalpeGlApiPref pref, gboolean strict, const WalpeGlEglFacts *egl,
                                  WalpeGlTryFn try_rung, gpointer user, WalpeGlApiChoice *out)
{
    if (walpe_gl_api_choose (pref, egl, try_rung, user, out) || strict || pref == WALPE_GL_API_AUTO)
        return out->rung >= 0;
    /* the saved setting names an API this machine has no context for: the
     * rungs auto would have tried besides, i.e. the other API's */
    const WalpeGlApiChoice first = *out;
    const WalpeGlApiPref other = pref == WALPE_GL_API_GL ? WALPE_GL_API_GLES : WALPE_GL_API_GL;
    const gboolean ok = walpe_gl_api_choose (other, egl, try_rung, user, out);
    char tried[sizeof out->tried];
    g_snprintf (tried, sizeof tried, "%s; %s", first.tried, out->tried);
    g_strlcpy (out->tried, tried, sizeof out->tried);
    out->forced = FALSE;
    if (!ok) {
        g_snprintf (out->why, sizeof out->why,
                    "no usable OpenGL context: OpenGL 3.3, or OpenGL ES 3.1 or later is needed "
                    "(gl_api = %s is saved; the other API was tried as well)",
                    walpe_gl_api_name (pref));
        return FALSE;
    }
    WalpeGlRung ladder[WALPE_GL_LADDER_MAX];
    const int n = walpe_gl_api_ladder (WALPE_GL_API_AUTO, ladder);
    for (int i = 0; i < n; i++)
        if (ladder[i].es == out->got.es && ladder[i].major == out->got.major &&
            ladder[i].minor == out->got.minor)
            out->rung = i;
    out->setting_fallback = TRUE;
    out->fell_back = out->got.es;
    char name[32];
    walpe_gl_rung_name (&out->got, name, sizeof name);
    g_snprintf (out->why, sizeof out->why,
                "%s: the saved gl_api = %s cannot be had on this machine (no %s context); "
                "auto is used for this run, the setting stays as saved",
                name, walpe_gl_api_name (pref),
                pref == WALPE_GL_API_GL ? "OpenGL 3.3" : "OpenGL ES 3.1 or later");
    return TRUE;
}

int walpe_gl_essl_version (int es_version)
{
    return es_version >= 32 ? 320 : es_version >= 31 ? 310 : 300;
}

/* ------------------------------------------------------ the fallback table */

gboolean walpe_gl_facts_has (const WalpeGlFacts *f, const char *ext)
{
    if (!f || !ext)
        return FALSE;
    if (f->has_ext)
        return f->has_ext (ext, f->user);
    return has_word (f->extensions, ext);
}

const char *walpe_glf_how_name (WalpeGlfHow h)
{
    switch (h) {
    case WALPE_GLF_NATIVE: return "native";
    case WALPE_GLF_EXTENSION: return "extension";
    case WALPE_GLF_REPLACED: return "replaced";
    default: return "off";
    }
}

static void item (WalpeGlfItem *out, WalpeGlFeature f, WalpeGlfHow how, const char *name,
                  const char *via, const char *effect)
{
    out[f].feature = f;
    out[f].how = how;
    out[f].name = name;
    out[f].via = via;
    out[f].effect = effect;
}

/* the first of up to three extensions the context has, else NULL */
static const char *first_ext (const WalpeGlFacts *f, const char *a, const char *b, const char *c)
{
    if (a && walpe_gl_facts_has (f, a))
        return a;
    if (b && walpe_gl_facts_has (f, b))
        return b;
    if (c && walpe_gl_facts_has (f, c))
        return c;
    return NULL;
}

void walpe_gl_plan (const WalpeGlFacts *f, WalpeGlfItem out[WALPE_GLF_N])
{
    const gboolean es = f->es;
    const int v = f->version;
    const char *x;
    memset (out, 0, sizeof (WalpeGlfItem) * WALPE_GLF_N);

    /* effect shaders are written for GLSL 3.30 semantics (implicit int ->
     * float conversions); for GLSL ES they are translated */
    if (!es)
        item (out, WALPE_GLF_SHADER_DIALECT, WALPE_GLF_NATIVE, "effect shader dialect", NULL, NULL);
    else
        item (out, WALPE_GLF_SHADER_DIALECT, WALPE_GLF_REPLACED, "effect shader dialect",
              v >= 32 ? "GLSL ES 3.20 output" : v >= 31 ? "GLSL ES 3.10 output" : "GLSL ES 3.00 output",
              "shaders are translated; one that cannot be translated is reported and its effect "
              "is left out");

    if (!es)
        item (out, WALPE_GLF_BGRA_UPLOAD, WALPE_GLF_NATIVE, "BGRA upload", NULL, NULL);
    else if (walpe_gl_facts_has (f, "GL_EXT_texture_format_BGRA8888"))
        item (out, WALPE_GLF_BGRA_UPLOAD, WALPE_GLF_EXTENSION, "BGRA upload",
              "GL_EXT_texture_format_BGRA8888", NULL);
    else
        item (out, WALPE_GLF_BGRA_UPLOAD, WALPE_GLF_REPLACED, "BGRA upload",
              "bytes reordered on the CPU", "more CPU time per uploaded picture, text and web / video frame");

    if (walpe_gl_facts_has (f, "GL_EXT_texture_compression_s3tc"))
        item (out, WALPE_GLF_S3TC, WALPE_GLF_EXTENSION, "S3TC textures",
              "GL_EXT_texture_compression_s3tc", NULL);
    else
        item (out, WALPE_GLF_S3TC, WALPE_GLF_REPLACED, "S3TC textures", "decoded on the CPU",
              "DXT textures take 4 to 8 times the video memory");

    if (walpe_gl_facts_has (f, "GL_EXT_texture_filter_anisotropic") ||
        (!es && v >= 46))
        item (out, WALPE_GLF_ANISOTROPIC, v >= 46 && !es ? WALPE_GLF_NATIVE : WALPE_GLF_EXTENSION,
              "anisotropic filtering", v >= 46 && !es ? NULL : "GL_EXT_texture_filter_anisotropic", NULL);
    else
        item (out, WALPE_GLF_ANISOTROPIC, WALPE_GLF_OFF, "anisotropic filtering", NULL,
              "minified textures are filtered trilinear only");

    if (!es)
        item (out, WALPE_GLF_LOD_BIAS, WALPE_GLF_NATIVE, "texture LOD bias", NULL, NULL);
    else
        item (out, WALPE_GLF_LOD_BIAS, WALPE_GLF_OFF, "texture LOD bias", NULL,
              "minified textures are sampled without the -0.5 bias: slightly softer");

    if (!es || v >= 32)
        item (out, WALPE_GLF_FLOAT_TARGETS, WALPE_GLF_NATIVE, "effect float targets", NULL, NULL);
    else if ((x = first_ext (f, "GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float", NULL)))
        item (out, WALPE_GLF_FLOAT_TARGETS, WALPE_GLF_EXTENSION, "effect float targets", x, NULL);
    else
        item (out, WALPE_GLF_FLOAT_TARGETS, WALPE_GLF_REPLACED, "effect float targets",
              "RGBA8 targets", "effects that declare r16f / rg1616f targets are clamped to 0..1; "
              "WALPE_FX_RT16F has no effect");

    if (!es)
        item (out, WALPE_GLF_NORM16, WALPE_GLF_NATIVE, "16-bit video planes", NULL, NULL);
    else if (walpe_gl_facts_has (f, "GL_EXT_texture_norm16"))
        item (out, WALPE_GLF_NORM16, WALPE_GLF_EXTENSION, "16-bit video planes",
              "GL_EXT_texture_norm16", NULL);
    else
        item (out, WALPE_GLF_NORM16, WALPE_GLF_REPLACED, "16-bit video planes",
              "repacked to 8 bits on the CPU", "10-bit video is shown with 8 bits per channel");

    if (!es || v >= 31)
        item (out, WALPE_GLF_TEX_LEVEL_QUERY, WALPE_GLF_NATIVE, "texture level queries", NULL, NULL);
    else
        item (out, WALPE_GLF_TEX_LEVEL_QUERY, WALPE_GLF_OFF, "texture level queries", NULL,
              "OpenGL ES 3.0 cannot run WalPE");

    if (!es)
        item (out, WALPE_GLF_TEX_READBACK, WALPE_GLF_NATIVE, "texture read-back", NULL, NULL);
    else
        item (out, WALPE_GLF_TEX_READBACK, WALPE_GLF_REPLACED, "texture read-back",
              "read through a framebuffer", "WALPE_PROBE and the memory report by texture are "
              "not available");

    if (!es)
        item (out, WALPE_GLF_TIMER_QUERY, WALPE_GLF_NATIVE, "GPU timer queries", NULL, NULL);
    else if (walpe_gl_facts_has (f, "GL_EXT_disjoint_timer_query"))
        item (out, WALPE_GLF_TIMER_QUERY, WALPE_GLF_EXTENSION, "GPU timer queries",
              "GL_EXT_disjoint_timer_query", NULL);
    else
        item (out, WALPE_GLF_TIMER_QUERY, WALPE_GLF_REPLACED, "GPU timer queries",
              "wall-clock time around glFinish",
              "the profiler's per-pass times include the wait for the GPU and disturb the frame");

    if (!es)
        item (out, WALPE_GLF_SAMPLE_COUNTS, WALPE_GLF_NATIVE, "exact sample counts", NULL, NULL);
    else
        item (out, WALPE_GLF_SAMPLE_COUNTS, WALPE_GLF_OFF, "exact sample counts", NULL,
              "the pixel statistics of the profiler and of WALPE_PIX_STATS stay empty");

    if (!es && (v >= 46 || walpe_gl_facts_has (f, "GL_ARB_pipeline_statistics_query")))
        item (out, WALPE_GLF_PIPELINE_STATS, v >= 46 ? WALPE_GLF_NATIVE : WALPE_GLF_EXTENSION,
              "pipeline statistics", v >= 46 ? NULL : "GL_ARB_pipeline_statistics_query", NULL);
    else
        item (out, WALPE_GLF_PIPELINE_STATS, WALPE_GLF_OFF, "pipeline statistics", NULL,
              "shaded-pixel counts are not measured");

    if ((!es && v >= 43) || (es && v >= 32))
        item (out, WALPE_GLF_DEBUG_OUTPUT, WALPE_GLF_NATIVE, "driver debug output", NULL, NULL);
    else if (walpe_gl_facts_has (f, "GL_KHR_debug"))
        item (out, WALPE_GLF_DEBUG_OUTPUT, WALPE_GLF_EXTENSION, "driver debug output",
              "GL_KHR_debug", NULL);
    else
        item (out, WALPE_GLF_DEBUG_OUTPUT, WALPE_GLF_OFF, "driver debug output", NULL,
              "the profiler report has no driver messages");

    if (!es && (v >= 44 || walpe_gl_facts_has (f, "GL_ARB_buffer_storage")))
        item (out, WALPE_GLF_PERSISTENT_MAP, v >= 44 ? WALPE_GLF_NATIVE : WALPE_GLF_EXTENSION,
              "persistent buffer mapping", v >= 44 ? NULL : "GL_ARB_buffer_storage", NULL);
    else
        item (out, WALPE_GLF_PERSISTENT_MAP, WALPE_GLF_OFF, "persistent buffer mapping", NULL,
              "video_upload = persist / direct fall back to pbo");

    if (!es)
        item (out, WALPE_GLF_POINT_SIZE_ENABLE, WALPE_GLF_NATIVE, "program point size", NULL, NULL);
    else
        item (out, WALPE_GLF_POINT_SIZE_ENABLE, WALPE_GLF_REPLACED, "program point size",
              "always on in OpenGL ES", NULL);

    if (!es)
        item (out, WALPE_GLF_FX_FUSION, WALPE_GLF_NATIVE, "fused effect passes", NULL, NULL);
    else
        item (out, WALPE_GLF_FX_FUSION, WALPE_GLF_OFF, "fused effect passes", NULL,
              "fx_quality = fast runs the member passes one by one (its other savings stay)");
}

char *walpe_gl_plan_describe (const WalpeGlfItem items[WALPE_GLF_N])
{
    GString *s = g_string_new (NULL);
    for (int i = 0; i < WALPE_GLF_N; i++) {
        const WalpeGlfItem *it = &items[i];
        /* what is there natively or through an extension is the full
         * feature: only the differences are listed */
        if (it->how == WALPE_GLF_NATIVE || it->how == WALPE_GLF_EXTENSION || !it->name)
            continue;
        if (s->len)
            g_string_append (s, "; ");
        if (it->how == WALPE_GLF_OFF)
            g_string_append_printf (s, "%s: off", it->name);
        else
            g_string_append_printf (s, "%s: %s", it->name, it->via ? it->via : walpe_glf_how_name (it->how));
    }
    return g_string_free (s, FALSE);
}
