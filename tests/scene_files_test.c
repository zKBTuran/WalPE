/* scene_files_test - what the text files of a scene wallpaper (project.json,
 * scene.json and the files they name) and the entries of its folder must not
 * be able to do. Every file is generated here.
 *
 *   scene-files-test [pkg|notes|text|particles|all]
 *
 *  pkg    the package of a scene wallpaper is a regular file of its folder:
 *         a FIFO called scene.pkg does not block the load, a symbolic link
 *         to a package somewhere else is not opened; a package of its own
 *         loads.
 *  notes  the scan behind the options window's hints (propnotes.h) reads the
 *         wallpaper's own .json files: not the ones behind a symbolic link
 *         that leaves the folder (a file, a directory, the package). Which
 *         options get the hint "unused", and that 30000 options do not take
 *         minutes.
 *  text   (needs a surfaceless GL 3.3 context, else skipped) the text layers
 *         of a scene together stay within the texture budget, in both scene
 *         layouts: the layers that fit are drawn, the others are not, and a
 *         text that changes every frame is counted once. A point size
 *         beyond any font size is refused without a complaint of the text
 *         library.
 *  particles  particle files: particles that sit on a particle of their
 *         parent system (eventfollow) stay on it while others die around it,
 *         and 20000 deaths a step with 40 such child systems do not take
 *         minutes; all particle systems together hold 400000 particles;
 *         a full system emits by the same rule as before and its emitters
 *         cost nothing while it is full.
 *
 * Checks that may not come back run in a child process with a deadline.
 * Private temp dir = HOME and the XDG dirs.
 * `meson test -C build scene-files` */
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/propnotes.h"
#include "engine/weparticles.h"
#include "common/util.h"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char *T;      /* temp root (realpath) */

#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

static char *P (const char *rel)
{
    return g_build_filename (T, rel, NULL);
}

static void mkfile (const char *rel, const void *data, gssize len)
{
    char *p = P (rel);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    if (!g_file_set_contents (p, data, len, NULL))
        g_error ("cannot write %s", p);
    g_free (d);
    g_free (p);
}

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

/* a scene package holding one file */
static void mkpkg (const char *rel, const char *name, const char *text)
{
    GByteArray *a = g_byte_array_new ();
    put32 (a, 8);
    g_byte_array_append (a, (const guint8 *) "PKGV0016", 8);
    put32 (a, 1);
    put32 (a, (guint32) strlen (name));
    g_byte_array_append (a, (const guint8 *) name, (guint) strlen (name));
    put32 (a, 0);
    put32 (a, (guint32) strlen (text));
    g_byte_array_append (a, (const guint8 *) text, (guint) strlen (text));
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
}

/* messages are counted, not printed: the hostile files produce many */
static int warnings, criticals;
static GString *logged;     /* warning texts (bounded), for the checks that look for one */
static GLogWriterOutput log_writer (GLogLevelFlags level, const GLogField *fields, gsize n,
                                    gpointer user)
{
    (void) user;
    if (!(level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)))
        return G_LOG_WRITER_HANDLED;
    warnings++;
    if (level & G_LOG_LEVEL_CRITICAL)
        criticals++;
    for (gsize i = 0; i < n && logged && logged->len < 256 * 1024; i++)
        if (!g_strcmp0 (fields[i].key, "MESSAGE") && fields[i].value) {
            if (fields[i].length < 0)
                g_string_append (logged, fields[i].value);
            else
                g_string_append_len (logged, fields[i].value, MIN (fields[i].length, 1024));
            g_string_append_c (logged, '\n');
        }
    return G_LOG_WRITER_HANDLED;
}

/* run fn (arg) in a child; its return value, or -1 when it is not back
 * after `secs` seconds, used more than `cpu` seconds of processor time
 * (0 = no limit; the child is killed in both cases) or died */
