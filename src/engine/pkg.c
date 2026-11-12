/* pkg.c - Wallpaper Engine .pkg archives (scene.pkg).
 *
 * Format (reverse-engineered from workshop content, matches the layout
 * WE's editor writes): all integers little-endian.
 *   [u32 magicLen=8]["PKGV0016"][u32 fileCount]
 *   per file: [u32 pathLen][path][u32 offset][u32 size]
 *   ... then the raw data blob (offsets from the start of the blob region,
 *   which equals the end of the entry table).
 * The archive is NOT encrypted or compressed. Extraction writes every
 * entry under dest_dir (path-traversal guarded); the cache is reused
 * until the .pkg changes (stamp file). The cache is bounded: see
 * pkg_cache_trim_locked. */
#include "pkg.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static guint32 rd32 (const guint8 *p)
{
    return (guint32) p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}

static gboolean safe_rel_path (const char *p)
{
    if (!p || !*p || p[0] == '/' || strstr (p, ".."))
        return FALSE;
    return TRUE;
}

/* What one package may create in the cache. Bytes are bounded by the
 * archive's size (below); files and directories cost disk and inodes
 * without holding a byte: 100000 empty entries, each in a directory chain
 * of its own as long as a 1024-byte name allows, made half a million
 * directories (2 GiB of blocks) from one megabyte of package - nothing of
 * it counted against the extraction budget or the cache limit, and
 * pkg_tree_remove (32 levels) could not delete it again. The largest of 88
 * Workshop packages has 403 files in 104 directories, 6 levels deep. */
#define PKG_MAX_DEPTH 32        /* path components of one entry, the file's name included */
#define PKG_MAX_OBJECTS 32768   /* files + directories of one package */
#define PKG_STAMP ".walpe-pkg-stamp"
#define PKG_SRC ".walpe-pkg-src"     /* the package the copy was made from */

/* Is the first component of an entry's path one of the cache's own two
 * names? Whatever lies under them is the cache's: as a file the stamp says
 * that the copy is complete and which package it is of, and a directory of
 * that name keeps the cache from ever writing its stamp - "./" in front of
 * the name, or the name as a directory with an entry below it, used to get
 * past the comparison of the whole path. With a directory for a stamp the
 * package was extracted anew at every load, by every monitor; with one for
 * the source note the copy was never recognised as that of a package that is
 * gone. Letter case is ignored (a cache on a file system that folds it). */
static gboolean pkg_reserved_path (const char *p)
{
    for (;;) {
        while (*p == '/')
            p++;
        if (p[0] == '.' && (p[1] == '/' || !p[1]))
            p++;
        else
            break;
    }
    gsize n = strcspn (p, "/");
    return (n == strlen (PKG_STAMP) && !g_ascii_strncasecmp (p, PKG_STAMP, n)) ||
           (n == strlen (PKG_SRC) && !g_ascii_strncasecmp (p, PKG_SRC, n));
}

/* number of components of a relative path ("a//b/" has two) */
static int pkg_path_depth (const char *p)
{
    int n = 0;
    for (const char *c = p; *c;) {
        while (*c == '/')
            c++;
        if (!*c)
            break;
        n++;
        while (*c && *c != '/')
            c++;
    }
    return n;
}

/* the directories path lies in that no earlier entry named: added to seen,
 * returns how many */
static guint pkg_new_dirs (GHashTable *seen, const char *path)
{
    guint n = 0;
    for (const char *c = strchr (path, '/'); c; c = strchr (c + 1, '/')) {
        if (c == path || c[-1] == '/')
            continue;
        char *dir = g_strndup (path, c - path);
        if (g_hash_table_add (seen, dir))
            n++;
    }
    return n;
}

