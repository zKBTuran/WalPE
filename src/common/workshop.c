/* workshop.c - steamcmd workshop installs + Steam library discovery
 * (see workshop.h) */
#include "workshop.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <glib/gstdio.h>

/* ---------------------------------------------------------- validation */

static gboolean all_digits (const char *s, gsize n)
{
    if (n == 0 || n > 20)
        return FALSE;
    gboolean nonzero = FALSE;
    for (gsize i = 0; i < n; i++) {
        if (!g_ascii_isdigit (s[i]))
            return FALSE;
        if (s[i] != '0')
            nonzero = TRUE;
    }
    return nonzero;
}

char *walpe_workshop_parse_id (const char *input)
{
    if (!input)
        return NULL;
    char *s = g_strstrip (g_strdup (input));
    char *out = NULL;
    if (all_digits (s, strlen (s))) {
        out = g_strdup (s);
    } else {
        if (!g_ascii_strncasecmp (s, "steamcommunity.com/", 19) ||
            !g_ascii_strncasecmp (s, "www.steamcommunity.com/", 23)) {
            char *t = g_strconcat ("https://", s, NULL);   /* pasted without scheme */
            g_free (s);
            s = t;
        }
        GUri *u = g_uri_parse (s, G_URI_FLAGS_NONE, NULL);
        const char *host = u ? g_uri_get_host (u) : NULL;
        const char *scheme = u ? g_uri_get_scheme (u) : NULL;
        const char *path = u ? g_uri_get_path (u) : NULL;
        const char *query = u ? g_uri_get_query (u) : NULL;
        if (u && host && (!g_ascii_strcasecmp (scheme, "https") || !g_ascii_strcasecmp (scheme, "http")) &&
            (!g_ascii_strcasecmp (host, "steamcommunity.com") ||
             !g_ascii_strcasecmp (host, "www.steamcommunity.com")) &&
            path && (g_str_has_prefix (path, "/sharedfiles/filedetails") ||
                     g_str_has_prefix (path, "/workshop/filedetails")) && query) {
            GHashTable *q = g_uri_parse_params (query, -1, "&", G_URI_PARAMS_NONE, NULL);
            const char *id = q ? g_hash_table_lookup (q, "id") : NULL;
            if (id && all_digits (id, strlen (id)))
                out = g_strdup (id);
            if (q)
                g_hash_table_unref (q);
        }
        if (u)
            g_uri_unref (u);
    }
    g_free (s);
    return out;
}

gboolean walpe_workshop_valid_user (const char *user)
{
    gsize n = user ? strlen (user) : 0;
    if (n < 2 || n > 64)
        return FALSE;
    for (gsize i = 0; i < n; i++)
        if (!g_ascii_isalnum (user[i]) && user[i] != '_')
            return FALSE;
    return TRUE;
}

/* ------------------------------------------------------- library finder */

static void add_root (GPtrArray *out, const char *dir)
{
    char *real = g_file_test (dir, G_FILE_TEST_IS_DIR) ? realpath (dir, NULL) : NULL;
    if (!real)
        return;
    gboolean dup = FALSE;
    for (guint i = 0; i < out->len; i++)
        dup |= !g_strcmp0 (g_ptr_array_index (out, i), real);
    if (!dup)
        g_ptr_array_add (out, g_strdup (real));
    free (real);
}

/* every "path" "<dir>" entry of a libraryfolders.vdf (no in-place edits).
 * Steam keeps it in <root>/steamapps/ (current) and <root>/config/ (older
 * clients and the root library's own copy). */
static void scan_vdf (GPtrArray *out, const char *steam_root, const char *sub)
{
    char *vdf = g_build_filename (steam_root, sub, "libraryfolders.vdf", NULL);
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents (vdf, &data, &len, NULL) && len < 4 * 1024 * 1024) {
        for (char *p = data; (p = strstr (p, "\"path\"")); p += 6) {
            char *q1 = strchr (p + 6, '"');
            char *nl = strchr (p + 6, '\n');
            if (!q1 || (nl && q1 > nl))
                continue;
            char *q2 = strchr (q1 + 1, '"');
            if (!q2 || q2 - q1 - 1 > 1024)
                continue;
            char *raw = g_strndup (q1 + 1, (gsize) (q2 - q1 - 1));
            /* vdf escapes backslashes: "\\" -> "\" */
            char *lib = g_strcompress (raw);
            g_free (raw);
            if (g_path_is_absolute (lib))
                add_root (out, lib);
            g_free (lib);
        }
    }
    g_free (data);
    g_free (vdf);
}

char **walpe_steam_library_roots (void)
{
    GPtrArray *out = g_ptr_array_new ();
    const char *home = g_get_home_dir ();
    const char *roots[] = { ".local/share/Steam", ".steam/steam", ".steam/root",
                            ".var/app/com.valvesoftware.Steam/.local/share/Steam", NULL };
    for (int i = 0; roots[i]; i++) {
        char *r = g_build_filename (home, roots[i], NULL);
        add_root (out, r);
        scan_vdf (out, r, "steamapps");
        scan_vdf (out, r, "config");
        g_free (r);
    }
    g_ptr_array_add (out, NULL);
    return (char **) g_ptr_array_free (out, FALSE);
}

