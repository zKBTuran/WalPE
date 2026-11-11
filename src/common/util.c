#include "util.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static char *expand_path_raw (const char *path)
{
    if (!path || !*path)
        return NULL;
    if (path[0] == '~') {
        const char *rest = path + 1;
        if (*rest == '/' || *rest == '\0') {
            const char *home = g_get_home_dir ();
            return g_build_filename (home, rest[0] ? rest + 1 : "", NULL);
        }
        /* ~user */
        char *user = g_strndup (rest, strcspn (rest, "/"));
        struct passwd *pw = getpwnam (user);
        g_free (user);
        if (pw)
            return g_build_filename (pw->pw_dir, strchr (rest, '/') ? strchr (rest, '/') + 1 : "", NULL);
        return g_strdup (path);
    }
    if (path[0] != '/') {
        char *cwd = g_get_current_dir ();
        char *abs = g_build_filename (cwd, path, NULL);
        g_free (cwd);
        return abs;
    }
    return g_strdup (path);
}

/* One spelling per file. A wallpaper's path is a key: for its options in
 * config.json, for "which monitor shows it", for the scene's own lookups.
 * `walpe-ctl open dir/` (tab completion), "dir//x" or "a/./b" named the
 * same wallpaper as the GUI's "dir" but matched none of them: options set
 * in the GUI were saved and never applied. Redundant separators, "." and
 * ".." components and the trailing separator go; symbolic links are not
 * resolved (the path stays the one the user knows). */
char *walpe_expand_path (const char *path)
{
    char *abs = expand_path_raw (path);
    if (!abs || abs[0] != '/')
        return abs;
#if GLIB_CHECK_VERSION (2, 58, 0)
    char *canon = g_canonicalize_filename (abs, NULL);
    g_free (abs);
    return canon;
#else
    for (gsize n = strlen (abs); n > 1 && abs[n - 1] == '/'; n--)
        abs[n - 1] = 0;
    return abs;
#endif
}

char *walpe_read_file (const char *path, gsize *len)
{
    char *data = NULL;
    /* regular files only: a device or FIFO named by a wallpaper
     * ("/dev/zero" as a texture) would be read until memory runs out */
    if (!path || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return NULL;
    if (!g_file_get_contents (path, &data, len, NULL))
        return NULL;
    return data;
}

gboolean walpe_file_exists (const char *path)
{
    return path && g_file_test (path, G_FILE_TEST_EXISTS);
}

/* path, which the text of its name puts below root, is still below root
 * once symbolic links are resolved. Nothing there (yet): the text decides. */
static gboolean real_path_inside (const char *path, const char *root)
{
    char *rp = realpath (path, NULL);
    if (!rp)
        return errno == ENOENT || errno == ENOTDIR;
    char *rr = realpath (root, NULL);
    gboolean in = FALSE;
    if (rr) {
        gsize n = strlen (rr);
        while (n > 1 && rr[n - 1] == G_DIR_SEPARATOR)
            n--;
        in = n == 1 ? rp[1] != '\0'
                    : strncmp (rp, rr, n) == 0 && rp[n] == G_DIR_SEPARATOR && rp[n + 1] != '\0';
    }
    free (rp);
    free (rr);
    return in;
}

gboolean walpe_path_is_inside (const char *path, const char *root)
{
    if (!path || !*path || !root || !*root)
        return FALSE;
    /* by its text first: "." and ".." are folded */
    char *cp = g_canonicalize_filename (path, NULL);
    char *cr = g_canonicalize_filename (root, NULL);
    gsize n = strlen (cr);
    while (n > 1 && cr[n - 1] == G_DIR_SEPARATOR)
        n--;
    gboolean in = n == 1 ? cp[0] == G_DIR_SEPARATOR && cp[1] != '\0'
                         : strncmp (cp, cr, n) == 0 && cp[n] == G_DIR_SEPARATOR && cp[n + 1] != '\0';
    /* ... then as the file system resolves it. WalPE's own Workshop install
     * and the .pkg extraction write no symbolic links, but a folder Steam
     * downloaded, or one the user unpacked from an archive, can hold one:
     * "materials/a.tex" -> a file anywhere the user can read was opened as
     * the wallpaper's own texture, json or font. A link that stays inside
     * the wallpaper (or a wallpaper folder that is itself reached through
     * links) is fine.
     * The name as given is resolved, not the folded one: the kernel takes
     * "linkdir/../x" through the link's target, the folding does not. */
    if (in)
        in = real_path_inside (path, cr);
    g_free (cp);
    g_free (cr);
    return in;
}

char *walpe_asset_path (const char *root, const char *name)
{
    if (!root || !*root || !name || !*name)
        return NULL;
    char *p = g_build_filename (root, name, NULL);
    if (walpe_path_is_inside (p, root) && g_file_test (p, G_FILE_TEST_IS_REGULAR))
        return p;
    g_free (p);
    return NULL;
}

gboolean walpe_dir_is_private (const char *dir)
{
    GStatBuf st;
    return dir && *dir && g_stat (dir, &st) == 0 && S_ISDIR (st.st_mode) &&
           st.st_uid == getuid () && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

gboolean walpe_write_private (const char *path, const char *data, gssize len, GError **err)
{
    if (!path || !data) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "nothing to write");
        return FALSE;
    }
    gsize n = len < 0 ? strlen (data) : (gsize) len, off = 0;
    /* a name of its own (O_EXCL, never an existing file or a link), 0600
     * from the first moment */
    char *tmp = g_strconcat (path, ".XXXXXX", NULL);
    int fd = g_mkstemp_full (tmp, O_WRONLY | O_CLOEXEC, 0600);
    gboolean ok = fd >= 0;
    if (ok) {
        fchmod (fd, 0600);   /* a umask may have taken the owner's bits too */
        while (ok && off < n) {
            ssize_t w = write (fd, data + off, n - off);
            if (w < 0 && errno == EINTR)
                continue;
            ok = w > 0;
            off += w > 0 ? (gsize) w : 0;
        }
        ok = ok && fsync (fd) == 0;
        ok = (close (fd) == 0) && ok;
        ok = ok && g_rename (tmp, path) == 0;
    }
    if (!ok) {
        int e = errno;
        if (fd >= 0)
            g_unlink (tmp);
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "cannot write %s: %s", path,
                     g_strerror (e));
    }
    g_free (tmp);
    return ok;
}

