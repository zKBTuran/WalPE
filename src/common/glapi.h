/* glapi.h - which OpenGL API renders: desktop OpenGL or OpenGL ES.
 *
 * WalPE draws with desktop OpenGL 3.3. Many ARM GPUs (Mali with panfrost /
 * panthor, Broadcom V3D, older Adreno) and a few old PC ones offer OpenGL
 * ES 3.1 / 3.2 but no desktop OpenGL 3.3; without an ES path such a
 * machine could only render on the CPU. The ES path is the same renderer:
 * the same GL calls where both APIs have them (nearly all, see the table
 * in walpe_gl_plan), GLSL ES instead of GLSL 3.30 for the shaders, and a
 * replacement or a named "off" for the few things ES lacks.
 *
 * Order of preference (setting `gl_api`, environment WALPE_GL_API):
 *   auto   desktop OpenGL 3.3 when the driver gives such a context, else
 *          OpenGL ES 3.2, else OpenGL ES 3.1               (the default)
 *   gl     desktop OpenGL 3.3
 *   gles   OpenGL ES 3.2, else 3.1, even where desktop OpenGL exists
 *          (how the ES path is exercised on a desktop driver or llvmpipe)
 * WALPE_GL_API is strict - a diagnostic for one run: that API or nothing.
 * The saved setting is not: when it names an API this machine has no
 * context for, the other API's rungs are tried as `auto` would (a saved
 * value must never leave a daemon that cannot start and that walpe-ctl,
 * which needs it running, cannot be used to repair); the saved value
 * stays as it is and the note says what happened.
 * On a driver with desktop OpenGL 3.3 `auto` therefore creates exactly the
 * context it always created, with the same attributes.
 * OpenGL ES 3.0 is not enough: glGetTexLevelParameter, which the texture
 * code asks on its normal paths, came with 3.1.
 *
 * This file is the part without EGL and GL: the preference, the ladder,
 * the walk over it with a caller-supplied "try this rung" function, and
 * the table of what the ES path does about each desktop-only item. Tests
 * drive it with made-up capability sets. */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    WALPE_GL_API_AUTO = 0,
    WALPE_GL_API_GL,
    WALPE_GL_API_GLES,
} WalpeGlApiPref;

/* "auto" | "gl" | "gles" (also "opengl", "desktop", "es", "opengles",
 * "opengl-es"; case does not matter). FALSE: not one of them, *out
 * untouched. */
gboolean walpe_gl_api_parse (const char *s, WalpeGlApiPref *out);
const char *walpe_gl_api_name (WalpeGlApiPref p);   /* "auto", "gl", "gles" */
/* WALPE_GL_API: a WalpeGlApiPref, or -1 when unset, empty or not one of
 * the names (the setting decides then) */
int walpe_gl_api_env_override (const char *value);

typedef struct {
    gboolean es;
    int major, minor;
} WalpeGlRung;

#define WALPE_GL_MIN_GL_MAJOR 3
#define WALPE_GL_MIN_GL_MINOR 3
#define WALPE_GL_MIN_ES_MAJOR 3
#define WALPE_GL_MIN_ES_MINOR 1
#define WALPE_GL_LADDER_MAX 4

/* the context versions to ask for, most wanted first; returns how many */
int walpe_gl_api_ladder (WalpeGlApiPref pref, WalpeGlRung out[WALPE_GL_LADDER_MAX]);
/* "OpenGL 3.3" / "OpenGL ES 3.2" into buf; returns buf */
const char *walpe_gl_rung_name (const WalpeGlRung *r, char *buf, gsize n);

/* What EGL says before a context is asked for. A rung whose API is ruled
 * out here is skipped without a try. */
typedef struct {
    /* EGL_CLIENT_APIS ("OpenGL OpenGL_ES OpenVG"); NULL = not known */
    const char *client_apis;
    /* EGL_RENDERABLE_TYPE of the window config in use; config_known FALSE
     * = not asked */
    gboolean config_known;
    gboolean config_gl;         /* EGL_OPENGL_BIT */
    gboolean config_es3;        /* EGL_OPENGL_ES3_BIT */
} WalpeGlEglFacts;

/* TRUE when a context of that rung could be made (the caller keeps it) */
typedef gboolean (*WalpeGlTryFn) (const WalpeGlRung *rung, gpointer user);

typedef struct {
    int rung;                   /* index in the ladder of `pref`; -1 = none */
    WalpeGlRung got;
    /* auto took OpenGL ES because desktop OpenGL 3.3 is not there */
    gboolean fell_back;
    /* the preference forced the API (gl / gles) */
    gboolean forced;
    /* walpe_gl_api_choose_for (): the saved setting named an API without
     * a context here and the other API was taken for this run (forced is
     * FALSE then; rung is the index in the ladder of auto) */
    gboolean setting_fallback;
    /* every rung and what became of it: "OpenGL 3.3: refused; OpenGL ES
     * 3.2: ok" */
    char tried[240];
    /* one sentence for the log and `walpe-ctl status`: which path runs
     * and why */
    char why[240];
} WalpeGlApiChoice;

