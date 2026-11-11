/* scenecpu - dev tool (not installed): what a scene costs the render thread
 * per frame, by phase (engine/cpuphase.h), without a monitor.
 *
 *   scenecpu <wallpaper dir> [--size WxH] [--warm N] [--frames M]
 *            [--raster] [--hash K] [--seed S]
 *
 * The scene is loaded and rendered as sceneshot does (EGL pbuffer or
 * Mesa's surfaceless platform; no X server, no daemon). N warm-up frames,
 * then M measured ones; one line of key=value pairs on stdout:
 *   frames, drawn (not skipped as unchanged), thread_ms (the whole
 *   gl_render call, thread CPU time per frame), one <phase>_ms per phase,
 *   pt_live (live particles per frame), pt_draws, pt_kb (vertex data per
 *   frame), text (text layers rasterised per frame),
 *   with WALPE_CPU_PHASES=2 also sim_hash / vtx_hash (every particle's
 *   state after each step / every byte of particle vertex data), and with
 *   --hash K the FNV-1a hash of the picture after K more frames that are
 *   rasterised for real (frame_hash).
 *
 * Unless --raster is given, warm-up and measured frames run with
 * GL_RASTERIZER_DISCARD: a software rasteriser needs seconds per frame
 * for a 4K scene and would bury the engine's own time; no decision of the
 * engine depends on a pixel. Vertex work still runs (on Mesa llvmpipe:
 * on this thread, inside the draw calls - phases draw and pt_gl).
 *
 * Every GRand the engine makes is seeded from --seed (default 20261110),
 * and the tool sets WALPE_CPU_PHASES=1 and WALPE_PROP_PROBE (virtual
 * 30 fps scene clock, fixed date for scripts, seeded Math.random) unless
 * they are set already: two runs of one build print the same counts and
 * hashes, so two builds can be compared. */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include "engine/content.h"
#include "engine/cpuphase.h"
#include "engine/glutil.h"
#include "common/config.h"
#include "common/util.h"

static guint32 seed_base = 20261110u;

/* the executable's definition wins over libglib's for the statically
 * linked engine: every particle system gets a known seed */
GRand *g_rand_new (void)
{
    static gint n;
    return g_rand_new_with_seed (seed_base + 7919u * (guint32) g_atomic_int_add (&n, 1));
}