char *walpe_dir_of (const char *path)
{
    char *abs = walpe_expand_path (path);
    char *dir = g_path_get_dirname (abs);
    g_free (abs);
    return dir;
}

/* ------------------------------------------------------------------ json */

const char *wpj_str (JsonObject *o, const char *k, const char *dflt)
{
    if (!o) return dflt;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_VALUE (n))
        return dflt;
    const char *s = json_node_get_string (n);
    return s ? s : dflt;
}

static JsonNode *numeric_node (JsonObject *o, const char *k)
{
    if (!o) return NULL;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n))
        return NULL;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE || t == G_TYPE_INT64 || t == G_TYPE_INT)
        return n;
    return NULL;
}

double wpj_num (JsonObject *o, const char *k, double dflt)
{
    JsonNode *n = numeric_node (o, k);
    return n ? json_node_get_double (n) : dflt;
}

gboolean wpj_bool (JsonObject *o, const char *k, gboolean dflt)
{
    if (!o) return dflt;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_VALUE (n))
        return dflt;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_BOOLEAN) return json_node_get_boolean (n);
    if (t == G_TYPE_DOUBLE || t == G_TYPE_INT64) return json_node_get_double (n) != 0.0;
    return dflt;
}

JsonObject *wpj_obj (JsonObject *o, const char *k)
{
    if (!o) return NULL;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_OBJECT (n))
        return NULL;
    return json_node_get_object (n);
}

JsonArray *wpj_arr (JsonObject *o, const char *k)
{
    if (!o) return NULL;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_ARRAY (n))
        return NULL;
    return json_node_get_array (n);
}

guint wpj_arr_len (JsonArray *a)
{
    return a ? json_array_get_length (a) : 0;
}

JsonObject *wpj_arr_obj (JsonArray *a, guint i)
{
    if (!a || i >= json_array_get_length (a))
        return NULL;
    JsonNode *n = json_array_get_element (a, i);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_OBJECT (n))
        return NULL;
    return json_node_get_object (n);
}

gboolean wpj_num2 (JsonObject *o, const char *k, double *x, double *y)
{
    JsonArray *a = wpj_arr (o, k);
    if (a && json_array_get_length (a) >= 2) {
        JsonNode *e0 = json_array_get_element (a, 0);
        JsonNode *e1 = json_array_get_element (a, 1);
        if (JSON_NODE_HOLDS_VALUE (e0) && JSON_NODE_HOLDS_VALUE (e1)) {
            *x = json_node_get_double (e0);
            *y = json_node_get_double (e1);
            return TRUE;
        }
    }
    JsonNode *n = numeric_node (o, k);
    if (n) {
        *x = *y = json_node_get_double (n);
        return TRUE;
    }
    return FALSE;
}

