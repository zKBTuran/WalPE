#include "store.h"
#include "common/util.h"
#include "common/uitext.h"
#include "common/workshop.h"
#include "engine/content.h"
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

G_DEFINE_TYPE (WalpeEntry, walpe_entry, G_TYPE_OBJECT)

static void walpe_entry_finalize (GObject *obj)
{
    WalpeEntry *e = WALPE_ENTRY (obj);
    g_free (e->path);
    g_free (e->title);
    g_free (e->type);
    g_free (e->preview);
    g_free (e->description);
    g_free (e->id);
    g_free (e->search);
    if (e->props_root)
        json_node_unref (e->props_root);
    G_OBJECT_CLASS (walpe_entry_parent_class)->finalize (obj);
}

static void walpe_entry_class_init (WalpeEntryClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = walpe_entry_finalize;
}

static void walpe_entry_init (WalpeEntry *e)
{
    e->type = g_strdup ("image");
}

static WalpeEntry *entry_new (const char *path, const char *title, const char *type)
{
    WalpeEntry *e = g_object_new (WALPE_TYPE_ENTRY, NULL);
    e->path = g_strdup (path);
    /* a project.json's title or a file name: one clean line (uitext.h) -
     * it is shown on a tile, in a tooltip, in the detail header and in
     * status sentences. 20000 characters without a space cost 2.6 s of
     * layout per tile. */
    e->title = walpe_ui_text (title, 200, WALPE_UITEXT_LINE);
    g_strstrip (e->title);
    g_free (e->type);
    e->type = g_strdup (type);
    e->id = g_path_get_basename (path);
    return e;
}

/* casefolded haystack for the library search box */
static void entry_index (WalpeEntry *e)
{
    char *s = g_strdup_printf ("%s %s %s", e->title ? e->title : "", e->id ? e->id : "",
                               e->type ? e->type : "");
    char *n = g_utf8_normalize (s, -1, G_NORMALIZE_ALL);
    g_free (e->search);
    e->search = g_utf8_casefold (n ? n : s, -1);
    g_free (n);
    g_free (s);
}

void walpe_store_entry_free (WalpeEntry *e)
{
    if (e)
        g_object_unref (e);
}

static const char *media_exts[] = {
    "mp4", "mkv", "webm", "mov", "avi", "m4v", "gif", "webp", "png", "jpg", "jpeg",
    "bmp", "avif", "tex", NULL,
};

static gboolean is_media (const char *name)
{
    const char *dot = strrchr (name, '.');
    if (!dot)
        return FALSE;
    dot++;
    for (int i = 0; media_exts[i]; i++)
        if (!g_ascii_strcasecmp (dot, media_exts[i]))
            return TRUE;
    return FALSE;
}

static const char *classify_media (const char *name)
{
    const char *dot = strrchr (name, '.');
    dot = dot ? dot + 1 : "";
    if (!g_ascii_strcasecmp (dot, "gif") || !g_ascii_strcasecmp (dot, "webp"))
        return "gif";
    if (!g_ascii_strcasecmp (dot, "mp4") || !g_ascii_strcasecmp (dot, "mkv") ||
        !g_ascii_strcasecmp (dot, "webm") || !g_ascii_strcasecmp (dot, "mov") ||
        !g_ascii_strcasecmp (dot, "avi") || !g_ascii_strcasecmp (dot, "m4v"))
        return "video";
    return "image";
}

/* options the pane shows a control for */
static guint count_options (JsonObject *props)
{
    if (!props)
        return 0;
    guint n = 0;
    GList *ks = json_object_get_members (props);
    for (GList *k = ks; k; k = k->next) {
        const char *t = wpj_str (wpj_obj (props, k->data), "type", "");
        if (!strcmp (t, "bool") || !strcmp (t, "slider") || !strcmp (t, "combo") ||
            !strcmp (t, "color") || !strcmp (t, "textinput"))
            n++;
    }
    g_list_free (ks);
    return n;
}

