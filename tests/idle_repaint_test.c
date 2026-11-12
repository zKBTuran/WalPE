/* idle_repaint_test.c - an idle scene must repaint when something outside
 * its layers changes the picture (integration gate).
 *
 * With frame skipping and the static bake a scene in which nothing moves
 * draws its first frame and then nothing (0 fps). Everything that changes
 * what the scene draws while it runs has to show in the frame signature
 * (scenenode.c: fsig, node_signature, content->picture_serial), or the
 * old picture stays on screen until something else changes - and layers
 * in the bake keep it for good.
 *
 * The test renders a generated scene of solid colour layers (no texture
 * files, no fonts) on Mesa's surfaceless platform, lets it go idle and then
 * makes each live change that exists in this tree. Every one must
 *   - draw a frame on the very next tick (one frame interval),
 *   - change the picture (or, for an undo, give the earlier picture back
 *     byte for byte),
 *   - and leave the scene idle again on the tick after.
 * Changes covered: "Adjust layers" move / scale / hide / show / highlight
 * on and off / reset; render_scale down and back up. Changes that reload
 * the wallpaper (option changes, texture_slack, tex_downscale) need
 * nothing: a load draws.
 *
 * And the other way round (parked_checks): a layer that puts no pixel on
 * the frame - parked outside the view - draws nothing, so an effect on it
 * that follows the time must not keep the frames coming, nor keep the still
 * layers around it out of the static bake. Counted: frames drawn, and draw
 * calls per frame.
 * Skipped (exit 77) without EGL surfaceless / GL 3.3.
 * `meson test -C build idle-repaint` */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/config.h"
#include "common/layeradj.h"
#include "common/quality.h"
#include "engine/scenenode_effects.h"
#include "engine/content.h"
#include "engine/weparticles.h"

static int checks, fails;

static void result (gboolean ok, const char *what)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s\n", ok ? "ok  " : "FAIL", what);
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

static void layer (GString *s, int id, const char *origin, const char *size, const char *color,
                   double alpha)
{
    g_string_append_printf (s,
        "%s{\"id\":%d,\"name\":\"l%d\",\"image\":\"models/solid.json\",\"origin\":\"%s\","
        "\"size\":\"%s\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"%s\",\"alpha\":%.3f,"
        "\"visible\":true}", id > 1 ? "," : "", id, id, origin, size, color, alpha);
}

/* five still layers: nothing in this scene moves by itself */
static char *make_scene (const char *tmp)
{
    char *dir = g_build_filename (tmp, "scene", NULL);
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0.10 0.20 0.30\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[");
    layer (s, 1, "160 90 0", "320 180", "0.2 0.5 0.3", 1.0);
    layer (s, 2, "100 60 0", "120 80", "0.9 0.2 0.1", 1.0);
    layer (s, 3, "200 110 0", "140 60", "0.1 0.3 0.9", 0.5);
    layer (s, 4, "150 130 0", "90 50", "1.0 0.9 0.2", 0.8);
    layer (s, 5, "60 150 0", "70 30", "0.3 0.9 0.9", 0.7);
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"idle repaint test\",\"type\":\"scene\","
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

static WalpeContent *C;
static guint8 *screen;   /* what the window shows */

/* one tick as the daemon does it: a drawn frame goes to the screen and
 * leaves an undefined back buffer, an unchanged one leaves everything
 * alone. TRUE = a frame was drawn. */
static gboolean tick (void)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, W, H);
    C->next_frame_us = 0;
    C->surface_lost = FALSE;
    C->frame_unchanged = FALSE;
    C->gl_render (C, W, H, 0.0f, 0.0f);
    glFinish ();
    if (C->frame_unchanged)
        return FALSE;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, screen);
    glDisable (GL_SCISSOR_TEST);
    glClearColor (1.0f, 0.0f, 1.0f, 1.0f);   /* "after the swap": garbage */
    glClear (GL_COLOR_BUFFER_BIT);
    return TRUE;
}

/* after a change: the next tick draws, the picture is `want` (or differs
 * from `before` when want is NULL), the tick after that is idle again */
static void changed (const char *what, const guint8 *before, const guint8 *want)
{
    gboolean drew = tick ();
    char *m = g_strdup_printf ("%s: a frame is drawn on the next tick", what);
    result (drew, m);
    g_free (m);
    if (want) {
        m = g_strdup_printf ("%s: the earlier picture is back, byte for byte", what);
        result (drew && !memcmp (screen, want, NPX), m);
    } else {
        m = g_strdup_printf ("%s: the picture changed", what);
        result (drew && memcmp (screen, before, NPX) != 0, m);
    }
    g_free (m);
    gboolean again = tick ();
    m = g_strdup_printf ("%s: idle again on the tick after", what);
    result (!again, m);
    g_free (m);
}

