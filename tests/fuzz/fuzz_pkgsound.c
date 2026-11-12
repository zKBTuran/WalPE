/* fuzz_pkgsound - the package cache's copy without sound files
 * (src/engine/pkg.c: pkg_extract_mode, walpe_pkg_fetch_sound,
 * walpe_pkg_deferred_path). A copy for a silent wallpaper leaves the
 * package's sound files out, lists them in the copy, and a later walk over
 * the same package writes them. The input is the package; its entry table
 * decides what each of the three walks writes.
 *
 * Nothing is written (as in fuzz_pkg.c the two calls that create directories
 * and files are replaced, and the removal of the list). Checked:
 *  - the three walks agree on whether the package is acceptable, and a
 *    refused one writes nothing in any of them;
 *  - the copy without sound and the later walk write, between them, exactly
 *    what the whole extraction writes: every entry by one of them, no FILE
 *    by both (several entries may name one file; a file is a sound file by
 *    its name and its first bytes, and the last entry decides), every file
 *    with the bytes the whole extraction leaves in it, and the list names
 *    exactly the entries the later walk writes;
 *  - no walk writes one of the cache's own files (the stamp, the source
 *    note, the list), and every name lies below the copy;
 *  - the place walpe_pkg_deferred_path gives for an outstanding file is the
 *    same under every spelling of its name ("./a//b" is "a/b"), lies inside
 *    the copy, and a name with ".." gets none;
 *  - a package with more sound files than a copy is made without (256) is
 *    copied whole, and so is one with none: no list, nothing fetched later;
 *  - walpe_pkg_fetch_sound writes what the later walk writes when the
 *    package is the one the copy was made of, nothing when it has changed,
 *    and does not act on a folder that is no copy of the cache.
 * The copy is a directory made once whose three bookkeeping files are links
 * to memory files. */
#include "fuzz_common.h"
#include <glib/gstdio.h>

#define FUZZ_DEST "/walpe-fuzz-dest"
static const char *dest = FUZZ_DEST;    /* where the walk under test may write */
static GHashTable *wrote;               /* the entry paths that walk wrote */
static GHashTable *files;               /* the files it wrote (a path spelled one way) -> what the last
                                         * write put there: its length and a sum of its bytes */
static guint64 wrote_bytes;
static gboolean list_removed;

static void fuzz_name_ok (const char *path)
{
    gsize n = strlen (dest);
    FUZZ_CHECK (!strncmp (path, dest, n) && path[n] == '/');
    char **parts = g_strsplit (path + n, "/", -1);
    gboolean first = TRUE;
    for (char **c = parts; *c; c++) {
        FUZZ_CHECK (strcmp (*c, "..") != 0);
        if (!**c || !strcmp (*c, "."))
            continue;
        if (first)      /* the cache's own files are not a package's to write */
            FUZZ_CHECK (g_ascii_strcasecmp (*c, ".walpe-pkg-stamp") && g_ascii_strcasecmp (*c, ".walpe-pkg-src") &&
                        g_ascii_strcasecmp (*c, ".walpe-pkg-nosound"));
        first = FALSE;
    }
    g_strfreev (parts);
}

static char *fuzz_file_key (const char *path);

static int fuzz_mkdir (const char *path, int mode)
{
    (void) mode;
    char *with = g_strconcat (path, "/", NULL);
    fuzz_name_ok (with);
    g_free (with);
    return 0;
}

static gboolean fuzz_set_contents (const char *path, const char *data, gssize len, GError **err)
{
    (void) err;
    fuzz_name_ok (path);
    FUZZ_CHECK (len >= 0);
    guint64 sum = ((guint64) fuzz_touch (data, (size_t) len) << 32) | (guint32) len;
    wrote_bytes += (guint64) len;
    g_hash_table_add (wrote, g_strdup (path));
    guint64 *held = g_new (guint64, 1);
    *held = sum;
    g_hash_table_replace (files, fuzz_file_key (path), held);
    return TRUE;
}

static int fuzz_unlink (const char *path)
{
    char *base = g_path_get_basename (path);
    FUZZ_CHECK (!strcmp (base, ".walpe-pkg-nosound"));  /* the only file these walks remove */
    g_free (base);
    list_removed = TRUE;
    return 0;
}