gboolean walpe_entry_load_props (WalpeEntry *e)
{
    if (!e || !e->has_props)
        return FALSE;
    if (e->props_root)
        return TRUE;
    char *pj = g_build_filename (e->path, "project.json", NULL);
    JsonNode *root = wpj_parse_file (pj);
    g_free (pj);
    if (!root || !JSON_NODE_HOLDS_OBJECT (root)) {
        if (root)
            json_node_unref (root);
        return FALSE;
    }
    JsonObject *general = wpj_obj (json_node_get_object (root), "general");
    e->props = wpj_obj (general, "properties");
    e->localization = wpj_obj (general, "localization");
    e->props_root = root;
    return e->props != NULL;
}

void walpe_entry_unload_props (WalpeEntry *e)
{
    if (!e || !e->props_root)
        return;
    json_node_unref (e->props_root);
    e->props_root = NULL;
    e->props = NULL;
    e->localization = NULL;
}

static gint entry_cmp (gconstpointer a, gconstpointer b)
{
    return (gint) g_strcmp0 (((WalpeEntry *) a)->title, ((WalpeEntry *) b)->title);
}

/* media file entry; videos use the <stem>.jpg the importer leaves next to
 * them as their grid thumbnail (videos have no cheap in-process preview) */
static WalpeEntry *entry_from_media (const char *path, const char *name)
{
    WalpeEntry *e = entry_new (path, name, classify_media (name));
    e->preview = g_strdup (path);
    if (g_strcmp0 (e->type, "video") == 0) {
        g_free (e->preview);
        e->preview = NULL;
        gsize bl = strlen (name);
        if (bl > 4 && !g_ascii_strcasecmp (name + bl - 4, ".mp4")) {
            char *stem = g_strndup (name, bl - 4);
            char *jpg = g_strconcat (stem, ".jpg", NULL);
            char *dirn = g_path_get_dirname (path);
            char *jp = g_build_filename (dirn, jpg, NULL);
            if (walpe_file_exists (jp))
                e->preview = jp;
            else
                g_free (jp);
            g_free (stem);
            g_free (jpg);
            g_free (dirn);
        }
    }
    return e;
}

/* a "<stem>.jpg" whose "<stem>.mp4" sits next to it is the video's grid
 * thumbnail (written by the importer) — not a library entry of its own */
static gboolean is_video_thumbnail (const char *dir, const char *name)
{
    gsize bl = strlen (name);
    if (bl < 5 || g_ascii_strcasecmp (name + bl - 4, ".jpg"))
        return FALSE;
    char *stem = g_strndup (name, bl - 4);
    gboolean hit = FALSE;
    static const char *const exts[] = { ".mp4", ".MP4", NULL };
    for (int i = 0; exts[i] && !hit; i++) {
        char *v = g_strconcat (stem, exts[i], NULL);
        char *vp = g_build_filename (dir, v, NULL);
        hit = g_file_test (vp, G_FILE_TEST_IS_REGULAR);
        g_free (v);
        g_free (vp);
    }
    g_free (stem);
    return hit;
}