static gboolean set_ioerr (GError **err, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static gboolean set_ioerr (GError **err, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    char *msg = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s", msg);
    g_free (msg);
    return FALSE;
}

gboolean walpe_pkg_extract (const char *pkg_path, const char *dest_dir, GError **err)
{
    /* mmap instead of a heap copy: pkgs can be hundreds of MB.
     * A regular file only (O_NONBLOCK, then fstat): the package is whatever
     * "*.pkg" the wallpaper's folder holds, and opening a FIFO of that name
     * never returned - with the lock of the package cache held, so no other
     * packaged scene loaded after it either. */
    int fd = g_open (pkg_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
    struct stat sb;
    GMappedFile *map = fd >= 0 && fstat (fd, &sb) == 0 && S_ISREG (sb.st_mode)
                           ? g_mapped_file_new_from_fd (fd, FALSE, NULL) : NULL;
    if (fd >= 0)
        close (fd);     /* the mapping stays */
    if (!map) {
        set_ioerr (err, "pkg: cannot open %s", pkg_path);
        return FALSE;
    }
    const guint8 *buf = (const guint8 *) g_mapped_file_get_contents (map);
    gsize len = g_mapped_file_get_length (map);
    if (!buf || len < 24) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "pkg: too small: %s", pkg_path);
        g_mapped_file_unref (map);
        return FALSE;
    }

    gsize off = 0;
    guint32 mlen = rd32 (buf + off);
    off += 4;
    if (mlen != 8 || off + mlen + 4 > len || memcmp (buf + off, "PKGV", 4) != 0) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "pkg: bad magic in %s", pkg_path);
        g_mapped_file_unref (map);
        return FALSE;
    }
    off += mlen;
    guint32 n = rd32 (buf + off);
    off += 4;
    if (n > 100000) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "pkg: absurd file count %u", n);
        g_mapped_file_unref (map);
        return FALSE;
    }

    /* pass 1: parse the whole entry table; offsets are relative to the
     * table's END (linux-wallpaperengine's baseOffset) */
    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
    GHashTable *dirs = NULL;     /* directories the entries so far lie in */
    GArray *foffs = g_array_new (FALSE, FALSE, sizeof (guint32));
    GArray *fszs = g_array_new (FALSE, FALSE, sizeof (guint32));
    for (guint32 i = 0; i < n && off + 8 <= len; i++) {
        guint32 pl = rd32 (buf + off);
        off += 4;
        if (pl > 1024 || off + pl + 8 > len) {
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "pkg: entry %u corrupt", i);
            goto fail;
        }
        char *path = g_malloc (pl + 1);
        memcpy (path, buf + off, pl);
        path[pl] = 0;
        off += pl;
        guint32 foff = rd32 (buf + off);
        off += 4;
        guint32 fsz = rd32 (buf + off);
        off += 4;
        g_ptr_array_add (paths, path);
        g_array_append_val (foffs, foff);
        g_array_append_val (fszs, fsz);
    }
    gsize base = off;
    GError *ioerr = NULL;
    /* entries of a real pkg follow each other in the data region. Entries
     * that share bytes let a small archive write without limit (100000
     * entries x one blob); a few times the archive's own size is the most
     * an extraction may put on disk */
    guint64 written = 0, budget = (guint64) len * 4 + (16u << 20);
    guint objects = 0;
    dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    /* pass 2: every entry is checked before the first one is written (a
     * package that is refused writes nothing), pass 3: extract */
    for (int writing = 0; writing < 2; writing++) {
        for (guint32 i = 0; i < paths->len; i++) {
            const char *path = g_ptr_array_index (paths, i);
            guint32 foff = g_array_index (foffs, guint32, i);
            guint32 fsz = g_array_index (fszs, guint32, i);
            if (!safe_rel_path (path) || base > len || foff > len - base ||
                fsz > len - base - foff) {
                if (!writing)
                    g_debug ("pkg: skipping entry %s", walpe_log_str (path));
                continue;
            }
            /* the cache's own bookkeeping files are not the package's to
             * write */
            if (pkg_reserved_path (path)) {
                if (!writing)
                    g_debug ("pkg: skipping entry %s (reserved name)", walpe_log_str (path));
                continue;
            }
            if (!writing) {
                if (pkg_path_depth (path) > PKG_MAX_DEPTH) {
                    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "pkg: the path of an entry has more than %d parts: %s",
                                 PKG_MAX_DEPTH, pkg_path);
                    goto fail;
                }
                objects += 1 + pkg_new_dirs (dirs, path);
                if (objects > PKG_MAX_OBJECTS) {
                    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "pkg: more than %d files and directories: %s", PKG_MAX_OBJECTS,
                                 pkg_path);
                    goto fail;
                }
                written += fsz;
                if (written > budget) {
                    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "pkg: entries overlap (they add up to more than the archive "
                                 "holds): %s", pkg_path);
                    goto fail;
                }
                continue;
            }
            char *dst = g_build_filename (dest_dir, path, NULL);
            char *dstdir = g_path_get_dirname (dst);
            /* the copy is the user's alone (it was 0755: with a permissive
             * umask others could add files to what the daemon then loads) */
            g_mkdir_with_parents (dstdir, 0700);
            g_free (dstdir);
            if (!g_file_set_contents (dst, (const char *) buf + base + foff, fsz, &ioerr)) {
                g_propagate_error (err, ioerr);
                ioerr = NULL;
                g_free (dst);
                goto fail;
            }
            g_free (dst);
        }
    }

    g_hash_table_unref (dirs);
    g_array_free (foffs, TRUE);
    g_array_free (fszs, TRUE);
    g_ptr_array_free (paths, TRUE);
    g_mapped_file_unref (map);
    return TRUE;

