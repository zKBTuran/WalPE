/* gpucaps.c - the driver's strings and extension list -> WalpeGlCaps,
 * the required-capability sentences and the format fallback table. No GL
 * here: see gpucaps.h */
#include "common/gpucaps.h"
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------- helpers */

static gboolean has (const char *lower_hay, const char *needle)
{
    return lower_hay && strstr (lower_hay, needle) != NULL;
}

/* `word` stands in `lower_hay` as a whole word (letters, digits and '_'
 * on neither side) */
static gboolean has_word (const char *lower_hay, const char *word)
{
    if (!lower_hay)
        return FALSE;
    gsize wl = strlen (word);
    for (const char *p = lower_hay; (p = strstr (p, word)) != NULL; p += wl) {
        gboolean left = p == lower_hay || !(g_ascii_isalnum (p[-1]) || p[-1] == '_');
        gboolean right = !(g_ascii_isalnum (p[wl]) || p[wl] == '_');
        if (left && right)
            return TRUE;
    }
    return FALSE;
}

static gboolean has_any_word (const char *lower_hay, const char *const *words)
{
    for (int i = 0; words[i]; i++)
        if (has_word (lower_hay, words[i]))
            return TRUE;
    return FALSE;
}

/* the first "M.m" in s: TRUE when found */
static gboolean first_version (const char *s, int *maj, int *min, const char **after)
{
    for (const char *p = s; p && *p; p++) {
        if (!g_ascii_isdigit (*p))
            continue;
        char *end = NULL;
        long a = strtol (p, &end, 10);
        if (*end == '.' && g_ascii_isdigit (end[1])) {
            *maj = (int) CLAMP (a, 0, 9999);
            const char *m = end + 1;
            *min = (int) CLAMP (strtol (m, &end, 10), 0, 9999);
            if (after)
                *after = end;
            return TRUE;
        }
        p = end - 1;
    }
    return FALSE;
}

/* "a.b.c" / "a.b" right after `tag` in lower-case s */
static void version_after (const char *lower, const char *tag, int *a, int *b, int *c)
{
    const char *p = lower ? strstr (lower, tag) : NULL;
    *a = *b = *c = 0;
    if (!p)
        return;
    const char *after = NULL;
    if (!first_version (p + strlen (tag), a, b, &after))
        return;
    if (after && *after == '.' && g_ascii_isdigit (after[1]))
        *c = (int) CLAMP (strtol (after + 1, NULL, 10), 0, 99999);
}

/* the text in the last pair of parentheses, or NULL (inside `lower`) */
static char *last_parens (const char *lower)
{
    const char *open = lower ? strrchr (lower, '(') : NULL;
    const char *close = open ? strchr (open, ')') : NULL;
    return open && close ? g_strndup (open + 1, (gsize) (close - open - 1)) : NULL;
}

/* ---------------------------------------------------------- code names */

/* AMD processors with graphics, as Mesa's radeonsi names the chip in
 * GL_RENDERER ("AMD Radeon Graphics (radeonsi, renoir, LLVM ...)") */
static const char *const amd_apu_names[] = {
    "kaveri", "kabini", "mullins", "carrizo", "stoney", "raven", "raven2", "picasso", "renoir",
    "lucienne", "cezanne", "barcelo", "vangogh", "yellow_carp", "rembrandt", "mendocino",
    "phoenix", "phoenix2", "hawk_point", "gfx1103_r1", "gfx1103_r1x", "gfx1103_r2",
    "gfx1103_r2x", "gfx1150", "gfx1151", "gfx1152", "strix", "krackan", "cyan_skillfish",
    "raphael_mendocino", "raphael", "granite_ridge", NULL
};
/* cards radeonsi drives */
static const char *const amd_card_names[] = {
    "tahiti", "pitcairn", "verde", "oland", "hainan", "bonaire", "hawaii", "topaz", "tonga",
    "fiji", "polaris10", "polaris11", "polaris12", "vegam", "vega10", "vega12", "vega20",
    "navi10", "navi12", "navi14", "navi21", "navi22", "navi23", "navi24", "navi31", "navi32",
    "navi33", "navi44", "navi48", "sienna_cichlid", "navy_flounder", "dimgrey_cavefish",
    "beige_goby", "gfx1200", "gfx1201", NULL
};
/* chips of Mesa's r600 driver; the first five are processor graphics */
static const char *const amd_r600_igp[] = { "palm", "sumo", "sumo2", "aruba", "rs780", "rs880",
                                            NULL };