GList *walpe_store_scan (const char *dirpath)
{
    char *dir = walpe_expand_path (dirpath);
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d) {
        g_free (dir);
        return NULL;
    }
    GList *out = NULL;
    const char *name;
    while ((name = g_dir_read_name (d))) {
        if (name[0] == '.')
            continue;
        char *full = g_build_filename (dir, name, NULL);
        if (g_file_test (full, G_FILE_TEST_IS_DIR)) {
            char *pj = g_build_filename (full, "project.json", NULL);
            JsonNode *root = wpj_parse_file (pj);
            if (root && !JSON_NODE_HOLDS_OBJECT (root)) {
                /* "[1,2]" or a bare value: not a project; listed as broken */
                json_node_unref (root);
                root = NULL;
                if (g_file_test (pj, G_FILE_TEST_EXISTS)) {
                    WalpeEntry *e = entry_new (full, name, "unsupported");
                    e->description = g_strdup ("project.json is not a JSON object");
                    out = g_list_prepend (out, e);
                    g_free (pj);
                    g_free (full);
                    continue;
                }
            }
            if (!root && g_file_test (pj, G_FILE_TEST_EXISTS)) {
                /* unreadable / invalid project.json: show it (so the user
                 * sees why) instead of silently dropping the folder */
                WalpeEntry *e = entry_new (full, name, "unsupported");
                e->description = g_strdup ("project.json could not be parsed");
                out = g_list_prepend (out, e);
            } else if (root) {
                JsonObject *o = json_node_get_object (root);
                const char *title = wpj_str (o, "title", name);
                {
                    /* nothing printable in it: the folder's name */
                    char *shown = walpe_ui_text (title, 200, WALPE_UITEXT_LINE);
                    g_strstrip (shown);
                    if (!*shown)
                        title = name;
                    g_free (shown);
                }
                const char *type = wpj_str (o, "type", "");
                char *ltype = g_ascii_strdown (*type ? type : "scene", -1);
                if (!*type && json_object_has_member (o, "dependency")) {
                    /* a preset has no "type" of its own: it is what its base
                     * item is (a preset of a web wallpaper was listed, badged
                     * and filtered as a scene). Base not installed: as before. */
                    WalpeType bt = walpe_detect_type (full, NULL);
                    if (bt != WALPE_TYPE_NONE) {
                        g_free (ltype);
                        ltype = g_strdup (walpe_type_name (bt));
                    }
                }
                WalpeEntry *e = entry_new (full, title, ltype);
                g_free (ltype);
                const char *desc = wpj_str (o, "description", NULL);
                if (desc)   /* the first 60000 bytes: nobody reads more in a side pane */
                    e->description = strlen (desc) > 60000 ? g_utf8_make_valid (desc, 60000)
                                                           : g_strdup (desc);
                const char *prev = wpj_str (o, "preview", NULL);
                if (prev) {
                    /* like every file a wallpaper names, the preview is a
                     * regular file below the wallpaper's own folder, also
                     * once links are resolved (walpe_asset_path). The name
                     * was joined to the folder and taken when something
                     * existed there: "preview": "../../Pictures/x.jpg", or
                     * a link in the folder, put any picture the user can
                     * read on the tile and into the thumbnail cache. */
                    char *pp = walpe_asset_path (full, prev);
                    struct stat pst;
                    if (pp) {
                        e->preview = pp;
                        if (stat (pp, &pst) == 0) {
                            e->preview_mtime = (gint64) pst.st_mtime;
                            e->preview_size = (gint64) pst.st_size;
                        }
                    }
                }
                /* an animated preview next to a still one (or to none that
                 * exists) is the one shown: the grid plays it (thumbs.c),
                 * and its first frame is the still where animation is off */
                gsize pl = e->preview ? strlen (e->preview) : 0;
                if (pl < 4 || g_ascii_strcasecmp (e->preview + pl - 4, ".gif")) {
                    char *gp = walpe_asset_path (full, "preview.gif");
                    if (gp) {
                        g_free (e->preview);
                        e->preview = gp;
                    }
                }
                JsonObject *general = wpj_obj (o, "general");
                JsonObject *props = wpj_obj (general, "properties");
                /* what the list needs to know about the options; the tree
                 * itself is read again when they are shown */
                e->has_props = props != NULL;
                e->n_options = count_options (props);
                json_node_unref (root);
                out = g_list_prepend (out, e);
            } else {
                /* project-less folder (e.g. "imported/"): one extra level so
                 * imported media files show up in the library */
                GDir *sd = g_dir_open (full, 0, NULL);
                if (sd) {
                    const char *sn;
                    while ((sn = g_dir_read_name (sd))) {
                        if (sn[0] == '.')
                            continue;
                        char *sfull = g_build_filename (full, sn, NULL);
                        if (!g_file_test (sfull, G_FILE_TEST_IS_DIR) && is_media (sn) &&
                            !is_video_thumbnail (full, sn)) {
                            WalpeEntry *se = entry_from_media (sfull, sn);
                            out = g_list_prepend (out, se);
                        }
                        g_free (sfull);
                    }
                    g_dir_close (sd);
                }
            }
            g_free (pj);
        } else if (is_media (name) && !is_video_thumbnail (dir, name)) {
            WalpeEntry *e = entry_from_media (full, name);
            out = g_list_prepend (out, e);
        }
        g_free (full);
    }
    g_dir_close (d);
    g_free (dir);
    for (GList *l = out; l; l = l->next)
        entry_index (l->data);
    return g_list_sort (out, entry_cmp);
}

