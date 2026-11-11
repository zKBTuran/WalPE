/* glcaps.c - reads the current context into the process's WalpeGlCaps:
 * see glcaps.h and common/gpucaps.h */
#include "glcaps.h"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <string.h>
#include <stdlib.h>

static GMutex caps_lock;
static WalpeGlCaps *caps_ptr;           /* set once (g_atomic_pointer) */
static char *caps_err, *caps_scene_err; /* under caps_lock until caps_ptr is set */
static gint fmt_logged[WALPE_FMT_N];

const WalpeGlCaps *walpe_gl_caps (void)
{
    return g_atomic_pointer_get (&caps_ptr);
}

const char *walpe_gl_caps_error (void)
{
    return g_atomic_pointer_get (&caps_ptr) ? caps_err : NULL;
}

const char *walpe_gl_caps_scene_error (void)
{
    return g_atomic_pointer_get (&caps_ptr) ? caps_scene_err : NULL;
}

/* the extension names of the current context, separated by spaces, less
 * those WALPE_GL_LACKS names */
static char *read_extensions (int gl_ver)
{
    GString *s = g_string_new (NULL);
    if (gl_ver >= 30) {
        GLint n = 0;
        glGetIntegerv (GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; i++) {
            const char *e = (const char *) glGetStringi (GL_EXTENSIONS, (GLuint) i);
            if (e && *e)
                g_string_append_printf (s, "%s%s", s->len ? " " : "", e);
        }
    } else {
        /* before OpenGL 3.0 the list is one string */
        const char *e = (const char *) glGetString (GL_EXTENSIONS);
        if (e)
            g_string_append (s, e);
    }
    const char *lacks = g_getenv ("WALPE_GL_LACKS");
    if (lacks && *lacks) {
        char **drop = g_strsplit_set (lacks, ", ", -1);
        char **have = g_strsplit (s->str, " ", -1);
        g_string_truncate (s, 0);
        for (int i = 0; have[i]; i++) {
            if (!*have[i] || g_strv_contains ((const char *const *) drop, have[i]))
                continue;
            g_string_append_printf (s, "%s%s", s->len ? " " : "", have[i]);
        }
        g_strfreev (have);
        g_strfreev (drop);
    }
    return g_string_free (s, FALSE);
}

static gboolean ext_listed (const char *list, const char *name)
{
    gsize nl = strlen (name);
    for (const char *p = list; (p = strstr (p, name)) != NULL; p += nl)
        if ((p == list || p[-1] == ' ') && (p[nl] == 0 || p[nl] == ' '))
            return TRUE;
    return FALSE;
}

/* caps_lock held */
static void caps_publish (WalpeGlCaps *c, gboolean log)
{
    caps_err = walpe_gl_caps_missing (c);
    caps_scene_err = walpe_gl_caps_scene_missing (c);
    if (log) {
        char *d = walpe_gl_caps_describe (c);
        g_message ("gl: %s", d);
        g_free (d);
        if (g_getenv ("WALPE_GL_LACKS"))
            g_message ("gl: WALPE_GL_LACKS is set: the extensions it names are treated as absent");
        if (caps_err)
            g_critical ("gl: %s", caps_err);
    }
    g_atomic_pointer_set (&caps_ptr, c);
}