static const char *const amd_r600_names[] = {
    "r600", "rv610", "rv630", "rv670", "rv620", "rv635", "rs780", "rs880", "rv770", "rv730",
    "rv710", "rv740", "cedar", "redwood", "juniper", "cypress", "hemlock", "palm", "sumo",
    "sumo2", "barts", "turks", "caicos", "cayman", "aruba", NULL
};
/* Intel generations Mesa's crocus driver drives (the others are iris) */
static const char *const intel_crocus_names[] = { "i965", "g33", "g45", "g4x", "gm45", "ilk",
                                                  "snb", "ivb", "byt", "hsw", NULL };
/* Intel's cards */
static const char *const intel_card_names[] = { "dg1", "dg2", "bmg", "atsm", "ats-m", "acm",
                                                NULL };

/* ---------------------------------------------------------------- names */

const char *walpe_gl_driver_name (WalpeGlDriver d)
{
    static const char *const n[WALPE_GLDRV_N] = {
        [WALPE_GLDRV_UNKNOWN] = "unknown", [WALPE_GLDRV_NVIDIA] = "nvidia",
        [WALPE_GLDRV_RADEONSI] = "radeonsi", [WALPE_GLDRV_R600] = "r600",
        [WALPE_GLDRV_R300] = "r300", [WALPE_GLDRV_IRIS] = "iris",
        [WALPE_GLDRV_CROCUS] = "crocus", [WALPE_GLDRV_I965] = "i965",
        [WALPE_GLDRV_NOUVEAU] = "nouveau", [WALPE_GLDRV_ZINK] = "zink",
        [WALPE_GLDRV_LLVMPIPE] = "llvmpipe", [WALPE_GLDRV_SOFTPIPE] = "softpipe",
        [WALPE_GLDRV_SWIFTSHADER] = "swiftshader", [WALPE_GLDRV_VIRGL] = "virgl",
        [WALPE_GLDRV_SVGA] = "svga", [WALPE_GLDRV_D3D12] = "d3d12",
        [WALPE_GLDRV_PANFROST] = "panfrost", [WALPE_GLDRV_FREEDRENO] = "freedreno",
        [WALPE_GLDRV_V3D] = "v3d", [WALPE_GLDRV_ETNAVIV] = "etnaviv",
        [WALPE_GLDRV_ASAHI] = "asahi", [WALPE_GLDRV_AMD_CLOSED] = "amd-closed",
    };
    return (guint) d < WALPE_GLDRV_N && n[d] ? n[d] : "unknown";
}

const char *walpe_gl_vendor_class_name (WalpeGlVendorClass v)
{
    switch (v) {
    case WALPE_GLVENDOR_NVIDIA:   return "NVIDIA";
    case WALPE_GLVENDOR_AMD:      return "AMD";
    case WALPE_GLVENDOR_INTEL:    return "Intel";
    case WALPE_GLVENDOR_ARM:      return "ARM";
    case WALPE_GLVENDOR_QUALCOMM: return "Qualcomm";
    case WALPE_GLVENDOR_BROADCOM: return "Broadcom";
    case WALPE_GLVENDOR_APPLE:    return "Apple";
    case WALPE_GLVENDOR_VIRTUAL:  return "virtual";
    case WALPE_GLVENDOR_SOFTWARE: return "software";
    default:                      return "other";
    }
}

/* ---------------------------------------------------------------- parse */

