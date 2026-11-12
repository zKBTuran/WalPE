/* fuzz_scenefile - any file of a scene wallpaper, one to four at a time,
 * through the loader and two frames of the renderer: scene.json,
 * project.json, model / material / effect / particle files, shaders,
 * textures, a puppet model, a sound file (src/engine/content.c, scene.c,
 * scenenode*.c, weparticles.c, puppet.c, common/tex.c, ...).
 *
 * The wallpaper's files are a fixed set, written once when the harness
 * starts (FUZZ_ASSETS, an archive as in fuzz_archive.h:
 * tests/fuzz/seeds/scenefile.assets) and only read afterwards. The
 * input is an archive of the same form with up to four files of that set:
 * for the time of the input the engine finds the input's bytes under those
 * names. Nothing is written for that - open () and fopen () are defined
 * here and hand out a memory file for the names of the input (the engine
 * and GLib both end up in them), so this harness is as fast as the parsers
 * and can run where the multi-file harness (fuzz_scene.c, which writes its
 * files) cannot. A name that is not in the set is ignored.
 * Work per input: when the input brings no scene.json of its own, the scene
 * is one that holds only the layers its files are about (variants/<kind>.json
 * of the set: a particle file is loaded by a scene with the particle layer
 * alone, a shader by one layer with that effect) - the whole scene with its
 * text layer costs twenty times as much per input.
 * OpenGL is the stand-in of fakegl.c. Scripts: see fuzz_scene.c. */
#include "fuzz_archive.h"
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/propnotes.h"
#include <dlfcn.h>
#include <epoxy/gl.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef FUZZ_ASSETS
#error "FUZZ_ASSETS: the path of the archive with the scene's files"
#endif

#define SLOTS 4               /* files of the input */
#define OVER (SLOTS + 1)       /* + the scene chosen for them */
static const char *dir;
static gsize dir_len;
static char *over_name[OVER];       /* absolute, canonical; NULL = slot not used */
static const char *over_file[OVER];
static char *variant_file;
static GHashTable *known;           /* the names of the set (relative) */

/* the file to open for path: the input's, when path names one of its files */
static const char *overlay (const char *path)
{
    if (!path || !dir || strncmp (path, dir, dir_len))
        return path;
    char *canon = strstr (path, "//") || strstr (path, "/.") ? g_canonicalize_filename (path, NULL) : NULL;
    const char *p = canon ? canon : path, *hit = path;
    for (int i = 0; i < OVER; i++)
        if (over_name[i] && !strcmp (p, over_name[i]))
            hit = over_file[i];
    g_free (canon);
    return hit;
}

#define REAL(ret, name, ...) \
    static ret (*real) (__VA_ARGS__); \
    if (!real) \
        real = (ret (*) (__VA_ARGS__)) dlsym (RTLD_NEXT, name)

/* (the C names are other ones than the symbols: with _FILE_OFFSET_BITS=64
 * the headers already name open () open64 () and fopen () fopen64 ()) */
#define EXPORT(sym) __asm__ (sym) __attribute__ ((visibility ("default")))
int fz_open (const char *path, int flags, ...) EXPORT ("open");
int fz_open64 (const char *path, int flags, ...) EXPORT ("open64");
int fz_open_2 (const char *path, int flags) EXPORT ("__open_2");
int fz_open64_2 (const char *path, int flags) EXPORT ("__open64_2");
FILE *fz_fopen (const char *path, const char *mode) EXPORT ("fopen");
FILE *fz_fopen64 (const char *path, const char *mode) EXPORT ("fopen64");

static mode_t mode_arg (int flags, va_list ap)
{
    return flags & (O_CREAT | O_TMPFILE) ? (mode_t) va_arg (ap, int) : 0;
}

int fz_open (const char *path, int flags, ...)
{
    va_list ap;
    va_start (ap, flags);
    mode_t mode = mode_arg (flags, ap);
    va_end (ap);
    REAL (int, "open", const char *, int, ...);
    return real (overlay (path), flags, mode);
}

int fz_open64 (const char *path, int flags, ...)
{
    va_list ap;
    va_start (ap, flags);
    mode_t mode = mode_arg (flags, ap);
    va_end (ap);
    REAL (int, "open64", const char *, int, ...);
    return real (overlay (path), flags, mode);
}

/* (what open () is called as from code built with _FORTIFY_SOURCE) */
int fz_open_2 (const char *path, int flags)
{
    REAL (int, "__open_2", const char *, int);
    return real (overlay (path), flags);
}

int fz_open64_2 (const char *path, int flags)
{
    REAL (int, "__open64_2", const char *, int);
    return real (overlay (path), flags);
}

FILE *fz_fopen (const char *path, const char *mode)
{
    REAL (FILE *, "fopen", const char *, const char *);
    return real (overlay (path), mode);
}

FILE *fz_fopen64 (const char *path, const char *mode)
{
    REAL (FILE *, "fopen64", const char *, const char *);
    return real (overlay (path), mode);
}

static void redraw (gpointer user) { (void) user; }

/* which small scene loads a file of the set (the first match) */
static const struct { const char *prefix, *variant; } scene_for[] = {
    { "particles/", "particle" }, { "materials/p.json", "particle" },
    { "effects/fx2/", "fx2" }, { "materials/effects/fx2.", "fx2" }, { "shaders/effects/fx2.", "fx2" },
    { "effects/fx/", "fx" }, { "materials/effects/fx.", "fx" }, { "shaders/effects/fx.", "fx" },
    { "models/a", "image" }, { "materials/a.", "image" },
    { "models/d.json", "dxt" }, { "materials/d.", "dxt" }, { "materials/m.tex", "mask" },
    { "sounds/", "sound" }, { "models/pass.json", "pass" }, { "project.json", "options" },
};

