/* scene_hostile_test - a scene whose scripts drive its layers and effects
 * with hostile values, through the real scene (scenenode.c) and renderer.
 *
 *   scene-hostile-test [values|parents|all]
 *
 * Generated scenes (solid layers, a compose layer, a full-screen compose
 * layer, a text layer, parent links, one small effect: no files of any
 * wallpaper) on Mesa's surfaceless platform with a pbuffer as the window.
 *  1. "values": scripts on origin / scale / angles / color / alpha /
 *     visible / text of every kind of layer, on an effect's visibility and
 *     on a material constant return, frame after frame, zero, tiny, huge
 *     (1e308: the script interface brings it into range), negative and
 *     non-finite vectors, megabytes of text, text that is not UTF-8, and
 *     re-parent layers to themselves and in circles. 48 frames are drawn:
 *     nothing may crash, every frame comes back in time, and the effect
 *     targets stay within what the window needs.
 *  2. "parents": a script chains 100 000 layers parent to parent at run
 *     time (the load limits such chains; a script could rebuild one, and
 *     the per-frame transform walk went one stack frame down per layer).
 * Run under ASan + UBSan in the sanitizer builds. Skipped (exit 77)
 * without EGL surfaceless / GL 3.3 or without QuickJS.
 * `meson test -C build scene-hostile` */
#include "walpe-build-config.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/scenenode_effects.h"

static int checks, fails;

static void result (gboolean ok, const char *what)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s\n", ok ? "ok  " : "FAIL", what);
    fflush (stdout);
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

static void rm_rf (const char *path)
{
    GDir *dir = g_dir_open (path, 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name (dir)) != NULL) {
            char *c = g_build_filename (path, n, NULL);
            if (g_file_test (c, G_FILE_TEST_IS_DIR) && !g_file_test (c, G_FILE_TEST_IS_SYMLINK))
                rm_rf (c);
            else
                g_unlink (c);
            g_free (c);
        }
        g_dir_close (dir);
    }
    g_rmdir (path);
}

/* a JSON string from C text */
static char *jstr (const char *s)
{
    GString *o = g_string_new ("\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            g_string_append_printf (o, "\\%c", *s);
        else if (*s == '\n')
            g_string_append (o, "\\n");
        else if (*s == '\t')
            g_string_append (o, "\\t");
        else
            g_string_append_c (o, *s);
    }
    g_string_append_c (o, '"');
    return g_string_free (o, FALSE);
}

/* {"script": src, "value": value} */
static char *scripted (const char *src, const char *value)
{
    char *js = jstr (src);
    char *r = g_strdup_printf ("{\"script\":%s,\"value\":%s}", js, value);
    g_free (js);
    return r;
}

/* what the scripts return, by frame: every script has the table and its
 * own counter */
#define VALUES_JS \
    "'use strict';\n" \
    "const V = [ new Vec3(0, 0, 0), new Vec3(1e-30, 1e-30, 1), new Vec3(1e308, 1e308, 1e308), new Vec3(-1e308, 1e308, 1),\n" \
    "  new Vec3(NaN, NaN, NaN), new Vec3(Infinity, -Infinity, 0), new Vec3(1e9, 1e9, 1), new Vec3(-1e9, -1e9, -1),\n" \
    "  new Vec3(3.5e38, 3.5e38, 1), new Vec3(1e-9, 1e9, 1), new Vec3(-0, -0, -0), new Vec3(0.5, -0.5, 1),\n" \
    "  new Vec3(1e5, 1e-5, 1), new Vec3(123456789, 987654321, 0), new Vec3(1, 1, 1), new Vec3(2, 2, 1) ];\n" \
    "const N = [ 0, -1, 1e308, -1e308, NaN, Infinity, 1e-320, 0.5, 2147483648, -2147483649, 1, 1e9, 4096.5, -0, 255, 1 ];\n" \
    "let f = 0;\n"

static const char *VEC_JS = VALUES_JS
    "export function update(value) { f++; return V[f % V.length]; }\n";
static const char *NUM_JS = VALUES_JS
    "export function update(value) { f++; return N[f % N.length]; }\n";
static const char *BOOL_JS = VALUES_JS
    "export function update(value) { f++; return (f % 3) !== 0; }\n";