/* which driver, from the lower-case strings */
static void identify (WalpeGlCaps *c, const char *v, const char *r, const char *ver)
{
    c->mesa = has (ver, "mesa");
    /* CPU rasterisers first: Mesa's software drivers also appear inside
     * the strings of layered drivers (zink on lavapipe) */
    if (has (r, "llvmpipe")) {
        c->driver = WALPE_GLDRV_LLVMPIPE;
    } else if (has (r, "softpipe") || has (r, "swrast") || has (r, "software rasterizer")) {
        c->driver = WALPE_GLDRV_SOFTPIPE;
    } else if (has (r, "swiftshader")) {
        c->driver = WALPE_GLDRV_SWIFTSHADER;
    }
    if (c->driver != WALPE_GLDRV_UNKNOWN) {
        c->software = TRUE;
        c->vendor_class = WALPE_GLVENDOR_SOFTWARE;
        return;
    }
    /* the word the renderer used before this file existed (glutil.c): a
     * renderer that calls itself "Software ..." is one */
    if (c->renderer && strstr (c->renderer, "Software")) {
        c->software = TRUE;
        c->vendor_class = WALPE_GLVENDOR_SOFTWARE;
        return;
    }
    if (g_str_has_prefix (r, "zink")) {
        c->driver = WALPE_GLDRV_ZINK;
        c->vendor_class = has (r, "nvidia") ? WALPE_GLVENDOR_NVIDIA
                        : has (r, "radv") || has (r, "amd") || has (r, "radeon") ? WALPE_GLVENDOR_AMD
                        : has (r, "anv") || has (r, "intel") ? WALPE_GLVENDOR_INTEL
                        : WALPE_GLVENDOR_OTHER;
        return;
    }
    if (has (r, "virgl")) {
        c->driver = WALPE_GLDRV_VIRGL;
        c->vendor_class = WALPE_GLVENDOR_VIRTUAL;
        return;
    }
    if (has (r, "svga3d")) {
        c->driver = WALPE_GLDRV_SVGA;
        c->vendor_class = WALPE_GLVENDOR_VIRTUAL;
        return;
    }
    if (g_str_has_prefix (r, "d3d12")) {
        c->driver = WALPE_GLDRV_D3D12;
        c->vendor_class = WALPE_GLVENDOR_VIRTUAL;
        return;
    }
    if (!c->mesa && (has (v, "nvidia") || has (ver, "nvidia"))) {
        c->driver = WALPE_GLDRV_NVIDIA;
        c->vendor_class = WALPE_GLVENDOR_NVIDIA;
        return;
    }
    if (c->mesa) {
        if (has (v, "nouveau") || (r[0] == 'n' && r[1] == 'v' && g_ascii_isxdigit (r[2]))) {
            c->driver = WALPE_GLDRV_NOUVEAU;
            c->vendor_class = WALPE_GLVENDOR_NVIDIA;
            return;
        }
        if (has (v, "intel") || has (r, "intel")) {
            c->vendor_class = WALPE_GLVENDOR_INTEL;
            char *code = last_parens (r);
            if (g_str_has_prefix (r, "mesa dri intel"))
                c->driver = WALPE_GLDRV_I965;
            else if (code && has_any_word (code, intel_crocus_names))
                c->driver = WALPE_GLDRV_CROCUS;
            else
                c->driver = WALPE_GLDRV_IRIS;
            g_free (code);
            return;
        }
        if (has (r, "radeonsi") || has (r, "radeon") || has (r, "amd ") || has (v, "amd") ||
            has (v, "ati ") || g_str_has_prefix (r, "ati ")) {
            c->vendor_class = WALPE_GLVENDOR_AMD;
            if (has (r, "radeonsi"))
                c->driver = WALPE_GLDRV_RADEONSI;
            else if (has_any_word (r, amd_r600_names))
                c->driver = WALPE_GLDRV_R600;
            else if (c->gl_major > 0 && c->gl_major < 3 && !c->es)
                c->driver = WALPE_GLDRV_R300;
            else
                c->driver = WALPE_GLDRV_RADEONSI;
            return;
        }
        if (has (r, "panfrost") || has (v, "panfrost") || has (r, "mali")) {
            c->driver = WALPE_GLDRV_PANFROST;
            c->vendor_class = WALPE_GLVENDOR_ARM;
            return;
        }
        if (has (v, "freedreno") || has (r, "adreno") ||
            (r[0] == 'f' && r[1] == 'd' && g_ascii_isdigit (r[2]))) {
            c->driver = WALPE_GLDRV_FREEDRENO;
            c->vendor_class = WALPE_GLVENDOR_QUALCOMM;
            return;
        }
        if (g_str_has_prefix (r, "v3d") || g_str_has_prefix (r, "vc4") || has (v, "broadcom")) {
            c->driver = WALPE_GLDRV_V3D;
            c->vendor_class = WALPE_GLVENDOR_BROADCOM;
            return;
        }
        if (has (v, "etnaviv") || has (r, "vivante")) {
            c->driver = WALPE_GLDRV_ETNAVIV;
            return;
        }
        if (g_str_has_prefix (r, "apple") || has (v, "asahi")) {
            c->driver = WALPE_GLDRV_ASAHI;
            c->vendor_class = WALPE_GLVENDOR_APPLE;
            return;
        }
        return;
    }
    if (has (v, "ati technologies") || has (v, "advanced micro devices")) {
        c->driver = WALPE_GLDRV_AMD_CLOSED;
        c->vendor_class = WALPE_GLVENDOR_AMD;
        return;
    }
    /* drivers this file has no name for: at least whose chip it is */
    if (has (v, "nvidia"))
        c->vendor_class = WALPE_GLVENDOR_NVIDIA;
    else if (has (v, "intel"))
        c->vendor_class = WALPE_GLVENDOR_INTEL;
    else if (has (v, "qualcomm"))
        c->vendor_class = WALPE_GLVENDOR_QUALCOMM;
    else if (has (v, "broadcom"))
        c->vendor_class = WALPE_GLVENDOR_BROADCOM;
    else if (has_word (v, "arm"))
        c->vendor_class = WALPE_GLVENDOR_ARM;
    else if (has (v, "apple"))
        c->vendor_class = WALPE_GLVENDOR_APPLE;
}

