/* nocase.h - a file a wallpaper names, found the way Windows finds it
 *
 * Wallpaper Engine runs on Windows, where "Materials/Sky.TEX" opens the
 * file materials/sky.tex and "js\app.js" the file js/app.js. A wallpaper
 * made there may name its own files in another letter case than they have
 * on disk (hand-written pages of web wallpapers do it often: "JS/App.js"
 * next to a folder js) or with backslashes. Here such a name found
 * nothing: the layer, the effect, the font, the page's script or picture
 * was missing, without a word a user could act on.
 *
 * walpe_path_nocase () is asked only after the name as written found no
 * file - a name that matches costs what it always did. It walks the name
 * one part at a time below `root`: a part that exists as written is taken
 * as it is (one lstat); only for a part that does not, its directory is
 * read for an entry that differs in letter case alone. Two such entries
 * cannot exist on Windows; where a folder made on Linux has them, the one
 * that sorts first is taken, so that every run picks the same file.
 *
 * It is no check of its own: the path it returns is put through the same
 * tests as a name that matched (below the wallpaper once links are
 * resolved, a regular file) by the caller. It never climbs: ".." takes
 * back a part of the name and nothing above `root`.
 *
 * Header only (common/util.c and engine/web.c include it; the web module
 * needs no new function of the daemon for it). */
#pragma once
#include <glib.h>
#include <string.h>
#include <sys/stat.h>

/* Do two names of directory entries differ in letter case only? Text that
 * is UTF-8 is compared the way g_utf8_casefold folds it (the letters of
 * every script); other bytes by ASCII letters alone. */
static inline gboolean walpe_name_nocase_equal (const char *a, const char *b_folded, gboolean b_utf8,
                                                const char *b_raw)
{
    if (b_utf8 && g_utf8_validate (a, -1, NULL)) {
        char *af = g_utf8_casefold (a, -1);
        gboolean eq = !strcmp (af, b_folded);
        g_free (af);
        return eq;
    }
    return !g_ascii_strcasecmp (a, b_raw);
}

/* The entry of `dir` that is `part` in another letter case: its name as it
 * is on disk (g_free), or NULL. */
static inline char *walpe_dir_entry_nocase (const char *dir, const char *part)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d)
        return NULL;
    gboolean utf8 = g_utf8_validate (part, -1, NULL);
    char *folded = utf8 ? g_utf8_casefold (part, -1) : NULL;
    char *best = NULL;
    const char *e;
    while ((e = g_dir_read_name (d)))
        if (walpe_name_nocase_equal (e, folded, utf8, part) && (!best || strcmp (e, best) < 0)) {
            g_free (best);
            best = g_strdup (e);
        }
    g_dir_close (d);
    g_free (folded);
    return best;
}

/* root/name with every part of name spelled as it is on disk, or NULL when
 * a part names nothing in any letter case. backslash: '\' separates parts
 * too. The result exists (a file, a directory, a link ...); what it is and
 * where it leads is the caller's to check. g_free() */
static inline char *walpe_path_nocase (const char *root, const char *name, gboolean backslash)
{
    if (!root || !*root || !name || !*name)
        return NULL;
    const char *seps = backslash ? "/\\" : "/";
    /* the parts of the name, "." dropped and ".." folded inside it */
    GPtrArray *parts = g_ptr_array_new_with_free_func (g_free);
    gboolean ok = TRUE;
    for (const char *c = name; *c && ok;) {
        gsize n = strcspn (c, seps);
        if (n == 2 && c[0] == '.' && c[1] == '.') {
            if (parts->len)
                g_ptr_array_remove_index (parts, parts->len - 1);
            else
                ok = FALSE;         /* would leave root */
        } else if (n && !(n == 1 && c[0] == '.')) {
            g_ptr_array_add (parts, g_strndup (c, n));
        }
        c += n;
        if (*c)
            c++;
    }
    if (!parts->len)
        ok = FALSE;
    char *cur = ok ? g_strdup (root) : NULL;
    for (guint i = 0; cur && i < parts->len; i++) {
        const char *part = g_ptr_array_index (parts, i);
        char *next = g_build_filename (cur, part, NULL);
        struct stat st;
        if (lstat (next, &st) != 0) {
            g_free (next);
            char *as_on_disk = walpe_dir_entry_nocase (cur, part);
            next = as_on_disk ? g_build_filename (cur, as_on_disk, NULL) : NULL;
            g_free (as_on_disk);
        }
        g_free (cur);
        cur = next;
    }
    g_ptr_array_unref (parts);
    return cur;
}