#define g_mkdir_with_parents fuzz_mkdir
#define g_file_set_contents fuzz_set_contents
#define g_unlink fuzz_unlink
#include "../../src/engine/pkg.c"
#undef g_mkdir_with_parents
#undef g_file_set_contents
#undef g_unlink

/* the file a written path names, as the package reader spells one file */
static char *fuzz_file_key (const char *path)
{
    return pkg_path_key (path + strlen (dest));
}

static char *copy;          /* <cache>/walpe/pkg/copy: a copy with a list of outstanding files */
static char *whole;         /* <cache>/walpe/pkg/whole: one without (nothing was left out) */

static void link_to (const char *dir, const char *name, const char *file)
{
    char *f = g_build_filename (dir, name, NULL);
    if ((unlink (f), symlink (file, f)) != 0)
        abort ();
    g_free (f);
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    static const char *d;       /* (held for the leak check: it lives as long as the process) */
    d = fuzz_dir ("pkgsound");
    char *cache = g_build_filename (d, "cache", NULL);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);       /* before anything asks for it */
    copy = g_build_filename (g_get_user_cache_dir (), "walpe", "pkg", "copy", NULL);
    whole = g_build_filename (g_get_user_cache_dir (), "walpe", "pkg", "whole", NULL);
    if (strcmp (g_get_user_cache_dir (), cache) || g_mkdir_with_parents (copy, 0700) != 0 ||
        g_mkdir_with_parents (whole, 0700) != 0)
        abort ();
    g_free (cache);
    const char *pkg = fuzz_file (0, (const uint8_t *) "", 0);
    link_to (copy, PKG_NOSOUND, fuzz_file (1, (const uint8_t *) "", 0));
    link_to (copy, PKG_SRC, fuzz_file (2, (const uint8_t *) pkg, strlen (pkg)));
    link_to (copy, PKG_STAMP, fuzz_file (3, (const uint8_t *) "", 0));
    link_to (whole, PKG_SRC, fuzz_file (2, (const uint8_t *) pkg, strlen (pkg)));
    link_to (whole, PKG_STAMP, fuzz_file (3, (const uint8_t *) "", 0));
    wrote = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    return 0;
}

/* one walk: what it wrote is in `wrote` and `files` afterwards (the caller takes them) */
static gboolean walk (const char *pkg, int mode, GString *left, GHashTable **set, GHashTable **fset, guint64 *bytes)
{
    GError *err = NULL;
    g_hash_table_remove_all (wrote);
    g_hash_table_remove_all (files);
    wrote_bytes = 0;
    gboolean ok = pkg_extract_mode (pkg, dest, mode, left, &err);
    FUZZ_CHECK (ok || err);
    g_clear_error (&err);
    *set = wrote;
    *fset = files;
    *bytes = wrote_bytes;
    wrote = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    return ok;
}

