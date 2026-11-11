/* library.c - see library.h */
#include "library.h"
#include "util.h"
#include "workshop.h"
#include "layeradj.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ roots */

typedef struct {
    char *dir;            /* realpath */
    WalpeRootKind kind;
    gboolean steam_client; /* (also) a Steam client library's workshop folder */
} Root;

static void root_free (gpointer p)
{
    Root *r = p;
    g_free (r->dir);
    g_free (r);
}

/* realpath of an existing directory (g_free), or NULL */
static char *real_dir (const char *p)
{
    char *r = p && *p ? realpath (p, NULL) : NULL;
    if (!r)
        return NULL;
    char *out = g_strdup (r);
    free (r);
    struct stat st;
    if (stat (out, &st) != 0 || !S_ISDIR (st.st_mode)) {
        g_free (out);
        return NULL;
    }
    return out;
}

static void roots_add (GPtrArray *roots, const char *dir, WalpeRootKind kind)
{
    char *real = real_dir (dir);
    if (!real)
        return;
    gboolean dup = !strcmp (real, "/");   /* "/" is never a library */
    for (guint i = 0; i < roots->len && !dup; i++) {
        Root *o = roots->pdata[i];
        dup = !strcmp (o->dir, real);
        /* the same folder under two hats (the WalPE library pointed at
         * Steam's folder): it keeps its kind and is a Steam folder too */
        if (dup && kind == WALPE_ROOT_STEAM)
            o->steam_client = TRUE;
    }
    if (dup) {
        g_free (real);
        return;
    }
    Root *r = g_new0 (Root, 1);
    r->dir = real;
    r->kind = kind;
    r->steam_client = kind == WALPE_ROOT_STEAM;
    g_ptr_array_add (roots, r);
}

static GPtrArray *library_roots (const char *library_dir)
{
    GPtrArray *roots = g_ptr_array_new_with_free_func (root_free);
    char *lib = walpe_expand_path (library_dir && *library_dir ? library_dir
                                                               : walpe_default_dir ());
    roots_add (roots, lib, WALPE_ROOT_LIBRARY);
    g_free (lib);
    char **steam = walpe_workshop_steam_libraries ();
    for (int i = 0; steam[i]; i++)
        roots_add (roots, steam[i], WALPE_ROOT_STEAM);
    g_strfreev (steam);
    /* the steamcmd root the last Workshop-tab install downloaded into
     * (workshop.c learn_root): its copy stays there after the install */
    char *cache = g_build_filename (g_get_user_cache_dir (), "walpe", "steamcmd-root", NULL);
    char *sc = NULL;
    if (g_file_get_contents (cache, &sc, NULL, NULL) && g_path_is_absolute (g_strstrip (sc))) {
        char *d = g_build_filename (sc, "steamapps", "workshop", "content", WALPE_WE_APPID, NULL);
        roots_add (roots, d, WALPE_ROOT_STEAMCMD);
        g_free (d);
    }
    g_free (sc);
    g_free (cache);
    return roots;
}

/* ------------------------------------------------------------- validation */

/* p is dir, or lies below it */
static gboolean is_or_inside (const char *p, const char *dir)
{
    gsize n = strlen (dir);
    if (strncmp (p, dir, n))
        return FALSE;
    return p[n] == 0 || p[n] == '/' || (n > 0 && dir[n - 1] == '/');
}

/* what the library scan lists as loose media (src/ui/store.c media_exts) */
static gboolean is_media_name (const char *name)
{
    static const char *const exts[] = {
        "mp4", "mkv", "webm", "mov", "avi", "m4v", "gif", "webp", "png", "jpg", "jpeg",
        "bmp", "avif", "tex", NULL,
    };
    const char *dot = strrchr (name, '.');
    if (!dot)
        return FALSE;
    for (int i = 0; exts[i]; i++)
        if (!g_ascii_strcasecmp (dot + 1, exts[i]))
            return TRUE;
    return FALSE;
}

/* Steam names a workshop item's folder by its id, and so does the
 * Workshop tab's install (<library>/<id>) */
