/* particle-scene-test - particle objects of a scene and the user's options
 *. A scene made by the test (480 x 270, one unit = one
 * pixel) is loaded through the content API like a wallpaper and drawn into
 * a pbuffer with the virtual 30 fps scene clock:
 *
 *  1. the instance colour: "colorn" (normalized), the older "color" in
 *     0..255 (ignored by the earlier build: 1378563229's orange fireflies were white),
 *     "colorn" first when a scene has both;
 *  2. options that feed "count": a slider's default from project.json, the
 *     binding's own value when the option is not declared, an option whose
 *     value is no number (the earlier build: count 0, the system switched off);
 *  3. options that show / hide a particle object: a checkbox and a combo
 *     condition;
 *  4. the same after the user changed the options (config + reload, which
 *     is what `walpe-ctl setprop` does);
 *  5. (no GL) the note the options window shows for an option that only
 *     feeds the "count" of particle systems that are full at any setting
 *     (3163060610's "Count" slider on its dust), and for no other option.
 *
 * A "row" object emits 5 x count particles a second from one point, moving
 * right at 150 units/s: after 2 s they are 10 x count dots, 30 / count
 * pixels apart, and the test counts them.
 * Needs an EGL device or Mesa's surfaceless platform with a GL 3.3 pbuffer;
 * skipped otherwise. Works in a private temp dir that is also its HOME. */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/propnotes.h"
#include "common/config.h"
#include "common/util.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                 printf (__VA_ARGS__); printf ("\n"); } \
                           else { printf ("ok: "); printf (__VA_ARGS__); printf ("\n"); } } while (0)

enum { W = 480, H = 270 };

static void put_file (const char *dir, const char *rel, const void *data, gssize len)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    g_file_set_contents (p, data, len, NULL);
    g_free (d);
    g_free (p);
}

/* TEXV0005 container with one raw RGBA8888 image */
static void put_tex (const char *dir, const char *rel, int w, int h, guint32 rgba)
{
    GByteArray *o = g_byte_array_new ();
    guint32 hdr[7] = { 0, 0, (guint32) w, (guint32) h, (guint32) w, (guint32) h, 0 };
    guint32 n = (guint32) (w * h * 4);
    guint32 b1[2] = { 1, 0xffffffff }, mips = 1, m[5] = { (guint32) w, (guint32) h, 0, n, n };
    g_byte_array_append (o, (const guint8 *) "TEXV0005\0TEXI0001", 18);
    g_byte_array_append (o, (const guint8 *) hdr, sizeof hdr);
    g_byte_array_append (o, (const guint8 *) "TEXB0003", 9);
    g_byte_array_append (o, (const guint8 *) b1, sizeof b1);
    g_byte_array_append (o, (const guint8 *) &mips, 4);
    g_byte_array_append (o, (const guint8 *) m, sizeof m);
    for (guint32 i = 0; i < n / 4; i++)
        g_byte_array_append (o, (const guint8 *) &rgba, 4);
    put_file (dir, rel, o->data, o->len);
    g_byte_array_unref (o);
}

/* a particle object; io = its instanceoverride members, vis = its "visible" */
#define OBJ(id, name, file, x, y, io, vis) \
    "{\"id\":" #id ",\"name\":\"" name "\",\"particle\":\"particles/" file ".json\"," \
    "\"origin\":\"" #x " " #y " 0\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\"," \
    "\"instanceoverride\":{\"id\":" #id "0" io "},\"visible\":" vis "}"