char **walpe_workshop_steam_libraries (void)
{
    GPtrArray *out = g_ptr_array_new ();
    char **roots = walpe_steam_library_roots ();
    for (int i = 0; roots[i]; i++) {
        char *d = g_build_filename (roots[i], "steamapps", "workshop", "content",
                                    WALPE_WE_APPID, NULL);
        add_root (out, d);
        g_free (d);
    }
    g_strfreev (roots);
    g_ptr_array_add (out, NULL);
    return (char **) g_ptr_array_free (out, FALSE);
}

/* ---------------------------------------------------------- tree copy */

/* copy regular files + dirs; symlinks and specials are skipped so a
 * workshop item can never make us read or write outside the two trees */
static gboolean copy_tree (const char *src, const char *dst, guint64 *bytes, gsize *live,
                           GCancellable *cancel, GError **err)
{
    if (g_cancellable_set_error_if_cancelled (cancel, err))
        return FALSE;
    if (g_mkdir_with_parents (dst, 0755) != 0) {
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (errno), "mkdir %s: %s", dst,
                     g_strerror (errno));
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
        GStatBuf st;
        if (g_lstat (s, &st) == 0) {
            if (S_ISDIR (st.st_mode)) {
                ok = copy_tree (s, t, bytes, live, cancel, err);
            } else if (S_ISREG (st.st_mode)) {
                GFile *fs = g_file_new_for_path (s), *ft = g_file_new_for_path (t);
                ok = g_file_copy (fs, ft, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                  cancel, NULL, NULL, err);
                *bytes += (guint64) st.st_size;
                if (live)   /* read by the main thread for progress */
                    g_atomic_pointer_add (live, (gssize) st.st_size);
                g_object_unref (fs);
                g_object_unref (ft);
            }
        }
        g_free (s);
        g_free (t);
    }
    g_dir_close (d);
    return ok;
}

static void rm_tree (const char *p)
{
    GStatBuf st;
    if (!p || g_lstat (p, &st) != 0)
        return;
    if (S_ISDIR (st.st_mode)) {
        GDir *d = g_dir_open (p, 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name (d))) {
            char *c = g_build_filename (p, n, NULL);
            rm_tree (c);
            g_free (c);
        }
        if (d)
            g_dir_close (d);
        g_rmdir (p);
    } else {
        g_unlink (p);
    }
}

/* ---------------------------------------------------------------- job */

struct _WalpeWorkshopJob {
    char *user;
    char *library;
    GQueue todo;              /* ids still to install (char*) */
    GHashTable *seen;         /* ids already queued (cycle guard) */
    char *cur_id;
    char *phase;
    char *title;
    char *first_dir;          /* installed dir of the requested item */
    GSubprocess *proc;
    GDataInputStream *out;
    GString *log;             /* steamcmd output tail */
    char *downloaded;         /* path steamcmd reported */
    GCancellable *cancel;
    guint timeout_id;
    char *part;               /* copy in progress */
    WalpeWorkshopDone done;
    gpointer done_user;
    gboolean finished;
    gboolean freed;           /* owner let go; pending callbacks just unref */
    gboolean copying;         /* a copy thread owns `part` */
    int refs;                 /* owner + each pending async operation */
    GPid pgid;                /* steamcmd's process group (0 = none) */
    /* progress (version 0.11): steamcmd prints little while it downloads a
     * workshop item, so the download is measured on disk: the size of
     * <steamcmd root>/steamapps/workshop/{downloads,temp}/431960/<id>
     * against the item's file_size (from the workshop details) */
    guint64 expected;         /* bytes of the requested item, 0 = unknown */
    double dl_frac;           /* 0..1 of the current download, -1 unknown */
    char *sc_root;            /* steamcmd's root, from its output */
    guint poll_id;            /* 1 s on-disk size poll while steamcmd runs */
    int retries;              /* steamcmd "Timeout downloading item" retries */
    gsize copied;             /* bytes copied into the library (atomic) */
    char *req_id;             /* the requested item (expected applies to it) */
    char **roots;             /* candidate steamcmd roots for the size poll */
    /* one steamcmd run is over when the process has exited AND its output
     * has been read to the end; whichever comes last decides (run_decide).
     * Deciding on the exit alone raced with the last output lines: the
     * reason steamcmd printed ("No cached credentials") was not in the
     * log yet and the user got the generic error. */
    gboolean run_exited, run_ok, run_eof;
    guint drain_id;           /* exited, output still open: wait_output_cb */
    guint drain_lines, drain_seen, drain_quiet;
    /* walpe_workshop_install is still running: a failure now is returned
     * by it (start_error), `done` is not called - the caller does not know
     * the job yet and would drop the result */
    gboolean starting;
    char *start_error;
};