static gboolean is_workshop_id (const char *name)
{
    gsize n = strlen (name);
    return n >= 1 && n <= 20 && strspn (name, "0123456789") == n;
}

static gboolean has_project (const char *dir)
{
    char *pj = g_build_filename (dir, "project.json", NULL);
    gboolean ok = g_file_test (pj, G_FILE_TEST_IS_REGULAR);
    g_free (pj);
    return ok;
}

static void target_free (gpointer p)
{
    WalpeDelTarget *t = p;
    if (!t)
        return;
    g_free (t->path);
    g_free (t->link_target);
    g_free (t);
}

#define REFUSE(code, ...) g_set_error (err, G_IO_ERROR, code, __VA_ARGS__)

/* the one gate: `path` as a deletable target, or NULL + err */
static WalpeDelTarget *check_target (GPtrArray *roots, const char *path, GError **err)
{
    if (!path || !g_path_is_absolute (path)) {
        REFUSE (G_IO_ERROR_INVALID_FILENAME, "“%s” is not an absolute path", path ? path : "");
        return NULL;
    }
    char **comp = g_strsplit (path, "/", -1);
    gboolean dotdot = FALSE;
    for (int i = 0; comp[i]; i++)
        dotdot |= !strcmp (comp[i], "..");
    g_strfreev (comp);
    if (dotdot) {
        REFUSE (G_IO_ERROR_INVALID_FILENAME, "“%s” contains “..”", path);
        return NULL;
    }
    char *canon = g_canonicalize_filename (path, NULL);
    char *name = g_path_get_basename (canon);
    char *parent = g_path_get_dirname (canon);
    char *rparent = NULL, *full = NULL;
    WalpeDelTarget *t = NULL;
    Root *root = NULL;
    int depth = 0;

    if (!*name || name[0] == '.' || strchr (name, '/')) {
        REFUSE (G_IO_ERROR_INVALID_FILENAME, "“%s” is not a library entry", path);
        goto out;
    }
    /* the folder it sits in, symlinks resolved, must be a library */
    rparent = real_dir (parent);
    if (!rparent) {
        REFUSE (G_IO_ERROR_NOT_FOUND, "“%s” does not exist", parent);
        goto out;
    }
    for (guint i = 0; i < roots->len && !root; i++) {
        Root *r = roots->pdata[i];
        if (!strcmp (rparent, r->dir)) {
            root = r;
            depth = 1;
        }
    }
    if (!root) {
        /* loose media one level down ("imported/"): WalPE library only */
        char *pp = g_path_get_dirname (rparent);
        char *pn = g_path_get_basename (rparent);
        for (guint i = 0; i < roots->len && !root; i++) {
            Root *r = roots->pdata[i];
            if (r->kind == WALPE_ROOT_LIBRARY && !strcmp (pp, r->dir) && pn[0] != '.') {
                root = r;
                depth = 2;
            }
        }
        g_free (pp);
        g_free (pn);
    }
    if (!root) {
        /* e.g. media listed through a linked folder: it lives elsewhere */
        REFUSE (G_IO_ERROR_NOT_SUPPORTED,
                "“%s” is not an entry of the wallpaper library or of a Steam Workshop folder%s",
                path, strcmp (parent, rparent) ? " (its folder is a link that leads elsewhere)"
                                               : "");
        goto out;
    }
    full = g_build_filename (rparent, name, NULL);
    struct stat st, pst;
    if (lstat (full, &st) != 0 || stat (rparent, &pst) != 0) {
        REFUSE (G_IO_ERROR_NOT_FOUND, "“%s” does not exist (any more)", full);
        goto out;
    }
    /* never a library itself, nor anything a library or the home lies in */
    gboolean holds = FALSE;
    for (guint i = 0; i < roots->len; i++)
        holds |= is_or_inside (((Root *) roots->pdata[i])->dir, full);
    char *home = realpath (g_get_home_dir (), NULL);
    if (home) {
        holds |= is_or_inside (home, full);
        free (home);
    }
    if (holds) {
        REFUSE (G_IO_ERROR_NOT_SUPPORTED,
                "“%s” is a wallpaper library (or contains one), not a wallpaper", full);
        goto out;
    }

    if (S_ISLNK (st.st_mode)) {
        /* removing a link never touches what it points to; still only
         * where the library lists entries */
        if (root->kind != WALPE_ROOT_LIBRARY || (depth == 2 && has_project (rparent))) {
            REFUSE (G_IO_ERROR_NOT_SUPPORTED,
                    "“%s” is a symbolic link outside the WalPE library", full);
            goto out;
        }
        t = g_new0 (WalpeDelTarget, 1);
        t->kind = WALPE_DEL_LINK;
        t->link_target = g_file_read_link (full, NULL);
    } else if (S_ISDIR (st.st_mode)) {
        if (depth != 1) {
            REFUSE (G_IO_ERROR_NOT_SUPPORTED,
                    "“%s” is a folder inside a library folder, not a wallpaper", full);
            goto out;
        }
        if (st.st_dev != pst.st_dev) {
            REFUSE (G_IO_ERROR_NOT_SUPPORTED, "“%s” is a mount point", full);
            goto out;
        }
        if (!has_project (full)) {
            REFUSE (G_IO_ERROR_NOT_SUPPORTED,
                    "“%s” has no project.json: it is not a wallpaper folder", full);
            goto out;
        }
        t = g_new0 (WalpeDelTarget, 1);
        t->kind = WALPE_DEL_DIR;
    } else if (S_ISREG (st.st_mode)) {
        if (root->kind != WALPE_ROOT_LIBRARY || !is_media_name (name)) {
            REFUSE (G_IO_ERROR_NOT_SUPPORTED, "“%s” is not a wallpaper", full);
            goto out;
        }
        if (depth == 2 && has_project (rparent)) {
            REFUSE (G_IO_ERROR_NOT_SUPPORTED,
                    "“%s” is a file inside a wallpaper folder; delete the wallpaper instead",
                    full);
            goto out;
        }
        t = g_new0 (WalpeDelTarget, 1);
        t->kind = WALPE_DEL_FILE;
    } else {
        REFUSE (G_IO_ERROR_NOT_SUPPORTED, "“%s” is not a file or folder", full);
        goto out;
    }
    t->path = full;
    full = NULL;
    t->root = root->kind;
    t->steam_client = root->steam_client;
out:
    g_free (full);
    g_free (rparent);
    g_free (parent);
    g_free (name);
    g_free (canon);
    return t;
}

