/* sceneshot - dev tool (not installed): render a wallpaper off screen
 * through the content API and write frame N as a PNG. No X server and no
 * daemon: an EGL pbuffer on the first EGL device (or Mesa's surfaceless
 * platform) is the "monitor".
 *
 *   sceneshot <wallpaper dir> <out.png> [--size WxH] [--frames N]
 *             [--render WxH] [--scale-filter bilinear|nearest] [--tex-limit PIXELS]
 *
 * --render: the render resolution chosen for the "monitor" (content.h
 * render_w / render_h) when it is not its own size; --scale-filter and
 * --tex-limit: the content's scale_filter and tex_limit. With --render a
 * second line is printed: the hash of the frame sampled at the centre of
 * every render pixel, when the surface is a whole multiple of the render
 * size. With --scale-filter nearest that line equals the first line of a
 * run at --size <the render size> without --render: a monitor set to a
 * smaller resolution shows what a monitor of that resolution shows.
 *
 * Prints the frame's FNV-1a hash (the pixels as read back), so two builds
 * can be compared: set WALPE_PROP_PROBE=<dir> for the virtual 30 fps scene
 * clock (propprobe.h) and the frames of a scene without random particles
 * are the same on every run.
 * The user's config (~/.config/walpe/config.json, i.e. the wallpaper's
 * saved options and layer adjustments) is loaded like the daemon does.
 *
 * WALPE_GL_API=gles in the environment (the daemon's variable): the frame
 * is drawn with an OpenGL ES 3.2 / 3.1 context instead of the desktop
 * OpenGL 3.3 one, so that the two paths can be compared on one driver.
 * WALPE_GL_DEBUG=1: a debug context; the driver's KHR_debug messages are
 * counted, the first ones of type "error" go to stderr, and a third line
 * "gl debug: N error(s), M other message(s), glGetError 0x..." is printed. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "engine/content.h"
#include "engine/glutil.h"
#include "common/config.h"
#include "common/glapi.h"
#include "common/util.h"

/* WALPE_GL_DEBUG: what the driver said on the KHR_debug channel */
static int dbg_errors, dbg_other;

static void GLAPIENTRY debug_cb (GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                 const GLchar *message, const void *user)
{
    (void) source;
    (void) id;
    (void) severity;
    (void) user;
    if (type != GL_DEBUG_TYPE_ERROR) {
        dbg_other++;
        return;
    }
    if (dbg_errors++ < 8)
        fprintf (stderr, "sceneshot: gl debug: %.*s\n", length < 0 ? 400 : MIN ((int) length, 400), message);
}

static EGLDisplay open_display (void)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLint maj, min;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && dpy == EGL_NO_DISPLAY; i++) {
                EGLDisplay d = eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
                if (d != EGL_NO_DISPLAY && eglInitialize (d, &maj, &min))
                    dpy = d;
            }
    }
    if (dpy == EGL_NO_DISPLAY &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        EGLDisplay d = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA,
                                                 EGL_DEFAULT_DISPLAY, NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize (d, &maj, &min))
            dpy = d;
    }
    return dpy;
}