/* integrated or discrete, as far as the renderer string says */
static WalpeGpuClass class_from_renderer (const WalpeGlCaps *c, const char *r)
{
    if (c->software)
        return WALPE_GPU_UNKNOWN;
    switch (c->vendor_class) {
    case WALPE_GLVENDOR_NVIDIA:
        return WALPE_GPU_DISCRETE;
    case WALPE_GLVENDOR_INTEL: {
        if (!c->mesa)
            return WALPE_GPU_UNKNOWN;
        char *code = last_parens (r);
        WalpeGpuClass k = code && has_any_word (code, intel_card_names) ? WALPE_GPU_DISCRETE
                                                                         : WALPE_GPU_INTEGRATED;
        g_free (code);
        return k;
    }
    case WALPE_GLVENDOR_AMD:
        if (c->driver == WALPE_GLDRV_R600)
            return has_any_word (r, amd_r600_igp) ? WALPE_GPU_INTEGRATED : WALPE_GPU_DISCRETE;
        if (has_any_word (r, amd_apu_names))
            return WALPE_GPU_INTEGRATED;
        if (has_any_word (r, amd_card_names))
            return WALPE_GPU_DISCRETE;
        return WALPE_GPU_UNKNOWN;
    case WALPE_GLVENDOR_ARM:
    case WALPE_GLVENDOR_QUALCOMM:
    case WALPE_GLVENDOR_BROADCOM:
    case WALPE_GLVENDOR_APPLE:
        return WALPE_GPU_INTEGRATED;
    case WALPE_GLVENDOR_VIRTUAL:
        return WALPE_GPU_VIRTUAL;
    default:
        return WALPE_GPU_UNKNOWN;
    }
}

void walpe_gl_caps_clear (WalpeGlCaps *c)
{
    if (!c)
        return;
    g_free (c->vendor);
    g_free (c->renderer);
    g_free (c->version);
    g_free (c->glsl);
    if (c->extensions)
        g_hash_table_unref (c->extensions);
    memset (c, 0, sizeof *c);
}

gboolean walpe_gl_caps_has_ext (const WalpeGlCaps *c, const char *name)
{
    return c && c->extensions && name && g_hash_table_contains (c->extensions, name);
}

gboolean walpe_gl_caps_at_least (const WalpeGlCaps *c, gboolean es, int major, int minor)
{
    if (!c || !c->es != !es)
        return FALSE;
    return c->gl_major > major || (c->gl_major == major && c->gl_minor >= minor);
}