static double thread_ms (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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

static const char *const phase_names[WALPE_CPH_N] = {
    "other", "script", "video", "world", "text", "pt_sim", "sig", "draw", "pt_build", "pt_gl",
    "post",
};

int main (int argc, char **argv)
{
    const char *dir = NULL;
    int w = 1920, h = 1080, warm = 60, frames = 120, hash_frames = 0;
    gboolean raster = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "--size") && i + 1 < argc) {
            if (sscanf (argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 8192 || h > 8192) {
                fprintf (stderr, "--size WxH\n");
                return 1;
            }
        } else if (!strcmp (argv[i], "--warm") && i + 1 < argc) {
            int n = atoi (argv[++i]);
            warm = CLAMP (n, 0, 100000);
        } else if (!strcmp (argv[i], "--frames") && i + 1 < argc) {
            int n = atoi (argv[++i]);
            frames = CLAMP (n, 1, 100000);
        } else if (!strcmp (argv[i], "--hash") && i + 1 < argc) {
            int n = atoi (argv[++i]);
            hash_frames = CLAMP (n, 1, 100000);
        } else if (!strcmp (argv[i], "--seed") && i + 1 < argc) {
            seed_base = (guint32) g_ascii_strtoull (argv[++i], NULL, 10);
        } else if (!strcmp (argv[i], "--raster")) {
            raster = TRUE;
        } else if (!dir) {
            dir = argv[i];
        }
    }
    if (!dir) {
        fprintf (stderr, "usage: scenecpu <wallpaper dir> [--size WxH] [--warm N] [--frames M] "
                         "[--raster] [--hash K] [--seed S]\n");
        return 1;
    }
    char *probe_dir = NULL;
    if (!g_getenv ("WALPE_PROP_PROBE")) {
        probe_dir = g_dir_make_tmp ("scenecpu-XXXXXX", NULL);
        if (probe_dir)
            g_setenv ("WALPE_PROP_PROBE", probe_dir, TRUE);
    }
    /* far away: the scene's state digest (propprobe.h) is not wanted here */
    g_setenv ("WALPE_PROP_PROBE_FRAME", "99999999", FALSE);
    g_setenv ("WALPE_CPU_PHASES", "1", FALSE);

    EGLDisplay dpy = open_display ();
    EGLConfig cfg;
    EGLint nc = 0;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        fprintf (stderr, "scenecpu: no EGL display with a GL pbuffer config\n");
        return 77;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        fprintf (stderr, "scenecpu: no GL 3.3 pbuffer context\n");
        return 77;
    }
    walpe_glutil_init ();
    WalpeConfig *conf = walpe_config_load ();
    char *abs = walpe_expand_path (dir);
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = abs };
    GError *err = NULL;
    WalpeContent *c = walpe_content_new (abs, &opts, &err);
    if (!c || !c->gl_init (c, &err)) {
        fprintf (stderr, "scenecpu: %s\n", err ? err->message : "load failed");
        return 2;
    }
    WalpeCphStats st;
    double t0 = 0, total = 0;
    int drawn = 0;
    if (!raster)
        glEnable (GL_RASTERIZER_DISCARD);
    for (int f = 0; f < warm + frames; f++) {
        if (f == warm) {
            walpe_cph_get (&st, TRUE);
            t0 = thread_ms ();
        }
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, w, h);
        c->gl_render (c, w, h, 0.0f, 0.0f);
        if (f >= warm && !c->frame_unchanged)
            drawn++;
    }
    total = thread_ms () - t0;
    walpe_cph_get (&st, TRUE);
    glDisable (GL_RASTERIZER_DISCARD);

    GString *o = g_string_new (NULL);
    const double n = (double) frames;
    g_string_append_printf (o, "frames=%d drawn=%d thread_ms=%.4f", frames, drawn, total / n);
    for (int i = 0; i < WALPE_CPH_N; i++)
        g_string_append_printf (o, " %s_ms=%.4f", phase_names[i], st.ms[i] / n);
    g_string_append_printf (o, " pt_live=%.1f pt_draws=%.2f pt_kb=%.1f text=%.3f",
                            (double) st.pt_live / n, (double) st.pt_draws / n,
                            (double) st.pt_bytes / n / 1024.0, (double) st.text_renders / n);
    if (walpe_cph_level >= 2)
        g_string_append_printf (o, " sim_hash=%016" G_GINT64_MODIFIER "x vtx_hash=%016"
                                G_GINT64_MODIFIER "x", st.pt_sim_hash, st.pt_vtx_hash);
    if (hash_frames > 0) {
        /* the picture, for real: K frames rasterised, the last one read back */
        for (int f = 0; f < hash_frames; f++) {
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            glViewport (0, 0, w, h);
            if (f == hash_frames - 1)
                c->surface_lost = TRUE;   /* drawn even if nothing changed */
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
        g_free (px);
        walpe_cph_get (&st, TRUE);
        g_string_append_printf (o, " frame_hash=%016" G_GINT64_MODIFIER "x", hash);
        if (walpe_cph_level >= 2)
            g_string_append_printf (o, " sim_hash2=%016" G_GINT64_MODIFIER "x vtx_hash2=%016"
                                    G_GINT64_MODIFIER "x", st.pt_sim_hash, st.pt_vtx_hash);
    }
    printf ("%s\n", o->str);
    g_string_free (o, TRUE);
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
    g_free (abs);
    walpe_config_free (conf);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    if (probe_dir) {
        /* the probe writes nothing before its digest frame; the directory is ours */
        g_rmdir (probe_dir);
        g_free (probe_dir);
    }
    return 0;
}
