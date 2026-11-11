/* scene_props_test - what a wallpaper script sees of a real scene.
 *
 * tests/scenescript_test.c runs the script engine against a stand-in host;
 * this one runs scripts in a generated scene (solid layers, a text layer,
 * a particle object, user options, a stored user value) through the
 * content API on Mesa's surfaceless platform and lets them report with
 * console.log:
 *   1. set-then-get of every layer property a script can write gives back
 *      the value written: origin, scale, color, alpha, visible, solid, the
 *      text layer's text / pointsize / maxwidth / padding / alignment /
 *      row and width limits, the parent, a particle object's instance
 *      overrides (count, rate, size, lifetime, alpha, speed, colorn, a
 *      control point), play / pause / stop of a particle object;
 *   2. engine.userProperties has every option with the type of its
 *      default: checkbox boolean, slider number (whole and fractional
 *      defaults), combo by the type of its values, text as text even when
 *      it reads like a number or a boolean, colour as Vec3 - for the
 *      defaults and for a value the user stored;
 *   3. init () of every script runs before any applyUserProperties (), that
 *      before any update (); scripts of one frame run in layer order and
 *      share the `shared` object; thisLayer is the script's own layer in
 *      init, update and inside a timer callback; engine.canvasSize is the
 *      scene's size.
 * Values are chosen exact in single precision (the scene stores floats).
 * Skipped (exit 0, "skip") without EGL surfaceless / GL 3.3 or QuickJS. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/config.h"
#include "common/util.h"
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

/* the scripts' report lines: "REPORT <group>: <n> ok[; BAD <name> ...]" */
static GPtrArray *reports;
static int script_errors;

