/* scenecount - dev tool (not installed): what a wallpaper's frames cost,
 * as counters, and what they look like, as hashes. Renders off screen like
 * sceneshot (EGL pbuffer on the first EGL device or Mesa's surfaceless
 * platform; no X server, no daemon), really rasterizing every frame.
 *
 *   scenecount <wallpaper dir> [--size WxH] [--warm N] [--frames M] [--tsv]
 *
 * N warm-up frames (default 8), then M measured frames (default 4). Prints
 *   - the FNV-1a hash of every measured frame and one hash over all of
 *     them: two builds, or one build with a switch on and off
 *     (WALPE_FX_REGION=0 ...), draw the same pictures when the lines are
 *     the same. With WALPE_PROP_PROBE=<dir> (virtual scene clock) and
 *     seeded particles the frames repeat from run to run;
 *   - per measured frame, averaged: fragments shaded (the driver's count
 *     of fragment shader invocations, GL_ARB_pipeline_statistics_query;
 *     in pixels and in screens), draw calls, clears and the pixels they
 *     cover, framebuffer blits / texture copies and their pixels, render
 *     target switches, program and texture binds, uniform uploads, scissor
 *     calls, and the sum of all GL calls counted (the calls a frame is
 *     made of: draws, clears, copies, binds, uniforms, state);
 *   - the effect passes drawn / served from the static cache, and how many
 *     pixels of their targets the passes shaded (fx_region_stats).
 * Counting is done here, by wrappers put into libepoxy's dispatch
 * pointers (as the GPU profiler does): the engine is not instrumented.
 * Nothing here is a time: on a software renderer times say nothing about
 * a GPU. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/scenenode_effects.h"
#include "common/config.h"
#include "common/util.h"

enum { C_DRAW, C_CLEAR, C_BLIT, C_COPY, C_FBO, C_PROG, C_TEX, C_UNI, C_SCISSOR, C_STATE, C_BUF,
       C_N };
static guint64 cnt[C_N], clear_px, blit_px, copy_px;
static int vp_w, vp_h, sc_x, sc_y, sc_w, sc_h;
static gboolean sc_on;

#define REAL(fn) static __typeof__ (epoxy_##fn) r_##fn
REAL (glDrawArrays); REAL (glDrawElements); REAL (glDrawArraysInstanced);
REAL (glDrawElementsInstanced); REAL (glClear); REAL (glBlitFramebuffer);
REAL (glCopyTexSubImage2D); REAL (glBindFramebuffer); REAL (glUseProgram); REAL (glBindTexture);
REAL (glActiveTexture); REAL (glUniform1f); REAL (glUniform2f); REAL (glUniform3f);
REAL (glUniform4f); REAL (glUniform1i); REAL (glUniform1fv); REAL (glUniform2fv);
REAL (glUniform3fv); REAL (glUniform4fv); REAL (glUniformMatrix3fv); REAL (glUniformMatrix4fv);
REAL (glBindVertexArray); REAL (glViewport); REAL (glScissor); REAL (glEnable); REAL (glDisable);
REAL (glBlendFunc); REAL (glBlendFuncSeparate); REAL (glBlendEquation); REAL (glBufferData);
REAL (glBufferSubData); REAL (glBindBuffer); REAL (glColorMask); REAL (glClearColor);
REAL (glInvalidateFramebuffer); REAL (glTexSubImage2D); REAL (glTexImage2D);

static void w_glDrawArrays (GLenum m, GLint f, GLsizei c) { cnt[C_DRAW]++; r_glDrawArrays (m, f, c); }
static void w_glDrawElements (GLenum m, GLsizei c, GLenum t, const void *i)
{ cnt[C_DRAW]++; r_glDrawElements (m, c, t, i); }
static void w_glDrawArraysInstanced (GLenum m, GLint f, GLsizei c, GLsizei n)
{ cnt[C_DRAW]++; r_glDrawArraysInstanced (m, f, c, n); }
static void w_glDrawElementsInstanced (GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n)
{ cnt[C_DRAW]++; r_glDrawElementsInstanced (m, c, t, i, n); }
static void w_glClear (GLbitfield mask)
{
    cnt[C_CLEAR]++;
    /* what a clear covers: the scissor box when the test is on, else the
     * viewport (the engine sets the viewport to the target it draws) */
    clear_px += sc_on ? (guint64) MAX (0, sc_w) * MAX (0, sc_h) : (guint64) vp_w * vp_h;
    r_glClear (mask);
}
static void w_glBlitFramebuffer (GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g,
                                 GLint h, GLbitfield m, GLenum fl)
{
    cnt[C_BLIT]++;
    blit_px += (guint64) abs (g - e) * (guint64) abs (h - f);
    r_glBlitFramebuffer (a, b, c, d, e, f, g, h, m, fl);
}
static void w_glCopyTexSubImage2D (GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y,
                                   GLsizei w, GLsizei h)
{
    cnt[C_COPY]++;
    copy_px += (guint64) w * h;
    r_glCopyTexSubImage2D (t, l, xo, yo, x, y, w, h);
}
static void w_glBindFramebuffer (GLenum t, GLuint f) { cnt[C_FBO]++; r_glBindFramebuffer (t, f); }
static void w_glUseProgram (GLuint p) { cnt[C_PROG]++; r_glUseProgram (p); }
static void w_glBindTexture (GLenum t, GLuint x) { cnt[C_TEX]++; r_glBindTexture (t, x); }
static void w_glActiveTexture (GLenum t) { cnt[C_STATE]++; r_glActiveTexture (t); }
static void w_glUniform1f (GLint l, GLfloat a) { cnt[C_UNI]++; r_glUniform1f (l, a); }
static void w_glUniform2f (GLint l, GLfloat a, GLfloat b) { cnt[C_UNI]++; r_glUniform2f (l, a, b); }
static void w_glUniform3f (GLint l, GLfloat a, GLfloat b, GLfloat c)
{ cnt[C_UNI]++; r_glUniform3f (l, a, b, c); }
static void w_glUniform4f (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{ cnt[C_UNI]++; r_glUniform4f (l, a, b, c, d); }
static void w_glUniform1i (GLint l, GLint a) { cnt[C_UNI]++; r_glUniform1i (l, a); }
static void w_glUniform1fv (GLint l, GLsizei n, const GLfloat *v) { cnt[C_UNI]++; r_glUniform1fv (l, n, v); }
static void w_glUniform2fv (GLint l, GLsizei n, const GLfloat *v) { cnt[C_UNI]++; r_glUniform2fv (l, n, v); }
static void w_glUniform3fv (GLint l, GLsizei n, const GLfloat *v) { cnt[C_UNI]++; r_glUniform3fv (l, n, v); }
static void w_glUniform4fv (GLint l, GLsizei n, const GLfloat *v) { cnt[C_UNI]++; r_glUniform4fv (l, n, v); }
static void w_glUniformMatrix3fv (GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{ cnt[C_UNI]++; r_glUniformMatrix3fv (l, n, t, v); }
static void w_glUniformMatrix4fv (GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{ cnt[C_UNI]++; r_glUniformMatrix4fv (l, n, t, v); }
static void w_glBindVertexArray (GLuint a) { cnt[C_STATE]++; r_glBindVertexArray (a); }
static void w_glViewport (GLint x, GLint y, GLsizei w, GLsizei h)
{ cnt[C_STATE]++; vp_w = w; vp_h = h; r_glViewport (x, y, w, h); }
static void w_glScissor (GLint x, GLint y, GLsizei w, GLsizei h)
{ cnt[C_SCISSOR]++; sc_x = x; sc_y = y; sc_w = w; sc_h = h; r_glScissor (x, y, w, h); }
static void w_glEnable (GLenum c) { cnt[C_STATE]++; if (c == GL_SCISSOR_TEST) sc_on = TRUE; r_glEnable (c); }
static void w_glDisable (GLenum c) { cnt[C_STATE]++; if (c == GL_SCISSOR_TEST) sc_on = FALSE; r_glDisable (c); }
static void w_glBlendFunc (GLenum s, GLenum d) { cnt[C_STATE]++; r_glBlendFunc (s, d); }
static void w_glBlendFuncSeparate (GLenum a, GLenum b, GLenum c, GLenum d)
{ cnt[C_STATE]++; r_glBlendFuncSeparate (a, b, c, d); }
static void w_glBlendEquation (GLenum m) { cnt[C_STATE]++; r_glBlendEquation (m); }
static void w_glBufferData (GLenum t, GLsizeiptr n, const void *d, GLenum u)
{ cnt[C_BUF]++; r_glBufferData (t, n, d, u); }
static void w_glBufferSubData (GLenum t, GLintptr o, GLsizeiptr n, const void *d)
{ cnt[C_BUF]++; r_glBufferSubData (t, o, n, d); }
static void w_glBindBuffer (GLenum t, GLuint b) { cnt[C_STATE]++; r_glBindBuffer (t, b); }
static void w_glColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ cnt[C_STATE]++; r_glColorMask (r, g, b, a); }
static void w_glClearColor (GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ cnt[C_STATE]++; r_glClearColor (r, g, b, a); }
static void w_glInvalidateFramebuffer (GLenum t, GLsizei n, const GLenum *a)
{ cnt[C_STATE]++; r_glInvalidateFramebuffer (t, n, a); }
static void w_glTexSubImage2D (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
                               GLenum ty, const void *p)
{ cnt[C_BUF]++; r_glTexSubImage2D (t, l, x, y, w, h, f, ty, p); }
static void w_glTexImage2D (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f,
                            GLenum ty, const void *p)
{ cnt[C_BUF]++; r_glTexImage2D (t, l, i, w, h, b, f, ty, p); }

#define HOOK(fn) do { \
        void *real_ = (void *) eglGetProcAddress (#fn); \
        if (!real_) { fprintf (stderr, "scenecount: no address for %s\n", #fn); return FALSE; } \
        memcpy (&r_##fn, &real_, sizeof (real_)); \
        epoxy_##fn = w_##fn; \
    } while (0)

static gboolean hooks (void)
{
    HOOK (glDrawArrays); HOOK (glDrawElements); HOOK (glDrawArraysInstanced);
    HOOK (glDrawElementsInstanced); HOOK (glClear); HOOK (glBlitFramebuffer);
    HOOK (glCopyTexSubImage2D); HOOK (glBindFramebuffer); HOOK (glUseProgram);
    HOOK (glBindTexture); HOOK (glActiveTexture); HOOK (glUniform1f); HOOK (glUniform2f);
    HOOK (glUniform3f); HOOK (glUniform4f); HOOK (glUniform1i); HOOK (glUniform1fv);
    HOOK (glUniform2fv); HOOK (glUniform3fv); HOOK (glUniform4fv); HOOK (glUniformMatrix3fv);
    HOOK (glUniformMatrix4fv); HOOK (glBindVertexArray); HOOK (glViewport); HOOK (glScissor);
    HOOK (glEnable); HOOK (glDisable); HOOK (glBlendFunc); HOOK (glBlendFuncSeparate);
    HOOK (glBlendEquation); HOOK (glBufferData); HOOK (glBufferSubData); HOOK (glBindBuffer);
    HOOK (glColorMask); HOOK (glClearColor); HOOK (glInvalidateFramebuffer);
    HOOK (glTexSubImage2D); HOOK (glTexImage2D);
    return TRUE;
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
    const char *dir = NULL;
    int w = 1920, h = 1080, warm = 8, frames = 4;
    gboolean tsv = FALSE;
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
        } else if (!strcmp (argv[i], "--tsv")) {
            tsv = TRUE;
        } else if (!dir) {
            dir = argv[i];
        }
    }
    if (!dir) {
        fprintf (stderr, "usage: scenecount <wallpaper dir> [--size WxH] [--warm N] [--frames M] [--tsv]\n");
        return 1;
    }
    EGLDisplay dpy = open_display ();
    EGLConfig cfg;
    EGLint nc = 0;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        fprintf (stderr, "scenecount: no EGL display with a GL pbuffer config\n");
        return 77;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        fprintf (stderr, "scenecount: no GL 3.3 pbuffer context\n");
        return 77;
    }
    walpe_glutil_init ();
    WalpeConfig *conf = walpe_config_load ();
    char *abs = walpe_expand_path (dir);
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = abs };
    GError *err = NULL;
    WalpeContent *c = walpe_content_new (abs, &opts, &err);
    if (!c || !c->gl_init (c, &err)) {
        fprintf (stderr, "scenecount: %s\n", err ? err->message : "load failed");
        return 2;
    }
    for (int f = 0; f < warm; f++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, w, h);
        c->gl_render (c, w, h, 0.0f, 0.0f);
    }
    glFinish ();
    const gboolean stats = epoxy_has_gl_extension ("GL_ARB_pipeline_statistics_query");
    GLuint q = 0;
    if (stats)
        glGenQueries (1, &q);
    if (!hooks ())
        return 3;
    guint8 *px = g_malloc ((gsize) w * h * 4);
    guint64 all = 1469598103934665603ULL, frag = 0;
    guint run0 = 0, cached0 = 0, run1 = 0, cached1 = 0;
    guint64 full0 = 0, drawn0 = 0, full1 = 0, drawn1 = 0;
    guint rp0 = 0, rr0 = 0, rp1 = 0, rr1 = 0;
    fx_pass_stats (&run0, &cached0, FALSE);
    fx_region_stats (NULL, NULL, NULL, NULL, TRUE);
    memset (cnt, 0, sizeof (cnt));
    clear_px = blit_px = copy_px = 0;
    vp_w = w;
    vp_h = h;
    int drawn_frames = 0;
    for (int f = 0; f < frames; f++) {
        r_glBindFramebuffer (GL_FRAMEBUFFER, 0);
        r_glViewport (0, 0, w, h);
        vp_w = w, vp_h = h;
        guint64 d0 = cnt[C_DRAW];
        if (stats)
            glBeginQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB, q);
        c->frame_unchanged = FALSE;
        c->gl_render (c, w, h, 0.0f, 0.0f);
        if (stats) {
            guint64 v = 0;
            glEndQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB);
            glGetQueryObjectui64v (q, GL_QUERY_RESULT, &v);
            frag += v;
        }
        if (cnt[C_DRAW] != d0 || !c->frame_unchanged)
            drawn_frames++;
        glFinish ();
        r_glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        guint64 hash = 1469598103934665603ULL;
        for (gsize i = 0; i < (gsize) w * h * 4; i++) {
            if ((i & 3) == 3)
                continue;   /* the pbuffer's alpha is not the picture */
            hash = (hash ^ px[i]) * 1099511628211ULL;
            all = (all ^ px[i]) * 1099511628211ULL;
        }
        printf ("frame %d hash %016" G_GINT64_MODIFIER "x\n", warm + f + 1, hash);
    }
    fx_pass_stats (&run1, &cached1, FALSE);
    fx_region_stats (&full1, &drawn1, &rp1, &rr1, FALSE);
    double asked = 0, floor_px = 0;
    fx_region_potential (&asked, &floor_px);
    guint64 calls = 0;
    for (int i = 0; i < C_N; i++)
        calls += cnt[i];
    const double n = frames, scr = (double) w * h;
    {
        guint ck_runs = 0, ck_bad = 0;
        fx_region_check_stats (&ck_runs, &ck_bad);
        if (g_getenv ("WALPE_FX_REGION_CHECK"))
            printf ("regioncheck runs %u differed %u\n", ck_runs, ck_bad);
    }
    printf ("frames %d (%d drew) %dx%d allhash %016" G_GINT64_MODIFIER "x\n", frames, drawn_frames,
            w, h, all);
    if (tsv) {
        printf ("TSV\tshaded_px\tshaded_x\tdraws\tclears\tclear_x\tblits\tblit_x\tcopies\tcopy_x\t"
                "fbo_binds\tprograms\ttex_binds\tuniforms\tscissors\tgl_calls\tfx_passes\tfx_cached\t"
                "fx_target_x\tfx_shaded_x\tfx_restricted\tfx_rects\tfx_asked_x\tfx_floor_x\n");
        printf ("TSV\t%.0f\t%.3f\t%.1f\t%.1f\t%.3f\t%.1f\t%.3f\t%.1f\t%.3f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t"
                "%.1f\t%.1f\t%.1f\t%.3f\t%.3f\t%.1f\t%.1f\t%.3f\t%.3f\n",
                frag / n, frag / n / scr, cnt[C_DRAW] / n, cnt[C_CLEAR] / n, clear_px / n / scr,
                cnt[C_BLIT] / n, blit_px / n / scr, cnt[C_COPY] / n, copy_px / n / scr,
                cnt[C_FBO] / n, cnt[C_PROG] / n, cnt[C_TEX] / n, cnt[C_UNI] / n, cnt[C_SCISSOR] / n,
                calls / n, (run1 - run0) / n, (cached1 - cached0) / n, (full1 - full0) / n / scr,
                (drawn1 - drawn0) / n / scr, (rp1 - rp0) / n, (rr1 - rr0) / n, asked / n / scr,
                floor_px / n / scr);
    } else {
        printf ("per frame: shaded %.0f px = %.3f screens%s; draws %.1f; clears %.1f = %.3f screens; "
                "blits %.1f = %.3f screens; texture copies %.1f = %.3f screens\n",
                frag / n, frag / n / scr, stats ? "" : " (no pipeline statistics here)",
                cnt[C_DRAW] / n, cnt[C_CLEAR] / n, clear_px / n / scr, cnt[C_BLIT] / n,
                blit_px / n / scr, cnt[C_COPY] / n, copy_px / n / scr);
        printf ("per frame: render target binds %.1f; programs %.1f; texture binds %.1f; uniforms %.1f; "
                "scissor %.1f; GL calls counted %.1f\n",
                cnt[C_FBO] / n, cnt[C_PROG] / n, cnt[C_TEX] / n, cnt[C_UNI] / n, cnt[C_SCISSOR] / n,
                calls / n);
        printf ("per frame: effect passes drawn %.1f, from the cache %.1f; their targets %.3f screens, "
                "shaded %.3f screens (%.1f passes restricted, %.1f rectangles)\n",
                (run1 - run0) / n, (cached1 - cached0) / n, (full1 - full0) / n / scr,
                (drawn1 - drawn0) / n / scr, (rp1 - rp0) / n, (rr1 - rr0) / n);
    }
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
    return 0;
}
