/* glfeat.c - the process's record of the OpenGL API in use (glfeat.h) */
#include "glfeat.h"
#include <epoxy/gl.h>
#include <string.h>

static struct {
    gboolean es;
    int version;                    /* OpenGL ES: 31, 32; desktop: 0 (not asked) */
    WalpeGlfItem plan[WALPE_GLF_N];
} F;
static gint f_known;
static GMutex f_lock;
/* what walpe_gl_feat_plan () hands out before a context was asked: the
 * desktop table, built once and never written again. (It used to hand out
 * F.plan itself, which walpe_gl_feat_init () fills a moment later on a
 * render thread while `walpe-ctl status` may still be reading it on the
 * main thread - on OpenGL ES an item "off" whose name was not yet set.) */
static WalpeGlfItem f_before[WALPE_GLF_N];
static gboolean f_before_set;

static gboolean ext_of_context (const char *name, gpointer user)
{
    (void) user;
    return epoxy_has_gl_extension (name);
}

/* desktop OpenGL: the table with every item native. The optional items of
 * that path (S3TC, anisotropic filtering, buffer storage, ...) keep the
 * checks the code has always made where it uses them; they are not asked
 * again here. */
static void plan_desktop (WalpeGlfItem *out)
{
    static const char all[] =
        "GL_EXT_texture_compression_s3tc GL_EXT_texture_filter_anisotropic "
        "GL_ARB_pipeline_statistics_query GL_KHR_debug GL_ARB_buffer_storage";
    WalpeGlFacts facts = { FALSE, 46, all, NULL, NULL };
    walpe_gl_plan (&facts, out);
    for (int i = 0; i < WALPE_GLF_N; i++) {
        out[i].how = WALPE_GLF_NATIVE;
        out[i].via = NULL;
        out[i].effect = NULL;
    }
}

void walpe_gl_feat_init (void)
{
    if (g_atomic_int_get (&f_known))
        return;
    g_mutex_lock (&f_lock);
    if (!g_atomic_int_get (&f_known)) {
        F.es = !epoxy_is_desktop_gl ();
        if (!F.es) {
            F.version = 0;
            plan_desktop (F.plan);
        } else {
            F.version = epoxy_gl_version ();
            WalpeGlFacts facts = { TRUE, F.version, NULL, ext_of_context, NULL };
            walpe_gl_plan (&facts, F.plan);
            char *d = walpe_gl_plan_describe (F.plan);
            g_message ("gl: OpenGL ES %d.%d path, shaders as GLSL ES %d%s%s", F.version / 10,
                       F.version % 10, walpe_gl_essl_version (F.version),
                       *d ? "; replaced or off: " : "", d);
            g_free (d);
            if (F.version < WALPE_GL_MIN_ES_MAJOR * 10 + WALPE_GL_MIN_ES_MINOR)
                g_warning ("gl: OpenGL ES %d.%d is below the minimum (%d.%d): texture level "
                           "queries are missing", F.version / 10, F.version % 10,
                           WALPE_GL_MIN_ES_MAJOR, WALPE_GL_MIN_ES_MINOR);
        }
        g_atomic_int_set (&f_known, 1);
    }
    g_mutex_unlock (&f_lock);
}

gboolean walpe_gl_feat_known (void)
{
    return g_atomic_int_get (&f_known) != 0;
}

gboolean walpe_gl_feat_es (void)
{
    return g_atomic_int_get (&f_known) && F.es;
}

int walpe_gl_feat_es_version (void)
{
    return walpe_gl_feat_es () ? F.version : 0;
}

int walpe_gl_feat_essl (void)
{
    return walpe_gl_feat_es () ? walpe_gl_essl_version (F.version) : 0;
}

WalpeGlfHow walpe_gl_feat_how (WalpeGlFeature f)
{
    if (!g_atomic_int_get (&f_known) || (int) f < 0 || f >= WALPE_GLF_N)
        return WALPE_GLF_NATIVE;
    return F.plan[f].how;
}

const WalpeGlfItem *walpe_gl_feat_plan (void)
{
    if (g_atomic_int_get (&f_known))
        return F.plan;      /* not written any more */
    g_mutex_lock (&f_lock);
    if (!f_before_set) {
        plan_desktop (f_before);
        f_before_set = TRUE;
    }
    g_mutex_unlock (&f_lock);
    return f_before;
}

void walpe_gl_feat_override (int es, int version, const char *extensions)
{
    g_mutex_lock (&f_lock);
    if (es < 0) {
        memset (&F, 0, sizeof (F));
        g_atomic_int_set (&f_known, 0);
    } else {
        F.es = es != 0;
        F.version = F.es ? version : 0;
        if (F.es) {
            /* the plan keeps pointers to static texts only: the extension
             * string is not kept */
            WalpeGlFacts facts = { TRUE, version, extensions ? extensions : "", NULL, NULL };
            walpe_gl_plan (&facts, F.plan);
        } else {
            plan_desktop (F.plan);
        }
        g_atomic_int_set (&f_known, 1);
    }
    g_mutex_unlock (&f_lock);
}