const WalpeGlCaps *walpe_gl_caps_init (void)
{
    WalpeGlCaps *c = g_atomic_pointer_get (&caps_ptr);
    if (c)
        return c;
    g_mutex_lock (&caps_lock);
    c = g_atomic_pointer_get (&caps_ptr);
    if (!c) {
        const char *vendor = (const char *) glGetString (GL_VENDOR);
        const char *renderer = (const char *) glGetString (GL_RENDERER);
        const char *version = (const char *) glGetString (GL_VERSION);
        const char *glsl = (const char *) glGetString (GL_SHADING_LANGUAGE_VERSION);
        int gl_ver = epoxy_gl_version ();
        char *ext = read_extensions (gl_ver);
        WalpeGlLimits lim = { 0 };
        GLint v = 0;
        glGetIntegerv (GL_MAX_TEXTURE_SIZE, &v);
        lim.max_texture_size = v;
        {
            /* diagnostics: a smaller limit than the driver's, to exercise
             * the path of a GPU with small textures */
            const char *mt = g_getenv ("WALPE_GL_MAX_TEXTURE");
            int want = mt && *mt ? atoi (mt) : 0;
            if (want >= 1024 && want < lim.max_texture_size)
                lim.max_texture_size = want;
        }
        if (gl_ver >= 30) {
            v = 0;
            glGetIntegerv (GL_MAX_RENDERBUFFER_SIZE, &v);
            lim.max_renderbuffer_size = v;
            v = 0;
            glGetIntegerv (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &v);
            lim.max_texture_units = v;
            v = 0;
            glGetIntegerv (GL_MAX_SAMPLES, &v);
            lim.max_samples = v;
        }
        /* asked only where the enum exists (an unknown enum would leave a
         * GL error for whoever checks next) */
        if (ext_listed (ext, "GL_EXT_texture_filter_anisotropic") ||
            ext_listed (ext, "GL_ARB_texture_filter_anisotropic") ||
            (epoxy_is_desktop_gl () && gl_ver >= 46)) {
            GLfloat a = 0.0f;
            glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &a);
            lim.max_anisotropy = a;
        }
        /* The API and version are what epoxy says (every check written
         * before this file asks epoxy): a version string the parser reads
         * differently is handed over in the usual form, the driver's own
         * text kept behind it for the driver names in it. */
        char *ver_fixed = NULL;
        {
            gboolean es = !epoxy_is_desktop_gl ();
            WalpeGlCaps probe;
            walpe_gl_caps_parse (&probe, NULL, NULL, version, NULL, NULL, NULL);
            if (probe.gl_ver != gl_ver || !probe.es != !es)
                ver_fixed = g_strdup_printf ("%s%d.%d (%s)", es ? "OpenGL ES " : "", gl_ver / 10,
                                             gl_ver % 10, version ? version : "");
            walpe_gl_caps_clear (&probe);
        }
        c = g_new0 (WalpeGlCaps, 1);
        walpe_gl_caps_parse (c, vendor, renderer, ver_fixed ? ver_fixed : version, glsl, ext, &lim);
        g_free (ver_fixed);
        g_free (ext);
        {
            /* EGL side: can a DMA-BUF become an EGLImage on this display? */
            EGLDisplay dpy = eglGetCurrentDisplay ();
            c->egl_image_dmabuf = dpy != EGL_NO_DISPLAY &&
                                  epoxy_has_egl_extension (dpy, "EGL_EXT_image_dma_buf_import");
        }
        caps_publish (c, TRUE);
    }
    g_mutex_unlock (&caps_lock);
    return c;
}

WalpeFmtChoice walpe_gl_format (WalpeFmtUse use)
{
    const WalpeGlCaps *c = walpe_gl_caps ();
    WalpeFmtChoice ch = walpe_gl_caps_format (c, use);
    if (c && ch.how != WALPE_FMT_NATIVE && (guint) use < WALPE_FMT_N &&
        g_atomic_int_compare_and_exchange (&fmt_logged[use], 0, 1)) {
        char *s = walpe_fmt_choice_describe (&ch, c);
        g_message ("gl: %s", s);
        g_free (s);
    }
    return ch;
}

void walpe_gl_caps_override (const char *vendor, const char *renderer, const char *version,
                             const char *glsl, const char *extensions)
{
    g_mutex_lock (&caps_lock);
    /* the previous structure may still be read by someone: it is left
     * alone (tests only) */
    g_atomic_pointer_set (&caps_ptr, NULL);
    caps_err = caps_scene_err = NULL;
    for (guint i = 0; i < WALPE_FMT_N; i++)
        g_atomic_int_set (&fmt_logged[i], 0);
    if (vendor || renderer || version || glsl || extensions) {
        WalpeGlCaps *c = g_new0 (WalpeGlCaps, 1);
        walpe_gl_caps_parse (c, vendor, renderer, version, glsl, extensions, NULL);
        caps_publish (c, FALSE);
    }
    g_mutex_unlock (&caps_lock);
}