void walpe_gl_caps_parse (WalpeGlCaps *c, const char *vendor, const char *renderer,
                          const char *version, const char *glsl, const char *extensions,
                          const WalpeGlLimits *limits)
{
    memset (c, 0, sizeof *c);
    c->vendor = g_strdup (vendor ? vendor : "");
    c->renderer = g_strdup (renderer ? renderer : "");
    c->version = g_strdup (version ? version : "");
    c->glsl = g_strdup (glsl ? glsl : "");
    if (limits)
        c->limits = *limits;
    char *v = g_ascii_strdown (c->vendor, -1), *r = g_ascii_strdown (c->renderer, -1);
    char *ver = g_ascii_strdown (c->version, -1), *sl = g_ascii_strdown (c->glsl, -1);

    /* "OpenGL ES 3.2 Mesa 24.0.9", "4.6 (Core Profile) Mesa 24.0.9", "4.6.0 NVIDIA 550.120" */
    c->es = g_str_has_prefix (ver, "opengl es");
    first_version (ver, &c->gl_major, &c->gl_minor, NULL);
    c->gl_ver = c->gl_major * 10 + MIN (c->gl_minor, 9);
    {
        /* "4.60 NVIDIA", "3.30", "OpenGL ES GLSL ES 3.20" */
        int a = 0, b = 0;
        const char *after = NULL;
        if (first_version (sl, &a, &b, &after)) {
            /* "3.3" means 3.30; "3.30" is two digits already */
            gboolean one_digit = after && after - 1 >= sl && !(after - 2 >= sl &&
                                 g_ascii_isdigit (after[-2]));
            c->glsl_ver = a * 100 + (one_digit ? b * 10 : MIN (b, 99));
        }
    }
    identify (c, v, r, ver);
    if (c->mesa)
        version_after (ver, "mesa", &c->drv_major, &c->drv_minor, &c->drv_patch);
    else if (c->driver == WALPE_GLDRV_NVIDIA)
        version_after (ver, "nvidia", &c->drv_major, &c->drv_minor, &c->drv_patch);
    c->gpu_class = class_from_renderer (c, r);

    /* the extension list as a set */
    GHashTable *ext = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    if (extensions) {
        char **names = g_strsplit_set (extensions, " \t\n", -1);
        for (int i = 0; names[i]; i++)
            if (*names[i])
                g_hash_table_add (ext, names[i]);
            else
                g_free (names[i]);
        g_free (names);
    }
#define E(name) g_hash_table_contains (ext, name)
    const gboolean es = c->es;
    const int gv = c->gl_ver;

    c->s3tc = E ("GL_EXT_texture_compression_s3tc");
    c->rgtc = es ? E ("GL_EXT_texture_compression_rgtc")
                 : gv >= 30 || E ("GL_ARB_texture_compression_rgtc") ||
                   E ("GL_EXT_texture_compression_rgtc");
    c->bptc = es ? E ("GL_EXT_texture_compression_bptc")
                 : gv >= 42 || E ("GL_ARB_texture_compression_bptc");
    c->etc2 = es ? gv >= 30 : gv >= 43 || E ("GL_ARB_ES3_compatibility");
    c->astc_ldr = E ("GL_KHR_texture_compression_astc_ldr");
    c->anisotropic = E ("GL_EXT_texture_filter_anisotropic") ||
                     E ("GL_ARB_texture_filter_anisotropic") || (!es && gv >= 46);
    c->swizzle = es ? gv >= 30
                    : gv >= 33 || E ("GL_ARB_texture_swizzle") || E ("GL_EXT_texture_swizzle");
    c->lod_bias = !es;
    c->bgra_upload = !es;
    c->bgra_ext = es && E ("GL_EXT_texture_format_BGRA8888");
    c->norm16 = !es || E ("GL_EXT_texture_norm16");
    c->float_linear = !es || E ("GL_OES_texture_float_linear");
    c->get_tex_image = !es;
    c->tex_level_query = !es || gv >= 31;
    c->copy_image = es ? gv >= 32 || E ("GL_EXT_copy_image") || E ("GL_OES_copy_image")
                       : gv >= 43 || E ("GL_ARB_copy_image");

    /* (OpenGL ES 3.2 took GL_EXT_color_buffer_float into the core: a 3.2
     * driver need not list the extension) */
    c->color_float = es ? gv >= 32 || E ("GL_EXT_color_buffer_float") : gv >= 30;
    c->color_half_float = c->color_float || (es && E ("GL_EXT_color_buffer_half_float"));
    c->invalidate_fb = es ? gv >= 30 : gv >= 43 || E ("GL_ARB_invalidate_subdata");

    c->buffer_storage = !es && (gv >= 44 || E ("GL_ARB_buffer_storage"));
    c->timer_query = !es && (gv >= 33 || E ("GL_ARB_timer_query"));
    c->disjoint_timer_query = es && gv >= 30 && E ("GL_EXT_disjoint_timer_query");
    c->samples_passed = !es;
    c->pipeline_stats = !es && (gv >= 46 || E ("GL_ARB_pipeline_statistics_query"));
    c->khr_debug = E ("GL_KHR_debug") || (!es && gv >= 43);
    c->nvx_gpu_memory_info = E ("GL_NVX_gpu_memory_info");
    c->ati_meminfo = E ("GL_ATI_meminfo");

    c->oes_egl_image = E ("GL_OES_EGL_image");
    c->oes_egl_image_external = E ("GL_OES_EGL_image_external");
#undef E
    c->extensions = ext;
    c->n_extensions = g_hash_table_size (ext);

    /* OpenGL ES: 3.1 is the minimum of the tree (glGetTexLevelParameter is
     * called without a check; the context ladder of common/glapi.h never
     * makes an ES 3.0 context), and the effect shaders have a GLSL ES
     * output from there on (engine/fx_essl.h) */
    c->base_ok = es ? gv >= 31 : gv >= 33;
    c->scene_shaders = (!es && gv >= 33 && (c->glsl_ver == 0 || c->glsl_ver >= 330)) ||
                       (es && gv >= 31);
    g_free (v);
    g_free (r);
    g_free (ver);
    g_free (sl);
}

