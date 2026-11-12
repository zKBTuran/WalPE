/* scene_clock_test - the scene clock around a pause.
 *
 * A paused scene's clock stands still (scenenode.c, scenenode_set_paused):
 * g_Time, SceneScript's engine.runtime, timers and property animations go
 * on from the instant of the pause when the scene resumes. The daemon still
 * draws single frames while a monitor is paused - the X server exposed a
 * part of the window, the first frame of a wallpaper loaded under a
 * window, a layer the user is adjusting. Such a frame used to be drawn at
 * the present time unless "Skip hidden areas" had set a clip: scripts saw
 * engine.runtime jump ahead by the length of the pause so far and fall
 * back at the resume (a clock that runs backwards), every setInterval
 * timer that fired in such a frame was next due that much after the
 * resume (a 50 ms timer silent for as long as the wallpaper had been
 * paused), and the picture of a "paused" wallpaper moved with every
 * exposed frame.
 *
 * The test renders a generated scene (two solid colour layers, one with a
 * script on its alpha) on Mesa's surfaceless platform with the calls the
 * daemon makes: frames, set_paused (TRUE), two forced frames 400 ms apart,
 * set_paused (FALSE), frames. The script reports what it sees on every
 * frame through console.log; the test reads the lines.
 *   - engine.runtime never decreases;
 *   - a frame drawn while paused has frametime 0 and the runtime of the
 *     other frame drawn while paused;
 *   - the two frames drawn while paused are the same picture;
 *   - the 50 ms interval timer fires again within the frames after the
 *     resume.
 * None of the checks depends on how fast this machine is: a slow run can
 * only hide a fault, not report one.
 * Skipped (exit 77) without EGL surfaceless / GL 3.3 or QuickJS. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/config.h"
#include "engine/content.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; \
                           if (!(c)) { \
                               fails++; \
                               printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                           } else { \
                               printf ("ok   "); \
                           } \
                           printf (__VA_ARGS__); \
                           printf ("\n"); } while (0)

/* what the script logged on a frame: "TICK <n> rt=<s> ft=<s> fired=<n>" */
typedef struct { int n; double rt, ft; int fired; } Tick;
static GArray *ticks;
static int script_errors;

static GLogWriterOutput log_writer (GLogLevelFlags level, const GLogField *fields, gsize n, gpointer u)
{
    (void) u;
    for (gsize i = 0; i < n; i++) {
        if (strcmp (fields[i].key, "MESSAGE"))
            continue;
        const char *m = fields[i].value, *t = m ? strstr (m, "TICK ") : NULL;
        Tick k;
        if (t && sscanf (t, "TICK %d rt=%lf ft=%lf fired=%d", &k.n, &k.rt, &k.ft, &k.fired) == 4) {
            g_array_append_val (ticks, k);
            return G_LOG_WRITER_HANDLED;
        }
        if (m && (strstr (m, "script error") || strstr (m, "timer error") || strstr (m, "out of time")))
            script_errors++;
    }
    return g_log_writer_default (level, fields, n, NULL);
}

static void put (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

/* the script: alpha follows the runtime (so the picture shows the clock),
 * an interval timer counts, every frame is reported */
static const char *SCRIPT =
    "'use strict';\\n"
    "let n = 0, fired = 0;\\n"
    "export function init(value) {\\n"
    "  engine.setInterval(function () { fired++; }, 50);\\n"
    "  return value;\\n"
    "}\\n"
    "export function update(value) {\\n"
    "  n++;\\n"
    "  console.log('TICK ' + n + ' rt=' + engine.runtime.toFixed(6) + ' ft=' + engine.frametime.toFixed(6) + ' fired=' + fired);\\n"
    "  return 0.15 + 0.8 * ((engine.runtime * 0.9) % 1);\\n"
    "}\\n";

static char *make_scene (const char *tmp)
{
    char *dir = g_build_filename (tmp, "scene", NULL);
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    char *sc = g_strdup_printf (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":["
        "{\"id\":1,\"name\":\"back\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\","
        "\"size\":\"320 180\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0.1 0.2 0.3\",\"alpha\":1,"
        "\"visible\":true},"
        "{\"id\":2,\"name\":\"probe\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\","
        "\"size\":\"200 100\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\","
        "\"alpha\":{\"script\":\"%s\",\"value\":1.0},\"visible\":true}]}", SCRIPT);
    put (dir, "scene.json", sc);
    g_free (sc);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"scene clock test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    return dir;
}

static void rm_rf (const char *path)
{
    GDir *d = g_dir_open (path, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name (d))) {
            char *p = g_build_filename (path, n, NULL);
            if (g_file_test (p, G_FILE_TEST_IS_DIR) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK))
                rm_rf (p);
            else
                g_unlink (p);
            g_free (p);
        }
        g_dir_close (d);
    }
    g_rmdir (path);
}