/* the input's files: each named one of the set gets a memory file */
static int take (const uint8_t *data, size_t size)
{
    static const char mark[] = "=== ";
    const uint8_t *end = data + size, *p = data;
    int n = 0;
    const char *variant = NULL;
    gboolean own_scene = FALSE;
    for (int i = 0; i < OVER; i++)
        g_clear_pointer (&over_name[i], g_free);
    if (size < 5 || memcmp (p, mark, 4))
        return 0;
    while (p < end && n < SLOTS) {
        const uint8_t *nl = memchr (p, '\n', (size_t) (end - p));
        if (!nl)
            break;
        char *name = g_strndup ((const char *) p + 4, (size_t) (nl - p - 4));
        const uint8_t *body = nl + 1, *next = body;
        for (;;) {
            const uint8_t *q = next < end ? memchr (next, '\n', (size_t) (end - next)) : NULL;
            if (!q) { next = end; break; }
            if ((size_t) (end - q) > 4 && !memcmp (q + 1, mark, 4)) { next = q; break; }
            next = q + 1;
        }
        if (g_hash_table_contains (known, name)) {
            gboolean again = FALSE;
            char *abs = g_build_filename (dir, name, NULL);
            for (int i = 0; i < n; i++)
                again |= !strcmp (over_name[i], abs);
            if (again) {
                g_free (abs);
            } else {
                over_file[n] = fuzz_file (n, body, (size_t) (next - body));
                over_name[n++] = abs;
                own_scene |= !strcmp (name, "scene.json");
                for (guint k = 0; !variant && k < G_N_ELEMENTS (scene_for); k++)
                    if (g_str_has_prefix (name, scene_for[k].prefix))
                        variant = scene_for[k].variant;
            }
        }
        g_free (name);
        p = next < end ? next + 1 : end;
    }
    if (n && !own_scene) {
        g_free (variant_file);
        char *rel = g_strdup_printf ("variants/%s.json", variant ? variant : "solid");
        variant_file = g_build_filename (dir, rel, NULL);
        g_free (rel);
        if (g_file_test (variant_file, G_FILE_TEST_IS_REGULAR)) {
            over_file[SLOTS] = variant_file;
            over_name[SLOTS] = g_build_filename (dir, "scene.json", NULL);
        }
    }
    return n;
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    g_setenv ("WALPE_SOUND_NULL", "1", TRUE);
    g_unsetenv ("WALPE_WE_ASSETS");
    const char *d0 = fuzz_dir ("scenefile");
    fuzz_archive_dir = d0;
    const char *assets = g_getenv ("WALPE_FUZZ_ASSETS");
    char *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents (assets && *assets ? assets : FUZZ_ASSETS, &d, &n, NULL)) {
        g_printerr ("fuzz_scenefile: cannot read %s (set WALPE_FUZZ_ASSETS)\n", assets && *assets ? assets : FUZZ_ASSETS);
        exit (1);
    }
    fuzz_unpack ((const uint8_t *) d, n, NULL);
    g_free (d);
    known = g_hash_table_new (g_str_hash, g_str_equal);
    /* as the engine names it after it has resolved links (walpe_asset_path) */
    char *canon = realpath (d0, NULL);
    if (!canon)
        abort ();
    for (int i = 0; i < n_written; i++)
        g_hash_table_add (known, g_strdup (written[i] + strlen (d0) + 1));
    dir = canon;
    dir_len = strlen (dir);
    walpe_glutil_init ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    if (!take (data, size))
        return 0;
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
    g_clear_error (&err);
    if (c) {
        c->request_redraw = redraw;
        if (!c->gl_init || c->gl_init (c, &err)) {
            static const int sizes[2][2] = { { 64, 36 }, { 31, 57 } };
            for (int f = 0; f < 2; f++) {
                glBindFramebuffer (GL_FRAMEBUFFER, 0);
                glViewport (0, 0, sizes[f][0], sizes[f][1]);
                c->next_frame_us = 0;
                c->surface_lost = f == 1;
                if (c->pointer) {
                    c->pointer (c, WALPE_PTR_MOVE, 0.25f + 0.5f * (float) f, 0.5f);
                    c->pointer (c, f ? WALPE_PTR_UP : WALPE_PTR_DOWN, 0.5f, 0.5f);
                }
                if (c->gl_render)
                    c->gl_render (c, sizes[f][0], sizes[f][1], f ? 0.5f : 0.0f, -0.25f);
            }
            if (c->set_paused) {
                c->set_paused (c, TRUE);
                c->set_paused (c, FALSE);
            }
        }
        g_clear_error (&err);
        if (c->gl_free)
            c->gl_free (c);
        c->free (c);
    }
    /* what the options window reads of the same folder */
    JsonNode *defs = walpe_content_property_defs (dir, NULL);
    if (defs)
        json_node_unref (defs);
    JsonObject *notes = walpe_prop_notes (dir);
    if (notes)
        json_object_unref (notes);
    for (int i = 0; i < OVER; i++)
        g_clear_pointer (&over_name[i], g_free);
    return 0;
}