static void adj (WalpeConfig *cfg, const char *dir, const char *key, double dx, double dy,
                 double scale, int vis)
{
    WalpeLayerAdj a = { .key = (char *) key, .dx = dx, .dy = dy, .scale = scale, .vis = vis };
    const char *why = NULL;
    if (!walpe_layers_put (cfg, dir, &a, NULL, &why))
        result (FALSE, why ? why : "walpe_layers_put");
}

static void idle_checks (const char *tmp)
{
    WalpeConfig *cfg = walpe_config_load ();
    char *dir = make_scene (tmp);
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    C = walpe_content_new (dir, &opts, &err);
    if (!C || (C->request_redraw = redraw_cb, !C->gl_init (C, &err))) {
        printf ("load failed: %s\n", err ? err->message : "?");
        result (FALSE, "the scene loads");
        return;
    }
    screen = g_malloc0 (NPX);
    guint8 *first = g_malloc (NPX), *prev = g_malloc (NPX), *keep = g_malloc (NPX);

    result (tick (), "the first frame is drawn");
    memcpy (first, screen, NPX);
    gboolean a = tick (), b = tick ();
    result (!a && !b, "nothing moves: the next two ticks draw nothing (the scene is idle)");

    /* ---- "Adjust layers" */
    memcpy (prev, screen, NPX);
    adj (cfg, dir, "2:l2", 40, 20, 1, WALPE_LAYER_VIS_DEFAULT);
    changed ("layer moved", prev, NULL);

    memcpy (prev, screen, NPX);
    adj (cfg, dir, "2:l2", 40, 20, 1.5, WALPE_LAYER_VIS_DEFAULT);
    changed ("layer scaled", prev, NULL);
    memcpy (keep, screen, NPX);              /* moved + scaled, visible */

    memcpy (prev, screen, NPX);
    adj (cfg, dir, "2:l2", 40, 20, 1.5, WALPE_LAYER_VIS_HIDDEN);
    changed ("layer hidden", prev, NULL);

    adj (cfg, dir, "2:l2", 40, 20, 1.5, WALPE_LAYER_VIS_DEFAULT);
    changed ("layer shown again", NULL, keep);

    memcpy (prev, screen, NPX);
    const char *hk[] = { "4:l4" };
    walpe_layers_set_highlight (dir, hk, 1);
    changed ("highlight on", prev, NULL);

    walpe_layers_set_highlight (dir, NULL, 0);
    changed ("highlight off", NULL, keep);

    result (walpe_layers_remove (cfg, dir, NULL) == 1, "reset removes the adjustment");
    changed ("adjustments reset", NULL, first);

    /* ---- effect pass programs (fx_specialize.h): with fx_specialize or
     * fx_quality=fast a pass changes programs while the scene runs (after
     * the comparison with the plain program; back when a script writes a
     * baked constant), and fx_picture_serial () moves with it. The setters
     * move it too. This scene has no effects: one frame is drawn, the
     * picture is the same. */
    {
        guint s0 = fx_picture_serial ();
        fx_set_specialize (TRUE);
        result (fx_picture_serial () != s0, "fx_specialize on: fx_picture_serial moves");
        changed ("fx_specialize on", NULL, first);
        fx_set_specialize (FALSE);
        changed ("fx_specialize off", NULL, first);
        fx_set_shader_quality ("fast");
        changed ("fx_quality fast", NULL, first);
        fx_set_shader_quality ("exact");
        changed ("fx_quality exact", NULL, first);
        s0 = fx_picture_serial ();
        fx_set_shader_quality ("exact");
        result (fx_picture_serial () == s0 && !tick (),
                "a setter with the value it has moves nothing: the scene stays idle");
    }

    /* ---- render scale (a setting applied to the running scene) */
    if (C->set_render_scale) {
        memcpy (prev, screen, NPX);
        C->set_render_scale (C, 50);
        changed ("render_scale 50", prev, NULL);
        C->set_render_scale (C, 100);
        changed ("render_scale back to 100", NULL, first);
    } else {
        printf ("note: this tree has no render_scale\n");
    }

    /* ---- quality knobs (applied to the running scene: effect update rate
     * and its mode, effect resolution, post-processing; a preset arrives as
     * those). This scene has no effects: one frame is drawn, the picture
     * is the same. The same values again move nothing. */
    if (C->set_quality) {
        WalpeQuality q;
        walpe_quality_preset (WALPE_PRESET_HIGH, &q);
        C->set_quality (C, &q);
        tick ();
        q.fx_rate = 2;
        C->set_quality (C, &q);
        changed ("quality: effects every 2nd frame", NULL, first);
        q.fx_rate_all = 1;
        C->set_quality (C, &q);
        changed ("quality: all effects held", NULL, first);
        q.fx_res = WALPE_FXRES_SCREEN;
        C->set_quality (C, &q);
        changed ("quality: effect resolution screen", NULL, first);
        q.post = WALPE_POST_OFF;
        C->set_quality (C, &q);
        changed ("quality: post-processing off", NULL, first);
        C->set_quality (C, &q);
        result (!tick (), "quality: the same values again: the scene stays idle");
        walpe_quality_preset (WALPE_PRESET_HIGH, &q);
        C->set_quality (C, &q);
        changed ("quality: back to high", NULL, first);
    } else {
        printf ("note: this tree has no quality knobs\n");
    }

    /* ---- particle settings (applied to running scenes: what particle
     * objects draw changes although nothing in the scene did). This scene
     * has no particles: one frame is drawn, the picture is the same. */
    {
        int lowres = 0, density = 100;
        gboolean inst = TRUE;
        walpe_weparticles_settings_get (&lowres, &density, &inst);
        walpe_weparticles_set_density (density == 60 ? 50 : 60);
        changed ("particle_density changed", NULL, first);
        walpe_weparticles_set_density (density);
        changed ("particle_density back", NULL, first);
        walpe_weparticles_set_lowres (lowres == 2 ? 0 : 2);
        changed ("particle_lowres changed", NULL, first);
        walpe_weparticles_set_lowres (lowres);
        changed ("particle_lowres back", NULL, first);
        walpe_weparticles_set_instanced (!inst);
        changed ("particle_instancing changed", NULL, first);
        walpe_weparticles_set_instanced (inst);
        changed ("particle_instancing back", NULL, first);
    }

    if (C->gl_free)
        C->gl_free (C);
    C->free (C);
    C = NULL;
    walpe_config_free (cfg);
    g_free (first);
    g_free (prev);
    g_free (keep);
    g_free (screen);
    g_free (dir);
}