fail:
    g_clear_pointer (&dirs, g_hash_table_unref);
    g_array_free (foffs, TRUE);
    g_array_free (fszs, TRUE);
    g_ptr_array_free (paths, TRUE);
    g_mapped_file_unref (map);
    return FALSE;
}

/* extract pkg_path into a stable cache dir keyed by the project dir's
 * basename + a hash of its full path (two projects with the same folder
 * name no longer collide); re-extracts when the pkg size/mtime changes.
 * Serialized: every monitor thread loads the same wallpaper at once and
 * must not extract it concurrently (the second caller finds the stamp).
 * Returns the cache dir (g_free) or NULL with err set. */
static GMutex pkg_lock;

/* ---- the cache is bounded -------------------------------------------
 * Every packaged scene ever opened used to stay extracted for good (the
 * disk use of each one doubled, and nothing removed the copy of a
 * wallpaper that was deleted or unsubscribed). Now, after an extraction
 * has added to the cache:
 *   - a copy whose package no longer exists is removed,
 *   - a copy without a stamp (an extraction that was interrupted) is
 *     removed,
 *   - the least recently used copies are removed until the cache is back
 *     under its limit (1 GiB; WALPE_PKG_CACHE_MB=<n> to change it, 0 = no
 *     limit).
 * A copy a loaded wallpaper reads from is never removed: the caller holds
 * it from walpe_pkg_extract_cached to walpe_pkg_cache_release. An evicted
 * wallpaper is simply extracted again the next time it is opened (about
 * 0.1-0.3 s per 100 MB). Only the daemon uses the cache. */
static GHashTable *pkg_live;         /* cache dir -> holds (pkg_lock) */

static gint64 pkg_cache_limit (void)
{
    const char *e = g_getenv ("WALPE_PKG_CACHE_MB");
    gint64 mb = 1024;
    if (e && *e) {
        char *end = NULL;
        gint64 v = g_ascii_strtoll (e, &end, 10);
        if (end && !*end && v >= 0)
            mb = v;
    }
    return mb > 0 ? mb << 20 : G_MAXINT64;
}

/* bytes under dir; nothing is followed out of it (symlinks count as
 * themselves) and the walk is bounded */
