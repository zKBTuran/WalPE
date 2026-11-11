/* gles_scene_test.c - the same scenes on desktop OpenGL and on OpenGL ES.
 *
 *   gles-scene-test                    the registered test
 *   gles-scene-test --scene DIR [--exact] [--size WxH]
 *                                      any wallpaper directory, both APIs
 *   gles-scene-test --render gl|gles DIR OUT W H     (what the two above run)
 *
 * The OpenGL ES path is the desktop renderer with GLSL ES shaders and a
 * few replacements (common/glapi.h). On one and the same driver it has to
 * draw the same picture. The registered test generates small scenes (solid
 * colour layers; effects whose shaders are written here in Wallpaper
 * Engine's dialect, with the int / float mixing GLSL ES rejects, a
 * two-pass effect with a half-size target, one with a float target),
 * renders three frames of each on a virtual clock in two child processes -
 * one with a desktop OpenGL 3.3 context, one with an OpenGL ES 3.2 / 3.1
 * context, both on Mesa's surfaceless platform - and compares the frames
 * as read back. A process renders with one API only (the shader headers
 * and the API record are per process), hence the children.
 *
 * Tolerance, by what the scene uses:
 *   A  nothing of the fallback table in play, same driver: the frames are
 *      byte-identical (TOL_A: 0 differing colour bytes). The generated
 *      scenes are of this class.
 *   B  a replacement that changes sampling on purpose - no texture LOD
 *      bias on OpenGL ES (minified mipmapped textures are a little
 *      softer), S3TC decoded instead of sampled compressed, BGRA through
 *      another upload path: mean absolute difference at most TOL_B_MEAN,
 *      at most TOL_B_PERMILLE of the colour bytes off by more than
 *      TOL_B_STEP. The --scene mode uses this class unless --exact.
 *   C  effect float targets replaced by RGBA8 (OpenGL ES below 3.2 without
 *      the extensions): values above 1 clamp, pictures may differ
 *      visibly; reported, not judged.
 * Different drivers (or a real mobile GPU, where precision and filtering
 * are the hardware's) are no subject of this test.
 *
 * Skipped (exit 77) when either context cannot be made.
 * `meson test -C build gles-scene` */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/glfeat.h"
#include "engine/scenenode_effects.h"

#define W 320
#define H 180
#define N_SHOTS 3
static const int shot_at[N_SHOTS] = { 2, 6, 12 };   /* frames of the 30 fps virtual clock */

#define TOL_A 0
#define TOL_B_MEAN 0.5
#define TOL_B_STEP 16
#define TOL_B_PERMILLE 20

static int checks, fails;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

/* ------------------------------------------------------------ the scenes */

static void put (const char *dir, const char *rel, const char *text)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
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

#define VERT "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n" \
             "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n" \
             "void main() {\n\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n" \
             "\tv_TexCoord = a_TexCoord;\n}\n"
#define MATERIAL(sh) "{\"passes\":[{\"shader\":\"effects/" sh "\",\"blending\":\"normal\"," \
                     "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\",\"cullmode\":\"nocull\"}]}"

static void layer (GString *s, int id, const char *origin, const char *size, const char *color,
                   double alpha, const char *fx)
{
    g_string_append_printf (s,
        "%s{\"id\":%d,\"name\":\"l%d\",\"image\":\"models/solid.json\",\"origin\":\"%s\","
        "\"size\":\"%s\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"%s\",\"alpha\":%.3f,"
        "\"visible\":true%s}", id > 1 ? "," : "", id, id, origin, size, color, alpha, fx);
}

static char *fx_ref (const char *name, int id, int passes)
{
    GString *s = g_string_new (NULL);
    g_string_append_printf (s, ",\"effects\":[{\"file\":\"effects/%s/effect.json\",\"id\":%d,"
                            "\"name\":\"%s\",\"visible\":true,\"passes\":[", name, id, name);
    for (int i = 0; i < passes; i++)
        g_string_append (s, i ? ",{}" : "{}");
    g_string_append (s, "]}]");
    return g_string_free (s, FALSE);
}