/* Walks the ladder of `pref`. egl may be NULL (nothing known: every rung
 * is tried). FALSE: no rung gave a context; out->why says what was asked
 * for. */
gboolean walpe_gl_api_choose (WalpeGlApiPref pref, const WalpeGlEglFacts *egl,
                              WalpeGlTryFn try_rung, gpointer user, WalpeGlApiChoice *out);
/* What the daemon decides with. strict (pref comes from WALPE_GL_API):
 * walpe_gl_api_choose () - that API or nothing. Not strict (pref is the
 * saved setting): when gl / gles gives no context, the rungs of the other
 * API are walked as well - what auto would have tried, without asking
 * twice for what was just refused. On success out->setting_fallback is
 * TRUE, out->why names what runs, what was saved and that auto is used
 * for this run; out->tried lists the rungs of both walks. With auto, and
 * whenever the first walk gives a context, the result (and every request
 * made through try_rung) is walpe_gl_api_choose ()'s. */
gboolean walpe_gl_api_choose_for (WalpeGlApiPref pref, gboolean strict, const WalpeGlEglFacts *egl,
                                  WalpeGlTryFn try_rung, gpointer user, WalpeGlApiChoice *out);

/* GLSL ES version of an OpenGL ES context version (major * 10 + minor):
 * 30 -> 300, 31 -> 310, 32 and later -> 320 */
int walpe_gl_essl_version (int es_version);

/* ---- what each API offers: the fallback table */

typedef enum {
    WALPE_GLF_SHADER_DIALECT = 0,   /* effect shaders: GLSL 3.30 */
    WALPE_GLF_BGRA_UPLOAD,          /* BGRA bytes into an RGBA8 texture */
    WALPE_GLF_S3TC,                 /* DXT1 / DXT3 / DXT5 kept compressed */
    WALPE_GLF_ANISOTROPIC,
    WALPE_GLF_LOD_BIAS,             /* GL_TEXTURE_LOD_BIAS texture parameter */
    WALPE_GLF_FLOAT_TARGETS,        /* R16F / RG16F / RGBA16F render targets */
    WALPE_GLF_NORM16,               /* R16 / RG16 video planes (10-bit video) */
    WALPE_GLF_TEX_LEVEL_QUERY,      /* glGetTexLevelParameter */
    WALPE_GLF_TEX_READBACK,         /* glGetTexImage (diagnostics) */
    WALPE_GLF_TIMER_QUERY,          /* GPU time per pass in the profiler */
    WALPE_GLF_SAMPLE_COUNTS,        /* GL_SAMPLES_PASSED (statistics) */
    WALPE_GLF_PIPELINE_STATS,       /* fragment shader invocations (statistics) */
    WALPE_GLF_DEBUG_OUTPUT,         /* the driver's messages in the profiler */
    WALPE_GLF_PERSISTENT_MAP,       /* video_upload = persist / direct */
    WALPE_GLF_POINT_SIZE_ENABLE,    /* GL_PROGRAM_POINT_SIZE */
    WALPE_GLF_FX_FUSION,            /* fx_quality = fast: fused displacement passes */
    WALPE_GLF_N
} WalpeGlFeature;

typedef enum {
    WALPE_GLF_NATIVE = 0,       /* part of the API version in use */
    WALPE_GLF_EXTENSION,        /* there through the extension `via` */
    WALPE_GLF_REPLACED,         /* done another way (`via` says how) */
    WALPE_GLF_OFF,              /* not available: `effect` says what that means */
} WalpeGlfHow;

typedef struct {
    WalpeGlFeature feature;
    WalpeGlfHow how;
    const char *name;           /* for people and for status: "BGRA upload" */
    const char *via;            /* extension name / the replacement; NULL when native or off */
    const char *effect;         /* what the user gets instead; NULL = no difference */
} WalpeGlfItem;

typedef struct {
    gboolean es;
    int version;                /* major * 10 + minor: 33, 46; OpenGL ES: 31, 32 */
    /* either the extension names separated by spaces ... */
    const char *extensions;
    /* ... or a function that knows (the running context: epoxy) */
    gboolean (*has_ext) (const char *name, gpointer user);
    gpointer user;
} WalpeGlFacts;

gboolean walpe_gl_facts_has (const WalpeGlFacts *f, const char *ext);
/* fills out[0 .. WALPE_GLF_N - 1], indexed by WalpeGlFeature */
void walpe_gl_plan (const WalpeGlFacts *f, WalpeGlfItem out[WALPE_GLF_N]);
const char *walpe_glf_how_name (WalpeGlfHow h);     /* "native", "extension", "replaced", "off" */
/* one line for the log: every item that is replaced or off, "; "-separated
 * ("" when nothing is); g_free */
char *walpe_gl_plan_describe (const WalpeGlfItem items[WALPE_GLF_N]);

G_END_DECLS