/* json-glib's parser recurses once per nested array / object and has no
 * depth limit before 1.10: one IPC line of 150000 '[' - or a scene.json
 * built that way - overflowed the stack and killed the process. Real files
 * nest a few dozen levels at most. Brackets inside strings do not count.
 *
 * The count has to read the text as the parser's scanner does, and
 * json-glib up to 1.8 reads more than JSON: 'single-quoted' strings
 * (without escapes) and a comment that runs from one '/' to the next. A
 * '"' inside one of those opened a string for this count only, so every
 * bracket behind it went uncounted: 150000 '[' after the five bytes of a
 * comment holding one quote crashed exactly as before. The plain reading
 * (double-quoted strings only) is exact up to the first ', / or # outside
 * a string; a text that has one - no file Wallpaper Engine writes does - is
 * counted again under every reading a scanner may have of quotes, slashes
 * and hashes (36: also the C and line comments other readers of "JSON with
 * comments" know), and the deepest reading decides. tests/parsers_test.c
 * holds this against the installed json-glib with random texts. */
#define WPJ_MAX_DEPTH 512

enum { WPJ_SQ_NONE, WPJ_SQ_RAW, WPJ_SQ_ESC };    /* 'strings': not known, without, with \ escapes */
enum { WPJ_SL_NONE, WPJ_SL_PAIR, WPJ_SL_LINE };  /* a '/': nothing, a comment to the next '/', "//" to end of line */

/* the deepest nesting of d under one reading (sq, slash as above; cc: C
 * comments; hash: '#' to end of line). lax (plain reading only): set when
 * a ', / or # stands outside a double-quoted string */
static int json_depth_as (const char *d, gsize len, int sq, gboolean cc, int slash, gboolean hash,
                          gboolean *lax)
{
    int depth = 0, deepest = 0;
    for (gsize i = 0; i < len; i++) {
        char c = d[i];
        if (c == '"' || (c == '\'' && sq != WPJ_SQ_NONE)) {
            gboolean esc = c == '"' || sq == WPJ_SQ_ESC;
            for (i++; i < len && d[i] != c; i++)
                if (esc && d[i] == '\\')
                    i++;
        } else if (c == '/' && cc && i + 1 < len && d[i + 1] == '*') {
            for (i += 2; i + 1 < len && !(d[i] == '*' && d[i + 1] == '/'); i++)
                ;
            i++;
        } else if (c == '/' && slash == WPJ_SL_PAIR) {
            for (i++; i < len && d[i] != '/'; i++)
                ;
        } else if ((c == '/' && slash == WPJ_SL_LINE && i + 1 < len && d[i + 1] == '/') ||
                   (c == '#' && hash)) {
            for (i++; i < len && d[i] != '\n'; i++)
                ;
        } else if (c == '[' || c == '{') {
            if (++depth > deepest)
                deepest = depth;
        } else if ((c == ']' || c == '}') && depth > 0) {
            depth--;
        } else if (lax && (c == '\'' || c == '/' || c == '#')) {
            *lax = TRUE;
        }
    }
    return deepest;
}

static gboolean json_depth_ok (const char *d, gsize len)
{
    gboolean lax = FALSE;
    if (json_depth_as (d, len, WPJ_SQ_NONE, FALSE, WPJ_SL_NONE, FALSE, &lax) > WPJ_MAX_DEPTH)
        return FALSE;
    if (!lax)
        return TRUE;
    for (int sq = WPJ_SQ_NONE; sq <= WPJ_SQ_ESC; sq++)
        for (int cc = 0; cc < 2; cc++)
            for (int slash = WPJ_SL_NONE; slash <= WPJ_SL_LINE; slash++)
                for (int hash = 0; hash < 2; hash++)
                    if (json_depth_as (d, len, sq, cc, slash, hash, NULL) > WPJ_MAX_DEPTH)
                        return FALSE;
    return TRUE;
}

/* the parse itself; *msg (if asked for) = why the text was refused, to be
 * freed by the caller; a text that parses but is no object gives NULL
 * without a message, as before */