/* kind 0: a time-driven one-pass effect; 1: two passes with a half-size
 * target; 2: two passes with an r16f target that holds values above 1.
 * Every fragment shader mixes integers into float arithmetic the way
 * shaders written against HLSL do. */
static char *make_scene (int kind)
{
    char *dir = g_dir_make_tmp ("walpe-gles-scene-XXXXXX", NULL);
    if (!dir)
        return NULL;
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    const char *vs[] = { "tw", "half1", "half2", "fl1", "fl2" };
    for (guint i = 0; i < G_N_ELEMENTS (vs); i++) {
        char *v = g_strdup_printf ("shaders/effects/%s.vert", vs[i]);
        put (dir, v, VERT);
        g_free (v);
    }
    put (dir, "shaders/effects/tw.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
         "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n\tfloat k = 0;\n"
         "\tfor (int i = 0; i < 3; i++) k += sin(v_TexCoord.x * 20 * (i + 1) + g_Time) / 3;\n"
         "\tc.rgb *= 0.6 + 0.4 * k;\n\tgl_FragColor = c;\n}\n");
    put (dir, "materials/effects/tw.json", MATERIAL ("tw"));
    put (dir, "effects/tw/effect.json",
         "{\"name\":\"tw\",\"group\":\"t\",\"passes\":[{\"material\":\"materials/effects/tw.json\"}]}");

    put (dir, "shaders/effects/half1.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
         "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
         "\tgl_FragColor = c * vec4(0.5, 1, 1, 1) * step(0, v_TexCoord.y);\n}\n");
    put (dir, "shaders/effects/half2.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
         "uniform float g_Time;\nvoid main() {\n\tfloat w = 0.5 + 0.5 * sin(g_Time * 2);\n"
         "\tgl_FragColor = mix(texSample2D(g_Texture0, v_TexCoord), texSample2D(g_Texture1, v_TexCoord), "
         "w > 1 ? 1 : w);\n}\n");
    put (dir, "materials/effects/half1.json", MATERIAL ("half1"));
    put (dir, "materials/effects/half2.json", MATERIAL ("half2"));
    put (dir, "effects/half/effect.json",
         "{\"name\":\"half\",\"group\":\"t\",\"passes\":["
         "{\"material\":\"materials/effects/half1.json\",\"target\":\"_rt_half\","
         "\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
         "{\"material\":\"materials/effects/half2.json\","
         "\"bind\":[{\"name\":\"_rt_half\",\"index\":0},{\"name\":\"previous\",\"index\":1}]}],"
         "\"fbos\":[{\"name\":\"_rt_half\",\"scale\":2}]}");

    put (dir, "shaders/effects/fl1.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
         "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
         "\tgl_FragColor = vec4(dot(c.rgb, vec3(1)) * 2);\n}\n");
    put (dir, "shaders/effects/fl2.frag",
         "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
         "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
         "\tfloat l = texSample2D(g_Texture1, v_TexCoord).r;\n"
         "\tgl_FragColor = vec4(c.rgb * l / 4, c.a);\n}\n");
    put (dir, "materials/effects/fl1.json", MATERIAL ("fl1"));
    put (dir, "materials/effects/fl2.json", MATERIAL ("fl2"));
    put (dir, "effects/fl/effect.json",
         "{\"name\":\"fl\",\"group\":\"t\",\"passes\":["
         "{\"material\":\"materials/effects/fl1.json\",\"target\":\"_rt_flbuf\"},"
         "{\"material\":\"materials/effects/fl2.json\",\"bind\":[{\"name\":\"_rt_flbuf\",\"index\":1}]}],"
         "\"fbos\":[{\"name\":\"_rt_flbuf\",\"format\":\"r16f\",\"scale\":1}]}");

    GString *s = g_string_new (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false,"
        "\"clearcolor\":\"0.10 0.20 0.30\",\"orthogonalprojection\":{\"height\":180,\"width\":320},"
        "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[");
    char *fx = kind == 0 ? fx_ref ("tw", 9, 1) : kind == 1 ? fx_ref ("half", 9, 2) : fx_ref ("fl", 9, 2);
    layer (s, 1, "160 90 0", "320 180", "0.2 0.5 0.3", 1.0, "");
    layer (s, 2, "100 60 0", "120 80", "0.9 0.2 0.1", 0.6, "");
    layer (s, 3, "200 110 0", "140 60", "0.1 0.3 0.9", 0.5, "");
    layer (s, 4, "230 60 0", "120 70", "0.8 0.8 1.0", 0.9, fx);
    layer (s, 5, "60 150 0", "70 30", "0.3 0.9 0.9", 0.7, "");
    g_free (fx);
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"gles scene test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
    return dir;
}