/* a layer that also reaches for the others */
static const char *REACH_JS = VALUES_JS
    "const names = ['bg', 'fx', 'child', 'compose', 'full', 'text', 'nope'];\n"
    "export function update(value) { f++;\n"
    "  for (const n of names) { const L = thisScene.getLayer(n); if (!L) continue;\n"
    "    const k = (f * 7 + n.length) % V.length;\n"
    "    if (f % 4 === 0) L.origin = V[k]; if (f % 4 === 1) L.scale = V[(k + 3) % V.length];\n"
    "    if (f % 4 === 2) L.angles = V[(k + 5) % V.length]; if (f % 5 === 0) L.alpha = N[k % N.length];\n"
    "    if (f % 6 === 0) L.color = V[(k + 1) % V.length]; if (f % 7 === 0) L.visible = !L.visible;\n"
    "    if (f % 9 === 0) { L.setParent(L); L.setParent(thisLayer); thisLayer.setParent(L); }\n"
    "    if (f % 11 === 0) L.setParent(null);\n"
    "    const e = L.getEffect(0); if (e) { if (f % 5 === 1) e.visible = !e.visible;\n"
    "      e.setMaterialProperty('gain', N[(f + 3) % N.length]); e.setMaterialProperty('tint', V[k]); } }\n"
    "  return V[(f * 3) % V.length]; }\n";
static const char *TEXT_JS = VALUES_JS
    "const T = [ '', 'x', 'y'.repeat(3000000), '\\ud800 lone', 'a\\u0000b', '\\n'.repeat(50000), '\\u202e\\u202d'.repeat(5000),\n"
    "  '%s%n%x', '<span foreground=\"red\">&amp;</b>', '\\t'.repeat(20000), '\\ufffd\\uffff', 'W'.repeat(40000) ];\n"
    "export function update(value) { f++; thisLayer.pointsize = N[f % N.length]; thisLayer.maxwidth = N[(f + 5) % N.length];\n"
    "  thisLayer.padding = N[(f + 9) % N.length]; thisLayer.horizontalalign = T[f % T.length]; thisLayer.maxrows = N[(f + 2) % N.length];\n"
    "  return T[f % T.length]; }\n";

static const char *VERT =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec2 v_TexCoord;\nvoid main() {\n"
    "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n";
static const char *FRAG =
    "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nuniform float g_Time;\n"
    "uniform float g_Gain; // {\"material\":\"gain\",\"default\":1}\n"
    "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"1 1 1\"}\n"
    "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "\tc.rgb *= (0.6 + 0.4 * sin(v_TexCoord.x * 20.0 + g_Time)) * g_Gain * g_Tint;\n\tgl_FragColor = c;\n}\n";

static void common_files (const char *dir)
{
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "models/compose.json", "{\"passthrough\":true}");
    put (dir, "models/full.json", "{\"passthrough\":true,\"fullscreen\":true}");
    put (dir, "shaders/effects/tw.vert", VERT);
    put (dir, "shaders/effects/tw.frag", FRAG);
    put (dir, "materials/effects/tw.json", "{\"passes\":[{\"shader\":\"effects/tw\"}]}");
    put (dir, "effects/tw/effect.json",
         "{\"name\":\"tw\",\"passes\":[{\"material\":\"materials/effects/tw.json\"}]}");
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"scene hostile test\",\"type\":\"scene\","
         "\"general\":{\"properties\":{}}}");
}

#define SCENE_HEAD \
    "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"}," \
    "\"general\":{\"clearcolor\":\"0.1 0.2 0.3\",\"orthogonalprojection\":{\"height\":180,\"width\":320}}," \
    "\"objects\":["