static void job_unref (WalpeWorkshopJob *j);

/* steamcmd is a shell wrapper around the real client: kill the whole
 * process group, not just the wrapper */
static void kill_steamcmd (WalpeWorkshopJob *j)
{
    if (j->pgid > 0)
        kill (-j->pgid, SIGKILL);
    else if (j->proc)
        g_subprocess_force_exit (j->proc);
    j->pgid = 0;
}

static void child_setup (gpointer user)
{
    (void) user;
    setpgid (0, 0);
}

static void set_phase (WalpeWorkshopJob *j, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static void set_phase (WalpeWorkshopJob *j, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    g_free (j->phase);
    char *raw = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    /* "%.120s" of a steamcmd line can end inside a multi-byte character:
     * the phase goes into the daemon's status reply, which has to stay
     * valid UTF-8 (a reply that is not is unreadable for every client) */
    j->phase = g_utf8_make_valid (raw, -1);
    g_free (raw);
}

static void job_finish (WalpeWorkshopJob *j, gboolean ok, const char *error)
{
    if (j->finished)
        return;
    j->finished = TRUE;
    if (j->timeout_id) {
        g_source_remove (j->timeout_id);
        j->timeout_id = 0;
    }
    if (j->poll_id) {
        g_source_remove (j->poll_id);
        j->poll_id = 0;
    }
    if (j->drain_id) {
        g_source_remove (j->drain_id);
        j->drain_id = 0;
    }
    if (ok)
        j->dl_frac = 1.0;
    kill_steamcmd (j);
    g_cancellable_cancel (j->cancel);
    if (j->part && !j->copying) {
        rm_tree (j->part);
        g_clear_pointer (&j->part, g_free);
    }
    set_phase (j, ok ? "done" : "failed");
    if (j->starting) {
        g_free (j->start_error);
        j->start_error = g_strdup (error ? error : "install failed");
        return;
    }
    if (j->done)
        j->done (j, ok, j->first_dir, error, j->done_user);
}

static void start_next (WalpeWorkshopJob *j);
static void copy_thread (GTask *t, gpointer src_obj, gpointer data, GCancellable *cancel);
static void copy_done (GObject *src_obj, GAsyncResult *res, gpointer user);
static void after_install (WalpeWorkshopJob *j, const char *dst);
static void learn_root (const char *downloaded);

/* steamcmd output -> a clear error for the usual failures */
static char *explain_failure (WalpeWorkshopJob *j)
{
    const char *l = j->log->str;
    if (strstr (l, "Rate Limit Exceeded"))
        return g_strdup ("Steam refused the login for now (rate limit exceeded); wait a while "
                         "before retrying");
    if (strstr (l, "No cached credentials") || strstr (l, "NoPromptForPassword") ||
        strstr (l, "Invalid Login Auth Code") || strstr (l, "Expired Login Auth Code") ||
        strstr (l, "Account Logon Denied") || strstr (l, "Cached credentials not found") ||
        strstr (l, "password:") || strstr (l, "Login Failure") || strstr (l, "Invalid Password") ||
        strstr (l, "Two-factor") || strstr (l, "Steam Guard") || strstr (l, "need two-factor"))
        return g_strdup_printf ("steamcmd has no cached login for '%s'. Open a terminal and run "
                                "`steamcmd +login %s` once (type your password and Steam Guard code "
                                "there, then quit); WalPE never asks for or stores them. Then retry.",
                                j->user, j->user);
    if (strstr (l, "Timeout downloading item"))
        return g_strdup_printf ("steamcmd timed out downloading %s (large items often do); "
                                "Download again: steamcmd resumes where it stopped", j->cur_id);
    if (strstr (l, "No subscription") || strstr (l, "Access Denied"))
        return g_strdup_printf ("Steam refused workshop item %s (account '%s' must own Wallpaper "
                                "Engine, app 431960)", j->cur_id, j->user);
    const char *e = strstr (l, "ERROR!");
    if (e) {
        /* one line of steamcmd, at most 200 bytes, valid UTF-8 after the cut */
        const char *nl = strchr (e, '\n');
        int n = nl ? (int) MIN (nl - e, 200) : 200;
        char *raw = g_strdup_printf ("steamcmd: %.*s", n, e);
        char *m = g_utf8_make_valid (raw, -1);
        g_free (raw);
        return m;
    }
    return g_strdup_printf ("steamcmd failed to download item %s", j->cur_id);
}

static void install_downloaded (WalpeWorkshopJob *j)
{
    char *src = j->downloaded;
    char *pj = src ? g_build_filename (src, "project.json", NULL) : NULL;
    if (!src || !g_file_test (pj, G_FILE_TEST_IS_REGULAR)) {
        g_free (pj);
        char *m = g_strdup_printf ("item %s: download has no project.json (%s)", j->cur_id,
                                   src ? src : "no path reported");
        job_finish (j, FALSE, m);
        g_free (m);
        return;
    }
    set_phase (j, "copying %s into the library", j->cur_id);
    learn_root (src);
    g_free (pj);
    /* the work copy has a hidden name, "<library>/.<id>.part": the library
     * scan skips dot names, so a copy in progress (or one left behind by a
     * crash) is not listed as a wallpaper. "<id>.part" was listed. */
    char *pname = g_strconcat (".", j->cur_id, ".part", NULL);
    g_free (j->part);
    j->part = g_build_filename (j->library, pname, NULL);
    g_free (pname);
    /* hundreds of MB: copy off the main loop (IPC + X must stay live) */
    j->copying = TRUE;
    j->refs++;
    GTask *t = g_task_new (NULL, j->cancel, copy_done, j);
    g_task_set_task_data (t, j, NULL);
    g_task_run_in_thread (t, copy_thread);
    g_object_unref (t);
}

typedef struct { guint64 bytes; } CopyRes;

/* worker: <part> <- downloaded tree, then swap it in as <library>/<id> */
static void copy_thread (GTask *t, gpointer src_obj, gpointer data, GCancellable *cancel)
{
    (void) src_obj;
    WalpeWorkshopJob *j = data;   /* only immutable-while-copying fields */
    char *dst = g_build_filename (j->library, j->cur_id, NULL);
    GError *err = NULL;
    CopyRes *r = g_new0 (CopyRes, 1);
    rm_tree (j->part);
    {
        /* leftovers of this item under the visible names older versions used */
        char *lp = g_strconcat (dst, ".part", NULL), *lo = g_strconcat (dst, ".old", NULL);
        rm_tree (lp);
        rm_tree (lo);
        g_free (lp);
        g_free (lo);
    }
    if (!copy_tree (j->downloaded, j->part, &r->bytes, &j->copied, cancel, &err)) {
        rm_tree (j->part);
        g_free (r);
        g_free (dst);
        g_task_return_error (t, err);
        return;
    }
    /* replace an older install: old aside, new in, old gone */
    char *oname = g_strconcat (".", j->cur_id, ".old", NULL);
    char *old = g_build_filename (j->library, oname, NULL);
    g_free (oname);
    rm_tree (old);
    if (g_file_test (dst, G_FILE_TEST_EXISTS))
        g_rename (dst, old);
    if (g_rename (j->part, dst) != 0) {
        int e = errno;
        g_rename (old, dst);
        rm_tree (j->part);
        g_task_return_new_error (t, G_IO_ERROR, g_io_error_from_errno (e),
                                 "cannot move %s into place: %s", dst, g_strerror (e));
        g_free (old);
        g_free (dst);
        g_free (r);
        return;
    }
    rm_tree (old);
    g_free (old);
    g_free (dst);
    g_task_return_pointer (t, r, g_free);
}

static void copy_done (GObject *src_obj, GAsyncResult *res, gpointer user)
{
    (void) src_obj;
    WalpeWorkshopJob *j = user;
    GError *err = NULL;
    CopyRes *r = g_task_propagate_pointer (G_TASK (res), &err);
    j->copying = FALSE;
    g_clear_pointer (&j->part, g_free);
    if (j->freed || j->finished) {
        g_clear_error (&err);
        g_free (r);
        job_unref (j);
        return;
    }
    if (!r) {
        char *m = g_strdup_printf ("copy failed: %s", err ? err->message : "?");
        g_clear_error (&err);
        job_finish (j, FALSE, m);
        g_free (m);
        job_unref (j);
        return;
    }
    char *dst = g_build_filename (j->library, j->cur_id, NULL);
    guint64 bytes = r->bytes;
    g_free (r);
    g_message ("workshop: installed %s (%" G_GUINT64_FORMAT " bytes) -> %s", j->cur_id, bytes, dst);
    after_install (j, dst);
    g_free (dst);
    job_unref (j);
}

/* main context: title + preset dependency, then the next queued id */
static void after_install (WalpeWorkshopJob *j, const char *dst)
{
    /* title for display + preset dependency */
    char *ipj = g_build_filename (dst, "project.json", NULL);
    JsonNode *root = wpj_parse_file (ipj);
    g_free (ipj);
    JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    if (!j->first_dir) {
        j->first_dir = g_strdup (dst);
        g_free (j->title);
        j->title = g_strdup (wpj_str (o, "title", j->cur_id));
    }
    JsonNode *dep = o ? json_object_get_member (o, "dependency") : NULL;
    char *depid = NULL;
    if (dep && JSON_NODE_HOLDS_VALUE (dep)) {
        if (json_node_get_value_type (dep) == G_TYPE_STRING)
            depid = walpe_workshop_parse_id (json_node_get_string (dep));
        else if (json_node_get_value_type (dep) == G_TYPE_INT64) {
            /* the same check as a string id: the number becomes a steamcmd
             * argument and a folder name (-5 is neither) */
            char *num = g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (dep));
            depid = walpe_workshop_parse_id (num);
            g_free (num);
        }
    }
    if (depid && !g_hash_table_contains (j->seen, depid) && g_hash_table_size (j->seen) < 4) {
        g_message ("workshop: %s depends on %s, installing it too", j->cur_id, depid);
        g_hash_table_add (j->seen, g_strdup (depid));
        g_queue_push_tail (&j->todo, depid);
        depid = NULL;
    }
    g_free (depid);
    if (root)
        json_node_unref (root);
    start_next (j);
}