/* ------------------------------------------------------------- the child */

static void redraw_cb (gpointer u) { (void) u; }

/* warnings of the engine that say a shader did not build ("compile
 * failed", "link failed"): the scene would be drawn without that effect,
 * on both paths alike, and the comparison would prove nothing */
static int shader_fails;

static void child_log (const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer user)
{
    (void) domain;
    (void) user;
    if (!(level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR)))
        return;
    fprintf (stderr, "%s\n", msg);
    if (strstr (msg, "compile failed") || strstr (msg, "link failed") || strstr (msg, "not compiled"))
        shader_fails++;
}

/* 0 = rendered; 77 = this API has no context here; else an error */
static int child_render (const char *api, const char *dir, const char *out, int w, int h)
{
    const gboolean es = !strcmp (api, "gles");
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        return 77;
    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[128], cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                            es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) ||
        !eglBindAPI (es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, cfgs, 128, &nc) || nc < 1)
        return 77;
    cfg = cfgs[0];
    for (int i = 0; i < nc; i++) {
        EGLint r = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &r);
        if (r == 8) {
            cfg = cfgs[i];
            break;
        }
    }
    EGLContext ctx = EGL_NO_CONTEXT;
    /* the versions the daemon asks for: OpenGL 3.3; OpenGL ES 3.2, then 3.1 */
    for (int minor = es ? 2 : 3; ctx == EGL_NO_CONTEXT && minor >= (es ? 1 : 3); minor--) {
        const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE };
        ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    }
    const EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx))
        return 77;
    g_log_set_default_handler (child_log, NULL);
    walpe_glutil_init ();
    fx_set_cache_enabled (TRUE);
    if (walpe_gl_feat_es () != es) {
        fprintf (stderr, "the context is not the API asked for\n");
        return 2;
    }
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c || !c->gl_init (c, &err)) {
        fprintf (stderr, "load failed: %s\n", err ? err->message : "?");
        return 2;
    }
    c->request_redraw = redraw_cb;
    FILE *f = fopen (out, "wb");
    if (!f)
        return 2;
    guint8 *px = g_malloc ((gsize) w * h * 4);
    int shot = 0;
    for (int frame = 1; shot < N_SHOTS; frame++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, w, h);
        c->next_frame_us = 0;
        c->surface_lost = TRUE;     /* draw every frame: the back buffer is read, not swapped */
        c->gl_render (c, w, h, 0.0f, 0.0f);
        glFinish ();
        if (frame != shot_at[shot])
            continue;
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (fwrite (px, 1, (gsize) w * h * 4, f) != (gsize) w * h * 4)
            return 2;
        shot++;
    }
    fclose (f);
    g_free (px);
    int compiled = 0, shared = 0, respec = 0, fallback = 0, baked = 0, identity = 0;
    fx_program_stats (&compiled, &shared, &respec, &fallback, &baked, &identity, FALSE);
    /* one line for the parent */
    printf ("api=%s programs=%d shader_fails=%d float_targets=%s gl_error=%d version=%s\n",
            es ? "gles" : "gl", compiled, shader_fails,
            walpe_glf_how_name (walpe_gl_feat_how (WALPE_GLF_FLOAT_TARGETS)), glGetError () != GL_NO_ERROR,
            (const char *) glGetString (GL_VERSION));
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return 0;
}

/* ------------------------------------------------------------ the parent */

typedef struct {
    int status;             /* the child's exit code; -1 = could not be run */
    int programs, shader_fails;
    gboolean gl_error;
    char how_float[16];
    guint8 *px;             /* N_SHOTS frames */
    gsize len;
} Shot;

