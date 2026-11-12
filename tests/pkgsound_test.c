/* pkgsound_test - a packaged wallpaper's sound files and the package cache
 * (src/engine/pkg.c, content.c, scenenode_sound.h, scenesound.c).
 *
 * While sound is off for a wallpaper nothing reads its sound files: the
 * cached copy of its package is made without them, and they are extracted
 * when they are needed. With a package the test writes (a scene, a WAV its
 * sound layer plays, a second sound file, an entry with the name of the
 * cache's own list):
 *
 *  A. the cache: files and bytes of the copy with sound off and on; the
 *     list of what was left out (a package cannot write it); where a file
 *     will be; a folder outside the cache is never answered for; a copy
 *     with a stale file of the same name, as an interrupted extraction or
 *     an older package leaves it; a package that changed meanwhile.
 *  B. a player (null device, no display): with sound off its file is not
 *     extracted and not opened; switched on, it is extracted and played.
 *  C. the scene (GL, skipped without a context): loaded with sound off,
 *     shown, sound switched on while it is shown: the file appears and the
 *     picture is the one before; loaded with sound on: the file is there
 *     after the load, same picture; the same scene without its sound layer
 *     and without a package: same picture.
 * The test makes no sound and contacts no sound server. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/pkg.h"
#include "engine/scenesound.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                 printf (__VA_ARGS__); printf ("\n"); } } while (0)

static char *T;     /* everything the test writes */

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

typedef struct { const char *path; const guint8 *data; gsize len; } Ent;

static void write_pkg (const char *file, const Ent *e, guint n, guint extra)
{
    GByteArray *a = g_byte_array_new ();
    put32 (a, 8);
    g_byte_array_append (a, (const guint8 *) "PKGV0016", 8);
    put32 (a, n);
    guint32 off = 0;
    for (guint i = 0; i < n; i++) {
        put32 (a, (guint32) strlen (e[i].path));
        g_byte_array_append (a, (const guint8 *) e[i].path, (guint) strlen (e[i].path));
        put32 (a, off);
        put32 (a, (guint32) e[i].len);
        off += (guint32) e[i].len;
    }
    for (guint i = 0; i < n; i++)
        g_byte_array_append (a, e[i].data, (guint) e[i].len);
    for (guint i = 0; i < extra; i++)       /* bytes no entry names: another size, the same entries */
        g_byte_array_append (a, (const guint8 *) "", 1);
    char *d = g_path_get_dirname (file);
    g_mkdir_with_parents (d, 0700);
    g_free (d);
    g_file_set_contents (file, (const char *) a->data, a->len, NULL);
    g_byte_array_unref (a);
}

/* 16-bit mono PCM, a 440 Hz sine */
static GByteArray *make_wav (int rate, int frames)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "RIFF", 4);
    put32 (a, (guint32) (36 + frames * 2));
    g_byte_array_append (a, (const guint8 *) "WAVEfmt ", 8);
    put32 (a, 16);
    put32 (a, 1 | (1 << 16));               /* PCM, one channel */
    put32 (a, (guint32) rate);
    put32 (a, (guint32) rate * 2);
    put32 (a, 2 | (16 << 16));              /* block align 2, 16 bits */
    g_byte_array_append (a, (const guint8 *) "data", 4);
    put32 (a, (guint32) frames * 2);
    for (int i = 0; i < frames; i++) {
        gint16 v = (gint16) (12000.0 * sin (2.0 * G_PI * 440.0 * i / rate));
        guint8 b[2] = { (guint8) (v & 0xff), (guint8) ((v >> 8) & 0xff) };
        g_byte_array_append (a, b, 2);
    }
    return a;
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

/* the wallpaper's own files of a copy: not the cache's notes, not project.json */
static void tally (const char *dir, const char *rel, guint *files, gint64 *bytes)
{
    char *here = rel ? g_build_filename (dir, rel, NULL) : g_strdup (dir);
    GDir *d = g_dir_open (here, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (here, n, NULL), *r = rel ? g_build_filename (rel, n, NULL) : g_strdup (n);
        GStatBuf st;
        if (g_lstat (p, &st) == 0 && S_ISDIR (st.st_mode)) {
            tally (dir, r, files, bytes);
        } else if (!(rel == NULL && (g_str_has_prefix (n, ".walpe-pkg-") || !strcmp (n, "project.json")))) {
            (*files)++;
            *bytes += (gint64) st.st_size;
        }
        g_free (p);
        g_free (r);
    }
    if (d)
        g_dir_close (d);
    g_free (here);
}