static void read_line_cb (GObject *src, GAsyncResult *res, gpointer user);

/* the current steamcmd run is over (exit status known, output read):
 * retry, install what it downloaded, or report why it failed */
static void run_decide (WalpeWorkshopJob *j)
{
    gboolean ok = j->run_ok;
    if (j->drain_id) {
        g_source_remove (j->drain_id);
        j->drain_id = 0;
    }
    g_clear_object (&j->proc);
    g_clear_object (&j->out);
    if (!(ok && j->downloaded) && strstr (j->log->str, "Timeout downloading item") &&
        j->retries < 2) {
        /* steamcmd gives up on big items after a fixed time but keeps the
         * chunks: running it again continues the download */
        j->retries++;
        g_message ("workshop: steamcmd timed out on %s, retry %d", j->cur_id, j->retries);
        g_queue_push_head (&j->todo, g_strdup (j->cur_id));
        start_next (j);
        return;
    }
    if (ok && j->downloaded) {
        install_downloaded (j);
    } else {
        char *m = explain_failure (j);
        job_finish (j, FALSE, m);
        g_free (m);
    }
}

/* steamcmd has exited but its output pipe has not reached its end. The
 * lines still in the pipe arrive within a few main-loop turns; only a
 * process that inherited the pipe and outlives steamcmd keeps it open.
 * Go on once nothing has arrived for 2 s (the pending read keeps the
 * job alive meanwhile). */