/* "NVIDIA GeForce ... (4.6.0 NVIDIA 550.120)" */
static char *driver_words (const WalpeGlCaps *c)
{
    return g_strdup_printf ("%s (%s)", c->renderer && *c->renderer ? c->renderer : "this driver",
                            c->version && *c->version ? c->version : "no version");
}

char *walpe_gl_caps_missing (const WalpeGlCaps *c)
{
    if (!c || c->base_ok)
        return NULL;
    char *who = driver_words (c);
    char *s = g_strdup_printf ("OpenGL 3.3 or OpenGL ES 3.1 is needed and %s offers %s %d.%d: "
                               "WalPE cannot draw with this driver (a newer driver, or "
                               "\"Render on CPU\", may help)",
                               who, c->es ? "OpenGL ES" : "OpenGL", c->gl_major, c->gl_minor);
    g_free (who);
    return s;
}

char *walpe_gl_caps_scene_missing (const WalpeGlCaps *c)
{
    if (!c || c->scene_shaders)
        return NULL;
    char *who = driver_words (c);
    char *s;
    if (c->es)
        s = g_strdup_printf ("scene wallpapers need OpenGL 3.3 or OpenGL ES 3.1 (their effect "
                             "shaders are written out as GLSL 3.30 or GLSL ES 3.10 / 3.20) and "
                             "%s gives only OpenGL ES %d.%d here", who, c->gl_major, c->gl_minor);
    else
        s = g_strdup_printf ("scene wallpapers need OpenGL 3.3 with GLSL 3.30 and %s offers "
                             "OpenGL %d.%d%s%s", who, c->gl_major, c->gl_minor,
                             c->glsl && *c->glsl ? ", GLSL " : "", c->glsl ? c->glsl : "");
    g_free (who);
    return s;
}