/* ------------------------------------------------------------------- plan */

static void forget_add (GPtrArray *a, const char *p)
{
    for (guint i = 0; i < a->len; i++)
        if (!strcmp (a->pdata[i], p))
            return;
    g_ptr_array_add (a, g_strdup (p));
}

/* presets (project.json "dependency": "<id>", no "file" of their own - as
 * engine/content.c reads them) in the roots that are based on workshop
 * item `id` (a preset installed in several roots counts once) */
static char **find_dependents (GPtrArray *roots, const char *id)
{
    GPtrArray *out = g_ptr_array_new ();
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < roots->len; i++) {
        const char *rd = ((Root *) roots->pdata[i])->dir;
        GDir *d = g_dir_open (rd, 0, NULL);
        const char *name;
        while (d && (name = g_dir_read_name (d))) {
            if (name[0] == '.' || !strcmp (name, id) || g_hash_table_contains (seen, name))
                continue;
            char *pj = g_build_filename (rd, name, "project.json", NULL);
            char *data = NULL;
            gsize len = 0;
            /* most items are no presets: skip the JSON parse for them */
            if (g_file_get_contents (pj, &data, &len, NULL) && len < 8 * 1024 * 1024 &&
                strstr (data, "\"dependency\"")) {
                JsonNode *root = wpj_parse_data (data, (gssize) len);
                JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root)
                                                                      : NULL;
                JsonNode *dn = o ? json_object_get_member (o, "dependency") : NULL;
                char *dep = NULL;
                if (dn && JSON_NODE_HOLDS_VALUE (dn) && !wpj_str (o, "file", NULL)) {
                    if (json_node_get_value_type (dn) == G_TYPE_STRING)
                        dep = g_strdup (json_node_get_string (dn));
                    else
                        dep = g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (dn));
                }
                if (dep && !strcmp (dep, id)) {
                    const char *title = wpj_str (o, "title", name);
                    g_ptr_array_add (out, g_strdup (*title ? title : name));
                    g_hash_table_add (seen, g_strdup (name));
                }
                g_free (dep);
                if (root)
                    json_node_unref (root);
            }
            g_free (data);
            g_free (pj);
        }
        if (d)
            g_dir_close (d);
    }
    g_hash_table_unref (seen);
    g_ptr_array_add (out, NULL);
    return (char **) g_ptr_array_free (out, FALSE);
}