static gboolean wait_output_cb (gpointer user)
{
    WalpeWorkshopJob *j = user;
    if (j->finished || j->freed) {
        j->drain_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (j->drain_seen != j->drain_lines) {
        j->drain_seen = j->drain_lines;
        j->drain_quiet = 0;
        return G_SOURCE_CONTINUE;
    }
    if (++j->drain_quiet < 8)
        return G_SOURCE_CONTINUE;
    j->drain_id = 0;
    g_message ("workshop: steamcmd exited but its output stayed open; going on without the rest");
    run_decide (j);
    return G_SOURCE_REMOVE;
}

static void proc_done_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    WalpeWorkshopJob *j = user;
    GError *err = NULL;
    gboolean ok = g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &err);
    g_clear_error (&err);
    if (j->freed || j->finished || G_SUBPROCESS (src) != j->proc) {
        job_unref (j);
        return;
    }
    j->pgid = 0;
    if (j->timeout_id) {    /* the timeout guards steamcmd, not the copy */
        g_source_remove (j->timeout_id);
        j->timeout_id = 0;
    }
    if (j->poll_id) {
        g_source_remove (j->poll_id);
        j->poll_id = 0;
    }
    j->run_exited = TRUE;
    j->run_ok = ok;
    if (j->run_eof) {
        run_decide (j);
    } else if (!j->drain_id) {
        j->drain_seen = j->drain_lines;
        j->drain_quiet = 0;
        j->drain_id = g_timeout_add (250, wait_output_cb, j);
    }
    job_unref (j);
}

static void handle_line (WalpeWorkshopJob *j, const char *line)
{
    if (j->log->len > 64 * 1024)
        g_string_erase (j->log, 0, 32 * 1024);
    g_string_append (j->log, line);
    g_string_append_c (j->log, '\n');
    /* Success. Downloaded item 3219908811 to "/home/u/.../431960/3219908811" (31 bytes) */
    const char *s = strstr (line, "Downloaded item ");
    if (s) {
        const char *q1 = strchr (s, '"');
        const char *q2 = q1 ? strchr (q1 + 1, '"') : NULL;
        if (q1 && q2) {
            /* steamcmd downloads an item into .../content/431960/<id>: a
             * line naming any other folder is not taken as the tree to
             * copy into the library */
            char *dl = g_strndup (q1 + 1, (gsize) (q2 - q1 - 1));
            char *bn = g_path_get_basename (dl);
            if (g_path_is_absolute (dl) && j->cur_id && !strcmp (bn, j->cur_id)) {
                g_free (j->downloaded);
                j->downloaded = dl;
            } else {
                g_message ("workshop: ignoring a download path that is not item %s's folder",
                           j->cur_id ? j->cur_id : "?");
                g_free (dl);
            }
            g_free (bn);
        }
    }
    /* Redirecting stderr to '/home/u/Steam/logs/stderr.txt' */
    const char *rd = strstr (line, "Redirecting stderr to '");
    if (rd) {
        const char *a = rd + 23, *b = strstr (a, "/logs/stderr.txt'");
        if (b && b > a) {
            g_free (j->sc_root);
            j->sc_root = g_strndup (a, (gsize) (b - a));
        }
    }
    /* app_update style "..., progress: 45.12 (123 / 456)" when printed */
    const char *pg = strstr (line, "progress: ");
    if (pg) {
        double v = g_ascii_strtod (pg + 10, NULL);
        if (v > 0 && v <= 100)
            j->dl_frac = MAX (j->dl_frac, v / 100.0);
    }
    if (strstr (line, "Logging in user") || strstr (line, "Logging in"))
        set_phase (j, "logging in to Steam as %s", j->user);
    else if (strstr (line, "Downloading item"))
        set_phase (j, "downloading %s", j->cur_id);
    else if (strstr (line, "Update state") || strstr (line, "progress:")) {
        char *t = g_strstrip (g_strdup (line));
        set_phase (j, "downloading %s: %.120s", j->cur_id, t);
        g_free (t);
    }
}