char *walpe_gl_caps_describe (const WalpeGlCaps *c)
{
    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "driver %s", walpe_gl_driver_name (c->driver));
    if (c->drv_major)
        g_string_append_printf (s, " %d.%d", c->drv_major, c->drv_minor);
    g_string_append_printf (s, " (%s%s%s), %s %d.%d", walpe_gl_vendor_class_name (c->vendor_class),
                            c->gpu_class != WALPE_GPU_UNKNOWN ? ", " : "",
                            c->gpu_class != WALPE_GPU_UNKNOWN ? walpe_gpu_class_name (c->gpu_class)
                                                              : "",
                            c->es ? "OpenGL ES" : "OpenGL", c->gl_major, c->gl_minor);
    if (c->glsl_ver)
        g_string_append_printf (s, ", GLSL %d.%02d", c->glsl_ver / 100, c->glsl_ver % 100);
    static const struct { const char *name; gsize off; } opt[] = {
        { "s3tc", G_STRUCT_OFFSET (WalpeGlCaps, s3tc) },
        { "bptc", G_STRUCT_OFFSET (WalpeGlCaps, bptc) },
        { "astc", G_STRUCT_OFFSET (WalpeGlCaps, astc_ldr) },
        { "anisotropic", G_STRUCT_OFFSET (WalpeGlCaps, anisotropic) },
        { "float-targets", G_STRUCT_OFFSET (WalpeGlCaps, color_float) },
        { "norm16", G_STRUCT_OFFSET (WalpeGlCaps, norm16) },
        { "buffer-storage", G_STRUCT_OFFSET (WalpeGlCaps, buffer_storage) },
        { "timer-query", G_STRUCT_OFFSET (WalpeGlCaps, timer_query) },
        { "debug", G_STRUCT_OFFSET (WalpeGlCaps, khr_debug) },
        { "memory-info", G_STRUCT_OFFSET (WalpeGlCaps, nvx_gpu_memory_info) },
        { "egl-image", G_STRUCT_OFFSET (WalpeGlCaps, oes_egl_image) },
        { "dma-buf", G_STRUCT_OFFSET (WalpeGlCaps, egl_image_dmabuf) },
    };
    GString *yes = g_string_new (NULL), *no = g_string_new (NULL);
    for (guint i = 0; i < G_N_ELEMENTS (opt); i++) {
        gboolean on = G_STRUCT_MEMBER (gboolean, c, opt[i].off);
        GString *t = on ? yes : no;
        g_string_append_printf (t, "%s%s", t->len ? " " : "", opt[i].name);
    }
    g_string_append_printf (s, "; has: %s; lacks: %s", yes->len ? yes->str : "-",
                            no->len ? no->str : "-");
    if (c->limits.max_texture_size)
        g_string_append_printf (s, "; largest texture %d", c->limits.max_texture_size);
    g_string_free (yes, TRUE);
    g_string_free (no, TRUE);
    return g_string_free (s, FALSE);
}

/* -------------------------------------------------------------- formats */

const char *walpe_fmt_use_name (WalpeFmtUse use)
{
    static const char *const n[WALPE_FMT_N] = {
        [WALPE_FMT_TEX_DXT1] = "DXT1 texture", [WALPE_FMT_TEX_DXT3] = "DXT3 texture",
        [WALPE_FMT_TEX_DXT5] = "DXT5 texture", [WALPE_FMT_TEX_R8] = "one-channel texture",
        [WALPE_FMT_TEX_RG8] = "two-channel texture", [WALPE_FMT_TEX_BGRA8] = "8-bit texture",
        [WALPE_FMT_RT_RGBA8] = "RGBA8 target", [WALPE_FMT_RT_R8] = "R8 target",
        [WALPE_FMT_RT_RG8] = "RG8 target", [WALPE_FMT_RT_R16F] = "R16F target",
        [WALPE_FMT_RT_RG16F] = "RG16F target", [WALPE_FMT_RT_RGBA16F] = "RGBA16F target",
        [WALPE_FMT_RT_RGB8] = "RGB8 target", [WALPE_FMT_RT_RGB10_A2] = "RGB10A2 target",
        [WALPE_FMT_DATA_RGBA32F] = "RGBA32F data texture",
        [WALPE_FMT_VIDEO_R8] = "8-bit video plane", [WALPE_FMT_VIDEO_RG8] = "8-bit video chroma plane",
        [WALPE_FMT_VIDEO_R16] = "16-bit video plane", [WALPE_FMT_VIDEO_RG16] = "16-bit video chroma plane",
    };
    return (guint) use < WALPE_FMT_N && n[use] ? n[use] : "format";
}