WalpeDelPlan *walpe_library_delete_plan (const char *path, const char *library_dir, GError **err)
{
    GPtrArray *roots = library_roots (library_dir);
    WalpeDelTarget *t = check_target (roots, path, err);
    if (!t) {
        g_ptr_array_unref (roots);
        return NULL;
    }
    t->primary = TRUE;
    WalpeDelPlan *plan = g_new0 (WalpeDelPlan, 1);
    plan->targets = g_ptr_array_new_with_free_func (target_free);
    g_ptr_array_add (plan->targets, t);
    char *name = g_path_get_basename (t->path);
    char *dir = g_path_get_dirname (t->path);

    /* an imported video's grid thumbnail: <stem>.jpg next to <stem>.mp4 */
    gsize nl = strlen (name);
    if (t->kind == WALPE_DEL_FILE && nl > 4 && !g_ascii_strcasecmp (name + nl - 4, ".mp4")) {
        char *stem = g_strndup (name, nl - 4);
        char *jn = g_strconcat (stem, ".jpg", NULL);
        char *jp = g_build_filename (dir, jn, NULL);
        WalpeDelTarget *th = g_file_test (jp, G_FILE_TEST_IS_REGULAR)
                                 ? check_target (roots, jp, NULL) : NULL;
        if (th && th->kind == WALPE_DEL_FILE) {
            th->kind = WALPE_DEL_THUMB;
            g_ptr_array_add (plan->targets, th);
        } else {
            target_free (th);
        }
        g_free (jp);
        g_free (jn);
        g_free (stem);
    }

    /* a workshop item is listed once, whichever folders hold it: all of
     * its copies go, or the one left behind is simply listed again */
    if ((t->kind == WALPE_DEL_DIR || t->kind == WALPE_DEL_LINK) && is_workshop_id (name)) {
        /* real ids have 9+ digits; a folder the user named "2024" is not
         * called a workshop item in the dialog */
        if (strlen (name) >= 8)
            plan->workshop_id = g_strdup (name);
        for (guint i = 0; i < roots->len; i++) {
            char *cand = g_build_filename (((Root *) roots->pdata[i])->dir, name, NULL);
            WalpeDelTarget *o = strcmp (cand, t->path) ? check_target (roots, cand, NULL) : NULL;
            if (o && (o->kind == WALPE_DEL_DIR || o->kind == WALPE_DEL_LINK))
                g_ptr_array_add (plan->targets, o);
            else
                target_free (o);
            g_free (cand);
        }
        plan->dependents = find_dependents (roots, name);
    }

    GPtrArray *forget = g_ptr_array_new ();
    forget_add (forget, path);
    for (guint i = 0; i < plan->targets->len; i++) {
        WalpeDelTarget *x = plan->targets->pdata[i];
        if (x->kind != WALPE_DEL_THUMB)
            forget_add (forget, x->path);
        if (x->root != WALPE_ROOT_LIBRARY || strstr (x->path, "/steamapps/workshop/content/"))
            plan->steam_managed = TRUE;
        if (x->steam_client)
            plan->steam_client = TRUE;
    }
    g_ptr_array_add (forget, NULL);
    plan->forget = (char **) g_ptr_array_free (forget, FALSE);
    g_free (dir);
    g_free (name);
    g_ptr_array_unref (roots);
    return plan;
}