static void read_line_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    WalpeWorkshopJob *j = user;
    gsize len = 0;
    GError *err = NULL;
    /* bytes, not UTF-8: read_line_finish_utf8 returns NULL for a line with
     * an invalid byte (a Latin-1 path, a localized message), which looked
     * like the end of the output - the "Downloaded item" line after it was
     * never read and nothing drained the pipe any more */
    char *raw = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (src), res, &len, &err);
    char *line = raw ? g_utf8_make_valid (raw, (gssize) len) : NULL;
    g_free (raw);
    g_clear_error (&err);
    if (j->freed || j->finished || G_DATA_INPUT_STREAM (src) != j->out) {
        g_free (line);     /* cancelled, or a stale stream */
        job_unref (j);
        return;
    }
    if (!line) {
        /* end of steamcmd's output (or the pipe failed): the run is
         * decided here if the process has already been reaped, else in
         * proc_done_cb */
        j->run_eof = TRUE;
        if (j->run_exited)
            run_decide (j);
        job_unref (j);
        return;
    }
    j->drain_lines++;
    handle_line (j, line);
    g_free (line);
    /* the pending read keeps the reference */
    g_data_input_stream_read_line_async (G_DATA_INPUT_STREAM (src), G_PRIORITY_DEFAULT,
                                         j->cancel, read_line_cb, j);
}

/* ------------------------------------------------- download progress */

static char *root_cache_path (void)
{
    return g_build_filename (g_get_user_cache_dir (), "walpe", "steamcmd-root", NULL);
}

/* ".../steamapps/workshop/content/431960/<id>" -> steamcmd root, kept for
 * the next job's progress poll */
static void learn_root (const char *downloaded)
{
    const char *m = downloaded ? strstr (downloaded, "/steamapps/workshop/content/") : NULL;
    if (!m || m == downloaded)
        return;
    char *root = g_strndup (downloaded, (gsize) (m - downloaded));
    char *path = root_cache_path ();
    char *dir = g_path_get_dirname (path);
    g_mkdir_with_parents (dir, 0700);
    g_file_set_contents (path, root, -1, NULL);
    g_free (dir);
    g_free (path);
    g_free (root);
}

static char **candidate_roots (void)
{
    GPtrArray *a = g_ptr_array_new ();
    char *path = root_cache_path ();
    char *learned = NULL;
    if (g_file_get_contents (path, &learned, NULL, NULL) && g_path_is_absolute (g_strstrip (learned)))
        g_ptr_array_add (a, learned);
    else
        g_free (learned);
    g_free (path);
    const char *home = g_get_home_dir ();
    const char *rel[] = { "Steam", ".steam/steamcmd", ".steam/steam", ".local/share/Steam",
                          ".steam/SteamCMD", ".local/share/steamcmd", NULL };
    for (int i = 0; rel[i]; i++)
        g_ptr_array_add (a, g_build_filename (home, rel[i], NULL));
    /* Valve's tarball: steamcmd.sh (symlinked into PATH) lives in its root */
    char *sc = g_find_program_in_path ("steamcmd");
    char *real = sc ? realpath (sc, NULL) : NULL;
    if (real) {
        g_ptr_array_add (a, g_path_get_dirname (real));
        free (real);
    }
    g_free (sc);
    g_ptr_array_add (a, NULL);
    return (char **) g_ptr_array_free (a, FALSE);
}

static guint64 dir_size (const char *p, int *budget)
{
    GStatBuf st;
    if (--*budget < 0 || g_lstat (p, &st) != 0)
        return 0;
    if (S_ISREG (st.st_mode))
        return (guint64) st.st_size;
    if (!S_ISDIR (st.st_mode))
        return 0;
    guint64 sum = 0;
    GDir *d = g_dir_open (p, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *c = g_build_filename (p, n, NULL);
        sum += dir_size (c, budget);
        g_free (c);
    }
    if (d)
        g_dir_close (d);
    return sum;
}