static char *make_scene (const char *tmp)
{
    char *dir = g_build_filename (tmp, "scene", NULL);
    put_file (dir, "project.json",
              "{\"file\":\"scene.json\",\"title\":\"particle scene test\",\"type\":\"scene\","
              "\"general\":{\"properties\":{"
              "\"amount\":{\"type\":\"slider\",\"text\":\"Amount\",\"min\":0,\"max\":4,\"value\":3},"
              "\"label\":{\"type\":\"textinput\",\"text\":\"Label\",\"value\":\"abc\"},"
              "\"show\":{\"type\":\"bool\",\"text\":\"Show\",\"value\":false},"
              "\"mode\":{\"type\":\"combo\",\"text\":\"Mode\",\"value\":\"2\",\"options\":["
              "{\"label\":\"one\",\"value\":\"1\"},{\"label\":\"two\",\"value\":\"2\"}]}}}}", -1);
    put_tex (dir, "materials/particle/sq.tex", 16, 16, 0xffffffff);
    put_file (dir, "materials/particle/sq.json",
              "{\"passes\":[{\"shader\":\"genericparticle\",\"blending\":\"translucent\","
              "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\",\"cullmode\":\"nocull\","
              "\"textures\":[\"particle/sq\"]}]}", -1);
    /* one white square, 30 pixels, for good */
    put_file (dir, "particles/one.json",
              "{\"emitter\":[{\"name\":\"boxrandom\",\"id\":1,\"distancemax\":\"0 0 0\","
              "\"instantaneous\":1,\"rate\":0}],"
              "\"initializer\":[{\"name\":\"lifetimerandom\",\"id\":2,\"min\":640,\"max\":640},"
              "{\"name\":\"sizerandom\",\"id\":3,\"min\":60,\"max\":60}],"
              "\"operator\":[],\"renderer\":[{\"name\":\"sprite\",\"id\":6}],"
              "\"material\":\"materials/particle/sq.json\",\"maxcount\":1}", -1);
    /* a row of 4-pixel dots: 5 a second, 150 units/s to the right */
    put_file (dir, "particles/row.json",
              "{\"emitter\":[{\"name\":\"boxrandom\",\"id\":1,\"distancemax\":\"0 0 0\",\"rate\":5}],"
              "\"initializer\":[{\"name\":\"lifetimerandom\",\"id\":2,\"min\":600,\"max\":600},"
              "{\"name\":\"sizerandom\",\"id\":3,\"min\":8,\"max\":8},"
              "{\"name\":\"velocityrandom\",\"id\":4,\"min\":\"150 0 0\",\"max\":\"150 0 0\"}],"
              "\"operator\":[{\"name\":\"movement\",\"id\":5}],"
              "\"renderer\":[{\"name\":\"sprite\",\"id\":6}],"
              "\"material\":\"materials/particle/sq.json\",\"maxcount\":100}", -1);
    put_file (dir, "scene.json",
        "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
        "\"general\":{\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":270,\"width\":480}},"
        "\"objects\":["
        OBJ (11, "amount", "row", 60, 240, ",\"count\":{\"user\":\"amount\",\"value\":1}", "true") ","
        OBJ (12, "undeclared", "row", 60, 210, ",\"count\":{\"user\":\"nosuch\",\"value\":2}", "true") ","
        OBJ (13, "text", "row", 60, 180, ",\"count\":{\"user\":\"label\",\"value\":1}", "true") ","
        OBJ (14, "checkbox", "row", 60, 150, "", "{\"user\":\"show\",\"value\":true}") ","
        OBJ (15, "combo two", "row", 60, 120, "",
             "{\"user\":{\"condition\":\"2\",\"name\":\"mode\"},\"value\":true}") ","
        OBJ (16, "combo one", "row", 60, 90, "",
             "{\"user\":{\"condition\":\"1\",\"name\":\"mode\"},\"value\":true}") ","
        OBJ (21, "old colour", "one", 120, 40, ",\"color\":\"255 0 0\"", "true") ","
        OBJ (22, "colorn", "one", 240, 40, ",\"colorn\":\"0 0 1\"", "true") ","
        OBJ (23, "both", "one", 360, 40, ",\"colorn\":\"0 1 0\",\"color\":\"255 0 0\"", "true")
        "]}", -1);
    return dir;
}