static int in_child (int (*fn) (const char *), const char *arg, int secs, int cpu)
{
    fflush (NULL);
    pid_t pid = fork ();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (cpu > 0) {
            struct rlimit rl = { (rlim_t) cpu, (rlim_t) cpu + 1 };
            setrlimit (RLIMIT_CPU, &rl);
        }
        _exit (fn (arg));
    }
    int st = 0;
    for (int ms = 0; ms < secs * 1000; ms += 20) {
        pid_t r = waitpid (pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED (st) ? WEXITSTATUS (st) : -1;
        g_usleep (20000);
    }
    kill (pid, SIGKILL);
    waitpid (pid, &st, 0);
    return -1;
}

/* an old-layout scene needs no GL context to be created: 10 = loaded */
static int load_scene (const char *dir)
{
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, NULL);
    if (!c)
        return 11;
    c->free (c);
    return 10;
}

/* --------------------------------------------------------------------- pkg */

#define OLD_SCENE "{\"object\":{\"width\":64,\"height\":64,\"layers\":[]}}"
#define PROJECT "{\"type\":\"scene\",\"file\":\"scene.json\"}"

static void test_pkg (void)
{
    char *fifo_dir = P ("pk/fifo"), *fifo = P ("pk/fifo/scene.pkg");
    char *link_dir = P ("pk/link"), *lnk = P ("pk/link/scene.pkg");
    char *own_dir = P ("pk/own"), *outside = P ("pk/outside/scene.pkg");
    mkfile ("pk/fifo/project.json", PROJECT, -1);
    mkfile ("pk/link/project.json", PROJECT, -1);
    mkfile ("pk/own/project.json", PROJECT, -1);
    mkpkg ("pk/outside/scene.pkg", "scene.json", OLD_SCENE);
    mkpkg ("pk/own/scene.pkg", "scene.json", OLD_SCENE);

    /* a package of the wallpaper's own loads */
    CHECK (in_child (load_scene, own_dir, 300, 0) == 10);
    if (mkfifo (fifo, 0600) != 0 || symlink (outside, lnk) != 0) {
        g_printerr ("scene-files-test: no FIFO / symbolic link here, that part is skipped\n");
    } else {
        /* a FIFO: nothing to load, and the answer comes */
        CHECK (in_child (load_scene, fifo_dir, 60, 0) == 11);
        /* a link to somebody else's package: not the wallpaper's */
        CHECK (in_child (load_scene, link_dir, 300, 0) == 11);
    }
    g_free (fifo_dir); g_free (fifo); g_free (link_dir); g_free (lnk);
    g_free (own_dir); g_free (outside);
}

/* ------------------------------------------------------------------- notes */

/* the note of option key at wallpaper dir ("" = none), g_free */
static char *note_of (const char *dir, const char *key)
{
    JsonObject *n = walpe_prop_notes (dir);
    char *v = g_strdup (n && json_object_has_member (n, key) ? json_object_get_string_member (n, key) : "");
    if (n)
        json_object_unref (n);
    return v;
}

#define OPT(k) "\"" k "\":{\"type\":\"slider\",\"text\":\"t\",\"value\":1,\"min\":0,\"max\":2}"
#define BIND(k) "{\"x\":{\"user\":\"" k "\",\"value\":1}}"

/* many options, each with a condition: 21 = the scan came back with a hint */
static int notes_many (const char *dir)
{
    char *n = note_of (dir, "p7");
    int r = !strcmp (n, "unused") ? 21 : 20;
    g_free (n);
    return r;
}

/* which options get the hint "unused": the rules, and the cost with a
 * project.json that declares tens of thousands of options */