/* ---- a layer parked outside the view ---------------------------------- */

static guint n_draw_calls;
static __typeof__ (epoxy_glDrawArrays) real_draw_arrays;
static void count_draw_arrays (GLenum mode, GLint first, GLsizei count)
{
    n_draw_calls++;
    real_draw_arrays (mode, first, count);
}

/* an effect that follows the time: the layer's colour pulses */
static void pulse_effect (const char *dir)
{
    put (dir, "shaders/effects/pulse.vert",
         "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n"
         "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
         "void main() {\n    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
         "\tv_TexCoord = a_TexCoord;\n}\n");
    put (dir, "shaders/effects/pulse.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
         "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
         "\tc.rgb *= 0.6 + 0.4 * sin(g_Time * 40.0);\n\tgl_FragColor = c;\n}\n");
    put (dir, "materials/effects/pulse.json",
         "{\"passes\":[{\"shader\":\"effects/pulse\",\"blending\":\"normal\",\"depthtest\":\"disabled\","
         "\"depthwrite\":\"disabled\",\"cullmode\":\"nocull\"}]}");
    put (dir, "effects/pulse/effect.json",
         "{\"name\":\"pulse\",\"group\":\"t\",\"passes\":[{\"material\":\"materials/effects/pulse.json\"}]}");
}

static void fx_layer (GString *s, int id, const char *origin, const char *size, const char *color)
{
    g_string_append_printf (s,
        ",{\"id\":%d,\"name\":\"l%d\",\"image\":\"models/solid.json\",\"origin\":\"%s\","
        "\"size\":\"%s\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"%s\",\"alpha\":1.0,"
        "\"visible\":true,\"effects\":[{\"file\":\"effects/pulse/effect.json\",\"id\":%d,"
        "\"name\":\"pulse\",\"visible\":true,\"passes\":[{}]}]}", id, id, origin, size, color, 100 + id);
}

/* a background and two still layers, a pulsing layer far to the right of
 * the 320 x 180 view, two more still layers; top = also a pulsing layer
 * in the middle of the view, on top of them all */
static char *make_parked_scene (const char *tmp, const char *name, gboolean top)
{
    char *dir = g_build_filename (tmp, name, NULL);
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    pulse_effect (dir);
    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0.10 0.20 0.30\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[");
    layer (s, 1, "160 90 0", "320 180", "0.2 0.5 0.3", 1.0);
    layer (s, 2, "100 60 0", "120 80", "0.9 0.2 0.1", 1.0);
    layer (s, 3, "200 110 0", "140 60", "0.1 0.3 0.9", 0.5);
    fx_layer (s, 6, "700 90 0", "60 60", "0.9 0.9 0.9");
    layer (s, 4, "150 130 0", "90 50", "1.0 0.9 0.2", 0.8);
    layer (s, 5, "60 150 0", "70 30", "0.3 0.9 0.9", 0.7);
    if (top)
        fx_layer (s, 7, "160 90 0", "40 40", "0.8 0.8 0.8");
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"parked layer test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    return dir;
}

static gboolean load_scene (const char *dir)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    C = walpe_content_new (dir, &opts, &err);
    if (!C || (C->request_redraw = redraw_cb, !C->gl_init (C, &err))) {
        printf ("load failed: %s\n", err ? err->message : "?");
        return FALSE;
    }
    return TRUE;
}