static guint64 measure_download (WalpeWorkshopJob *j)
{
    guint64 best = 0;
    int budget = 20000;
    const char *subs[] = { "downloads", "temp", NULL };
    for (int pass = 0; pass < 2; pass++) {
        const char *const *roots = pass == 0 ? (const char *const[]) { j->sc_root, NULL }
                                             : (const char *const *) j->roots;
        for (int r = 0; roots && roots[r]; r++) {
            for (int s = 0; subs[s]; s++) {
                char *d = g_build_filename (roots[r], "steamapps", "workshop", subs[s],
                                            WALPE_WE_APPID, j->cur_id, NULL);
                guint64 sz = dir_size (d, &budget);   /* once: MAX() evaluates twice */
                best = MAX (best, sz);
                g_free (d);
            }
        }
    }
    return best;
}

static gboolean poll_cb (gpointer user)
{
    WalpeWorkshopJob *j = user;
    if (j->finished || !j->proc || !j->expected || g_strcmp0 (j->cur_id, j->req_id))
        return G_SOURCE_CONTINUE;
    guint64 got = measure_download (j);
    if (got == 0)
        return G_SOURCE_CONTINUE;
    double f = MIN (0.99, (double) got / (double) j->expected);
    j->dl_frac = MAX (j->dl_frac, f);
    set_phase (j, "downloading %s: %d%% (%.1f of %.1f MB)", j->cur_id, (int) (j->dl_frac * 100),
               (double) got / 1048576.0, (double) j->expected / 1048576.0);
    return G_SOURCE_CONTINUE;
}

void walpe_workshop_set_expected_size (WalpeWorkshopJob *j, guint64 bytes)
{
    if (j)
        j->expected = bytes;
}

double walpe_workshop_progress (WalpeWorkshopJob *j)
{
    if (!j)
        return -1;
    if (j->finished)
        return !g_strcmp0 (j->phase, "done") ? 1.0 : -1;
    gboolean first = !g_strcmp0 (j->cur_id, j->req_id);
    if (j->copying) {
        if (!first || !j->expected)
            return -1;
        double c = (double) (gsize) g_atomic_pointer_get (&j->copied) / (double) j->expected;
        return 0.9 + 0.1 * MIN (1.0, c);
    }
    if (!first || j->dl_frac < 0)
        return -1;
    return 0.9 * j->dl_frac;
}

static gboolean timeout_cb (gpointer user)
{
    WalpeWorkshopJob *j = user;
    j->timeout_id = 0;
    char *m = g_strdup_printf ("steamcmd timed out downloading %s", j->cur_id);
    job_finish (j, FALSE, m);
    g_free (m);
    return G_SOURCE_REMOVE;
}

static void start_next (WalpeWorkshopJob *j)
{
    g_clear_pointer (&j->cur_id, g_free);
    g_clear_pointer (&j->downloaded, g_free);
    g_string_truncate (j->log, 0);
    j->cur_id = g_queue_pop_head (&j->todo);
    if (!j->cur_id) {
        job_finish (j, TRUE, NULL);
        return;
    }
    j->dl_frac = -1;
    g_atomic_pointer_set (&j->copied, 0);
    j->run_exited = j->run_ok = j->run_eof = FALSE;
    if (j->drain_id) {
        g_source_remove (j->drain_id);
        j->drain_id = 0;
    }
    if (!j->poll_id)
        j->poll_id = g_timeout_add_seconds (1, poll_cb, j);
    const char *argv[] = { "steamcmd", "+@ShutdownOnFailedCommand", "1",
                           "+@NoPromptForPassword", "1", "+login", j->user,
                           "+workshop_download_item", WALPE_WE_APPID, j->cur_id, "validate",
                           "+quit", NULL };
    GError *err = NULL;
    /* stdin closed (never a prompt), stderr merged into the parsed output,
     * own process group (killed as a whole on timeout/cancel) */
    GSubprocessLauncher *l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                        G_SUBPROCESS_FLAGS_STDERR_MERGE |
                                                        G_SUBPROCESS_FLAGS_STDIN_PIPE);
    g_subprocess_launcher_set_child_setup (l, child_setup, NULL, NULL);
    j->proc = g_subprocess_launcher_spawnv (l, argv, &err);
    g_object_unref (l);
    if (!j->proc) {
        char *m = g_strdup_printf ("cannot run steamcmd: %s", err ? err->message : "?");
        g_clear_error (&err);
        job_finish (j, FALSE, m);
        g_free (m);
        return;
    }
    const char *pid = g_subprocess_get_identifier (j->proc);
    j->pgid = pid ? (GPid) atoi (pid) : 0;
    g_output_stream_close (g_subprocess_get_stdin_pipe (j->proc), NULL, NULL);
    set_phase (j, "starting steamcmd for %s", j->cur_id);
    j->out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (j->proc));
    g_data_input_stream_set_newline_type (j->out, G_DATA_STREAM_NEWLINE_TYPE_ANY);
    j->refs += 2;   /* one per pending async operation */
    g_data_input_stream_read_line_async (j->out, G_PRIORITY_DEFAULT, j->cancel, read_line_cb, j);
    g_subprocess_wait_check_async (j->proc, NULL, proc_done_cb, j);
    const char *te = g_getenv ("WALPE_STEAMCMD_TIMEOUT");
    guint secs = te ? (guint) g_ascii_strtoull (te, NULL, 10) : 0;
    if (secs == 0)
        secs = 30 * 60;   /* big 4K items; steamcmd prints little meanwhile */
    if (j->timeout_id)
        g_source_remove (j->timeout_id);
    j->timeout_id = g_timeout_add_seconds (secs, timeout_cb, j);
}