static JsonNode *wpj_parse_quiet (const char *data, gssize len, char **msg)
{
    if (msg)
        *msg = NULL;
    if (!json_depth_ok (data, (gsize) len)) {
        if (msg)
            *msg = g_strdup_printf ("nested deeper than %d levels", WPJ_MAX_DEPTH);
        return NULL;
    }
    g_autoptr (JsonParser) p = json_parser_new ();
    g_autoptr (GError) err = NULL;
    if (!json_parser_load_from_data (p, data, len, &err)) {
        if (msg)
            *msg = g_strdup (err ? err->message : "?");
        return NULL;
    }
    JsonNode *root = json_parser_get_root (p);
    if (!root || !JSON_NODE_HOLDS_OBJECT (root))
        return NULL;
    return json_node_copy (root);
}

JsonNode *wpj_parse_data (const char *data, gssize len)
{
    if (!data) return NULL;
    if (len < 0)
        len = (gssize) strlen (data);
    g_autofree char *msg = NULL;
    JsonNode *n = wpj_parse_quiet (data, len, &msg);
    if (!n && msg)
        g_warning ("json parse failed: %s", msg);
    return n;
}

/* Wallpaper Engine's own JSON reader accepts a comma before the closing
 * ']' or '}' and its files rely on it (the builtin fluid simulation's
 * effect.json, hand-edited Workshop scenes); json-glib does not. Blank
 * such commas (outside strings; same length, so the positions in a later
 * error message still fit). TRUE when one was found. */
static gboolean json_blank_trailing_commas (char *d, gsize len)
{
    gboolean in_str = FALSE, any = FALSE;
    for (gsize i = 0; i < len; i++) {
        char c = d[i];
        if (in_str) {
            if (c == '\\')
                i++;
            else if (c == '"')
                in_str = FALSE;
        } else if (c == '"') {
            in_str = TRUE;
        } else if (c == ',') {
            gsize j = i + 1;
            while (j < len && (d[j] == ' ' || d[j] == '\t' || d[j] == '\r' || d[j] == '\n'))
                j++;
            if (j < len && (d[j] == ']' || d[j] == '}')) {
                d[i] = ' ';
                any = TRUE;
            }
        }
    }
    return any;
}

/* A JSON file a wallpaper brings is read whole and becomes a tree of about
 * twenty times its size: one of a gigabyte (cheap to ship, the Workshop
 * download is compressed) took all memory before anything looked at it.
 * The largest scene.json in 88 packaged Workshop scenes is 1.5 MB. */
#define WPJ_MAX_FILE_BYTES ((goffset) 32 * 1024 * 1024)

JsonNode *wpj_parse_file (const char *path)
{
    gsize len = 0;
    GStatBuf st;
    if (path && g_stat (path, &st) == 0 && (goffset) st.st_size > WPJ_MAX_FILE_BYTES) {
        g_warning ("json: %s is %.0f MiB, more than a wallpaper's json file may have (%d MiB); "
                   "not read", path, st.st_size / 1048576.0, (int) (WPJ_MAX_FILE_BYTES >> 20));
        return NULL;
    }
    char *data = walpe_read_file (path, &len);
    if (!data)
        return NULL;
    if ((goffset) len > WPJ_MAX_FILE_BYTES) {   /* grew between the look and the read */
        g_free (data);
        return NULL;
    }
    /* Wallpaper Engine json files sometimes carry a UTF-8 BOM */
    if (len >= 3 && (guchar) data[0] == 0xEF && (guchar) data[1] == 0xBB && (guchar) data[2] == 0xBF) {
        len -= 3;
        memmove (data, data + 3, len);
    }
    /* files only (IPC lines and shader annotations stay strict): a file
     * that fails as written is tried once more without trailing commas */
    g_autofree char *msg = NULL;
    JsonNode *n = wpj_parse_quiet (data, (gssize) len, &msg);
    if (!n && msg && json_blank_trailing_commas (data, len))
        n = wpj_parse_quiet (data, (gssize) len, NULL);
    if (!n && msg)
        g_warning ("json parse failed: %s", msg);
    g_free (data);
    return n;
}

/* escape " and \ so a path can be embedded in a hand-built JSON string */
char *walpe_json_escape (const char *s)
{
    if (!s)
        return g_strdup ("");
    GString *g = g_string_new (NULL);
    for (const char *p = s; *p; p++) {
        if ((guchar) *p < 0x20) {
            /* control chars must be escaped in JSON; a raw \n in a file
             * name also split the newline-delimited IPC request */
            g_string_append_printf (g, "\\u%04x", (guchar) *p);
            continue;
        }
        if (*p == '"' || *p == '\\')
            g_string_append_c (g, '\\');
        g_string_append_c (g, *p);
    }
    return g_string_free (g, FALSE);
}