static char *values_scene (void)
{
    char *dir = g_dir_make_tmp ("walpe-scene-hostile-XXXXXX", NULL);
    if (!dir)
        return NULL;
    common_files (dir);
    char *vec = scripted (VEC_JS, "\"1 1 1\""), *num = scripted (NUM_JS, "1"), *bl = scripted (BOOL_JS, "true");
    char *reach = scripted (REACH_JS, "\"160 90 0\""), *txt = scripted (TEXT_JS, "\"hello\"");
    char *fx = g_strdup_printf (",\"effects\":[{\"file\":\"effects/tw/effect.json\",\"id\":9,\"name\":\"tw\","
                                "\"visible\":%s,\"passes\":[{\"constantshadervalues\":{\"gain\":%s,\"tint\":%s}}]}]",
                                bl, num, vec);
    GString *s = g_string_new (SCENE_HEAD);
    g_string_append (s, "{\"id\":1,\"name\":\"bg\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\","
                        "\"size\":\"320 180\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0.2 0.5 0.3\","
                        "\"alpha\":1,\"visible\":true}");
    g_string_append_printf (s, ",{\"id\":2,\"name\":\"fx\",\"image\":\"models/solid.json\",\"origin\":\"100 60 0\","
                            "\"size\":\"120 80\",\"scale\":%s,\"angles\":\"0 0 0\",\"color\":%s,\"alpha\":%s,"
                            "\"visible\":true%s}", vec, vec, num, fx);
    g_string_append_printf (s, ",{\"id\":3,\"name\":\"child\",\"parent\":2,\"image\":\"models/solid.json\","
                            "\"origin\":%s,\"size\":\"40 30\",\"scale\":\"1 1 1\",\"angles\":%s,"
                            "\"color\":\"0.9 0.2 0.1\",\"alpha\":0.8,\"visible\":%s}", vec, vec, bl);
    g_string_append_printf (s, ",{\"id\":4,\"name\":\"compose\",\"image\":\"models/compose.json\",\"origin\":%s,"
                            "\"size\":\"120 80\",\"scale\":%s,\"angles\":%s,\"alpha\":1,\"visible\":true%s}",
                            reach, vec, vec, fx);
    g_string_append_printf (s, ",{\"id\":5,\"name\":\"full\",\"image\":\"models/full.json\",\"origin\":\"160 90 0\","
                            "\"size\":\"320 180\",\"scale\":%s,\"angles\":\"0 0 0\",\"alpha\":%s,\"visible\":true%s}",
                            vec, num, fx);
    g_string_append_printf (s, ",{\"id\":6,\"name\":\"text\",\"text\":%s,\"origin\":%s,\"size\":\"200 60\","
                            "\"scale\":%s,\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"alpha\":1,\"visible\":true,"
                            "\"pointsize\":12,\"horizontalalign\":\"center\",\"verticalalign\":\"center\"}",
                            txt, vec, vec);
    /* what the file itself may say (same sinks as the scripts) */
    g_string_append (s, ",{\"id\":7,\"name\":\"static\",\"image\":\"models/compose.json\",\"origin\":\"1e9 -1e9 0\","
                        "\"size\":\"1e7 1e7\",\"scale\":\"1e9 1e-9 1\",\"angles\":\"0 0 1e9\",\"alpha\":1,\"visible\":true");
    g_string_append (s, fx);
    g_string_append (s, "}]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    g_free (vec); g_free (num); g_free (bl); g_free (reach); g_free (txt); g_free (fx);
    return dir;
}

#define CHAIN_N 100000

static char *parents_scene (void)
{
    char *dir = g_dir_make_tmp ("walpe-scene-hostile-XXXXXX", NULL);
    if (!dir)
        return NULL;
    common_files (dir);
    /* each frame chains another 5 000 layers: layer i gets layer i + 1 as
     * its parent, so the walk for layer 0 goes through all of them. Two
     * handles made from thisLayer's prototype stand in for the layers (a
     * layer object keeps its index in a plain property). */
    char *chain = scripted (
        "'use strict';\nconst P = Object.getPrototypeOf(thisLayer), a = Object.create(P), b = Object.create(P);\n"
        "let at = 0, n = -1;\n"
        "export function update(value) { if (n < 0) n = thisScene.getLayerCount();\n"
        "  const end = Math.min(at + 5000, n - 1);\n"
        "  for (; at < end; at++) { a.__i = at; b.__i = at + 1; a.setParent(b); }\n"
        "  if (at === n - 1) { at++; console.log('chained ' + n + ' layers, parent of 0: ' + (thisLayer.getParent() ? 1 : 0)); }\n"
        "  return value; }\n", "true");
    GString *s = g_string_new (SCENE_HEAD);
    g_string_append_printf (s, "{\"id\":1,\"name\":\"n0\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\","
                            "\"size\":\"100 60\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\","
                            "\"alpha\":1,\"visible\":%s}", chain);
    for (int i = 1; i < CHAIN_N; i++)
        g_string_append_printf (s, ",{\"id\":%d,\"name\":\"n%d\",\"origin\":\"1 0 0\",\"scale\":\"1 1 1\","
                                "\"angles\":\"0 0 0\",\"visible\":true}", i + 1, i);
    g_string_append (s, "]}");
    put (dir, "scene.json", s->str);
    g_string_free (s, TRUE);
    g_free (chain);
    return dir;
}

static void redraw_cb (gpointer u) { (void) u; }

#define W 320
#define H 180

static WalpeContent *open_scene (const char *dir)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir, .render_scale = 100 };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c) {
        printf ("load failed: %s\n", err ? err->message : "?");
        return NULL;
    }
    c->request_redraw = redraw_cb;
    if (!c->gl_init (c, &err)) {
        printf ("gl_init failed: %s\n", err ? err->message : "?");
        c->free (c);
        return NULL;
    }
    return c;
}