GList *walpe_store_scan_library (const char *dirpath)
{
    GList *out = walpe_store_scan (dirpath);
    GHashTable *have = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = out; l; l = l->next)
        g_hash_table_add (have, g_strdup (((WalpeEntry *) l->data)->id));
    /* the user's existing Steam WE library, shown in place (read-only,
     * nothing copied); items also in the WalPE library are listed once */
    char **libs = walpe_workshop_steam_libraries ();
    GList *extra = NULL;
    for (int i = 0; libs[i]; i++) {
        GList *se = walpe_store_scan (libs[i]);
        for (GList *l = se; l; l = l->next) {
            WalpeEntry *e = l->data;
            if (g_hash_table_contains (have, e->id)) {
                g_object_unref (e);
                continue;
            }
            g_hash_table_add (have, g_strdup (e->id));
            e->steam = TRUE;
            extra = g_list_prepend (extra, e);
        }
        g_list_free (se);
    }
    g_strfreev (libs);
    g_hash_table_unref (have);
    return g_list_concat (out, g_list_sort (extra, entry_cmp));
}

/* ---------------------------------------------------------------- import */

/* Copy what the user chose into the library. The chosen file or folder may
 * itself be a symbolic link; INSIDE a folder only regular files and
 * folders are taken. A link in there used to be followed (g_file_test and
 * g_file_copy both do): one pointing at / or at a parent folder copied
 * whatever it reached into the library until the disk was full or the path
 * too long, and a FIFO or device node made the copy wait for ever. */
#define IMPORT_MAX_DEPTH 32

static gboolean copy_tree_at (const char *src, const char *dst, int depth, GError **err)
{
    struct stat st;
    if ((depth == 0 ? stat (src, &st) : lstat (src, &st)) != 0) {
        int e = errno;
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "%s: %s", src, g_strerror (e));
        return FALSE;
    }
    if (S_ISDIR (st.st_mode)) {
        if (depth > IMPORT_MAX_DEPTH) {
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS,
                         "%s: folders nested deeper than %d levels", src, IMPORT_MAX_DEPTH);
            return FALSE;
        }
        if (g_mkdir_with_parents (dst, 0755) < 0) {
            g_set_error (err, G_IO_ERROR, g_io_error_from_errno (errno), "mkdir %s", dst);
            return FALSE;
        }
        GDir *d = g_dir_open (src, 0, err);
        if (!d)
            return FALSE;
        const char *n;
        gboolean ok = TRUE;
        while (ok && (n = g_dir_read_name (d))) {
            char *s = g_build_filename (src, n, NULL);
            char *t = g_build_filename (dst, n, NULL);
            ok = copy_tree_at (s, t, depth + 1, err);
            g_free (s);
            g_free (t);
        }
        g_dir_close (d);
        return ok;
    }
    if (!S_ISREG (st.st_mode)) {
        if (depth > 0)
            return TRUE;    /* a link, FIFO, socket or device inside the folder: left out */
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                     "%s is not a file or a folder", src);
        return FALSE;
    }
    GFile *sf = g_file_new_for_path (src), *tf = g_file_new_for_path (dst);
    gboolean ok = g_file_copy (sf, tf, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, err);
    g_object_unref (sf);
    g_object_unref (tf);
    return ok;
}

static gboolean copy_tree (const char *src, const char *dst, GError **err)
{
    return copy_tree_at (src, dst, 0, err);
}

/* where a file imported as <base> goes: <dir>/<base>, unless another file
 * already has that name - then "<stem> (2).<ext>", "(3)", ... Two different
 * "video.mp4" used to share one name: the second replaced the first in the
 * library (also when the first was the wallpaper being shown). The same
 * file imported again (same size and time, or the library copy itself)
 * keeps its name. */