static void unload_scene (void)
{
    if (C->gl_free)
        C->gl_free (C);
    C->free (C);
    C = NULL;
}

static void parked_checks (const char *tmp)
{
    WalpeConfig *cfg = walpe_config_load ();
    screen = g_malloc0 (NPX);
    guint8 *idle = g_malloc (NPX);

    /* 1. nothing but the parked layer moves: the scene goes idle */
    char *dir = make_parked_scene (tmp, "parked", FALSE);
    if (!load_scene (dir)) {
        result (FALSE, "the scene with a parked layer loads");
        return;
    }
    result (tick (), "parked layer: the first frame is drawn");
    for (int i = 0; i < 4; i++)   /* (the frames in which the static layers settle) */
        tick ();
    memcpy (idle, screen, NPX);
    int drawn = 0;
    for (int i = 0; i < 8; i++)
        drawn += tick ();
    char *m = g_strdup_printf ("parked layer: its effect follows the time, no pixel of it is on the "
                               "frame: 8 ticks draw nothing (%d drawn)", drawn);
    result (drawn == 0, m);
    g_free (m);

    /* 2. moved into the view it pulses: frames come, with another picture */
    adj (cfg, dir, "6:l6", -540, 0, 1, WALPE_LAYER_VIS_DEFAULT);
    result (tick () && memcmp (screen, idle, NPX) != 0,
            "parked layer moved into the view: a frame is drawn and shows it");
    drawn = 0;
    for (int i = 0; i < 6; i++)
        drawn += tick ();
    result (drawn == 6, "in the view: every tick draws (the effect follows the time)");

    /* 3. and out again: one frame without it, the picture from before, idle */
    walpe_layers_remove (cfg, dir, NULL);
    gboolean back = tick ();
    result (back && !memcmp (screen, idle, NPX),
            "moved out again: one frame, the earlier picture byte for byte");
    for (int i = 0; i < 4; i++)
        tick ();
    drawn = 0;
    for (int i = 0; i < 6; i++)
        drawn += tick ();
    result (drawn == 0, "moved out again: idle");
    unload_scene ();
    g_free (dir);

    /* 4. with a layer that does move on top of them all: the four still
     * layers between the background and it come from the static bake
     * although the parked layer stands among them - a frame is the copy of
     * the bake, the moving layer's effect pass and its quad */
    dir = make_parked_scene (tmp, "parked-top", TRUE);
    if (!load_scene (dir)) {
        result (FALSE, "the scene with a parked and a moving layer loads");
        return;
    }
    for (int i = 0; i < 8; i++)
        tick ();
    void *addr = (void *) eglGetProcAddress ("glDrawArrays");
    memcpy (&real_draw_arrays, &addr, sizeof (addr));
    __typeof__ (epoxy_glDrawArrays) was = epoxy_glDrawArrays;
    epoxy_glDrawArrays = count_draw_arrays;
    n_draw_calls = 0;
    drawn = 0;
    for (int i = 0; i < 6; i++)
        drawn += tick ();
    epoxy_glDrawArrays = was;
    m = g_strdup_printf ("a moving layer above a parked one: 6 ticks draw 6 frames with 2 draw "
                         "calls each (%d frames, %u draw calls)", drawn, n_draw_calls);
    result (drawn == 6 && n_draw_calls == 12, m);
    g_free (m);
    unload_scene ();
    walpe_config_free (cfg);
    g_free (dir);
    g_free (idle);
    g_free (screen);
    screen = NULL;
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
    for (int i = 0; i < nc; i++) {   /* an 8-bit one, as the daemon's window */
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
    char *tmp = g_dir_make_tmp ("walpe-idle-repaint-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_BAKE");
    g_unsetenv ("WALPE_FRAME_SKIP");
    g_unsetenv ("WALPE_RENDER_SCALE");
    g_unsetenv ("WALPE_FX_SPEC");
    g_unsetenv ("WALPE_FX_QUALITY");
    g_unsetenv ("WALPE_PROP_PROBE");
    g_unsetenv ("WALPE_SKIP_IDS");
    idle_checks (tmp);
    parked_checks (tmp);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    rm_rf (tmp);
    g_free (tmp);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