static void redraw_cb (gpointer u) { (void) u; }

#define W 320
#define H 180
#define NPX ((gsize) W * H * 4)

/* one frame as the daemon asks for it; forced = the window lost its
 * picture (an Expose): drawn whatever the scene thinks has changed */
static void frame (WalpeContent *c, gboolean forced, guint8 *px)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, W, H);
    c->next_frame_us = 0;
    c->surface_lost = forced;
    c->frame_unchanged = FALSE;
    c->gl_render (c, W, H, 0.0f, 0.0f);
    glFinish ();
    if (px) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
}

static void clock_checks (const char *tmp)
{
    char *dir = make_scene (tmp);
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c || (c->request_redraw = redraw_cb, !c->gl_init (c, &err))) {
        printf ("load failed: %s\n", err ? err->message : "?");
        CHECK (FALSE, "the scene loads");
        g_free (dir);
        return;
    }
    guint8 *p1 = g_malloc0 (NPX), *p2 = g_malloc0 (NPX);

    /* running */
    for (int i = 0; i < 6; i++) {
        frame (c, FALSE, NULL);
        g_usleep (30000);
    }
    guint running = ticks->len;
    if (running == 0) {
        printf ("skip (no script ran: built without QuickJS?)\n");
        goto out;
    }
    /* paused: two frames the window asks for, 400 ms apart */
    c->set_paused (c, TRUE);
    g_usleep (400000);
    frame (c, TRUE, p1);
    g_usleep (400000);
    frame (c, TRUE, p2);
    guint paused_end = ticks->len;
    /* running again */
    c->set_paused (c, FALSE);
    for (int i = 0; i < 8; i++) {
        frame (c, FALSE, NULL);
        g_usleep (30000);
    }
    CHECK (running == 6 && paused_end == 8 && ticks->len == 16,
           "the script ran on every frame (%u running, %u while paused, %u after the resume)", running,
           paused_end - running, ticks->len - paused_end);
    CHECK (script_errors == 0, "no script error, no limit hit (%d)", script_errors);
    if (ticks->len != 16)
        goto out;
    const Tick *t = (const Tick *) (void *) ticks->data;

    int back = 0;
    double worst = 0;
    for (guint i = 1; i < ticks->len; i++)
        if (t[i].rt < t[i - 1].rt) {
            back++;
            worst = MAX (worst, t[i - 1].rt - t[i].rt);
        }
    CHECK (back == 0, "engine.runtime never decreases (%d steps back, the largest %.3f s)", back, worst);
    CHECK (t[6].ft == 0.0 && t[7].ft == 0.0,
           "frames drawn while paused have frametime 0 (%.3f s, %.3f s)", t[6].ft, t[7].ft);
    CHECK (t[6].rt == t[7].rt,
           "two frames drawn while paused, 400 ms apart, see one runtime (%.3f s, %.3f s; %.3f s before the pause)",
           t[6].rt, t[7].rt, t[5].rt);
    CHECK (!memcmp (p1, p2, NPX), "... and are the same picture");
    CHECK (t[15].fired - t[7].fired >= 2,
           "the 50 ms timer goes on after the resume (%d times in the 8 frames after it, %d times before the pause)",
           t[15].fired - t[7].fired, t[5].fired);
    /* the clock went on from where it stopped: 8 frames of 30 ms and more */
    CHECK (t[15].rt - t[7].rt >= 0.2, "the clock runs after the resume (%.3f s in 8 frames)", t[15].rt - t[7].rt);

out:
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
    g_free (p1);
    g_free (p2);
    g_free (dir);
}

int main (void)
{
    setvbuf (stdout, NULL, _IONBF, 0);
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skip (no EGL surfaceless platform)\n");
        return 77;
    }
    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[128], ecfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, cfgs, 128, &nc) || nc < 1) {
        printf ("skip (no EGL pbuffer config with desktop GL)\n");
        return 77;
    }
    ecfg = cfgs[0];
    for (int i = 0; i < nc; i++) {
        EGLint r = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &r);
        if (r == 8) {
            ecfg = cfgs[i];
            break;
        }
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, ecfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, ecfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        printf ("skip (no GL 3.3 context on a pbuffer)\n");
        return 77;
    }
    char *tmp = g_dir_make_tmp ("walpe-scene-clock-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_PROP_PROBE");
    g_unsetenv ("WALPE_NO_SCRIPTS");
    g_unsetenv ("WALPE_FRAME_SKIP");
    ticks = g_array_new (FALSE, FALSE, sizeof (Tick));
    g_log_set_writer_func (log_writer, NULL, NULL);
    clock_checks (tmp);
    g_array_free (ticks, TRUE);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    rm_rf (tmp);
    g_free (tmp);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    /* no script ran (a build without QuickJS): nothing was checked */
    return fails ? 1 : checks ? 0 : 77;
}