static char *import_name (const char *dir, const char *base, const char *src)
{
    char *dst = g_build_filename (dir, base, NULL);
    struct stat a, b;
    if (stat (src, &a) != 0 || !S_ISREG (a.st_mode))
        return dst;                     /* folders: merged into one, as before */
    const char *dot = strrchr (base, '.');
    gsize stem = dot && dot != base ? (gsize) (dot - base) : strlen (base);
    for (int n = 2; n < 1000; n++) {
        if (lstat (dst, &b) != 0)
            return dst;                 /* free */
        if ((a.st_dev == b.st_dev && a.st_ino == b.st_ino) ||
            (S_ISREG (b.st_mode) && a.st_size == b.st_size && a.st_mtime == b.st_mtime))
            return dst;                 /* this file */
        g_free (dst);
        char *nb = g_strdup_printf ("%.*s (%d)%s", (int) stem, base, n, base + stem);
        dst = g_build_filename (dir, nb, NULL);
        g_free (nb);
    }
    return dst;
}

char *walpe_store_import_file (const char *src_abs, const char *dest_dir, GError **err)
{
    char *abs = walpe_expand_path (src_abs);
    const char *base = strrchr (abs, '/');
    base = base ? base + 1 : abs;
    {
        /* A single file goes to <library>/imported/, where the library
         * lists videos, GIFs and pictures by their file name (is_media)
         * and nothing whose name starts with a dot. Any other file was
         * copied all the same and reported as imported: it never showed
         * up in the window and could not be removed from there either. */
        struct stat fst;
        if (stat (abs, &fst) == 0 && S_ISREG (fst.st_mode) && (base[0] == '.' || !is_media (base))) {
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         "%s is not a video, GIF or picture the library can show%s", base,
                         base[0] == '.' ? " (its name starts with a dot)" : "");
            g_free (abs);
            return NULL;
        }
    }
    char *sub = g_build_filename (dest_dir, "imported", NULL);
    g_mkdir_with_parents (sub, 0755);
    char *dst = import_name (sub, base, abs);
    if (!copy_tree (abs, dst, err)) {
        g_free (sub); g_free (dst); g_free (abs);
        return NULL;
    }
    /* a directory was chosen in the file dialog: wrap it in a project.json
     * so the library groups it; loose media files are handled by the scan */
    char *pjp = g_build_filename (dst, "project.json", NULL);
    if (g_file_test (dst, G_FILE_TEST_IS_DIR) && !walpe_file_exists (pjp)) {
        char *esc = walpe_json_escape (base);
        char *pj = g_strdup_printf ("{\"type\":\"scene\",\"title\":\"%s\"}\n", esc);
        g_free (esc);
        GError *e2 = NULL;
        if (!g_file_set_contents (pjp, pj, -1, &e2)) {
            if (e2 && !*err)
                *err = e2;
            else if (e2)
                g_error_free (e2);
        }
        g_free (pj);
    }
    g_free (pjp);
    g_free (sub);
    g_free (abs);
    return dst;
}

char *walpe_store_import_dir (const char *src_abs, const char *dest_dir, GError **err)
{
    char *abs = walpe_expand_path (src_abs);
    const char *base = strrchr (abs, '/');
    base = base ? base + 1 : abs;
    char *pjp = g_build_filename (abs, "project.json", NULL);
    gboolean has_pj = walpe_file_exists (pjp);
    g_free (pjp);
    if (!has_pj) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "%s has no project.json (not a Wallpaper Engine project)", abs);
        g_free (abs);
        return NULL;
    }
    /* the library folder is made by the first thing that goes into it: on
     * a new installation it is not there yet, and the link could not be
     * made (the first "Add folder" ended with "symlink <path>") */
    if (g_mkdir_with_parents (dest_dir, 0755) != 0) {
        int e = errno;
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e),
                     "cannot create the library folder %s: %s", dest_dir, g_strerror (e));
        g_free (abs);
        return NULL;
    }
    char *link = g_build_filename (dest_dir, base, NULL);
    if (symlink (abs, link) < 0) {
        int e = errno;
        struct stat a, b;
        /* the name is taken: fine when it is this very folder; another
         * folder of the same name was reported as added although the
         * library kept the old one */
        gboolean same = e == EEXIST && stat (abs, &a) == 0 && stat (link, &b) == 0 &&
                        a.st_dev == b.st_dev && a.st_ino == b.st_ino;
        if (!same) {
            if (e == EEXIST)
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_EXISTS,
                             "the library already has a folder named %s (another wallpaper); "
                             "rename one of them to add both", base);
            else
                g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e),
                             "cannot link %s into the library: %s", link, g_strerror (e));
            g_free (link);
            g_free (abs);
            return NULL;
        }
    }
    g_free (abs);
    return link;
}