WalpeWorkshopJob *walpe_workshop_install (const char *id, const char *steam_user,
                                          const char *library_dir,
                                          WalpeWorkshopDone done, gpointer user,
                                          GError **err)
{
    char *pid = walpe_workshop_parse_id (id);
    if (!pid) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "not a workshop id or steamcommunity.com filedetails URL: %s", id ? id : "");
        return NULL;
    }
    if (!walpe_workshop_valid_user (steam_user)) {
        g_free (pid);
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "set your Steam account name first: walpe-ctl set steam_user=<name> "
                     "(letters, digits, underscore)");
        return NULL;
    }
    char *sc = g_find_program_in_path ("steamcmd");
    if (!sc) {
        g_free (pid);
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "steamcmd not found in PATH (Gentoo: games-util/steamcmd from an overlay, "
                     "or Valve's tarball steamcmd_linux.tar.gz)");
        return NULL;
    }
    g_free (sc);
    if (g_mkdir_with_parents (library_dir, 0755) != 0) {
        g_free (pid);
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create %s", library_dir);
        return NULL;
    }
    WalpeWorkshopJob *j = g_new0 (WalpeWorkshopJob, 1);
    j->user = g_strdup (steam_user);
    j->library = g_strdup (library_dir);
    j->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add (j->seen, g_strdup (pid));
    g_queue_init (&j->todo);
    g_queue_push_tail (&j->todo, pid);
    j->log = g_string_new (NULL);
    j->cancel = g_cancellable_new ();
    j->done = done;
    j->done_user = user;
    j->phase = g_strdup ("queued");
    j->req_id = g_strdup (pid);
    j->roots = candidate_roots ();
    j->dl_frac = -1;
    j->refs = 1;
    j->starting = TRUE;
    start_next (j);
    j->starting = FALSE;
    if (j->finished) {
        /* steamcmd is in PATH but could not be started (missing
         * interpreter or loader, fork failure): an error like the checks
         * above, not a job that already ended */
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             j->start_error ? j->start_error : "cannot run steamcmd");
        walpe_workshop_free (j);
        return NULL;
    }
    return j;
}

const char *walpe_workshop_phase (WalpeWorkshopJob *j) { return j->phase; }
const char *walpe_workshop_current_id (WalpeWorkshopJob *j) { return j->cur_id; }
const char *walpe_workshop_title (WalpeWorkshopJob *j) { return j->title; }

void walpe_workshop_cancel (WalpeWorkshopJob *j)
{
    if (!j || j->finished)
        return;
    j->finished = TRUE;
    g_cancellable_cancel (j->cancel);
    kill_steamcmd (j);
    if (j->drain_id) {
        g_source_remove (j->drain_id);
        j->drain_id = 0;
    }
    if (j->poll_id) {
        g_source_remove (j->poll_id);
        j->poll_id = 0;
    }
    if (j->timeout_id) {
        g_source_remove (j->timeout_id);
        j->timeout_id = 0;
    }
    if (j->part && !j->copying) {
        rm_tree (j->part);
        g_clear_pointer (&j->part, g_free);
    }
}

void walpe_workshop_free (WalpeWorkshopJob *j)
{
    if (!j)
        return;
    walpe_workshop_cancel (j);
    j->freed = TRUE;
    job_unref (j);
}

static void job_unref (WalpeWorkshopJob *j)
{
    if (--j->refs > 0)
        return;
    g_clear_object (&j->proc);
    g_clear_object (&j->out);
    g_clear_object (&j->cancel);
    g_queue_clear_full (&j->todo, g_free);
    g_hash_table_unref (j->seen);
    g_string_free (j->log, TRUE);
    g_free (j->user);
    g_free (j->library);
    g_free (j->cur_id);
    g_free (j->phase);
    g_free (j->title);
    g_free (j->first_dir);
    g_free (j->downloaded);
    g_free (j->sc_root);
    g_free (j->req_id);
    g_free (j->start_error);
    g_strfreev (j->roots);
    g_free (j);
}