WalpeFmtChoice walpe_gl_caps_format (const WalpeGlCaps *c, WalpeFmtUse use)
{
    WalpeFmtChoice ch = { use, WALPE_FMT_NATIVE, use, NULL, NULL };
    if (!c)
        return ch;      /* before a context exists: nothing is known to be missing */
    switch (use) {
    case WALPE_FMT_TEX_DXT1:
    case WALPE_FMT_TEX_DXT3:
    case WALPE_FMT_TEX_DXT5:
        if (!c->s3tc) {
            ch.how = WALPE_FMT_CPU_DECODE;
            ch.as = WALPE_FMT_TEX_BGRA8;
            ch.reason = "GL_EXT_texture_compression_s3tc";
            ch.loss = use == WALPE_FMT_TEX_DXT1 ? "8 times the video memory and a decode at load"
                                                : "4 times the video memory and a decode at load";
        }
        break;
    case WALPE_FMT_TEX_R8:
    case WALPE_FMT_TEX_RG8:
        if (!c->swizzle) {
            ch.how = WALPE_FMT_CPU_DECODE;
            ch.as = WALPE_FMT_TEX_BGRA8;
            ch.reason = "texture swizzle (OpenGL 3.3 / GL_ARB_texture_swizzle)";
            ch.loss = use == WALPE_FMT_TEX_R8 ? "4 times the video memory" : "twice the video memory";
        }
        break;
    case WALPE_FMT_TEX_BGRA8:
        if (c->bgra_upload)
            break;
        if (c->bgra_ext) {
            ch.how = WALPE_FMT_BGRA_EXT;
            ch.reason = "BGRA uploads into RGBA8 (desktop OpenGL)";
        } else if (c->swizzle) {
            ch.how = WALPE_FMT_SWIZZLED;
            ch.reason = "BGRA uploads (GL_EXT_texture_format_BGRA8888)";
        } else {
            ch.how = WALPE_FMT_CPU_SWAP;
            ch.reason = "BGRA uploads (GL_EXT_texture_format_BGRA8888) and texture swizzle";
            ch.loss = "one pass over every picture at load";
        }
        break;
    case WALPE_FMT_RT_R16F:
    case WALPE_FMT_RT_RG16F:
    case WALPE_FMT_RT_RGBA16F:
        if (!c->color_half_float) {
            ch.how = WALPE_FMT_SUBSTITUTE;
            ch.as = WALPE_FMT_RT_RGBA8;
            ch.reason = "float render targets (GL_EXT_color_buffer_half_float / _float)";
            ch.loss = use == WALPE_FMT_RT_RGBA16F
                          ? "values outside 0..1 are clipped and steps are 1/255"
                          : "values outside 0..1 are clipped, steps are 1/255, and the target "
                            "takes 2 to 4 times the memory";
        }
        break;
    case WALPE_FMT_VIDEO_R16:
    case WALPE_FMT_VIDEO_RG16:
        if (!c->norm16) {
            ch.how = WALPE_FMT_SUBSTITUTE;
            ch.as = use == WALPE_FMT_VIDEO_R16 ? WALPE_FMT_VIDEO_R8 : WALPE_FMT_VIDEO_RG8;
            ch.reason = "16-bit normalised textures (GL_EXT_texture_norm16)";
            ch.loss = "10-bit video is reduced to 8 bits before it is uploaded";
        }
        break;
    case WALPE_FMT_DATA_RGBA32F:
    case WALPE_FMT_RT_RGBA8:
    case WALPE_FMT_RT_R8:
    case WALPE_FMT_RT_RG8:
    case WALPE_FMT_RT_RGB8:
    case WALPE_FMT_RT_RGB10_A2:
    case WALPE_FMT_VIDEO_R8:
    case WALPE_FMT_VIDEO_RG8:
    default:
        /* part of OpenGL 3.3 and of OpenGL ES 3.0: always there */
        break;
    }
    return ch;
}

char *walpe_fmt_choice_describe (const WalpeFmtChoice *ch, const WalpeGlCaps *c)
{
    if (!ch || ch->how == WALPE_FMT_NATIVE)
        return NULL;
    const char *what = walpe_fmt_use_name (ch->use), *as = walpe_fmt_use_name (ch->as);
    char *how;
    switch (ch->how) {
    case WALPE_FMT_CPU_DECODE:
        how = g_strdup_printf ("decoded on the CPU and stored as %s", as);
        break;
    case WALPE_FMT_SWIZZLED:
        how = g_strdup ("uploaded as RGBA with red and blue exchanged by a texture swizzle");
        break;
    case WALPE_FMT_CPU_SWAP:
        how = g_strdup ("red and blue exchanged on the CPU before the upload");
        break;
    case WALPE_FMT_BGRA_EXT:
        how = g_strdup ("uploaded in the extension's own BGRA format");
        break;
    default:
        how = g_strdup_printf ("%s is used instead", as);
        break;
    }
    char *s = g_strdup_printf ("%s: %s lacks %s; %s%s%s%s", what,
                               c ? walpe_gl_driver_name (c->driver) : "the driver",
                               ch->reason ? ch->reason : "it", how, ch->loss ? " (" : "",
                               ch->loss ? ch->loss : "", ch->loss ? ")" : "");
    g_free (how);
    return s;
}