static void inside_copy (const char *p)
{
    gsize n = strlen (copy);
    FUZZ_CHECK (!strncmp (p, copy, n) && p[n] == '/' && !strstr (p + n, "/../") && !g_str_has_suffix (p, "/.."));
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    const char *pkg = fuzz_file (0, data, size);
    GHashTable *all, *quiet, *sound, *f_all, *f_quiet, *f_sound;
    guint64 b_all, b_quiet, b_sound;
    GString *left = g_string_new (NULL);
    dest = FUZZ_DEST;
    gboolean ok = walk (pkg, PKG_ALL, NULL, &all, &f_all, &b_all);
    gboolean ok_quiet = walk (pkg, PKG_BUT_SOUND, left, &quiet, &f_quiet, &b_quiet);
    gboolean ok_sound = walk (pkg, PKG_ONLY_SOUND, NULL, &sound, &f_sound, &b_sound);
    FUZZ_CHECK (ok == ok_quiet && ok == ok_sound);
    if (!ok)
        FUZZ_CHECK (!g_hash_table_size (all) && !g_hash_table_size (quiet) && !g_hash_table_size (sound) &&
                    !left->len);

    GHashTableIter it;
    gpointer k, v;
    const gsize skip = strlen (FUZZ_DEST) + 1;
    /* Nothing listed: the copy is the whole package (it has no sound file, or more of them than a copy is
     * made without - then nothing is fetched later either). */
    const gboolean is_whole = ok && !left->len;
    if (is_whole) {
        FUZZ_CHECK (g_hash_table_size (quiet) == g_hash_table_size (all) && b_quiet == b_all &&
                    g_hash_table_size (f_quiet) == g_hash_table_size (f_all));
        g_hash_table_iter_init (&it, f_all);
        while (g_hash_table_iter_next (&it, &k, &v)) {
            const guint64 *have = g_hash_table_lookup (f_quiet, k);
            FUZZ_CHECK (have && *have == *(const guint64 *) v);
        }
        if (g_hash_table_size (f_sound))
            FUZZ_CHECK (g_hash_table_size (f_sound) > PKG_DEFER_MAX);
    } else {
        g_hash_table_iter_init (&it, quiet);
        while (g_hash_table_iter_next (&it, &k, NULL))
            FUZZ_CHECK (g_hash_table_contains (all, k) && !g_hash_table_contains (sound, k));
        g_hash_table_iter_init (&it, sound);
        while (g_hash_table_iter_next (&it, &k, NULL))
            FUZZ_CHECK (walpe_pkg_sound_name ((const char *) k + skip) && g_hash_table_contains (all, k));
        FUZZ_CHECK (g_hash_table_size (quiet) + g_hash_table_size (sound) == g_hash_table_size (all));
        FUZZ_CHECK (b_quiet + b_sound == b_all);
        /* file for file: no file is written by both (completing a copy never writes over a file the scene
         * has read), and each holds, after the two walks, what the whole extraction leaves in it -
         * whichever of several entries that name one file comes last */
        g_hash_table_iter_init (&it, f_quiet);
        while (g_hash_table_iter_next (&it, &k, &v)) {
            const guint64 *in_all = g_hash_table_lookup (f_all, k);
            FUZZ_CHECK (!g_hash_table_contains (f_sound, k) && in_all && *in_all == *(const guint64 *) v);
        }
        g_hash_table_iter_init (&it, f_sound);
        while (g_hash_table_iter_next (&it, &k, &v)) {
            const guint64 *in_all = g_hash_table_lookup (f_all, k);
            FUZZ_CHECK (in_all && *in_all == *(const guint64 *) v);
        }
        FUZZ_CHECK (g_hash_table_size (f_quiet) + g_hash_table_size (f_sound) == g_hash_table_size (f_all));
        FUZZ_CHECK (g_hash_table_size (f_sound) <= PKG_DEFER_MAX);
    }

    /* the list: every outstanding FILE once, under the one spelling the extraction uses for it, and no other */
    char **lines = g_strsplit (left->str, "\n", -1);
    guint n_lines = g_strv_length (lines);
    if (n_lines)
        FUZZ_CHECK (!*lines[--n_lines]);        /* (every line ends with its newline) */
    GHashTable *listed = g_hash_table_new (g_str_hash, g_str_equal);
    for (guint i = 0; i < n_lines; i++) {
        char *key = pkg_path_key (lines[i]);
        FUZZ_CHECK (walpe_pkg_sound_name (lines[i]) && !strcmp (key, lines[i]) &&
                    g_hash_table_contains (f_sound, lines[i]));
        FUZZ_CHECK (g_hash_table_add (listed, lines[i]));       /* (once) */
        g_free (key);
    }
    if (!is_whole)
        FUZZ_CHECK (g_hash_table_size (listed) == g_hash_table_size (f_sound));

    /* the copy of the cache: the list as the first walk left it */
    fuzz_file (1, (const uint8_t *) left->str, left->len);
    for (guint i = 0; i < n_lines && i < 24; i++) {
        char *want = g_build_filename (copy, lines[i], NULL);
        /* the file under its own spelling and under others a scene.json may use for the same file */
        const char *sl = strchr (lines[i], '/');
        char *dot = g_strconcat ("./", lines[i], NULL), *abs = g_strconcat ("/", lines[i], NULL);
        char *twice = sl ? g_strdup_printf ("%.*s//./%s", (int) (sl - lines[i]), lines[i], sl + 1) : g_strdup (lines[i]);
        const char *same[] = { lines[i], dot, abs, twice };
        for (guint t = 0; t < G_N_ELEMENTS (same); t++) {
            char *p = walpe_pkg_deferred_path (copy, same[t]);
            FUZZ_CHECK (p && !strcmp (p, want));
            g_free (p);
        }
        /* below "assets" as an asset name is looked up, and names that must not leave the copy */
        char *up = g_strconcat ("../", lines[i], NULL), *down = g_strconcat ("x/../", lines[i], NULL);
        const char *tries[] = { g_str_has_prefix (lines[i], "assets/") ? lines[i] + 7 : "x.mp3", up, down };
        for (guint t = 0; t < G_N_ELEMENTS (tries); t++) {
            char *q = walpe_pkg_deferred_path (copy, tries[t]);
            if (q)
                inside_copy (q);
            FUZZ_CHECK (t == 0 || !q);
            g_free (q);
        }
        if (g_str_has_prefix (lines[i], "assets/")) {
            char *q = walpe_pkg_deferred_path (copy, lines[i] + 7);
            FUZZ_CHECK (q);
            g_free (q);
        }
        g_free (up);
        g_free (down);
        g_free (dot);
        g_free (abs);
        g_free (twice);
        g_free (want);
        FUZZ_CHECK (!walpe_pkg_deferred_path (FUZZ_DEST, lines[i]));    /* no copy of the cache: not answered for */
    }
    /* (names of files the copy has already: not outstanding under their own name; under assets/ only when
     * the list has that file) */
    g_hash_table_iter_init (&it, quiet);
    for (guint i = 0; i < 24 && g_hash_table_iter_next (&it, &k, NULL); i++) {
        char *q = walpe_pkg_deferred_path (copy, (const char *) k + skip);
        if (q) {
            char *key = pkg_path_key ((const char *) k + skip);
            char *alt = g_strconcat ("assets/", key, NULL);
            char *want = g_build_filename (copy, alt, NULL);
            FUZZ_CHECK (!is_whole && g_hash_table_contains (listed, alt) && !strcmp (q, want));
            g_free (want);
            g_free (alt);
            g_free (key);
        }
        g_free (q);
    }

    /* the fetch: the package is the one the copy was made of ... */
    GStatBuf st;
    if (g_stat (pkg, &st) != 0)
        abort ();
    char *stamp = g_strdup_printf ("v3:%lld:%lld", (long long) st.st_mtime, (long long) st.st_size);
    fuzz_file (3, (const uint8_t *) stamp, strlen (stamp));
    g_free (stamp);
    if (is_whole) {
        /* a copy without a list has nothing outstanding: the fetch is done at once and writes nothing */
        dest = whole;
        g_hash_table_remove_all (wrote);
        g_hash_table_remove_all (files);
        list_removed = FALSE;
        FUZZ_CHECK (walpe_pkg_fetch_sound (whole) && !g_hash_table_size (wrote) && !list_removed);
        FUZZ_CHECK (!walpe_pkg_deferred_path (whole, "sounds/a.mp3"));
    } else {
        dest = copy;
        g_hash_table_remove_all (wrote);
        g_hash_table_remove_all (files);
        wrote_bytes = 0;
        list_removed = FALSE;
        gboolean fetched = walpe_pkg_fetch_sound (copy);
        FUZZ_CHECK (fetched == ok);
        FUZZ_CHECK (g_hash_table_size (wrote) == g_hash_table_size (sound) && wrote_bytes == b_sound);
        g_hash_table_iter_init (&it, files);    /* (the fetch touches no file the copy had) */
        while (g_hash_table_iter_next (&it, &k, NULL))
            FUZZ_CHECK (!g_hash_table_contains (f_quiet, k));
        FUZZ_CHECK (list_removed == ok);
        /* ... and one that has changed since */
        fuzz_file (3, (const uint8_t *) "v3:1:1", 6);
        g_hash_table_remove_all (wrote);
        list_removed = FALSE;
        FUZZ_CHECK (!walpe_pkg_fetch_sound (copy) && !g_hash_table_size (wrote) && !list_removed);
    }
    /* a folder that is no copy of the cache */
    FUZZ_CHECK (!walpe_pkg_fetch_sound (FUZZ_DEST) && !walpe_pkg_fetch_sound (g_get_user_cache_dir ()));
    dest = FUZZ_DEST;

    g_hash_table_unref (listed);
    g_strfreev (lines);
    g_string_free (left, TRUE);
    g_hash_table_unref (all);
    g_hash_table_unref (quiet);
    g_hash_table_unref (sound);
    g_hash_table_unref (f_all);
    g_hash_table_unref (f_quiet);
    g_hash_table_unref (f_sound);
    return 0;
}
