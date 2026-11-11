/* sceneshot - dev tool (not installed): render a wallpaper off screen
 * through the content API and write frame N as a PNG. No X server and no
 * daemon: an EGL pbuffer on the first EGL device (or Mesa's surfaceless
 * platform) is the "monitor".
 *
 *   sceneshot <wallpaper dir> <out.png> [--size WxH] [--frames N]
 *
 * Prints the frame's FNV-1a hash (the pixels as read back), so two builds
 * can be compared: set WALPE_PROP_PROBE=<dir> for the virtual 30 fps scene
 * clock (propprobe.h) and the frames of a scene without random particles
 * are the same on every run.
 * The user's config (~/.config/walpe/config.json, i.e. the wallpaper's
 * saved options and layer adjustments) is loaded like the daemon does. */
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
#include "common/util.h"

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
    for (int i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "--size") && i + 1 < argc) {
            if (sscanf (argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 8192 || h > 8192) {
                fprintf (stderr, "--size WxH\n");
                return 1;
            }
        } else if (!strcmp (argv[i], "--frames") && i + 1 < argc) {
            int n = atoi (argv[++i]);   /* CLAMP evaluates its argument more than once */
            frames = CLAMP (n, 1, 100000);
        } else if (!dir) {
            dir = argv[i];
        } else if (!out) {
            out = argv[i];
        }
    }
    if (!dir || !out) {
        fprintf (stderr, "usage: sceneshot <wallpaper dir> <out.png> [--size WxH] [--frames N]\n");
        return 1;
    }
    EGLDisplay dpy = open_display ();
    EGLConfig cfg;
    EGLint nc = 0;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        fprintf (stderr, "sceneshot: no EGL display with a GL pbuffer config\n");
        return 77;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        fprintf (stderr, "sceneshot: no GL 3.3 pbuffer context\n");
        return 77;
    }
    walpe_glutil_init ();
    WalpeConfig *conf = walpe_config_load ();
    char *abs = walpe_expand_path (dir);
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = abs };
    GError *err = NULL;
    WalpeContent *c = walpe_content_new (abs, &opts, &err);
    if (!c || !c->gl_init (c, &err)) {
        fprintf (stderr, "sceneshot: %s\n", err ? err->message : "load failed");
        return 2;
    }
    for (int f = 0; f < frames; f++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, w, h);
        c->gl_render (c, w, h, 0.0f, 0.0f);
    }
    glFinish ();
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