void walpe_del_plan_free (WalpeDelPlan *plan)
{
    if (!plan)
        return;
    g_ptr_array_unref (plan->targets);
    g_free (plan->workshop_id);
    g_strfreev (plan->forget);
    g_strfreev (plan->dependents);
    g_free (plan);
}

/* --------------------------------------------------------------- deleting */

/* the target must still be exactly what the plan saw */
static gboolean recheck (const WalpeDelTarget *t, const char *library_dir, GError **err)
{
    if (!t || !t->path) {
        REFUSE (G_IO_ERROR_INVALID_ARGUMENT, "nothing to delete");
        return FALSE;
    }
    GPtrArray *roots = library_roots (library_dir);
    WalpeDelTarget *now = check_target (roots, t->path, err);
    g_ptr_array_unref (roots);
    if (!now)
        return FALSE;
    WalpeDelKind want = t->kind == WALPE_DEL_THUMB ? WALPE_DEL_FILE : t->kind;
    gboolean ok = now->kind == want && !strcmp (now->path, t->path);
    target_free (now);
    if (!ok)
        REFUSE (G_IO_ERROR_FAILED, "“%s” changed while it was being deleted; nothing was done",
                t->path);
    return ok;
}

gboolean walpe_library_trash (const WalpeDelTarget *t, const char *library_dir, GError **err)
{
    if (!recheck (t, library_dir, err))
        return FALSE;
    GFile *f = g_file_new_for_path (t->path);
    /* a rename into the trash of the same filesystem; a symlink is moved
     * itself (never its target) */
    gboolean ok = g_file_trash (f, NULL, err);
    g_object_unref (f);
    return ok;
}

static gboolean fail_errno (GError **err, const char *what, const char *name)
{
    int e = errno;
    if (err && !*err)
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "%s %s: %s", what, name,
                     g_strerror (e));
    return FALSE;
}

/* remove directory `name` of the open directory pfd: opened without
 * following links, entries removed relative to its descriptor, and only
 * while still on filesystem `dev` */