typedef struct {
    WalpeContent *c;
    guint8 *px;
} Shot;

static gboolean shot_open (Shot *s, const char *dir)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = dir };
    memset (s, 0, sizeof *s);
    s->c = walpe_content_new (dir, &opts, &err);
    if (!s->c || !s->c->gl_init (s->c, &err)) {
        printf ("FAIL: scene %s: %s\n", dir, err ? err->message : "?");
        g_clear_error (&err);
        fails++;
        return FALSE;
    }
    s->px = g_malloc ((gsize) W * H * 4);
    return TRUE;
}

/* frame n of the scene (the virtual clock: (n - 1) / 30 s) */
static const guint8 *shot_frames (Shot *s, int n)
{
    for (int f = 0; f < n; f++) {
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, W, H);
        s->c->gl_render (s->c, W, H, 0.0f, 0.0f);
    }
    glFinish ();
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, s->px);
    return s->px;
}

static void shot_close (Shot *s)
{
    if (s->c) {
        if (s->c->gl_free)
            s->c->gl_free (s->c);
        s->c->free (s->c);
    }
    g_free (s->px);
    memset (s, 0, sizeof *s);
}

/* dots on pixel row y (GL rows: y up, as the scene): runs of lit pixels */
static int dots (const guint8 *px, int y)
{
    int n = 0;
    gboolean in = FALSE;
    for (int x = 0; x < W; x++) {
        gboolean lit = px[((gsize) y * W + x) * 4] > 100;
        n += lit && !in;
        in = lit;
    }
    return n;
}

static const guint8 *at (const guint8 *px, int x, int y)
{
    return px + ((gsize) y * W + x) * 4;
}

static void test_scene (WalpeConfig *cfg, const char *dir)
{
    Shot s;
    if (!shot_open (&s, dir))
        return;
    const guint8 *px = shot_frames (&s, 61);   /* 2 s */
    int n;

    /* 1. instance colour */
    const guint8 *c = at (px, 120, 40);
    CHECK (c[0] > 240 && c[1] < 15 && c[2] < 15,
           "instance \"color\": \"255 0 0\" (0..255, scenes older than \"colorn\"): %d %d %d, want red",
           c[0], c[1], c[2]);
    c = at (px, 240, 40);
    CHECK (c[0] < 15 && c[1] < 15 && c[2] > 240, "instance \"colorn\": \"0 0 1\": %d %d %d, want blue", c[0],
           c[1], c[2]);
    c = at (px, 360, 40);
    CHECK (c[0] < 15 && c[1] > 240 && c[2] < 15, "both given: \"colorn\" wins: %d %d %d, want green", c[0],
           c[1], c[2]);

    /* 2. options that feed count */
    n = dots (px, 240);
    CHECK (n >= 29 && n <= 31, "count bound to a slider, default 3: %d dots, want 30", n);
    n = dots (px, 210);
    CHECK (n >= 19 && n <= 21, "count bound to an option the project does not declare: its own value 2: "
           "%d dots, want 20", n);
    n = dots (px, 180);
    CHECK (n >= 9 && n <= 11, "count bound to an option that is text: not set, %d dots, want 10 (the earlier build: none)", n);

    /* 3. options that show / hide */
    n = dots (px, 150);
    CHECK (n == 0, "visible bound to a checkbox that is off: %d dots", n);
    n = dots (px, 120);
    CHECK (n >= 9 && n <= 11, "visible while the combo is \"2\" (it is): %d dots, want 10", n);
    n = dots (px, 90);
    CHECK (n == 0, "visible while the combo is \"1\" (it is \"2\"): %d dots", n);
    shot_close (&s);

    /* 4. the user changes the options: saved, then the scene is loaded again */
    walpe_config_set_prop (cfg, dir, "amount", "1");
    walpe_config_set_prop (cfg, dir, "show", "true");
    walpe_config_set_prop (cfg, dir, "mode", "1");
    walpe_config_set_prop (cfg, dir, "label", "2");
    if (!shot_open (&s, dir))
        return;
    px = shot_frames (&s, 61);
    n = dots (px, 240);
    CHECK (n >= 9 && n <= 11, "slider set to 1: %d dots, want 10", n);
    n = dots (px, 180);
    CHECK (n >= 19 && n <= 21, "text option set to \"2\": a number now, %d dots, want 20", n);
    n = dots (px, 150);
    CHECK (n >= 9 && n <= 11, "checkbox on: %d dots, want 10", n);
    n = dots (px, 120);
    CHECK (n == 0, "combo set to \"1\": the \"2\" object is hidden (%d dots)", n);
    n = dots (px, 90);
    CHECK (n >= 9 && n <= 11, "combo set to \"1\": the \"1\" object is shown (%d dots, want 10)", n);
    shot_close (&s);
}