static void notes_rules (void)
{
    static const struct { const char *key, *want; } R[] = {
        { "plain", "unused" },       /* nothing names it */
        { "shown", "" },             /* another option's condition does */
        { "self", "unused" },        /* only its own condition does */
        { "sho", "unused" },         /* "shown" is another word */
        { "read", "" },              /* a script reads it: x.read */
        { "quoted", "" },            /* ... or names it as a string */
        { "word", "unused" },        /* a bare word in a comment is no read */
        { "my-opt", "" },            /* a key that is no identifier, in a condition */
        { "odd key", "unused" },     /* ... and one nothing names */
        { "a.b", "" },               /* ... and one a script names as a string */
    };
    GString *pj = g_string_new ("{\"type\":\"scene\",\"file\":\"scene.json\",\"general\":{\"properties\":{");
    for (guint i = 0; i < G_N_ELEMENTS (R); i++)
        g_string_append_printf (pj, "\"%s\":{\"type\":\"bool\",\"text\":\"t\",\"value\":true%s},", R[i].key,
                                !strcmp (R[i].key, "self") ? ",\"condition\":\"self.value\"" :
                                !strcmp (R[i].key, "plain") ? ",\"condition\":\"shown.value && my-opt.value\"" : "");
    g_string_append (pj, "\"grp\":{\"type\":\"group\",\"text\":\"g\"}}}}");
    mkfile ("nr/wp/project.json", pj->str, -1);
    g_string_free (pj, TRUE);
    mkfile ("nr/wp/scene.json",
            "{\"object\":{\"layers\":[]},\"general\":{\"x\":{\"script\":"
            "\"// word\\nlet v = engine.userProperties.read; let w = p['quoted'] + p[\\\"a.b\\\"];\","
            "\"value\":1}}}", -1);
    char *wp = P ("nr/wp");
    for (guint i = 0; i < G_N_ELEMENTS (R); i++) {
        char *n = note_of (wp, R[i].key);
        if (strcmp (n, R[i].want)) {
            g_printerr ("FAIL note of option \"%s\": \"%s\", expected \"%s\"\n", R[i].key, n, R[i].want);
            failures++;
        }
        g_free (n);
    }
    g_free (wp);

    /* 30000 options p0 .. p29999; the condition of each names the next one
     * but one, except p5's, which names p5 itself: p7 is named by nobody and
     * gets the hint. Options x conditions used to be searched: minutes of
     * processor time, here 20 s are allowed. */
    pj = g_string_new ("{\"type\":\"scene\",\"file\":\"scene.json\",\"general\":{\"properties\":{");
    for (int i = 0; i < 30000; i++)
        g_string_append_printf (pj, "\"p%d\":{\"type\":\"slider\",\"text\":\"t\",\"value\":1,"
                                "\"condition\":\"p%d.value == 1\"},", i, i == 5 ? 5 : (i + 2) % 30000);
    g_string_append (pj, "\"lone\":{\"type\":\"slider\",\"text\":\"t\",\"value\":1}}}}");
    mkfile ("nr/many/project.json", pj->str, -1);
    g_string_free (pj, TRUE);
    mkfile ("nr/many/scene.json", OLD_SCENE, -1);
    char *many = P ("nr/many");
    CHECK (in_child (notes_many, many, 900, 20) == 21);
    g_free (many);
}

static void test_notes (void)
{
    /* options a .. d: nothing in the wallpaper uses a, b or c; d is bound in
     * a file of its own that is reached through a link inside the folder */
    mkfile ("nt/wp/project.json",
            "{\"type\":\"scene\",\"file\":\"scene.json\",\"general\":{\"properties\":{"
            OPT ("a") "," OPT ("b") "," OPT ("c") "," OPT ("d") "}}}", -1);
    mkfile ("nt/wp/scene.json", OLD_SCENE, -1);
    mkfile ("nt/wp/sub/own.json", BIND ("d"), -1);
    mkfile ("nt/outside/a.json", BIND ("a"), -1);
    mkfile ("nt/outside/deep/b.json", BIND ("b"), -1);
    char *wp = P ("nt/wp"), *l1 = P ("nt/wp/leak.json"), *l2 = P ("nt/wp/linkdir"),
         *l3 = P ("nt/wp/inner.json"), *l4 = P ("nt/wp/again");
    char *o1 = P ("nt/outside/a.json"), *o2 = P ("nt/outside");
    /* a second wallpaper whose package is somebody else's */
    mkfile ("nt/wp2/project.json",
            "{\"type\":\"scene\",\"file\":\"scene.json\",\"general\":{\"properties\":{" OPT ("a") "}}}", -1);
    mkpkg ("nt/outside/scene.pkg", "scene.json", "{\"objects\":[]}");
    char *wp2 = P ("nt/wp2"), *l5 = P ("nt/wp2/scene.pkg"), *o5 = P ("nt/outside/scene.pkg");
    if (symlink (o1, l1) != 0 || symlink (o2, l2) != 0 || symlink ("sub/own.json", l3) != 0 ||
        symlink (".", l4) != 0 || symlink (o5, l5) != 0) {
        g_printerr ("scene-files-test: no symbolic links here, notes part skipped\n");
    } else {
        char *n;
        /* bound only outside the wallpaper: unused here */
        n = note_of (wp, "a");
        CHECK (!strcmp (n, "unused"));
        g_free (n);
        n = note_of (wp, "b");
        CHECK (!strcmp (n, "unused"));
        g_free (n);
        n = note_of (wp, "c");
        CHECK (!strcmp (n, "unused"));
        g_free (n);
        /* bound in the wallpaper's own file: no note */
        n = note_of (wp, "d");
        CHECK (!*n);
        g_free (n);
        /* a package that is not the wallpaper's is not scanned: nothing is
         * known about the scene, so nothing is said */
        JsonObject *o = walpe_prop_notes (wp2);
        CHECK (o == NULL);
        if (o)
            json_object_unref (o);
    }
    notes_rules ();
    g_free (wp); g_free (l1); g_free (l2); g_free (l3); g_free (l4); g_free (o1); g_free (o2);
    g_free (wp2); g_free (l5); g_free (o5);
}