static gboolean rm_tree_at (int pfd, const char *name, dev_t dev, int depth, GError **err)
{
    if (depth > 64) {
        REFUSE (G_IO_ERROR_FAILED, "%s: folders nested too deep", name);
        return FALSE;
    }
    int fd = openat (pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return fail_errno (err, "cannot open", name);
    struct stat st;
    if (fstat (fd, &st) != 0) {
        fail_errno (err, "cannot stat", name);
        close (fd);
        return FALSE;
    }
    if (st.st_dev != dev) {
        close (fd);
        REFUSE (G_IO_ERROR_NOT_SUPPORTED, "%s is on another filesystem (a mount point?)", name);
        return FALSE;
    }
    if ((st.st_mode & S_IRWXU) != S_IRWXU)
        fchmod (fd, (st.st_mode & 07777) | S_IRWXU);   /* read-only folder of ours */
    DIR *d = fdopendir (fd);
    if (!d) {
        fail_errno (err, "cannot read", name);
        close (fd);
        return FALSE;
    }
    gboolean ok = TRUE;
    struct dirent *de;
    while (ok && (de = readdir (d))) {
        if (!strcmp (de->d_name, ".") || !strcmp (de->d_name, ".."))
            continue;
        struct stat cs;
        if (fstatat (fd, de->d_name, &cs, AT_SYMLINK_NOFOLLOW) != 0)
            ok = fail_errno (err, "cannot stat", de->d_name);
        else if (S_ISDIR (cs.st_mode))
            ok = rm_tree_at (fd, de->d_name, dev, depth + 1, err);
        else if (unlinkat (fd, de->d_name, 0) != 0)
            ok = fail_errno (err, "cannot remove", de->d_name);
    }
    closedir (d);   /* closes fd */
    if (ok && unlinkat (pfd, name, AT_REMOVEDIR) != 0)
        ok = fail_errno (err, "cannot remove", name);
    return ok;
}

gboolean walpe_library_remove (const WalpeDelTarget *t, const char *library_dir, GError **err)
{
    if (!recheck (t, library_dir, err))
        return FALSE;
    char *dir = g_path_get_dirname (t->path);
    char *name = g_path_get_basename (t->path);
    gboolean ok = FALSE;
    int pfd = open (dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st;
    if (pfd < 0) {
        fail_errno (err, "cannot open", dir);
    } else if (fstatat (pfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        fail_errno (err, "cannot stat", name);
    } else if (t->kind == WALPE_DEL_DIR) {
        if (S_ISDIR (st.st_mode))
            ok = rm_tree_at (pfd, name, st.st_dev, 0, err);
        else
            REFUSE (G_IO_ERROR_FAILED, "“%s” is no longer a folder", t->path);
    } else if (S_ISDIR (st.st_mode)) {
        REFUSE (G_IO_ERROR_FAILED, "“%s” became a folder", t->path);
    } else if (unlinkat (pfd, name, 0) != 0) {
        fail_errno (err, "cannot remove", name);
    } else {
        ok = TRUE;
    }
    if (pfd >= 0)
        close (pfd);
    g_free (name);
    g_free (dir);
    return ok;
}

/* ----------------------------------------------------------------- config */

/* <realpath of the parent>/<name>: the directory entry a path names */
static char *entry_key (const char *p)
{
    char *canon = g_canonicalize_filename (p, NULL);
    char *dir = g_path_get_dirname (canon);
    char *base = g_path_get_basename (canon);
    char *rdir = realpath (dir, NULL);
    char *key = rdir ? g_build_filename (rdir, base, NULL) : g_strdup (canon);
    free (rdir);
    g_free (base);
    g_free (dir);
    g_free (canon);
    return key;
}

gboolean walpe_path_same (const char *a, const char *b)
{
    if (!a || !b || !*a || !*b)
        return FALSE;
    if (!strcmp (a, b))
        return TRUE;
    if (!g_path_is_absolute (a) || !g_path_is_absolute (b))
        return FALSE;
    char *ka = entry_key (a), *kb = entry_key (b);
    gboolean same = !strcmp (ka, kb);
    g_free (ka);
    g_free (kb);
    return same;
}

guint walpe_config_forget_path (WalpeConfig *cfg, const char *path)
{
    if (!cfg || !path || !*path)
        return 0;
    guint n = 0;
    /* option overrides are keyed by the path as it was spelled when set:
     * the caller's, the canonical one and whatever the config itself used */
    GPtrArray *spell = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (spell, g_strdup (path));
    if (g_path_is_absolute (path)) {
        g_ptr_array_add (spell, g_canonicalize_filename (path, NULL));
        g_ptr_array_add (spell, entry_key (path));
    }
    if (walpe_path_same (cfg->current, path)) {
        g_ptr_array_add (spell, cfg->current);
        cfg->current = g_strdup ("");
        n++;
    }
    if (cfg->monitor_current) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, cfg->monitor_current);
        while (g_hash_table_iter_next (&it, &k, &v))
            if (walpe_path_same (v, path)) {
                g_ptr_array_add (spell, g_strdup (v));
                g_hash_table_iter_remove (&it);   /* follows `current` again */
                n++;
            }
    }
    for (guint i = 0; i < spell->len; i++) {
        n += walpe_config_clear_props (cfg, spell->pdata[i]);
        n += walpe_layers_remove (cfg, spell->pdata[i], NULL);   /* its layer adjustments */
        n += walpe_config_quality_forget (cfg, spell->pdata[i]);   /* its own quality settings */
    }
    g_ptr_array_unref (spell);
    return n;
}

guint walpe_config_forget_offline (const char *const *paths)
{
    WalpeConfig *c = walpe_config_load ();
    guint n = 0;
    for (int i = 0; paths && paths[i]; i++)
        n += walpe_config_forget_path (c, paths[i]);
    if (n)
        walpe_config_save (c);
    walpe_config_free (c);
    return n;
}