/* 5. option notes (propnotes.h): no GL */
static void test_notes (const char *tmp)
{
    char *dir = g_build_filename (tmp, "notes", NULL);
    put_file (dir, "project.json",
              "{\"file\":\"scene.json\",\"title\":\"notes\",\"type\":\"scene\",\"general\":{\"properties\":{"
              "\"dust\":{\"type\":\"slider\",\"text\":\"Dust count\",\"min\":0.01,\"max\":4,\"value\":1},"
              "\"few\":{\"type\":\"slider\",\"text\":\"Sparks\",\"min\":0,\"max\":4,\"value\":1},"
              "\"both\":{\"type\":\"slider\",\"text\":\"Dust and size\",\"min\":0.01,\"max\":4,\"value\":1},"
              "\"scripted\":{\"type\":\"slider\",\"text\":\"Dust by script\",\"min\":0.01,\"max\":4,\"value\":1},"
              "\"idle\":{\"type\":\"bool\",\"text\":\"Nothing\",\"value\":false}}}}", -1);
    /* the dust of 3163060610: 100 a second for 10 - 25 s, at most 50 alive */
    put_file (dir, "particles/dust.json",
              "{\"emitter\":[{\"name\":\"boxrandom\",\"id\":1,\"distancemax\":\"200 100 0\",\"rate\":100}],"
              "\"initializer\":[{\"name\":\"lifetimerandom\",\"id\":2,\"min\":10,\"max\":25}],"
              "\"operator\":[],\"renderer\":[{\"name\":\"sprite\",\"id\":3}],\"maxcount\":50}", -1);
    /* sparks: 5 a second for 2 s, room for 100 */
    put_file (dir, "particles/sparks.json",
              "{\"emitter\":[{\"name\":\"boxrandom\",\"id\":1,\"distancemax\":\"200 100 0\",\"rate\":5}],"
              "\"initializer\":[{\"name\":\"lifetimerandom\",\"id\":2,\"min\":2,\"max\":2}],"
              "\"operator\":[],\"renderer\":[{\"name\":\"sprite\",\"id\":3}],\"maxcount\":100}", -1);
    put_file (dir, "scene.json",
        "{\"general\":{\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":{\"height\":270,\"width\":480}},"
        "\"objects\":["
        "{\"id\":1,\"name\":\"dust a\",\"particle\":\"particles/dust.json\",\"origin\":\"100 100 0\","
        "\"instanceoverride\":{\"id\":10,\"count\":{\"user\":\"dust\",\"value\":1}}},"
        "{\"id\":2,\"name\":\"dust b\",\"particle\":\"particles/dust.json\",\"origin\":\"300 100 0\","
        "\"instanceoverride\":{\"id\":20,\"count\":{\"user\":\"dust\",\"value\":1}}},"
        "{\"id\":3,\"name\":\"sparks\",\"particle\":\"particles/sparks.json\",\"origin\":\"200 200 0\","
        "\"instanceoverride\":{\"id\":30,\"count\":{\"user\":\"few\",\"value\":1}}},"
        "{\"id\":4,\"name\":\"dust c\",\"particle\":\"particles/dust.json\",\"origin\":\"100 200 0\","
        "\"instanceoverride\":{\"id\":40,\"count\":{\"user\":\"both\",\"value\":1},"
        "\"size\":{\"user\":\"both\",\"value\":1}}},"
        "{\"id\":5,\"name\":\"dust d\",\"particle\":\"particles/dust.json\",\"origin\":\"300 200 0\","
        "\"instanceoverride\":{\"id\":50,\"count\":{\"user\":\"scripted\",\"value\":1},"
        "\"alpha\":{\"script\":\"export function update(value) { return engine.userProperties.scripted; }\","
        "\"value\":1}}}"
        "]}", -1);
    JsonObject *n = walpe_prop_notes (dir);
    const char *dust = n ? wpj_str (n, "dust", NULL) : NULL;
    CHECK (!g_strcmp0 (dust, "full:50"),
           "an option that only feeds the count of systems that are always full: note \"%s\", want \"full:50\"",
           dust ? dust : "(none)");
    CHECK (n && !json_object_has_member (n, "few"), "a count that does thin its system out: no note");
    CHECK (n && !json_object_has_member (n, "both"), "bound to a full system's count and to its size: no note");
    CHECK (n && !json_object_has_member (n, "scripted"), "also read by a script: no note");
    CHECK (n && !g_strcmp0 (wpj_str (n, "idle", NULL), "unused"), "an option nothing refers to: \"unused\" as before");
    char *text = walpe_prop_note_text (dust);
    CHECK (text && strstr (text, "50") && strstr (text, "little or no visible effect"),
           "the sentence for the user: %s", text ? text : "(none)");
    g_free (text);
    if (n)
        json_object_unref (n);
    g_free (dir);
}

static gboolean run_gl (const char *tmp)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLint maj, min, nc = 0;
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
        EGLDisplay d = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize (d, &maj, &min))
            dpy = d;
    }
    EGLConfig ec;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &ec, 1, &nc) || nc < 1)
        return FALSE;
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, ec, EGL_NO_CONTEXT, ca);
    EGLSurface surf = ctx != EGL_NO_CONTEXT ? eglCreatePbufferSurface (dpy, ec, pa) : EGL_NO_SURFACE;
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return FALSE;
    }
    printf ("GL: %s\n", (const char *) glGetString (GL_RENDERER));
    walpe_glutil_init ();
    WalpeConfig *cfg = walpe_config_load ();
    char *dir = make_scene (tmp);
    test_scene (cfg, dir);
    g_free (dir);
    walpe_config_free (cfg);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

/* remove the temp dir (only what this test made lives in it) */
static void rm_tree (const char *path)
{
    if (!g_file_test (path, G_FILE_TEST_IS_SYMLINK) && g_file_test (path, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open (path, 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name (d))) {
            char *c = g_build_filename (path, n, NULL);
            rm_tree (c);
            g_free (c);
        }
        if (d)
            g_dir_close (d);
        g_rmdir (path);
    } else {
        g_unlink (path);
    }
}

int main (void)
{
    char *tmp = g_dir_make_tmp ("walpe-ptscene-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    char *probe = g_build_filename (tmp, "probe", NULL);
    g_mkdir_with_parents (probe, 0700);
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_setenv ("WALPE_PROP_PROBE", probe, TRUE);   /* the virtual 30 fps scene clock */
    g_unsetenv ("WALPE_SKIP_IDS");
    g_unsetenv ("WALPE_PT_DENSITY");
    g_unsetenv ("WALPE_PT_CAP");
    test_notes (tmp);
    if (!run_gl (tmp))
        printf ("skip the rendering checks (no EGL device / surfaceless platform with a GL 3.3 pbuffer)\n");
    rm_tree (tmp);
    g_free (probe);
    g_free (tmp);
    printf ("particle-scene: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