static GLogWriterOutput log_writer (GLogLevelFlags level, const GLogField *fields, gsize n, gpointer u)
{
    (void) u;
    for (gsize i = 0; i < n; i++) {
        if (strcmp (fields[i].key, "MESSAGE"))
            continue;
        const char *m = fields[i].value, *t = m ? strstr (m, "REPORT ") : NULL;
        if (t) {
            g_ptr_array_add (reports, g_strdup (t + 7));
            return G_LOG_WRITER_HANDLED;
        }
        if (m && (strstr (m, "script error") || strstr (m, "timer error") || strstr (m, "out of time") ||
                  strstr (m, "scenescript: init") || strstr (m, "scenescript: update")))
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

/* shared by the scripts: a collector of named checks that logs one line */
#define JS_LIB \
    "function eqv(a, b) { return !!a && !!b && a.x === b.x && a.y === b.y && (b.z === undefined || a.z === b.z); }\n" \
    "function show(v) { return v && typeof v === 'object' ? '(' + v.x + ' ' + v.y + ' ' + v.z + ')' : typeof v + ':' + v; }\n" \
    "function Group(name) { this.name = name; this.ok = 0; this.bad = []; }\n" \
    "Group.prototype.same = function (what, got, want) {\n" \
    "  const ok = want && typeof want === 'object' ? eqv(got, want) : got === want;\n" \
    "  if (ok) this.ok++; else this.bad.push(what + ' got ' + show(got) + ' want ' + show(want)); };\n" \
    "Group.prototype.rt = function (what, obj, key, value) {\n" \
    "  let got; try { obj[key] = value; got = obj[key]; } catch (e) { got = 'threw ' + e; }\n" \
    "  this.same(what, got, value); };\n" \
    "Group.prototype.done = function () {\n" \
    "  console.log('REPORT ' + this.name + ': ' + this.ok + ' ok' + (this.bad.length ? '; BAD ' + this.bad.join('; BAD ') : '')); };\n"

/* on layer "panel" (the first layer): the round trips, the options */
static const char *SCRIPT_A =
    "'use strict';\n"
    JS_LIB
    "let frames = 0, timerLayer = null, upSeen = null;\n"
    "shared.order = [];\n"
    "export function init(value) {\n"
    "  shared.order.push('A.init');\n"
    "  shared.fromA = 41;\n"
    "  const g = new Group('layer');\n"
    "  const L = thisLayer, T = thisScene.getLayer('label'), C = thisScene.getLayer('child'), D = thisScene.getLayer('dust');\n"
    "  g.same('thisLayer is the layer by its name', thisScene.getLayer('panel'), L);\n"
    "  g.same('thisLayer.name', L.name, 'panel');\n"
    "  g.rt('origin', C, 'origin', new Vec3(12.5, -7.25, 0));\n"
    "  g.rt('scale', C, 'scale', new Vec3(1.5, 0.25, 1));\n"
    "  g.rt('negative scale', C, 'scale', new Vec3(-2, 0.5, 1));\n"
    "  g.rt('color', C, 'color', new Vec3(0.25, 0.5, 0.75));\n"
    "  g.rt('alpha', C, 'alpha', 0.5);\n"
    "  g.rt('visible off', C, 'visible', false);\n"
    "  g.rt('visible on', C, 'visible', true);\n"
    "  g.rt('solid off', C, 'solid', false);\n"
    "  g.rt('solid on', C, 'solid', true);\n"
    "  g.same('no parent', C.getParent(), undefined);\n"
    "  C.setParent(L);\n"
    "  g.same('parent set', C.getParent(), L);\n"
    "  g.same('child listed', L.getChildren().indexOf(C) >= 0, true);\n"
    "  C.setParent(null);\n"
    "  g.same('parent removed', C.getParent(), undefined);\n"
    "  g.done();\n"
    "  const t = new Group('text');\n"
    "  t.same('text of the file', T.text, 'hello');\n"
    "  t.rt('text', T, 'text', 'h\\u00e9llo w\\u00f6rld \\u65e5\\u672c\\u8a9e 12:34');\n"
    "  t.rt('empty text', T, 'text', '');\n"
    "  t.rt('pointsize', T, 'pointsize', 48);\n"
    "  t.rt('maxwidth', T, 'maxwidth', 300);\n"
    "  t.rt('padding', T, 'padding', 16);\n"
    "  t.rt('horizontalalign left', T, 'horizontalalign', 'left');\n"
    "  t.rt('horizontalalign right', T, 'horizontalalign', 'right');\n"
    "  t.rt('horizontalalign center', T, 'horizontalalign', 'center');\n"
    "  t.rt('verticalalign top', T, 'verticalalign', 'top');\n"
    "  t.rt('verticalalign bottom', T, 'verticalalign', 'bottom');\n"
    "  t.rt('limitwidth', T, 'limitwidth', true);\n"
    "  t.rt('limitrows', T, 'limitrows', true);\n"
    "  t.rt('maxrows', T, 'maxrows', 3);\n"
    "  t.rt('text again', T, 'text', 'A longer line that has to wrap inside three hundred units');\n"
    "  t.done();\n"
    "  const p = new Group('particles');\n"
    "  const I = D.instance;\n"
    "  p.same('count of the file', I.count, 2);\n"
    "  p.same('rate not in the file', I.rate, 1);\n"
    "  p.rt('count', I, 'count', 2.5);\n"
    "  p.rt('count 0', I, 'count', 0);\n"
    "  p.rt('rate', I, 'rate', 0.5);\n"
    "  p.rt('size', I, 'size', 3);\n"
    "  p.rt('lifetime', I, 'lifetime', 2);\n"
    "  p.rt('alpha', I, 'alpha', 0.5);\n"
    "  p.rt('speed', I, 'speed', 4);\n"
    "  p.rt('colorn', I, 'colorn', new Vec3(0.5, 0.25, 1));\n"
    "  p.rt('controlpoint1', I, 'controlpoint1', new Vec3(10, 20, 30));\n"
    "  p.same('playing at load', D.isPlaying(), true);\n"
    "  D.stop(); p.same('stopped', D.isPlaying(), false);\n"
    "  D.play(); p.same('playing again', D.isPlaying(), true);\n"
    "  p.done();\n"
    "  engine.setTimeout(function () { timerLayer = thisLayer; }, 0);\n"
    "  return value;\n"
    "}\n"
    "export function applyUserProperties(up) {\n"
    "  shared.order.push('A.props');\n"
    "  upSeen = up;\n"
    "}\n"
    "export function update(value) {\n"
    "  frames++;\n"
    "  shared.frameA = frames;\n"
    "  if (frames === 1) shared.order.push('A.update');\n"
    "  if (frames === 3) {\n"
    "    const u = new Group('options');\n"
    "    const P = engine.userProperties;\n"
    "    u.same('applyUserProperties got the options', upSeen, P);\n"
    "    u.same('checkbox', P.flag, false);\n"
    "    u.same('checkbox the user set', P.flag2, true);\n"
    "    u.same('slider, fractional default', P.speed, 0.5);\n"
    "    u.same('slider, whole default', P.amount, 3);\n"
    "    u.same('slider the user set', P.amount2, 7);\n"
    "    u.same('combo of numbers', P.mode, 2);\n"
    "    u.same('combo of text values', P.kind, 'b');\n"
    "    u.same('combo of text values the user set', P.kind2, 'c');\n"
    "    u.same('text', P.title, 'abc');\n"
    "    u.same('text that reads like a number', P.zip, '007');\n"
    "    u.same('text the user set to a number', P.title2, '12');\n"
    "    u.same('text the user set to true', P.title3, 'true');\n"
    "    u.same('colour', P.tint, new Vec3(1, 0.5, 0.25));\n"
    "    u.same('colour the user set', P.tint2, new Vec3(0, 0.5, 1));\n"
    "    u.done();\n"
    "    const o = new Group('order');\n"
    "    o.same('init, options, update', shared.order.join(' '), 'A.init B.init A.props B.props A.update B.update');\n"
    "    o.same('thisLayer in update', thisLayer, thisScene.getLayer('panel'));\n"
    "    o.same('thisLayer in a timer callback', timerLayer, thisScene.getLayer('panel'));\n"
    "    o.same('the later script wrote to shared', shared.fromB, 43);\n"
    "    o.same('... and saw this frame of the earlier script every time', shared.lagged, 0);\n"
    "    o.same('canvas width', engine.canvasSize.x, 320);\n"
    "    o.same('canvas height', engine.canvasSize.y, 180);\n"
    "    o.same('time of day in 0..1', engine.timeOfDay >= 0 && engine.timeOfDay < 1, true);\n"
    "    o.done();\n"
    "  }\n"
    "  return value;\n"
    "}\n";

/* on layer "child" (a later layer) */
static const char *SCRIPT_B =
    "'use strict';\n"
    "let frames = 0;\n"
    "export function init(value) {\n"
    "  shared.order.push('B.init');\n"
    "  shared.fromB = shared.fromA + 2;\n"
    "  shared.lagged = 0;\n"
    "  return value;\n"
    "}\n"
    "export function applyUserProperties(up) { shared.order.push('B.props'); }\n"
    "export function update(value) {\n"
    "  frames++;\n"
    "  if (frames === 1) shared.order.push('B.update');\n"
    "  if (shared.frameA !== frames) shared.lagged++;\n"
    "  return value;\n"
    "}\n";

static char *make_scene (const char *tmp)
{
    char *dir = g_build_filename (tmp, "scene", NULL);
    put (dir, "models/solid.json", "{\"solidlayer\":true}");
    put (dir, "particles/dust.json",
         "{\"emitter\":[{\"name\":\"boxrandom\",\"id\":1,\"distancemax\":\"20 20 0\",\"rate\":5}],"
         "\"initializer\":[{\"name\":\"lifetimerandom\",\"id\":2,\"min\":1,\"max\":1},"
         "{\"name\":\"sizerandom\",\"id\":3,\"min\":8,\"max\":8}],"
         "\"operator\":[{\"name\":\"movement\",\"id\":5}],"
         "\"renderer\":[{\"name\":\"sprite\",\"id\":6}],\"maxcount\":50}");
    char *a = walpe_json_escape (SCRIPT_A), *b = walpe_json_escape (SCRIPT_B);
    char *sc = g_strdup_printf (
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":180,\"width\":320}},"
        "\"objects\":["
        "{\"id\":1,\"name\":\"panel\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\","
        "\"size\":\"320 180\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0.1 0.2 0.3\","
        "\"alpha\":{\"script\":\"%s\",\"value\":1.0},\"visible\":true},"
        "{\"id\":2,\"name\":\"label\",\"text\":{\"value\":\"hello\"},\"font\":\"systemfont_arial\","
        "\"origin\":\"160 120 0\",\"size\":\"300 60\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\","
        "\"pointsize\":8,\"color\":\"1 1 1\",\"alpha\":1,\"visible\":true},"
        "{\"id\":3,\"name\":\"dust\",\"particle\":\"particles/dust.json\",\"origin\":\"80 40 0\","
        "\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"visible\":true,"
        "\"instanceoverride\":{\"id\":30,\"count\":2}},"
        "{\"id\":4,\"name\":\"child\",\"image\":\"models/solid.json\",\"origin\":\"40 40 0\","
        "\"size\":\"40 40\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"1 1 1\","
        "\"alpha\":{\"script\":\"%s\",\"value\":1.0},\"visible\":true}]}", a, b);
    put (dir, "scene.json", sc);
    g_free (sc);
    g_free (a);
    g_free (b);
    put (dir, "project.json",
         "{\"file\":\"scene.json\",\"title\":\"scene props test\",\"type\":\"scene\",\"general\":{\"properties\":{"
         "\"flag\":{\"type\":\"bool\",\"text\":\"Flag\",\"value\":false},"
         "\"flag2\":{\"type\":\"bool\",\"text\":\"Flag 2\",\"value\":false},"
         "\"speed\":{\"type\":\"slider\",\"text\":\"Speed\",\"min\":0,\"max\":4,\"value\":0.5},"
         "\"amount\":{\"type\":\"slider\",\"text\":\"Amount\",\"min\":0,\"max\":9,\"value\":3},"
         "\"amount2\":{\"type\":\"slider\",\"text\":\"Amount 2\",\"min\":0,\"max\":9,\"value\":3},"
         "\"mode\":{\"type\":\"combo\",\"text\":\"Mode\",\"value\":2,\"options\":["
         "{\"label\":\"one\",\"value\":1},{\"label\":\"two\",\"value\":2}]},"
         "\"kind\":{\"type\":\"combo\",\"text\":\"Kind\",\"value\":\"b\",\"options\":["
         "{\"label\":\"A\",\"value\":\"a\"},{\"label\":\"B\",\"value\":\"b\"},{\"label\":\"C\",\"value\":\"c\"}]},"
         "\"kind2\":{\"type\":\"combo\",\"text\":\"Kind 2\",\"value\":\"b\",\"options\":["
         "{\"label\":\"A\",\"value\":\"a\"},{\"label\":\"B\",\"value\":\"b\"},{\"label\":\"C\",\"value\":\"c\"}]},"
         "\"title\":{\"type\":\"textinput\",\"text\":\"Title\",\"value\":\"abc\"},"
         "\"zip\":{\"type\":\"textinput\",\"text\":\"Zip\",\"value\":\"007\"},"
         "\"title2\":{\"type\":\"textinput\",\"text\":\"Title 2\",\"value\":\"abc\"},"
         "\"title3\":{\"type\":\"textinput\",\"text\":\"Title 3\",\"value\":\"abc\"},"
         "\"tint\":{\"type\":\"color\",\"text\":\"Tint\",\"value\":\"1 0.5 0.25\"},"
         "\"tint2\":{\"type\":\"color\",\"text\":\"Tint 2\",\"value\":\"1 1 1\"}}}}");
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

/* the report of a group: its line, or NULL */
static const char *report (const char *group)
{
    gsize n = strlen (group);
    for (guint i = 0; i < reports->len; i++) {
        const char *r = g_ptr_array_index (reports, i);
        if (!strncmp (r, group, n) && r[n] == ':')
            return r;
    }
    return NULL;
}

static void group_ok (const char *group, int at_least)
{
    const char *r = report (group);
    int n = r ? atoi (r + strlen (group) + 2) : 0;
    CHECK (r && !strstr (r, "BAD") && n >= at_least, "scripts, %s: %s", group, r ? r : "(no report)");
}

static void props_checks (const char *tmp)
{
    char *dir = make_scene (tmp);
    /* what the user stored for this wallpaper (config.json keeps text) */
    WalpeConfig *cfg = walpe_config_load ();
    walpe_config_set_prop (cfg, dir, "flag2", "true");
    walpe_config_set_prop (cfg, dir, "amount2", "7");
    walpe_config_set_prop (cfg, dir, "kind2", "c");
    walpe_config_set_prop (cfg, dir, "title2", "12");
    walpe_config_set_prop (cfg, dir, "title3", "true");
    walpe_config_set_prop (cfg, dir, "tint2", "0 0.5 1");
    walpe_config_save (cfg);

    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    if (!c || (c->request_redraw = redraw_cb, !c->gl_init (c, &err))) {
        printf ("load failed: %s\n", err ? err->message : "?");
        CHECK (FALSE, "the scene loads");
        g_free (dir);
        walpe_config_free (cfg);
        return;
    }
    for (int i = 0; i < 5; i++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, W, H);
        c->next_frame_us = 0;
        c->surface_lost = FALSE;
        c->frame_unchanged = FALSE;
        c->gl_render (c, W, H, 0.0f, 0.0f);
        glFinish ();
        g_usleep (20000);
    }
    if (reports->len == 0 && script_errors == 0) {
        printf ("skip (no script ran: built without QuickJS?)\n");
    } else {
        CHECK (script_errors == 0, "no script error, no limit hit (%d)", script_errors);
        group_ok ("layer", 15);
        group_ok ("text", 15);
        group_ok ("particles", 14);
        group_ok ("options", 15);
        group_ok ("order", 8);
    }
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
    walpe_config_free (cfg);
    g_free (dir);
}

int main (void)
{
    setvbuf (stdout, NULL, _IONBF, 0);
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        printf ("skip (no EGL surfaceless platform)\n");
        return 0;
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
        return 0;
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
        return 0;
    }
    char *tmp = g_dir_make_tmp ("walpe-scene-props-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_PROP_PROBE");
    g_unsetenv ("WALPE_NO_SCRIPTS");
    reports = g_ptr_array_new_with_free_func (g_free);
    g_log_set_writer_func (log_writer, NULL, NULL);
    props_checks (tmp);
    g_ptr_array_unref (reports);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    rm_rf (tmp);
    g_free (tmp);
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