static void shot_clear (Shot *s)
{
    g_free (s->px);
    memset (s, 0, sizeof *s);
}

static char *self_exe;

static void run_child (const char *api, const char *dir, int w, int h, const char *probe_dir, Shot *s)
{
    memset (s, 0, sizeof *s);
    s->status = -1;
    char *out = g_strdup_printf ("%s/frames-%s.rgba", probe_dir, api);
    char ws[16], hs[16];
    g_snprintf (ws, sizeof ws, "%d", w);
    g_snprintf (hs, sizeof hs, "%d", h);
    char *argv[] = { self_exe, (char *) "--render", (char *) api, (char *) dir, out, ws, hs, NULL };
    /* the virtual 30 fps scene clock (propprobe.h): the same frames on
     * every run; nothing else of the environment is changed */
    char **envp = g_environ_setenv (g_get_environ (), "WALPE_PROP_PROBE", probe_dir, TRUE);
    char *so = NULL, *se = NULL;
    int wait_status = 0;
    GError *e = NULL;
    if (g_spawn_sync (NULL, argv, envp, G_SPAWN_DEFAULT, NULL, NULL, &so, &se, &wait_status, &e)) {
        s->status = WIFEXITED (wait_status) ? WEXITSTATUS (wait_status) : 128;
        const char *p = so ? strstr (so, "programs=") : NULL;
        if (p)
            s->programs = atoi (p + 9);
        p = so ? strstr (so, "shader_fails=") : NULL;
        s->shader_fails = p ? atoi (p + 13) : -1;
        p = so ? strstr (so, "float_targets=") : NULL;
        if (p)
            sscanf (p + 14, "%15s", s->how_float);
        p = so ? strstr (so, "gl_error=") : NULL;
        s->gl_error = p && p[9] == '1';
        if (s->status == 0)
            g_file_get_contents (out, (char **) &s->px, &s->len, NULL);
        else if (s->status != 77 && se && *se)
            printf ("      %s child: %.600s\n", api, se);
    } else {
        printf ("      cannot run %s: %s\n", self_exe, e ? e->message : "?");
        g_clear_error (&e);
    }
    g_unlink (out);
    g_free (out);
    g_free (so);
    g_free (se);
    g_strfreev (envp);
}

typedef struct { gsize differ, over_step; int max; double mean; } Diff;

/* colour bytes only: a pbuffer's alpha is not the picture */
static Diff compare (const guint8 *a, const guint8 *b, gsize n)
{
    Diff d = { 0, 0, 0, 0.0 };
    gsize colour = 0;
    double sum = 0;
    for (gsize i = 0; i < n; i++) {
        if ((i & 3) == 3)
            continue;
        int v = abs ((int) a[i] - (int) b[i]);
        colour++;
        sum += v;
        d.differ += v != 0;
        d.over_step += v > TOL_B_STEP;
        d.max = MAX (d.max, v);
    }
    d.mean = colour ? sum / (double) colour : 0.0;
    return d;
}