guint walpe_store_import_folder (const char *src, const char *dest_dir, guint *failed,
                                 char **first_err)
{
    guint n = 0, bad = 0;
    char *why = NULL;
    char *pj = g_build_filename (src, "project.json", NULL);
    if (g_file_test (pj, G_FILE_TEST_EXISTS)) {
        GError *err = NULL;
        char *dst = walpe_store_import_dir (src, dest_dir, &err);
        if (dst)
            n++;
        else {
            bad++;
            why = g_strdup (err ? err->message : "import failed");
        }
        g_free (dst);
        g_clear_error (&err);
    } else {
        GDir *d = g_dir_open (src, 0, NULL);
        const char *name;
        while (d && (name = g_dir_read_name (d))) {
            char *sub = g_build_filename (src, name, NULL);
            char *spj = g_build_filename (sub, "project.json", NULL);
            if (g_file_test (spj, G_FILE_TEST_EXISTS)) {
                GError *err = NULL;
                char *dst = walpe_store_import_dir (sub, dest_dir, &err);
                if (dst)
                    n++;
                else {
                    bad++;
                    if (!why)
                        why = g_strdup (err ? err->message : "import failed");
                }
                g_free (dst);
                g_clear_error (&err);
            }
            g_free (spj);
            g_free (sub);
        }
        if (d)
            g_dir_close (d);
    }
    g_free (pj);
    if (failed)
        *failed = bad;
    if (first_err)
        *first_err = why;
    else
        g_free (why);
    return n;
}

char *walpe_store_load_error_text (JsonObject *state)
{
    JsonArray *ms = state ? wpj_arr (state, "monitors") : NULL;
    guint n = wpj_arr_len (ms), with = 0;
    const char *why = NULL, *back = NULL;
    int first = -1;
    for (guint i = 0; i < n; i++) {
        JsonObject *m = wpj_arr_obj (ms, i);
        const char *e = m ? wpj_str (m, "load_error", NULL) : NULL;
        if (!e || !*e)
            continue;
        with++;
        if (!why) {
            why = e;
            back = wpj_str (m, "load_restored", NULL);
            first = (int) wpj_num (m, "index", i);
        }
    }
    if (!why)
        return NULL;
    /* the engine's sentence is "<path of the wallpaper>: <reason>". The
     * status line is one line, cut at its end: the name and what is shown
     * now come first, the reason (it often repeats the path) last */
    char *name = NULL;
    const char *sep = *why == '/' ? strstr (why, ": ") : NULL;
    if (sep && sep > why) {
        char *path = g_strndup (why, (gsize) (sep - why));
        name = g_path_get_basename (path);
        g_free (path);
        why = sep + 2;
    }
    GString *s = g_string_new (NULL);
    if (with < n)   /* not every monitor: say which */
        g_string_append_printf (s, "Monitor %d: ", first + 1);
    g_string_append_printf (s, "%s could not be loaded", name ? name : "the wallpaper");
    if (back && *back) {
        char *base = g_path_get_basename (back);
        g_string_append_printf (s, "; %s is shown again", base);
        g_free (base);
    }
    g_string_append_printf (s, ": %s", why);
    g_free (name);
    return g_string_free (s, FALSE);
}

char *walpe_store_steam_dir (void)
{
    const char *cands[] = {
        "~/.steam/steam/steamapps/workshop/content/431960",
        "~/.local/share/Steam/steamapps/workshop/content/431960",
        NULL,
    };
    for (int i = 0; cands[i]; i++) {
        char *p = walpe_expand_path (cands[i]);
        if (g_file_test (p, G_FILE_TEST_IS_DIR))
            return p;
        g_free (p);
    }
    return NULL;
}