int main (int argc, char **argv)
{
    const char *dir = NULL, *out = NULL;
    int w = 1920, h = 1080, frames = 3;
    int render_w = 0, render_h = 0, scale_filter = WALPE_SCALE_BILINEAR, tex_limit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "--size") && i + 1 < argc) {
            if (sscanf (argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 8192 || h > 8192) {
                fprintf (stderr, "--size WxH\n");
                return 1;
            }
        } else if (!strcmp (argv[i], "--frames") && i + 1 < argc) {
            int n = atoi (argv[++i]);   /* CLAMP evaluates its argument more than once */
            frames = CLAMP (n, 1, 100000);
        } else if (!strcmp (argv[i], "--render") && i + 1 < argc) {
            WalpeRes r;
            char *rerr = NULL;
            if (!walpe_res_parse (argv[++i], &r, &rerr) || r.kind != WALPE_RES_CUSTOM) {
                fprintf (stderr, "sceneshot: --render needs WIDTHxHEIGHT%s%s\n", rerr ? ": " : "",
                         rerr ? rerr : "");
                g_free (rerr);
                return 1;
            }
            render_w = r.w;
            render_h = r.h;
        } else if (!strcmp (argv[i], "--scale-filter") && i + 1 < argc) {
            scale_filter = walpe_scale_filter_parse (argv[++i]);
            if (scale_filter < 0) {
                fprintf (stderr, "sceneshot: --scale-filter bilinear or nearest\n");
                return 1;
            }
        } else if (!strcmp (argv[i], "--tex-limit") && i + 1 < argc) {
            int n = atoi (argv[++i]);
            tex_limit = CLAMP (n, 0, WALPE_TEXLIMIT_MAX);
        } else if (!dir) {
            dir = argv[i];
        } else if (!out) {
            out = argv[i];
        }
    }
    if (!dir || !out) {
        fprintf (stderr, "usage: sceneshot <wallpaper dir> <out.png> [--size WxH] [--frames N] "
                         "[--render WxH] [--scale-filter bilinear|nearest] [--tex-limit PIXELS]\n");
        return 1;
    }
    const gboolean es = walpe_gl_api_env_override (g_getenv ("WALPE_GL_API")) == WALPE_GL_API_GLES;
    const char *dbg_env = g_getenv ("WALPE_GL_DEBUG");
    const gboolean dbg = dbg_env && *dbg_env && *dbg_env != '0';
    EGLDisplay dpy = open_display ();
    EGLConfig cfg;
    EGLint nc = 0;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                            es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglBindAPI (es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        fprintf (stderr, "sceneshot: no EGL display with a %s pbuffer config\n", es ? "GLES 3" : "GL");
        return 77;
    }
    /* desktop: the context this tool always made; OpenGL ES: the versions
     * the daemon asks for, 3.2 then 3.1 */
    EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, es ? 2 : 3,
                    es ? EGL_NONE : EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                    EGL_NONE, EGL_NONE, EGL_NONE };
    if (dbg) {
        ca[es ? 4 : 6] = EGL_CONTEXT_OPENGL_DEBUG;
        ca[es ? 5 : 7] = EGL_TRUE;
    }
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT && es) {
        ca[3] = 1;
        ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    }
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        fprintf (stderr, "sceneshot: no %s pbuffer context\n", es ? "GLES 3.2 / 3.1" : "GL 3.3");
        return 77;
    }
    if (dbg) {
        const int v = epoxy_gl_version ();
        const gboolean core = es ? v >= 32 : v >= 43;
        if (es && !core && epoxy_has_gl_extension ("GL_KHR_debug")) {
            glDebugMessageCallbackKHR ((GLDEBUGPROCKHR) debug_cb, NULL);
            glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
            glEnable (GL_DEBUG_OUTPUT_KHR);
        } else if (core || epoxy_has_gl_extension ("GL_KHR_debug")) {
            glDebugMessageCallback ((GLDEBUGPROC) debug_cb, NULL);
            glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glEnable (GL_DEBUG_OUTPUT);
        } else {
            fprintf (stderr, "sceneshot: WALPE_GL_DEBUG: this context has no KHR_debug\n");
        }
    }
    walpe_glutil_init ();
    WalpeConfig *conf = walpe_config_load ();
    char *abs = walpe_expand_path (dir);
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = abs };
    GError *err = NULL;
    WalpeContent *c = walpe_content_new (abs, &opts, &err);
    /* the render resolution, as the daemon hands it over: before gl_init
     * and before every frame; nothing set = the surface's own size */
    const gboolean own_size = render_w > 0 && render_h > 0 && (render_w != w || render_h != h);
    if (c) {
        c->render_w = own_size ? render_w : 0;
        c->render_h = own_size ? render_h : 0;
        c->scale_filter = scale_filter;
        c->tex_limit = tex_limit;
    }
    if (!c || !c->gl_init (c, &err)) {
        fprintf (stderr, "sceneshot: %s\n", err ? err->message : "load failed");
        return 2;
    }
    for (int f = 0; f < frames; f++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, w, h);
        c->render_w = own_size ? render_w : 0;
        c->render_h = own_size ? render_h : 0;
        c->scale_filter = scale_filter;
        c->tex_limit = tex_limit;
        c->gl_render (c, w, h, 0.0f, 0.0f);
    }
    glFinish ();
    const GLenum gl_err = dbg ? glGetError () : GL_NO_ERROR;
    guint8 *px = g_malloc ((gsize) w * h * 4);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    guint64 hash = 1469598103934665603ULL;
    for (gsize i = 0; i < (gsize) w * h * 4; i++) {
        if ((i & 3) == 3)
            continue;   /* the pbuffer's alpha is not the picture */
        hash = (hash ^ px[i]) * 1099511628211ULL;
    }
    /* GL rows are bottom-up */
    guint8 *flip = g_malloc ((gsize) w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            memcpy (flip + ((gsize) y * w + x) * 3, px + ((gsize) (h - 1 - y) * w + x) * 4, 3);
    GdkPixbuf *pb = gdk_pixbuf_new_from_data (flip, GDK_COLORSPACE_RGB, FALSE, 8, w, h, w * 3, NULL,
                                              NULL);
    gboolean ok = gdk_pixbuf_save (pb, out, "png", &err, NULL);
    if (!ok)
        fprintf (stderr, "sceneshot: %s\n", err ? err->message : "png");
    printf ("%s frame %d %dx%d hash %016" G_GINT64_MODIFIER "x\n", out, frames, w, h, hash);
    if (own_size && w % render_w == 0 && h % render_h == 0 && w / render_w == h / render_h) {
        /* one sample per render pixel, at its centre, in the order of the
         * hash above: what a surface of the render size would hold */
        const int k = w / render_w;
        guint64 sh = 1469598103934665603ULL;
        for (int y = 0; y < render_h; y++)
            for (int x = 0; x < render_w; x++) {
                const guint8 *p = px + ((gsize) (y * k + k / 2) * w + (x * k + k / 2)) * 4;
                for (int ch = 0; ch < 3; ch++)
                    sh = (sh ^ p[ch]) * 1099511628211ULL;
            }
        printf ("render %dx%d sampled %016" G_GINT64_MODIFIER "x\n", render_w, render_h, sh);
    }
    if (dbg)
        printf ("gl debug: %d error(s), %d other message(s), glGetError 0x%04x\n", dbg_errors, dbg_other,
                (unsigned) gl_err);
    g_object_unref (pb);
    g_free (flip);
    g_free (px);
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
    g_free (abs);
    walpe_config_free (conf);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return ok ? 0 : 3;
}