static gboolean has (const char *dir, const char *rel)
{
    char *p = g_build_filename (dir, rel, NULL);
    gboolean is = g_file_test (p, G_FILE_TEST_IS_REGULAR);
    g_free (p);
    return is;
}

static gboolean same_bytes (const char *dir, const char *rel, const GByteArray *want)
{
    char *p = g_build_filename (dir, rel, NULL), *got = NULL;
    gsize n = 0;
    gboolean same = g_file_get_contents (p, &got, &n, NULL) && n == want->len && !memcmp (got, want->data, n);
    g_free (got);
    g_free (p);
    return same;
}

/* ---- the wallpaper ---- */
#define SCENE_HEAD \
    "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"}," \
    "\"general\":{\"ambientcolor\":\"0.3 0.3 0.3\",\"bloom\":false,\"cameraparallax\":false," \
    "\"clearcolor\":\"0.10 0.20 0.30\",\"orthogonalprojection\":{\"height\":180,\"width\":320}," \
    "\"skylightcolor\":\"0.3 0.3 0.3\"},\"objects\":[" \
    "{\"id\":1,\"name\":\"a\",\"image\":\"models/solid.json\",\"origin\":\"160 90 0\",\"size\":\"320 180\"," \
    "\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"color\":\"0.2 0.5 0.3\",\"alpha\":1.0,\"visible\":true}," \
    "{\"id\":2,\"name\":\"b\",\"image\":\"models/solid.json\",\"origin\":\"100 60 0\",\"size\":\"120 80\"," \
    "\"scale\":\"1 1 1\",\"angles\":\"0 0 0.4\",\"color\":\"0.9 0.2 0.1\",\"alpha\":0.6,\"visible\":true}"
#define SCENE_SOUND \
    ",{\"id\":3,\"name\":\"music\",\"sound\":[\"sounds/one.wav\"],\"volume\":1.0," \
    "\"playbackmode\":\"loop\",\"visible\":true}"
#define SCENE_TAIL "]}"
#define PROJECT "{\"file\":\"scene.json\",\"title\":\"pkg sound test\",\"type\":\"scene\"," \
                "\"general\":{\"properties\":{}}}"
static const char SOLID[] = "{\"solidlayer\":true}";
static const char SCENE[] = SCENE_HEAD SCENE_SOUND SCENE_TAIL;
static const char OGG[] = "OggS, or so this entry's name says: 60 bytes that nothing ever plays";
static const char EVIL[] = "sounds/planted.wav\n";
#define LIST "sounds/one.wav\nmusic/Two.OGG\n"

static char *item, *pkg;        /* <T>/wp/item, its scene.pkg */
static GByteArray *wav;
static gint64 plain_bytes;      /* scene.json + models/solid.json */

