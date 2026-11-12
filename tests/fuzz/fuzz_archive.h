/* fuzz_archive.h - the file archive of the scene harnesses:
 *     === scene.json\n<bytes>\n=== materials/a.json\n<bytes> ...
 * A line "=== <relative name>" starts a file (at most 24 files; names of
 * letters, digits and . _ - / only). fuzz_unpack () writes them below
 * fuzz_archive_dir, after removing the files of the call before. */
#pragma once
#include "fuzz_common.h"
#include <glib/gstdio.h>

#define MAX_FILES 24
static const char *fuzz_archive_dir;
static char *written[MAX_FILES];
static int n_written;

static gboolean name_ok (const char *n)
{
    /* (no package: its copy would be written to the user's cache) */
    if (!*n || *n == '/' || strstr (n, "..") || strlen (n) > 80 || g_str_has_suffix (n, ".pkg"))
        return FALSE;
    for (const char *c = n; *c; c++)
        if (!g_ascii_isalnum (*c) && !strchr ("._-/", *c))
            return FALSE;
    return !g_str_has_suffix (n, "/");
}

static void put (const char *rel, const uint8_t *d, size_t n)
{
    if (n_written >= MAX_FILES)
        return;
    char *p = g_build_filename (fuzz_archive_dir, rel, NULL);
    char *pd = g_path_get_dirname (p);
    g_mkdir_with_parents (pd, 0700);
    g_free (pd);
    if (g_file_set_contents (p, (const char *) d, (gssize) n, NULL))
        written[n_written++] = p;
    else
        g_free (p);
}

/* skip: a file name that is not written (NULL: none) */
static void fuzz_unpack (const uint8_t *data, size_t size, const char *skip)
{
    for (int i = 0; i < n_written; i++) {
        g_unlink (written[i]);
        g_free (written[i]);
    }
    n_written = 0;
    static const char mark[] = "=== ";
    const uint8_t *end = data + size, *p = data;
    if (size < 5 || memcmp (p, mark, 4))
        return;
    while (p < end) {
        /* p at a mark */
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
        if (name_ok (name) && g_strcmp0 (name, skip))
            put (name, body, (size_t) (next - body));
        g_free (name);
        p = next < end ? next + 1 : end;
    }
}