/* klass 'A', 'B' or 'C'. Returns FALSE when the comparison was skipped. */
static gboolean compare_scene (const char *name, const char *dir, int w, int h, char klass,
                               int min_programs)
{
    char *probe = g_dir_make_tmp ("walpe-gles-probe-XXXXXX", NULL);
    Shot gl, es;
    run_child ("gl", dir, w, h, probe, &gl);
    run_child ("gles", dir, w, h, probe, &es);
    rm_rf (probe);
    g_free (probe);
    gboolean ran = FALSE;
    if (gl.status == 77 || es.status == 77) {
        printf ("skip  %s (%s context not available here)\n", name,
                gl.status == 77 ? "desktop OpenGL 3.3" : "OpenGL ES 3.1");
    } else if (gl.status != 0 || es.status != 0 || !gl.px || !es.px ||
               gl.len != (gsize) w * h * 4 * N_SHOTS || es.len != gl.len) {
        char *d = g_strdup_printf ("gl exit %d, gles exit %d", gl.status, es.status);
        result (FALSE, name, d);
        g_free (d);
    } else {
        ran = TRUE;
        char *what = g_strdup_printf ("%s: both paths built the scene's effect programs, no GL error", name);
        char *d = g_strdup_printf ("gl %d program(s), %d shader failure(s); gles %d, %d", gl.programs,
                                   gl.shader_fails, es.programs, es.shader_fails);
        result (gl.programs >= min_programs && es.programs == gl.programs && gl.shader_fails == 0 &&
                es.shader_fails == 0 && !gl.gl_error && !es.gl_error, what, d);
        g_free (what);
        g_free (d);
        if (klass == 'A' && !strcmp (es.how_float, "replaced") && strstr (name, "float"))
            klass = 'C';        /* no float targets on this OpenGL ES: they clamp */
        gboolean moved = FALSE;
        for (int k = 0; k < N_SHOTS; k++) {
            const gsize fl = (gsize) w * h * 4;
            Diff df = compare (gl.px + k * fl, es.px + k * fl, fl);
            const gsize colour = (gsize) w * h * 3;
            gboolean ok = klass == 'A' ? df.differ <= TOL_A
                        : klass == 'B' ? df.mean <= TOL_B_MEAN &&
                                         df.over_step * 1000 <= colour * TOL_B_PERMILLE
                                       : TRUE;
            what = g_strdup_printf ("%s, frame %d, class %c", name, shot_at[k], klass);
            d = df.differ == 0 ? g_strdup ("identical")
                               : g_strdup_printf ("%" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " colour bytes "
                                                  "differ, max %d, mean %.4f, %" G_GSIZE_FORMAT " by more "
                                                  "than %d", df.differ, colour, df.max, df.mean,
                                                  df.over_step, TOL_B_STEP);
            result (ok, what, d);
            g_free (what);
            g_free (d);
            if (k > 0)
                moved |= memcmp (gl.px, gl.px + k * fl, fl) != 0;
        }
        if (strstr (name, "time"))
            result (moved, "... and the frames differ from each other (the clock runs, the effect draws)",
                    NULL);
    }
    shot_clear (&gl);
    shot_clear (&es);
    return ran;
}

int main (int argc, char **argv)
{
    if (argc >= 7 && !strcmp (argv[1], "--render"))
        return child_render (argv[2], argv[3], argv[4], atoi (argv[5]), atoi (argv[6]));
    self_exe = g_file_read_link ("/proc/self/exe", NULL);
    if (!self_exe)
        self_exe = g_strdup (argv[0]);
    if (argc >= 3 && !strcmp (argv[1], "--scene")) {
        int w = 1280, h = 720;
        char klass = 'B';
        for (int i = 3; i < argc; i++) {
            if (!strcmp (argv[i], "--exact"))
                klass = 'A';
            else if (!strcmp (argv[i], "--size") && i + 1 < argc &&
                     (sscanf (argv[++i], "%dx%d", &w, &h) != 2 || w < 16 || h < 16 || w > 4096 || h > 4096)) {
                fprintf (stderr, "--size WxH\n");
                return 2;
            }
        }
        char *abs = g_canonicalize_filename (argv[2], NULL);
        compare_scene (abs, abs, w, h, klass, 0);
        g_free (abs);
        printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
        return fails ? 1 : 0;
    }
    static const struct { const char *name; int kind, programs; } scenes[] = {
        { "one-pass time-driven effect", 0, 1 },
        { "two passes with a half-size target, time-driven", 1, 2 },
        { "two passes with a float target", 2, 2 },
    };
    int ran = 0;
    for (guint i = 0; i < G_N_ELEMENTS (scenes); i++) {
        char *dir = make_scene (scenes[i].kind);
        if (!dir) {
            result (FALSE, scenes[i].name, "no temporary directory");
            continue;
        }
        ran += compare_scene (scenes[i].name, dir, W, H, 'A', scenes[i].programs);
        rm_rf (dir);
        g_free (dir);
    }
    if (!ran && !fails) {
        /* (77: the test harness counts it as skipped, not as passed) */
        printf ("skipped (needs a desktop OpenGL 3.3 and an OpenGL ES 3.1 context on EGL surfaceless)\n");
        return 77;
    }
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