static void make_item (guint extra)
{
    const Ent e[] = {
        { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
        { "sounds/one.wav", wav->data, wav->len },
        { "models/solid.json", (const guint8 *) SOLID, strlen (SOLID) },
        { "music/Two.OGG", (const guint8 *) OGG, strlen (OGG) },
        { ".walpe-pkg-nosound", (const guint8 *) EVIL, strlen (EVIL) },
    };
    write_pkg (pkg, e, G_N_ELEMENTS (e), extra);
    char *pj = g_build_filename (item, "project.json", NULL);
    g_file_set_contents (pj, PROJECT, -1, NULL);
    g_free (pj);
}

static char *list_of (const char *cache)
{
    char *p = g_build_filename (cache, ".walpe-pkg-nosound", NULL), *text = NULL;
    g_file_get_contents (p, &text, NULL, NULL);
    g_free (p);
    return text;
}

static char *cache_copy (gboolean with_sound)
{
    GError *err = NULL;
    char *c = walpe_pkg_extract_cached_opts (pkg, item, with_sound, &err);
    if (!c)
        printf ("extraction failed: %s\n", err ? err->message : "?");
    g_clear_error (&err);
    return c;
}

/* forget the copy (nothing holds it) */
static void drop_copy (char *cache)
{
    walpe_pkg_cache_release (cache);
    rm_rf (cache);
    g_free (cache);
}

/* ---- what counts as a sound file ---- */
static gboolean same_text (const char *dir, const char *rel, const char *want)
{
    GByteArray *w = g_byte_array_new ();
    g_byte_array_append (w, (const guint8 *) want, (guint) strlen (want));
    gboolean same = same_bytes (dir, rel, w);
    g_byte_array_unref (w);
    return same;
}

static void test_kinds (void)
{
    static const char FONT[] = "wOFF: named like a sound file, read by something that is not a player";
    const Ent e[] = {
        { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
        { "models/solid.json", (const guint8 *) SOLID, strlen (SOLID) },
        { "sounds/one.wav", wav->data, wav->len },
        { "models/m.wav", (const guint8 *) SOLID, strlen (SOLID) },      /* a model */
        { "fonts/f.ogg", (const guint8 *) FONT, strlen (FONT) },         /* a font */
        { "twice/a.ogg", (const guint8 *) SOLID, strlen (SOLID) },       /* the last entry is the file: sound */
        { "twice/a.ogg", (const guint8 *) OGG, strlen (OGG) },
        { "twice/b.ogg", (const guint8 *) OGG, strlen (OGG) },           /* the last entry is the file: a model */
        { "./twice//b.ogg", (const guint8 *) SOLID, strlen (SOLID) },
        { "empty.mp3", (const guint8 *) "", 0 },
    };
    write_pkg (pkg, e, G_N_ELEMENTS (e), 0);
    char *c = cache_copy (FALSE);
    CHECK (c != NULL, "a copy without sound files (names that are not sound files)");
    if (c) {
        char *list = list_of (c);
        CHECK (same_text (c, "models/m.wav", SOLID) && same_text (c, "fonts/f.ogg", FONT) && has (c, "empty.mp3"),
               "sound off: a file that is only named like a sound file is in the copy");
        CHECK (!has (c, "sounds/one.wav") && !has (c, "twice/a.ogg"), "sound off: the sound files are not");
        CHECK (same_text (c, "twice/b.ogg", SOLID),
               "a file named twice whose last entry is no sound file: in the copy, the last entry's bytes");
        CHECK (!g_strcmp0 (list, "sounds/one.wav\ntwice/a.ogg\n"), "the list (a file named twice is in it once): \"%s\"",
               list ? list : "(none)");
        g_free (list);
        CHECK (walpe_pkg_fetch_sound (c), "the outstanding files are fetched");
        CHECK (same_bytes (c, "sounds/one.wav", wav) && same_text (c, "twice/a.ogg", OGG),
               "fetched: the sound files, of a file named twice the last entry's bytes");
        CHECK (same_text (c, "twice/b.ogg", SOLID) && same_text (c, "models/m.wav", SOLID),
               "... and the files the copy had are as they were");
        drop_copy (c);
    }
    c = cache_copy (TRUE);
    CHECK (c && same_text (c, "twice/a.ogg", OGG) && same_text (c, "twice/b.ogg", SOLID) &&
           same_text (c, "models/m.wav", SOLID) && same_bytes (c, "sounds/one.wav", wav),
           "a copy made with sound holds the same files");
    if (c)
        drop_copy (c);
    make_item (0);
}

/* ---- what no extraction can write is refused, whatever the sound setting ---- */
static char *cache_of_item (void);

static void test_refused (void)
{
    static char longname[300], fits[300];
    memset (longname, 'a', 245);
    strcpy (longname + 245, ".ogg");     /* 249 bytes: no room for the name it is written under */
    memset (fits, 'a', 244);
    strcpy (fits + 244, ".ogg");         /* 248: the longest that can be written */
    const struct { const char *what; gboolean ok; Ent e[3]; } t[] = {
        { "a sound file, then a folder of its name", FALSE,
          { { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
            { "x.ogg", (const guint8 *) OGG, strlen (OGG) },
            { "x.ogg/y.png", (const guint8 *) SOLID, strlen (SOLID) } } },
        { "a folder, then a sound file of its name", FALSE,
          { { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
            { "x.ogg/y.png", (const guint8 *) SOLID, strlen (SOLID) },
            { "x.ogg", (const guint8 *) OGG, strlen (OGG) } } },
        { "the same, spelled apart", FALSE,
          { { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
            { "./s//x.ogg", (const guint8 *) OGG, strlen (OGG) },
            { "s/./x.ogg/y.png", (const guint8 *) SOLID, strlen (SOLID) } } },
        { "a sound file with a name too long to write", FALSE,
          { { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
            { "models/solid.json", (const guint8 *) SOLID, strlen (SOLID) },
            { longname, (const guint8 *) OGG, strlen (OGG) } } },
        { "a sound file with the longest name", TRUE,
          { { "scene.json", (const guint8 *) SCENE, strlen (SCENE) },
            { "models/solid.json", (const guint8 *) SOLID, strlen (SOLID) },
            { fits, (const guint8 *) OGG, strlen (OGG) } } },
    };
    char *where = cache_of_item ();
    for (guint i = 0; i < G_N_ELEMENTS (t); i++) {
        write_pkg (pkg, t[i].e, 3, 0);
        for (int with_sound = 1; with_sound >= 0; with_sound--) {
            GError *err = NULL;
            rm_rf (where);
            char *c = walpe_pkg_extract_cached_opts (pkg, item, with_sound, &err);
            if (t[i].ok) {
                CHECK (c != NULL, "%s, sound %s: accepted (%s)", t[i].what, with_sound ? "on" : "off",
                       err ? err->message : "");
                CHECK (c && walpe_pkg_fetch_sound (c) && same_text (c, fits, OGG), "... and whole after the fetch");
            } else {
                CHECK (!c && err, "%s, sound %s: refused", t[i].what, with_sound ? "on" : "off");
                CHECK (!g_file_test (where, G_FILE_TEST_EXISTS), "... and nothing of it is in the cache");
            }
            g_clear_error (&err);
            if (c)
                drop_copy (c);
        }
    }
    g_free (where);
    make_item (0);
}

/* ---- A ---- */
static void test_cache (void)
{
    guint files = 0;
    gint64 bytes = 0;
    char *c = cache_copy (FALSE);
    CHECK (c != NULL, "a copy without sound files");
    if (!c)
        return;
    tally (c, NULL, &files, &bytes);
    char *list = list_of (c);
    CHECK (files == 2 && bytes == plain_bytes, "sound off: %u files, %d bytes in the copy (2 files, %d bytes)",
           files, (int) bytes, (int) plain_bytes);
    CHECK (!has (c, "sounds/one.wav") && !has (c, "music/Two.OGG"), "sound off: no sound file in the copy");
    CHECK (!g_strcmp0 (list, LIST), "the list of what was left out: \"%s\"", list ? list : "(none)");
    g_free (list);
    /* where a file will be; nothing for a name that is not outstanding */
    char *want = g_build_filename (c, "sounds/one.wav", NULL);
    char *p = walpe_pkg_deferred_path (c, "sounds/one.wav");
    CHECK (!g_strcmp0 (p, want), "the place of an outstanding file (%s)", p ? p : "none");
    g_free (p);
    /* the name as a scene may spell it: whatever the file system resolves
     * to that file */
    const char *also[] = { "./sounds/one.wav", "sounds//one.wav", "sounds/./one.wav", ".//sounds///./one.wav" };
    for (guint i = 0; i < G_N_ELEMENTS (also); i++) {
        p = walpe_pkg_deferred_path (c, also[i]);
        CHECK (!g_strcmp0 (p, want), "the place of an outstanding file, named \"%s\" (%s)", also[i], p ? p : "none");
        g_free (p);
    }
    g_free (want);
    const char *no[] = { "sounds/planted.wav", "scene.json", "sounds/ONE.wav", "one.wav", "../sounds/one.wav",
                         "music/two.ogg", "sounds/one.wav/", "sounds/one.wav/.", "./", "sounds" };
    for (guint i = 0; i < G_N_ELEMENTS (no); i++) {
        p = walpe_pkg_deferred_path (c, no[i]);
        CHECK (!p, "no place for \"%s\" (%s)", no[i], p ? p : "");
        g_free (p);
    }
    /* a folder that is no copy of the cache - a wallpaper's own - with a
     * list of its own making */
    char *planted = g_build_filename (item, ".walpe-pkg-nosound", NULL);
    g_file_set_contents (planted, LIST, -1, NULL);
    p = walpe_pkg_deferred_path (item, "sounds/one.wav");
    CHECK (!p && !walpe_pkg_fetch_sound (item) && !has (item, "sounds/one.wav"),
           "a folder outside the cache is not answered for and not written to");
    g_free (p);
    g_unlink (planted);
    g_free (planted);

    /* a stale file under the name (an older package's, or anything an
     * interrupted run left): the fetch writes every file anew */
    char *sd = g_build_filename (c, "sounds", NULL), *stale = g_build_filename (c, "sounds/one.wav", NULL);
    g_mkdir_with_parents (sd, 0700);
    g_file_set_contents (stale, "RIFF half a file", -1, NULL);
    CHECK (walpe_pkg_fetch_sound (c), "the outstanding files are fetched");
    list = list_of (c);
    CHECK (same_bytes (c, "sounds/one.wav", wav) && has (c, "music/Two.OGG") && !list,
           "... every one whole, and the list is gone");
    g_free (list);
    files = 0;
    bytes = 0;
    tally (c, NULL, &files, &bytes);
    CHECK (files == 4 && bytes == plain_bytes + (gint64) wav->len + (gint64) strlen (OGG),
           "sound on: %u files, %d bytes in the copy (4 files, %d bytes)", files, (int) bytes,
           (int) (plain_bytes + (gint64) wav->len + (gint64) strlen (OGG)));
    CHECK (walpe_pkg_fetch_sound (c), "nothing outstanding: nothing to do");
    g_free (sd);
    g_free (stale);
    /* a complete copy asked for without sound stays as it is */
    walpe_pkg_cache_release (c);
    g_free (c);
    c = cache_copy (FALSE);
    list = list_of (c);
    CHECK (c && has (c, "sounds/one.wav") && !list, "a complete copy stays complete");
    g_free (list);
    drop_copy (c);

    /* a copy without sound, asked for with sound: complete from that call on */
    c = cache_copy (FALSE);
    walpe_pkg_cache_release (c);
    g_free (c);
    c = cache_copy (TRUE);
    list = list_of (c);
    CHECK (c && same_bytes (c, "sounds/one.wav", wav) && has (c, "music/Two.OGG") && !list,
           "a copy without sound is completed by a load with sound");
    g_free (list);
    drop_copy (c);

    /* the package changed after the copy was made: its sound files are not
     * mixed into the old copy; the next load makes the copy anew */
    c = cache_copy (FALSE);
    make_item (3);
    CHECK (!walpe_pkg_fetch_sound (c) && !has (c, "sounds/one.wav"), "another package by now: nothing is fetched");
    list = list_of (c);
    CHECK (!g_strcmp0 (list, LIST), "... and the list stays");
    g_free (list);
    walpe_pkg_cache_release (c);
    g_free (c);
    c = cache_copy (TRUE);
    list = list_of (c);
    CHECK (c && same_bytes (c, "sounds/one.wav", wav) && !list, "the changed package is extracted anew, whole");
    g_free (list);
    drop_copy (c);

    /* the list cannot be written (something of its name is in the way in a
     * copy that is in use and is written over for a changed package): a
     * copy without a list would pass for whole, so it is made whole */
    c = cache_copy (FALSE);
    char *lf = g_build_filename (c, ".walpe-pkg-nosound", NULL);
    g_unlink (lf);
    g_mkdir (lf, 0700);
    make_item (5);
    char *c2 = cache_copy (FALSE);
    CHECK (c2 && same_bytes (c2, "sounds/one.wav", wav) && has (c2, "music/Two.OGG"),
           "no list can be written: the copy gets its sound files at once");
    p = c2 ? walpe_pkg_deferred_path (c2, "sounds/one.wav") : NULL;
    CHECK (!p && c2 && walpe_pkg_fetch_sound (c2), "... and nothing is outstanding");
    g_free (p);
    g_rmdir (lf);
    g_free (lf);
    if (c2)
        walpe_pkg_cache_release (c2);
    g_free (c2);
    drop_copy (c);
    make_item (0);
}

/* ---- B ---- */
static void fetch_cb (const char *dir) { walpe_pkg_fetch_sound (dir); }

static gboolean wait_for (gboolean (*cond) (gpointer), gpointer u, int ms)
{
    for (int i = 0; i < ms / 5; i++) {
        if (cond (u))
            return TRUE;
        g_usleep (5000);
    }
    return cond (u);
}

static gboolean opened (gpointer u)
{
    WalpeSoundStats st;
    walpe_sound_stats (u, &st);
    return st.opens >= 1 && st.frames > 0;
}

static gboolean no_threads (gpointer u)
{
    (void) u;
    return walpe_sound_threads () == 0;
}

static void test_player (void)
{
    char *c = cache_copy (FALSE);
    if (!c)
        return;
    char *file = walpe_pkg_deferred_path (c, "sounds/one.wav");
    const char *files[] = { file };
    WalpeSoundSpec spec = { .files = files, .nfiles = 1, .volume = 1.0f, .mode = WALPE_SOUND_LOOP,
                            .mintime = 0, .maxtime = 0, .name = "music" };
    walpe_sound_set_enabled (FALSE);
    WalpeSoundGroup *g = walpe_sound_group_new (item, -1);
    WalpeSound *s = walpe_sound_new (g, &spec);
    walpe_sound_set_prepare (s, fetch_cb, c);
    CHECK (g && s, "a player for a file that is not on disk yet");
    walpe_sound_play (s);
    walpe_sound_group_set_active (g, TRUE);
    g_usleep (300000);
    WalpeSoundStats st;
    walpe_sound_stats (s, &st);
    CHECK (st.opens == 0 && !has (c, "sounds/one.wav") && walpe_sound_threads () == 0,
           "sound off: nothing extracted, nothing opened, no thread (%d opens, %d threads)", st.opens,
           walpe_sound_threads ());
    walpe_sound_set_enabled (TRUE);
    CHECK (wait_for (opened, s, 5000), "sound switched on: the layer plays");
    walpe_sound_stats (s, &st);
    char *list = list_of (c);
    CHECK (same_bytes (c, "sounds/one.wav", wav) && !list && !st.error[0],
           "... from the file extracted at that moment (error \"%s\")", st.error);
    g_free (list);
    walpe_sound_group_free (g);
    wait_for (no_threads, NULL, 3000);
    walpe_sound_set_enabled (FALSE);
    g_free (file);
    drop_copy (c);
}

/* ---- C ---- */
#define W 320
#define H 180
static void redraw_cb (gpointer u) { (void) u; }

typedef struct { WalpeContent *c; guint8 px[W * H * 4]; gboolean drawn; } Run;

static gboolean run_open (Run *r, const char *dir)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir, .render_scale = 100 };
    memset (r, 0, sizeof *r);
    r->c = walpe_content_new (dir, &opts, &err);
    if (r->c)
        r->c->request_redraw = redraw_cb;
    if (!r->c || !r->c->gl_init (r->c, &err)) {
        printf ("load failed: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return FALSE;
    }
    return TRUE;
}

static void run_frame (Run *r)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, W, H);
    r->c->next_frame_us = 0;
    r->c->surface_lost = FALSE;
    r->c->frame_unchanged = FALSE;
    r->c->gl_render (r->c, W, H, 0.0f, 0.0f);
    glFinish ();
    if (r->c->frame_unchanged)
        return;
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glReadPixels (0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, r->px);
    r->drawn = TRUE;
}

static void run_close (Run *r)
{
    if (r->c) {
        if (r->c->gl_free)
            r->c->gl_free (r->c);
        r->c->free (r->c);
    }
    r->c = NULL;
}

static guint32 fnv (const guint8 *p, gsize n)
{
    guint32 h = 2166136261u;
    for (gsize i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static char *cache_of_item (void)
{
    char *bn = g_path_get_basename (item), *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, item, -1);
    char *base = g_strdup_printf ("%s-%.8s", bn, hash);
    char *c = g_build_filename (g_get_user_cache_dir (), "walpe", "pkg", base, NULL);
    g_free (base);
    g_free (hash);
    g_free (bn);
    return c;
}

static Run ra, rb, rc;      /* (three pictures: not on the stack) */

static void test_scene (void)
{
    char *c = cache_of_item ();
    rm_rf (c);
    walpe_sound_set_enabled (FALSE);
    if (!run_open (&ra, item)) {
        CHECK (FALSE, "the packaged scene loads");
        g_free (c);
        return;
    }
    for (int i = 0; i < 3; i++)
        run_frame (&ra);
    char *list = list_of (c);
    CHECK (ra.drawn && !has (c, "sounds/one.wav") && !g_strcmp0 (list, LIST),
           "scene shown with sound off: its copy has no sound file");
    g_free (list);
    guint files = 0;
    gint64 bytes = 0;
    tally (c, NULL, &files, &bytes);
    CHECK (files == 2 && bytes == plain_bytes, "... %u files, %d bytes", files, (int) bytes);
    guint32 h_off = fnv (ra.px, sizeof ra.px);
    /* sound is switched on while the scene is shown */
    walpe_sound_set_enabled (TRUE);
    gboolean there = FALSE;
    for (int i = 0; i < 500 && !there; i++) {
        run_frame (&ra);
        g_usleep (10000);
        there = has (c, "sounds/one.wav");
    }
    list = list_of (c);
    CHECK (there && same_bytes (c, "sounds/one.wav", wav) && !list,
           "sound switched on while the scene is shown: the file is extracted");
    g_free (list);
    for (int i = 0; i < 3; i++)
        run_frame (&ra);
    CHECK (fnv (ra.px, sizeof ra.px) == h_off, "... and the picture is the one before (%08x / %08x)",
           fnv (ra.px, sizeof ra.px), h_off);
    run_close (&ra);

    /* loaded with sound on: complete after the load, before a frame */
    rm_rf (c);
    if (run_open (&rb, item)) {
        CHECK (same_bytes (c, "sounds/one.wav", wav) && has (c, "music/Two.OGG") && !has (c, ".walpe-pkg-nosound"),
               "scene loaded with sound on: its copy is complete");
        for (int i = 0; i < 3; i++)
            run_frame (&rb);
        CHECK (rb.drawn && fnv (rb.px, sizeof rb.px) == h_off, "... same picture (%08x / %08x)",
               fnv (rb.px, sizeof rb.px), h_off);
    } else {
        CHECK (FALSE, "the packaged scene loads with sound on");
    }
    run_close (&rb);
    walpe_sound_set_enabled (FALSE);

    /* the scene without its sound layer, as plain files */
    char *plain = g_build_filename (T, "wp", "plain", NULL), *m = g_build_filename (plain, "models", NULL);
    g_mkdir_with_parents (m, 0700);
    char *f1 = g_build_filename (plain, "scene.json", NULL), *f2 = g_build_filename (m, "solid.json", NULL),
         *f3 = g_build_filename (plain, "project.json", NULL);
    g_file_set_contents (f1, SCENE_HEAD SCENE_TAIL, -1, NULL);
    g_file_set_contents (f2, SOLID, -1, NULL);
    g_file_set_contents (f3, PROJECT, -1, NULL);
    if (run_open (&rc, plain)) {
        for (int i = 0; i < 3; i++)
            run_frame (&rc);
        CHECK (rc.drawn && fnv (rc.px, sizeof rc.px) == h_off,
               "the scene without a sound layer: same picture (%08x / %08x)", fnv (rc.px, sizeof rc.px), h_off);
    } else {
        CHECK (FALSE, "the plain scene loads");
    }
    run_close (&rc);
    wait_for (no_threads, NULL, 3000);
    g_free (f1);
    g_free (f2);
    g_free (f3);
    g_free (m);
    g_free (plain);
    g_free (c);
}

/* ---- D: names the file system resolves, the scene's and the package's ---- */
#define SCENE_SPELT \
    ",{\"id\":3,\"name\":\"m1\",\"sound\":[\"./sounds//one.wav\"],\"playbackmode\":\"loop\",\"visible\":true}" \
    ",{\"id\":4,\"name\":\"m2\",\"sound\":[\"extra/three.wav\"],\"playbackmode\":\"loop\",\"visible\":true}" \
    ",{\"id\":5,\"name\":\"m3\",\"sound\":[\".//four.wav\"],\"playbackmode\":\"loop\",\"visible\":true}"
static const char SPELT[] = SCENE_HEAD SCENE_SPELT SCENE_TAIL;

static void test_spelt (void)
{
    const Ent e[] = {
        { "scene.json", (const guint8 *) SPELT, strlen (SPELT) },
        { "models/solid.json", (const guint8 *) SOLID, strlen (SOLID) },
        { "sounds/one.wav", wav->data, wav->len },
        { "./extra//three.wav", wav->data, wav->len },
        { "assets/./four.wav", wav->data, wav->len },
    };
    write_pkg (pkg, e, G_N_ELEMENTS (e), 0);
    char *c = cache_of_item ();
    for (int on_first = 0; on_first < 2; on_first++) {
        rm_rf (c);
        walpe_sound_set_enabled (on_first);
        if (!run_open (&ra, item)) {
            CHECK (FALSE, "the scene with spelled names loads");
            break;
        }
        run_frame (&ra);
        if (!on_first) {
            char *list = list_of (c);
            CHECK (!g_strcmp0 (list, "sounds/one.wav\nextra/three.wav\nassets/four.wav\n") &&
                   !has (c, "sounds/one.wav"), "the list has each file under one spelling: \"%s\"",
                   list ? list : "(none)");
            g_free (list);
            walpe_sound_set_enabled (TRUE);     /* ... while the scene is shown */
        }
        gboolean all = FALSE;
        for (int i = 0; i < 500 && !all; i++) {
            run_frame (&ra);
            g_usleep (10000);
            all = walpe_sound_threads () == 3 && has (c, "assets/four.wav") && !has (c, ".walpe-pkg-nosound");
        }
        CHECK (all && same_bytes (c, "sounds/one.wav", wav) && same_bytes (c, "extra/three.wav", wav) &&
               same_bytes (c, "assets/four.wav", wav) && !has (c, ".walpe-pkg-nosound"),
               "sound %s: each of the three layers has its player and its file (%d players)",
               on_first ? "on at the load" : "switched on while the scene is shown", walpe_sound_threads ());
        run_close (&ra);
        walpe_sound_set_enabled (FALSE);
        CHECK (wait_for (no_threads, NULL, 5000), "... and the players end with the scene");
    }
    g_free (c);
    make_item (0);
}

int main (void)
{
    T = g_dir_make_tmp ("walpe-pkgsound-XXXXXX", NULL);
    if (!T)
        return 77;
    /* the test's own cache and settings, before anything asks for them */
    char *cache = g_build_filename (T, "cache", NULL), *conf = g_build_filename (T, "config", NULL);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_setenv ("XDG_CONFIG_HOME", conf, TRUE);
    g_setenv ("WALPE_SOUND_NULL", "1", TRUE);
    g_free (cache);
    g_free (conf);
    item = g_build_filename (T, "wp", "item", NULL);
    pkg = g_build_filename (item, "scene.pkg", NULL);
    wav = make_wav (8000, 4000);
    plain_bytes = (gint64) strlen (SCENE) + (gint64) strlen (SOLID);
    make_item (0);

    CHECK (walpe_pkg_sound_name ("a/b.mp3") && walpe_pkg_sound_name ("B.OGG") && walpe_pkg_sound_name ("x.Wav") &&
           walpe_pkg_sound_name ("s/x.flac") && !walpe_pkg_sound_name ("x.tex") && !walpe_pkg_sound_name ("mp3") &&
           !walpe_pkg_sound_name ("a.mp3/scene.json") && !walpe_pkg_sound_name ("a\nb.mp3"),
           "which entries are sound files");
    test_cache ();
    test_kinds ();
    test_refused ();
    walpe_sound_allow (TRUE);
    walpe_sound_use_null (TRUE);
    test_player ();

    EGLDisplay dpy = epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")
                         ? eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                         : EGL_NO_DISPLAY;
    EGLint maj, min, nc = 0;
    EGLConfig cfgs[64];
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE };
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    if (dpy != EGL_NO_DISPLAY && eglInitialize (dpy, &maj, &min) && eglBindAPI (EGL_OPENGL_API) &&
        eglChooseConfig (dpy, attr, cfgs, 64, &nc) && nc >= 1) {
        ctx = eglCreateContext (dpy, cfgs[0], EGL_NO_CONTEXT, ca);
        surf = eglCreatePbufferSurface (dpy, cfgs[0], pa);
    }
    if (ctx != EGL_NO_CONTEXT && surf != EGL_NO_SURFACE && eglMakeCurrent (dpy, surf, surf, ctx)) {
        walpe_glutil_init ();
        test_scene ();
        test_spelt ();
        eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface (dpy, surf);
        eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
    } else {
        printf ("no GL 3.3 context with a pbuffer (EGL surfaceless platform): the scene part is skipped\n");
    }

    g_byte_array_unref (wav);
    rm_rf (T);
    printf ("pkgsound-test: %s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "ok", checks, fails);
    return fails ? 1 : 0;
}