/* -------------------------------------------------------------------- text */

#define VW 640
#define VH 360

static gboolean gl_up (void)
{
    if (!epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        return FALSE;
    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1)
        return FALSE;
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, VW, EGL_HEIGHT, VH, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    return ctx != EGL_NO_CONTEXT && surf != EGL_NO_SURFACE && eglMakeCurrent (dpy, surf, surf, ctx);
}

static void redraw_cb (gpointer u) { (void) u; }

static WalpeContent *open_scene (const char *dir)
{
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = dir, .render_scale = 100 };
    WalpeContent *c = walpe_content_new (dir, &opts, NULL);
    if (!c)
        return NULL;
    c->request_redraw = redraw_cb;
    if (!c->gl_init (c, NULL)) {
        c->free (c);
        return NULL;
    }
    return c;
}

static void draw (WalpeContent *c)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, VW, VH);
    c->next_frame_us = 0;
    c->surface_lost = TRUE;
    c->frame_unchanged = FALSE;
    c->gl_render (c, VW, VH, 0.0f, 0.0f);
    glFinish ();
}

/* is anything drawn within 24 pixels of (x, y) of the window */
static gboolean ink_at (int x, int y)
{
    guint8 px[48 * 48 * 4];
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (x - 24, y - 24, 48, 48, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (gsize i = 0; i < sizeof px; i += 4)
        if (px[i] > 64 || px[i + 1] > 64 || px[i + 2] > 64)
            return TRUE;
    return FALSE;
}

/* is anything drawn in the window at all */
static gboolean ink_any (void)
{
    guint8 *px = g_malloc (VW * VH * 4);
    gboolean any = FALSE;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, VW, VH, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (gsize i = 0; i < VW * VH * 4 && !any; i += 4)
        any = px[i] > 64 || px[i + 1] > 64 || px[i + 2] > 64;
    g_free (px);
    return any;
}

static void test_text (void)
{
    if (!gl_up ()) {
        g_printerr ("scene-files-test: no surfaceless GL 3.3 context, text part skipped\n");
        return;
    }
    walpe_glutil_init ();
    /* six text layers, each in a box of 2048 x 2048 pixels (16 MiB as a
     * texture), 100 scene pixels apart; the first one's text is a script's
     * and changes with every frame. With 40 MiB for the scene's textures
     * two of them fit. */
    GString *sc = g_string_new ("{\"general\":{\"orthogonalprojection\":{\"width\":640,\"height\":360}},"
                                "\"objects\":[");
    for (int i = 0; i < 6; i++)
        g_string_append_printf (sc, "%s{\"id\":%d,\"name\":\"t%d\",\"origin\":\"%d 180 0\",\"size\":\"2048 2048\","
                                "\"pointsize\":6,\"font\":\"systemfont_arial\",\"text\":%s}", i ? "," : "",
                                i + 1, i, 70 + i * 100,
                                i ? "\"X\""
                                  : "{\"script\":\"'use strict';\\nlet n = 0;\\nexport function update(value) "
                                    "{ n += 1; return 'X' + (n % 10); }\\n\",\"value\":\"X\"}");
    g_string_append (sc, "]}");
    mkfile ("tx/new/project.json", PROJECT, -1);
    mkfile ("tx/new/scene.json", sc->str, -1);
    g_string_free (sc, TRUE);
    char *dir = P ("tx/new");
    g_setenv ("WALPE_TEX_BUDGET_MB", "40", TRUE);
    g_string_truncate (logged, 0);
    WalpeContent *c = open_scene (dir);
    CHECK (c != NULL);
    if (c) {
        for (int f = 0; f < 12; f++)
            draw (c);
        CHECK (ink_at (70, 180));
        CHECK (ink_at (170, 180));
        for (int i = 2; i < 6; i++)
            CHECK (!ink_at (70 + i * 100, 180));
        CHECK (strstr (logged->str, "is not drawn") != NULL);
        c->gl_free (c);
        c->free (c);
    }
    /* no budget: all of them */
    g_setenv ("WALPE_TEX_BUDGET_MB", "0", TRUE);
    c = open_scene (dir);
    CHECK (c != NULL);
    if (c) {
        draw (c);
        draw (c);
        for (int i = 0; i < 6; i++)
            CHECK (ink_at (70 + i * 100, 180));
        c->gl_free (c);
        c->free (c);
    }
    g_free (dir);

    /* the old layout: a text layer is as large as its text; eight of about
     * 9 MiB each against 40 MiB */
    sc = g_string_new ("{\"object\":{\"width\":640,\"height\":360,\"layers\":[");
    for (int i = 0; i < 8; i++)
        g_string_append_printf (sc, "%s{\"text\":{\"text\":\"WW\",\"size\":1000,\"font\":\"Sans\"}}",
                                i ? "," : "");
    g_string_append (sc, "]}}");
    mkfile ("tx/old/project.json", PROJECT, -1);
    mkfile ("tx/old/scene.json", sc->str, -1);
    g_string_free (sc, TRUE);
    dir = P ("tx/old");
    g_setenv ("WALPE_TEX_BUDGET_MB", "40", TRUE);
    g_string_truncate (logged, 0);
    c = open_scene (dir);
    CHECK (c != NULL);
    if (c) {
        draw (c);
        CHECK (ink_any ());    /* (the first ones are there) */
        CHECK (strstr (logged->str, "is not drawn") != NULL);
        c->gl_free (c);
        c->free (c);
    }
    g_unsetenv ("WALPE_TEX_BUDGET_MB");
    g_free (dir);

    /* point sizes no font size holds (Pango counts in 1/1024 pixels, in an
     * int): the layers are not drawn, an ordinary one next to them is, and
     * no library is handed a negative size (it said so twice per layout) */
    static const char *const sizes[] = { "600000", "1e9", "1e30", "1e999", "\"inf\"", "\"1e39\"" };
    sc = g_string_new ("{\"general\":{\"orthogonalprojection\":{\"width\":640,\"height\":360}},\"objects\":["
                       "{\"id\":1,\"name\":\"ok\",\"origin\":\"70 180 0\",\"pointsize\":6,"
                       "\"font\":\"systemfont_arial\",\"text\":\"X\"}");
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
        g_string_append_printf (sc, ",{\"id\":%u,\"name\":\"big%u\",\"origin\":\"%u 180 0\",\"pointsize\":%s,"
                                "\"font\":\"systemfont_arial\",\"text\":\"X\"}", i + 2, i, 170 + i * 80, sizes[i]);
    g_string_append (sc, "]}");
    mkfile ("tx/points/project.json", PROJECT, -1);
    mkfile ("tx/points/scene.json", sc->str, -1);
    g_string_free (sc, TRUE);
    dir = P ("tx/points");
    criticals = 0;
    c = open_scene (dir);
    CHECK (c != NULL);
    if (c) {
        draw (c);
        draw (c);
        CHECK (ink_at (70, 180));
        for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
            CHECK (!ink_at (170 + (int) i * 80, 180));
        c->gl_free (c);
        c->free (c);
    }
    CHECK (criticals == 0);
    g_free (dir);
}

/* --------------------------------------------------------------- particles */

/* the particle system of file `name` below dir (no GL) */
static WalpeWeParticles *pt_load (const char *dir, const char *name)
{
    char *p = g_build_filename (dir, name, NULL);
    JsonNode *j = wpj_parse_file (p);
    g_free (p);
    if (!j)
        return NULL;
    WalpeWeParticleOverride ov;
    walpe_we_particle_override_default (&ov);
    WalpeWeParticles *w = walpe_weparticles_new (json_node_get_object (j), dir, NULL, &ov);
    json_node_unref (j);
    return w;
}

/* 20000 particles that live for a hundredth of a second, each with one
 * particle of each of 40 child systems on it: 31 = six steps were made */
static int pt_many_deaths (const char *dir)
{
    WalpeWeParticles *w = pt_load (dir, "burn.json");
    if (!w)
        return 30;
    for (int i = 0; i < 6; i++)
        walpe_weparticles_step (w, 1.0 / 30.0);
    int ok = walpe_weparticles_sys_count (w, 1) > 1000;   /* (the followers outlive their particles) */
    walpe_weparticles_free (w);
    return ok ? 31 : 30;
}

/* 128 systems of four emitters each that are full from the first step on
 * and go on asking: 32 = two thousand steps were made */
static int pt_full_emitters (const char *dir)
{
    WalpeWeParticles *w = pt_load (dir, "top.json");
    if (!w)
        return 30;
    for (int i = 0; i < 2000; i++)
        walpe_weparticles_step (w, 1.0 / 30.0);
    int ok = walpe_weparticles_systems (w) == 128 && walpe_weparticles_count (w) == 128;
    walpe_weparticles_free (w);
    return ok ? 32 : 30;
}

/* One emitter at `rate` particles a second into a system that holds `cap`
 * particles of `life` seconds: the emission as weparticles.c defines it,
 * step by step - what is left of the accumulator when the system is full
 * decides when the next particle comes. The engine's particles must have
 * these ages, in this order, after every step. */
static void pt_full_reference (const char *dir, const char *file, float rate, int cap, float life)
{
    char *js = g_strdup_printf ("{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":%.9g,\"distancemax\":\"9 9 0\"}],"
                                "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":%.9g,\"max\":%.9g}],"
                                "\"operator\":[],\"renderer\":[],\"maxcount\":%d}",
                                (double) rate, (double) life, (double) life, cap);
    char *rel = g_build_filename ("pt", file, NULL);
    mkfile (rel, js, -1);
    g_free (rel);
    g_free (js);
    WalpeWeParticles *w = pt_load (dir, file);
    CHECK (w != NULL);
    const double dt = 1.0 / 30.0;
    const float sdt = (float) dt;
    double acc = 0;
    float age[64], first[64];
    int n = 0, bad = 0, refused = 0, born = 0;
    for (int step = 0; w && step < 400; step++) {
        walpe_weparticles_step (w, dt);
        acc += rate * dt;
        int guard = 0;
        while (acc >= 1.0 && guard++ < 10000) {
            acc -= 1.0;
            if (n >= cap) {
                refused++;
                continue;
            }
            age[n] = 0;
            first[n] = (float) CLAMP (acc / rate, 1e-6, dt);
            n++;
            born++;
        }
        if (acc > 1.0)
            acc = 0;
        for (int i = 0; i < n; i++) {
            float fdt = sdt;
            if (first[i] > 0) {
                fdt = MIN (first[i], sdt);
                first[i] = 0;
            }
            age[i] += fdt;
            if (age[i] >= life) {
                n--;
                age[i] = age[n];
                first[i] = first[n];
                i--;
            }
        }
        bad += walpe_weparticles_count (w) != n;
        for (int i = 0; i < n && !bad; i++) {
            float pos[3], vel[3], a = -1;
            walpe_weparticles_peek (w, i, pos, vel, &a);
            bad += memcmp (&a, &age[i], sizeof a) != 0;
        }
    }
    if (bad || refused < 50 || born < 50) {
        g_printerr ("FAIL %s: %d steps differ from the reference (%d particles born, %d refused)\n",
                    file, bad, born, refused);
        failures++;
    }
    walpe_weparticles_free (w);
}

static void test_particles (void)
{
    /* 1. a system whose particles move and die at different ages; each
     * gets one particle of the child system "rider" when it is born, which
     * sits on it and outlives it */
    mkfile ("pt/host.json",
            "{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":1000,\"distancemax\":\"300 300 0\"}],"
            "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":0.05,\"max\":0.6},"
            "{\"name\":\"velocityrandom\",\"min\":\"-100 -100 0\",\"max\":\"100 100 0\"}],"
            "\"operator\":[],\"renderer\":[],\"maxcount\":2000,"
            "\"children\":[{\"name\":\"rider.json\",\"type\":\"eventfollow\"}]}", -1);
    mkfile ("pt/rider.json",
            "{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":0,\"instantaneous\":1,\"distancemax\":\"0 0 0\"}],"
            "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":0.7,\"max\":0.7}],"
            "\"operator\":[],\"renderer\":[],\"maxcount\":1}", -1);
    char *dir = P ("pt");
    WalpeWeParticles *w = pt_load (dir, "host.json");
    CHECK (w != NULL);
    int lost = 0, seen = 0, most = 0;
    for (int step = 0; w && step < 60; step++) {
        walpe_weparticles_step (w, 1.0 / 30.0);
        int nh = walpe_weparticles_sys_count (w, 0), nr = walpe_weparticles_sys_count (w, 1);
        float *rp = g_new (float, 3 * MAX (nr, 1));
        float vel[3], age;
        for (int k = 0; k < nr; k++)
            walpe_weparticles_sys_peek (w, 1, k, rp + 3 * k, vel, &age);
        for (int i = 0; i < nh; i++) {
            float hp[3];
            gboolean found = FALSE;
            walpe_weparticles_sys_peek (w, 0, i, hp, vel, &age);
            for (int k = 0; k < nr && !found; k++)
                found = !memcmp (hp, rp + 3 * k, sizeof hp);
            lost += !found;
            seen++;
        }
        most = MAX (most, nh);
        g_free (rp);
    }
    /* particles came, went (the system never fills), and each had its rider on it */
    CHECK (seen > 1000 && most > 100 && most < 1000);
    CHECK (lost == 0);
    walpe_weparticles_free (w);

    /* 2. the cost of many deaths under many followers (it was deaths x followers) */
    GString *burn = g_string_new ("{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":1000000000,"
                                  "\"instantaneous\":1000000,\"distancemax\":\"300 300 0\"}],"
                                  "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":0.01,\"max\":0.01}],"
                                  "\"operator\":[],\"renderer\":[],\"maxcount\":20000,\"children\":[");
    for (int i = 0; i < 40; i++)
        g_string_append_printf (burn, "%s{\"name\":\"ash.json\",\"type\":\"eventfollow\"}", i ? "," : "");
    g_string_append (burn, "]}");
    mkfile ("pt/burn.json", burn->str, -1);
    g_string_free (burn, TRUE);
    mkfile ("pt/ash.json",
            "{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":0,\"instantaneous\":1,\"distancemax\":\"0 0 0\"}],"
            "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":1000,\"max\":1000}],"
            "\"operator\":[],\"renderer\":[],\"maxcount\":1}", -1);
    CHECK (in_child (pt_many_deaths, dir, 900, 15) == 31);

    /* 3. all systems together hold 400000 particles: 30 particle objects
     * that each want 20000 for good get that many between them, and what
     * a freed object held is free again */
    mkfile ("pt/full.json",
            "{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":1000000000,\"instantaneous\":1000000,"
            "\"distancemax\":\"300 300 0\"}],"
            "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":1000,\"max\":1000}],"
            "\"operator\":[],\"renderer\":[],\"maxcount\":20000}", -1);
    WalpeWeParticles *obj[30];
    int total = 0;
    for (int i = 0; i < 30; i++) {
        obj[i] = pt_load (dir, "full.json");
        CHECK (obj[i] != NULL);
        for (int k = 0; obj[i] && k < 3; k++)
            walpe_weparticles_step (obj[i], 1.0 / 30.0);
        total += walpe_weparticles_count (obj[i]);
    }
    CHECK (walpe_weparticles_count (obj[0]) == 20000);
    CHECK (total == 400000);
    walpe_weparticles_set_playing (obj[1], -1);    /* stop (): its particles go */
    for (int i = 0; i < 30; i++)
        if (i != 1)
            walpe_weparticles_free (obj[i]);
    walpe_weparticles_free (obj[1]);
    w = pt_load (dir, "full.json");
    for (int k = 0; w && k < 3; k++)
        walpe_weparticles_step (w, 1.0 / 30.0);
    CHECK (walpe_weparticles_count (w) == 20000);
    walpe_weparticles_free (w);

    /* 4. emitters of a full system: the particles come exactly when the
     * emission rule says, and asking a full system costs next to nothing
     * (it was 10000 refused calls per emitter and step) */
    pt_full_reference (dir, "ref1.json", 45.0f, 1, 0.1f);
    pt_full_reference (dir, "ref2.json", 100.25f, 3, 0.25f);
    pt_full_reference (dir, "ref3.json", 1000.0f, 7, 0.07f);
    pt_full_reference (dir, "ref4.json", 31.0f, 2, 0.5f);
    {
        const char *em = "{\"name\":\"boxrandom\",\"rate\":1000000000,\"distancemax\":\"9 9 0\"}";
        const char *files[3] = { "pt/leaf.json", "pt/mid.json", "pt/top.json" };
        for (int f = 0; f < 3; f++) {
            GString *g = g_string_new ("{\"emitter\":[");
            for (int i = 0; i < 4; i++)
                g_string_append_printf (g, "%s%s", i ? "," : "", em);
            g_string_append (g, "],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":100000,\"max\":100000}],"
                                "\"operator\":[],\"renderer\":[],\"maxcount\":1,\"children\":[");
            for (int i = 0; f > 0 && i < 40; i++)
                g_string_append_printf (g, "%s{\"name\":\"%s\",\"type\":\"static\"}", i ? "," : "",
                                        files[f - 1] + 3);
            g_string_append (g, "]}");
            mkfile (files[f], g->str, -1);
            g_string_free (g, TRUE);
        }
        CHECK (in_child (pt_full_emitters, dir, 900, 15) == 32);
    }
    g_free (dir);
}

int main (int argc, char **argv)
{
    char *tmp = g_dir_make_tmp ("walpe-scene-files-test-XXXXXX", NULL);
    if (!tmp)
        g_error ("no temp dir");
    T = realpath (tmp, NULL);
    g_free (tmp);
    char *home = P ("home"), *cfg = P ("xdg-config"), *cache = P ("xdg-cache"),
         *data = P ("xdg-data");
    g_setenv ("HOME", home, TRUE);
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_setenv ("XDG_DATA_HOME", data, TRUE);
    g_setenv ("WALPE_NO_AUDIO", "1", TRUE);
    g_setenv ("WALPE_NO_MEDIA", "1", TRUE);
    g_mkdir_with_parents (home, 0755);
    logged = g_string_new (NULL);
    g_log_set_writer_func (log_writer, NULL, NULL);

    const char *only = argc > 1 && strcmp (argv[1], "all") ? argv[1] : NULL;
    if (!only || !strcmp (only, "pkg"))
        test_pkg ();
    if (!only || !strcmp (only, "notes"))
        test_notes ();
    if (!only || !strcmp (only, "text"))
        test_text ();
    if (!only || !strcmp (only, "particles"))
        test_particles ();

    g_printerr ("%s (%d failure%s, %d warnings logged)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s", warnings);
    char *rm[] = { "rm", "-rf", T, NULL };
    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (home);
    g_free (cfg);
    g_free (cache);
    g_free (data);
    free (T);
    return failures ? 1 : 0;
}