static gint64 pkg_tree_size (const char *dir, int depth, guint *budget)
{
    gint64 total = 4096;    /* the directory itself: one block */
    GDir *d = depth < 2 * PKG_MAX_DEPTH ? g_dir_open (dir, 0, NULL) : NULL;
    const char *n;
    while (d && *budget && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        GStatBuf st;
        (*budget)--;
        if (g_lstat (p, &st) == 0) {
            if (S_ISDIR (st.st_mode))
                total += pkg_tree_size (p, depth + 1, budget);
            else   /* apparent size in 4 KiB blocks: the same on every filesystem */
                total += ((gint64) st.st_size + 4095) & ~(gint64) 4095;
        }
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    return total;
}

/* remove a cache dir: files and symlinks are unlinked, only real
 * directories are entered */
static void pkg_tree_remove (const char *dir, int depth)
{
    GDir *d = depth < 2 * PKG_MAX_DEPTH ? g_dir_open (dir, 0, NULL) : NULL;
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        GStatBuf st;
        if (g_lstat (p, &st) == 0 && S_ISDIR (st.st_mode))
            pkg_tree_remove (p, depth + 1);
        else
            g_unlink (p);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (dir);
}

typedef struct { char *dir; gint64 used, size; } PkgCopy;

static gint pkg_copy_cmp (gconstpointer a, gconstpointer b)
{
    const PkgCopy *x = a, *y = b;
    return x->used < y->used ? -1 : x->used > y->used;
}

/* pkg_lock held; keep = the copy just written */
static void pkg_cache_trim_locked (const char *root, const char *keep)
{
    gint64 limit = pkg_cache_limit (), total = 0;
    GArray *copies = g_array_new (FALSE, FALSE, sizeof (PkgCopy));
    GDir *d = g_dir_open (root, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        PkgCopy c = { g_build_filename (root, n, NULL), 0, 0 };
        GStatBuf st;
        if (g_lstat (c.dir, &st) != 0 || !S_ISDIR (st.st_mode)) {
            g_free (c.dir);
            continue;
        }
        gboolean live = !strcmp (c.dir, keep) || (pkg_live && g_hash_table_contains (pkg_live, c.dir));
        char *stamp = g_build_filename (c.dir, PKG_STAMP, NULL);
        char *srcf = g_build_filename (c.dir, PKG_SRC, NULL);
        char *src = NULL;
        gboolean stamped = g_stat (stamp, &st) == 0;
        c.used = stamped ? (gint64) st.st_mtime : 0;
        gboolean gone = g_file_get_contents (srcf, &src, NULL, NULL) && src && *src == '/' &&
                        !g_file_test (src, G_FILE_TEST_EXISTS);
        g_free (stamp);
        g_free (srcf);
        if (!live && (!stamped || gone)) {
            g_message ("pkg: removing cached copy %s (%s)", walpe_log_str (c.dir),
                       gone ? "its package is gone" : "incomplete");
            pkg_tree_remove (c.dir, 0);
            g_free (src);
            g_free (c.dir);
            continue;
        }
        g_free (src);
        guint budget = 200000;
        c.size = pkg_tree_size (c.dir, 0, &budget);
        total += c.size;
        if (live)
            g_free (c.dir);          /* counted, never a candidate */
        else
            g_array_append_val (copies, c);
    }
    if (d)
        g_dir_close (d);
    g_array_sort (copies, pkg_copy_cmp);   /* least recently used first */
    for (guint i = 0; i < copies->len; i++) {
        PkgCopy *c = &g_array_index (copies, PkgCopy, i);
        if (total > limit) {
            g_message ("pkg: cache over its limit (%" G_GINT64_FORMAT " MiB): removing %s "
                       "(%" G_GINT64_FORMAT " MiB)", limit >> 20, c->dir, c->size >> 20);
            pkg_tree_remove (c->dir, 0);
            total -= c->size;
        }
        g_free (c->dir);
    }
    g_array_free (copies, TRUE);
}

/* pkg_lock held */
static void pkg_hold_locked (const char *cache)
{
    if (!pkg_live)
        pkg_live = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    guint n = GPOINTER_TO_UINT (g_hash_table_lookup (pkg_live, cache));
    g_hash_table_insert (pkg_live, g_strdup (cache), GUINT_TO_POINTER (n + 1));
}

void walpe_pkg_cache_release (const char *cache_dir)
{
    if (!cache_dir)
        return;
    g_mutex_lock (&pkg_lock);
    guint n = pkg_live ? GPOINTER_TO_UINT (g_hash_table_lookup (pkg_live, cache_dir)) : 0;
    if (n > 1)
        g_hash_table_insert (pkg_live, g_strdup (cache_dir), GUINT_TO_POINTER (n - 1));
    else if (n == 1)
        g_hash_table_remove (pkg_live, cache_dir);
    g_mutex_unlock (&pkg_lock);
}

char *walpe_pkg_extract_cached (const char *pkg_path, const char *project_dir, GError **err)
{
    char *bn = g_path_get_basename (project_dir);
    char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, project_dir, -1);
    char *base = g_strdup_printf ("%s-%.8s", bn, hash);
    g_free (bn);
    g_free (hash);
    GStatBuf st;
    gint64 mtime = 0, size = 0;
    if (g_stat (pkg_path, &st) == 0) {
        mtime = (gint64) st.st_mtime;
        size = (gint64) st.st_size;
    }
    char *cache = g_build_filename (g_get_user_cache_dir (), "walpe", "pkg", base, NULL);
    char *stamp = g_build_filename (cache, PKG_STAMP, NULL);
    char *stampcontent = g_strdup_printf ("v3:%lld:%lld", (long long) mtime, (long long) size);

    g_mutex_lock (&pkg_lock);
    char *old = NULL;
    g_file_get_contents (stamp, &old, NULL, NULL);
    char *cpj = g_build_filename (cache, "project.json", NULL);
    gboolean fresh = old && !g_strcmp0 (old, stampcontent) &&
                     g_file_test (cpj, G_FILE_TEST_IS_REGULAR);
    g_free (cpj);
    g_free (old);
    if (fresh) {
        g_utime (stamp, NULL);   /* used now: the last to be evicted */
        pkg_hold_locked (cache);
        g_mutex_unlock (&pkg_lock);
        g_free (stampcontent);
        g_free (stamp);
        g_free (base);
        return cache; /* up to date */
    }

    /* the package changed (or the copy is incomplete): the copy is made
     * anew, not written over - files the new package no longer has used to
     * stay and were loaded as if they were its own. A copy a loaded
     * wallpaper still reads from is written over as before. */
    if (!(pkg_live && g_hash_table_contains (pkg_live, cache)) &&
        g_file_test (cache, G_FILE_TEST_IS_DIR) && !g_file_test (cache, G_FILE_TEST_IS_SYMLINK))
        pkg_tree_remove (cache, 0);
    GError *e = NULL;
    if (!walpe_pkg_extract (pkg_path, cache, &e)) {
        /* what a refused package wrote before it was refused goes again
         * (unless a loaded wallpaper still reads an older copy there) */
        if (!(pkg_live && g_hash_table_contains (pkg_live, cache)) &&
            g_file_test (cache, G_FILE_TEST_IS_DIR) && !g_file_test (cache, G_FILE_TEST_IS_SYMLINK))
            pkg_tree_remove (cache, 0);
        g_mutex_unlock (&pkg_lock);
        g_propagate_error (err, e);
        g_free (stampcontent);
        g_free (stamp);
        g_free (cache);
        g_free (base);
        return NULL;
    }
    /* carry project.json over: it lives next to the pkg, not inside it
     * (always refreshed - a workshop update changes both) */
    char *srcpj = g_build_filename (project_dir, "project.json", NULL);
    char *dstpj = g_build_filename (cache, "project.json", NULL);
    if (g_file_test (srcpj, G_FILE_TEST_IS_REGULAR)) {
        GError *ce = NULL;
        gsize l = 0;
        char *data = NULL;
        if (g_file_get_contents (srcpj, &data, &l, &ce))
            g_file_set_contents (dstpj, data, l, &ce);
        g_clear_error (&ce);
        g_free (data);
    }
    g_free (srcpj);
    g_free (dstpj);
    g_file_set_contents (stamp, stampcontent, -1, NULL);
    char *srcf = g_build_filename (cache, PKG_SRC, NULL);
    g_file_set_contents (srcf, pkg_path, -1, NULL);
    g_free (srcf);
    pkg_hold_locked (cache);
    /* the cache grew: bring it back under its limit */
    char *root = g_path_get_dirname (cache);
    pkg_cache_trim_locked (root, cache);
    g_free (root);
    g_mutex_unlock (&pkg_lock);
    g_free (stampcontent);
    g_free (stamp);
    g_free (base);
    return cache;
}