static double frame (WalpeContent *c)
{
    gint64 t0 = g_get_monotonic_time ();
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, W, H);
    c->next_frame_us = 0;
    c->surface_lost = FALSE;
    c->frame_unchanged = FALSE;
    c->gl_render (c, W, H, 0.5f, 0.5f);
    glFinish ();
    return (g_get_monotonic_time () - t0) / 1e6;
}

static void close_scene (WalpeContent *c)
{
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
}

static void values_checks (void)
{
    char *dir = values_scene ();
    WalpeContent *c = dir ? open_scene (dir) : NULL;
    result (c != NULL, "values: the scene loads");
    if (c) {
        double worst = 0;
        gint64 most = 0;
        int most_n = 0;
        for (int f = 0; f < 48; f++) {
            double dt = frame (c);
            worst = MAX (worst, dt);
            gint64 bytes = 0;
            int n = 0;
            fx_targets_stats (&bytes, &n);
            if (bytes > most) {
                most = bytes;
                most_n = n;
            }
            g_usleep (2000);
        }
        char *m = g_strdup_printf ("values: 48 frames of hostile layer and effect values drawn (slowest %.2f s)",
                                   worst);
        result (worst < 60.0, m);
        g_free (m);
        /* a 320x180 window: its effect targets are a few hundred KiB. The
         * limit is far above that and far below what a target sized by a
         * runaway scale would take */
        m = g_strdup_printf ("values: effect targets stay within the window's needs (at most %d targets, "
                             "%.1f MiB)", most_n, most / 1048576.0);
        result (most < 64 * 1048576, m);
        g_free (m);
        close_scene (c);
    }
    if (dir) {
        rm_rf (dir);
        g_free (dir);
    }
}

static void parents_checks (void)
{
    char *dir = parents_scene ();
    gint64 t0 = g_get_monotonic_time ();
    WalpeContent *c = dir ? open_scene (dir) : NULL;
    result (c != NULL, "parents: a scene of 100 000 layers loads");
    if (c) {
        for (int f = 0; f < 30; f++)
            frame (c);
        char *m = g_strdup_printf ("parents: a script chained the layers parent to parent; 30 frames drawn "
                                   "(%.1f s with the load)", (g_get_monotonic_time () - t0) / 1e6);
        result (TRUE, m);
        g_free (m);
        close_scene (c);
    }
    if (dir) {
        rm_rf (dir);
        g_free (dir);
    }
}

static void quiet_log (const gchar *d, GLogLevelFlags l, const gchar *m, gpointer u)
{
    (void) d; (void) l; (void) m; (void) u;
}

int main (int argc, char **argv)
{
#ifndef HAVE_QUICKJS
    printf ("skip (built without QuickJS)\n");
    return 77;
#endif
    const char *only = argc > 1 && strcmp (argv[1], "all") ? argv[1] : NULL;
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skip (no EGL surfaceless platform)\n");
        return 77;
    }
    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[128], cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, cfgs, 128, &nc) || nc < 1) {
        printf ("skip (no usable GL config)\n");
        return 77;
    }
    cfg = cfgs[0];
    for (int i = 0; i < nc; i++) {
        EGLint r = 0;
        eglGetConfigAttrib (dpy, cfgs[i], EGL_RED_SIZE, &r);
        if (r == 8) {
            cfg = cfgs[i];
            break;
        }
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        printf ("skip (no GL 3.3 context with a pbuffer)\n");
        return 77;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    if (!g_getenv ("WALPE_SCENE_HOSTILE_VERBOSE"))
        g_log_set_default_handler (quiet_log, NULL);
    walpe_glutil_init ();
    fx_set_cache_enabled (TRUE);
    if (!only || !strcmp (only, "values"))
        values_checks ();
    if (!only || !strcmp (only, "parents"))
        parents_checks ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
