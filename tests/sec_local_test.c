/* sec_local_test - what the daemon and its clients trust on the local
 * machine. No daemon, no display.
 *
 *  - walpe_socket_path(): the control socket, its lock and the crash-loop
 *    marker only go into a directory that is the user's own; a runtime
 *    directory others can write to (XDG_RUNTIME_DIR=/tmp and the like) is
 *    not used, and with no private place at all the answer is a path
 *    nothing can be served on or connected to;
 *  - config.json: written 0600 whatever the umask, an existing 0644 file
 *    is narrowed when it is read, a hostile file loads into valid ranges;
 *  - walpe_write_private(): mode, a symbolic link is replaced, no temp
 *    file left;
 *  - the control socket: every reply is UTF-8 (an error text cut inside a
 *    character, a handler answering other bytes), a request line with a
 *    NUL byte is refused whole, a connection from another account is
 *    dropped (needs root to change accounts: skipped otherwise);
 *  - walpe_module_file_trusted(): what may be dlopen'ed;
 *  - walpe_layers_published(): a "layers" request that waits for a scene's
 *    next frame while the scene is unloaded (the entry it had found was
 *    read after it was freed).
 *
 * GLib reads XDG_RUNTIME_DIR, XDG_CONFIG_HOME and TMPDIR once per process:
 * the cases that depend on them run this program again as a child
 * (`sec-local-test --sockpath | --config ...`). */
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/config.h"
#include "common/ipc.h"
#include "common/layeradj.h"
#include "common/quality.h"
#include "common/util.h"
#include "engine/modules.h"

static int fails, skipped;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char *self_exe;
static char *top;   /* private scratch directory of this run */

/* run this program with `mode` and extra environment "K=V" pairs (NULL
 * ends); stdout without the trailing newline (g_free), NULL = it failed */
static char *child (const char *mode, const char *arg, ...)
{
    char **env = g_get_environ ();
    va_list ap;
    va_start (ap, arg);
    const char *kv;
    while ((kv = va_arg (ap, const char *))) {
        char **p = g_strsplit (kv, "=", 2);
        env = g_environ_setenv (env, p[0], p[1] ? p[1] : "", TRUE);
        g_strfreev (p);
    }
    va_end (ap);
    char *argv[] = { self_exe, (char *) mode, (char *) arg, NULL };
    char *out = NULL;
    int status = 0;
    gboolean ok = g_spawn_sync (NULL, argv, env, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, &status,
                                NULL) && WIFEXITED (status) && WEXITSTATUS (status) == 0;
    g_strfreev (env);
    if (!ok) {
        g_free (out);
        return NULL;
    }
    g_strchomp (out);
    return out;
}

static char *mkd (const char *name, mode_t mode)
{
    char *d = g_build_filename (top, name, NULL);
    g_mkdir (d, 0700);
    chmod (d, mode);   /* exactly, whatever the umask */
    return d;
}

/* --------------------------------------------------------- socket path */

static void test_socket_path (void)
{
    char *tmp = mkd ("tmp", 0700);
    char *tmpenv = g_strconcat ("TMPDIR=", tmp, NULL);
    char *fallback = g_strdup_printf ("%s/walpe-%lu/walpe.sock", tmp, (unsigned long) getuid ());
    char *fbdir = g_path_get_dirname (fallback);
    struct { const char *name; mode_t mode; gboolean used; } dirs[] = {
        { "rt-private", 0700, TRUE },
        { "rt-readable", 0755, TRUE },      /* ~/.cache without XDG_RUNTIME_DIR */
        { "rt-world", 0777, FALSE },
        { "rt-sticky", 01777, FALSE },      /* XDG_RUNTIME_DIR=/tmp */
        { "rt-group", 0770, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (dirs); i++) {
        char *d = mkd (dirs[i].name, dirs[i].mode);
        char *rtenv = g_strconcat ("XDG_RUNTIME_DIR=", d, NULL);
        char *want = dirs[i].used ? g_build_filename (d, "walpe.sock", NULL) : g_strdup (fallback);
        char *got = child ("--sockpath", "", rtenv, tmpenv, NULL);
        if (g_strcmp0 (got, want)) {
            g_printerr ("FAIL: runtime directory %s (mode %o): socket path %s, expected %s\n",
                        dirs[i].name, (guint) dirs[i].mode, got ? got : "(none)", want);
            fails++;
        }
        g_free (got);
        g_free (want);
        g_free (rtenv);
        g_free (d);
    }
    /* the fallback directory was made by the cases above: ours, 0700 */
    struct stat st;
    CHECK (lstat (fbdir, &st) == 0 && S_ISDIR (st.st_mode) && (st.st_mode & 0777) == 0700);

    /* a runtime directory that does not exist (left over in the
     * environment after the session that owned it ended) */
    {
        char *gone = g_build_filename (top, "rt-gone", NULL);
        char *rtenv = g_strconcat ("XDG_RUNTIME_DIR=", gone, NULL);
        char *got = child ("--sockpath", "", rtenv, tmpenv, NULL);
        CHECK (!g_strcmp0 (got, fallback));
        g_free (got);
        g_free (rtenv);
        g_free (gone);
    }
    /* a symbolic link to a directory of our own is one */
    {
        char *link = g_build_filename (top, "rt-link", NULL), *target = mkd ("rt-target", 0700);
        CHECK (symlink (target, link) == 0);
        char *rtenv = g_strconcat ("XDG_RUNTIME_DIR=", link, NULL);
        char *want = g_build_filename (link, "walpe.sock", NULL);
        char *got = child ("--sockpath", "", rtenv, tmpenv, NULL);
        CHECK (!g_strcmp0 (got, want));
        g_free (got);
        g_free (want);
        g_free (rtenv);
        g_free (link);
        g_free (target);
    }
    /* owned by another account (the name was taken before the login
     * script "created" it): needs root to make one */
    if (getuid () == 0) {
        char *d = mkd ("rt-foreign", 0700);
        CHECK (chown (d, 65534, 65534) == 0);
        char *rtenv = g_strconcat ("XDG_RUNTIME_DIR=", d, NULL);
        char *got = child ("--sockpath", "", rtenv, tmpenv, NULL);
        CHECK (!g_strcmp0 (got, fallback));
        g_free (got);
        g_free (rtenv);
        CHECK (chown (d, 0, 0) == 0);
        g_free (d);
    } else {
        g_print ("sec-local-test: not root, the foreign-owner cases are skipped\n");
        skipped++;
    }

    /* no private place at all: the fallback's name is someone else's
     * (here: a symbolic link, then a directory open to others) */
    {
        char *tmp2 = mkd ("tmp2", 0700), *world = mkd ("rt-world", 0777);
        char *tmpenv2 = g_strconcat ("TMPDIR=", tmp2, NULL);
        char *rtenv = g_strconcat ("XDG_RUNTIME_DIR=", world, NULL);
        char *squat = g_strdup_printf ("%s/walpe-%lu", tmp2, (unsigned long) getuid ());
        char *elsewhere = mkd ("elsewhere", 0700);
        CHECK (symlink (elsewhere, squat) == 0);
        char *got = child ("--sockpath", "", rtenv, tmpenv2, NULL);
        CHECK (!g_strcmp0 (got, WALPE_SOCKET_NONE));
        g_free (got);
        CHECK (unlink (squat) == 0);
        CHECK (g_mkdir (squat, 0700) == 0 && chmod (squat, 0755) == 0);
        got = child ("--sockpath", "", rtenv, tmpenv2, NULL);
        CHECK (!g_strcmp0 (got, WALPE_SOCKET_NONE));
        g_free (got);
        g_free (elsewhere);
        g_free (squat);
        g_free (rtenv);
        g_free (tmpenv2);
        g_free (world);
        g_free (tmp2);
    }
    g_free (fbdir);
    g_free (fallback);
    g_free (tmpenv);
    g_free (tmp);
}

/* ------------------------------------------------------------- server */

static GMainLoop *loop;
static char *sock;
static guint handled;

/* answers bytes that are not UTF-8 when asked to */
static char *handler (const char *req, gpointer user)
{
    (void) user;
    handled++;
    if (strstr (req, "latin1"))
        return g_strdup ("{\"ok\":true,\"name\":\"caf\xe9 \xff\xfe\"}");
    return g_strdup_printf ("{\"ok\":true,\"len\":%u}", (guint) strlen (req));
}

static int raw_connect (void)
{
    int fd = socket (AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy (a.sun_path, sock, sizeof (a.sun_path));
    if (connect (fd, (struct sockaddr *) &a, sizeof (a)) != 0) {
        close (fd);
        return -1;
    }
    return fd;
}

/* one reply line (the server sends the text and its newline in two
 * writes); FALSE = the connection ended first */
static gboolean recv_line (int fd, char *buf, gsize size)
{
    gsize got = 0;
    buf[0] = 0;
    while (got + 1 < size && !strchr (buf, '\n')) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        ssize_t n = poll (&p, 1, 5000) > 0 ? recv (fd, buf + got, size - 1 - got, 0) : -1;
        if (n <= 0)
            return FALSE;
        got += (gsize) n;
        buf[got] = 0;
    }
    return TRUE;
}

/* a client of another account: 0 = it got a reply, 1 = it connected and
 * got none, 2 = the account could not be changed, 3 = it could not even
 * connect (the socket is not reachable for it: nothing was tested) */
static int foreign_client (uid_t uid)
{
    pid_t pid = fork ();
    if (pid == 0) {
        if (setgroups (0, NULL) != 0 || setgid (uid ? 65534 : 0) != 0 || setuid (uid) != 0)
            _exit (2);
        int fd = raw_connect ();
        if (fd < 0)
            _exit (3);
        static const char line[] = "{\"cmd\":\"hello\"}\n";
        if (send (fd, line, sizeof line - 1, MSG_NOSIGNAL) != (ssize_t) sizeof line - 1)
            _exit (1);
        struct pollfd p = { .fd = fd, .events = POLLIN };
        char buf[64];
        _exit (poll (&p, 1, 1500) > 0 && recv (fd, buf, sizeof buf, 0) > 0 ? 0 : 1);
    }
    int st = 0;
    if (pid < 0 || waitpid (pid, &st, 0) != pid || !WIFEXITED (st))
        return 2;
    return WEXITSTATUS (st);
}

static gpointer client_thread (gpointer u)
{
    (void) u;
    /* a reply with bytes that are not UTF-8 arrives as UTF-8, still one
     * JSON document with its members */
    char *r = walpe_ipc_request (sock, "{\"cmd\":\"latin1\"}", 3000);
    CHECK (r && g_utf8_validate (r, -1, NULL));
    CHECK (r && g_str_has_prefix (r, "{\"ok\":true,\"name\":\"caf") && g_str_has_suffix (r, "\"}"));
    g_free (r);

    /* a request line with a NUL byte: refused whole, the handler never
     * sees the part before it */
    int fd = raw_connect ();
    CHECK (fd >= 0);
    if (fd >= 0) {
        guint before = handled;
        static const char line[] = "{\"cmd\":\"status\"}\0{\"cmd\":\"quit\"}\n";
        CHECK (send (fd, line, sizeof line - 1, MSG_NOSIGNAL) == (ssize_t) sizeof line - 1);
        char buf[256];
        CHECK (recv_line (fd, buf, sizeof buf) && strstr (buf, "\"ok\":false") &&
               strstr (buf, "NUL"));
        CHECK (handled == before);
        /* the connection goes on with the next line */
        CHECK (send (fd, "{\"cmd\":\"next\"}\n", 15, MSG_NOSIGNAL) == 15);
        CHECK (recv_line (fd, buf, sizeof buf) && strstr (buf, "\"ok\":true"));
        CHECK (handled == before + 1);
        close (fd);
    }

    /* peer check: with the socket file reachable by everybody (the worst
     * case: it is 0600 in a directory of our own), another account's
     * connection gets no answer; the same account and root do. The
     * server runs as this process's user, so this needs root. */
    if (getuid () == 0) {
        char *dir = g_path_get_dirname (sock);
        CHECK (chmod (dir, 0755) == 0 && chmod (sock, 0666) == 0);
        int own = foreign_client (0), other = foreign_client (65534);
        if (other == 2 || other == 3) {
            g_print ("sec-local-test: %s, the peer check is skipped\n",
                     other == 2 ? "cannot change account" : "socket not reachable for others");
            skipped++;
        } else {
            CHECK (own == 0);
            CHECK (other == 1);
        }
        CHECK (chmod (sock, 0600) == 0 && chmod (dir, 0700) == 0);
        g_free (dir);
    } else {
        skipped++;
    }
    g_main_loop_quit (loop);
    return NULL;
}

static void test_server (void)
{
    /* as root (peer check): under /tmp itself, where another account can
     * get as far as the socket once the modes are opened for that case */
    char *dir = getuid () == 0 ? g_mkdtemp (g_strdup ("/tmp/walpe-sec-peer-XXXXXX")) : NULL;
    gboolean in_tmp = dir != NULL;
    if (!dir)
        dir = mkd ("ipc", 0700);
    sock = g_build_filename (dir, "walpe.sock", NULL);
    loop = g_main_loop_new (NULL, FALSE);
    CHECK (walpe_ipc_serve (sock, handler, NULL));
    GThread *t = g_thread_new ("client", client_thread, NULL);
    g_main_loop_run (loop);
    g_thread_join (t);
    walpe_ipc_release (sock);

    /* the "no private directory" path: nothing is served, nothing is
     * reached, and nothing was created to get there */
    CHECK (!walpe_ipc_serve (WALPE_SOCKET_NONE, handler, NULL));
    CHECK (walpe_ipc_request (WALPE_SOCKET_NONE, "{\"cmd\":\"status\"}", 300) == NULL);
    CHECK (!walpe_daemon_running (WALPE_SOCKET_NONE));
    CHECK (!g_file_test (WALPE_SOCKET_NONE, G_FILE_TEST_EXISTS));
    CHECK (!g_file_test (WALPE_SOCKET_NONE ".lock", G_FILE_TEST_EXISTS));
    if (in_tmp) {
        char *lock = g_strconcat (sock, ".lock", NULL);
        g_unlink (lock);
        g_unlink (sock);
        g_rmdir (dir);
        g_free (lock);
    }
    g_free (sock);
    g_free (dir);
}

/* ------------------------------------------------- error text is UTF-8 */

static void test_error_text (void)
{
    /* the value's text is cut after 37 bytes: with 0..3 ASCII characters
     * in front, one of these puts the cut inside a 2-, 3- or 4-byte
     * character */
    static const char *const fill[] = { "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80" };
    for (guint f = 0; f < G_N_ELEMENTS (fill); f++)
        for (int lead = 0; lead < 5; lead++) {
            GString *v = g_string_new (NULL);
            for (int i = 0; i < lead; i++)
                g_string_append_c (v, 'x');
            for (int i = 0; i < 30; i++)
                g_string_append (v, fill[f]);
            for (int k = 0; k < 2; k++) {
                JsonObject *o = json_object_new ();
                json_object_set_string_member (o, k ? "fx_res" : "quality_preset", v->str);
                WalpeQuality q;
                walpe_quality_preset (WALPE_PRESET_HIGH, &q);
                char *err = NULL;
                CHECK (!walpe_quality_from_json (o, &q, NULL, NULL, &err));
                CHECK (err && g_utf8_validate (err, -1, NULL));
                CHECK (err && strstr (err, "..."));
                g_free (err);
                json_object_unref (o);
            }
            g_string_free (v, TRUE);
        }
}

/* ------------------------------------------------------ private write */

static guint dir_entries (const char *dir)
{
    guint n = 0;
    GDir *d = g_dir_open (dir, 0, NULL);
    while (d && g_dir_read_name (d))
        n++;
    if (d)
        g_dir_close (d);
    return n;
}

static void test_write_private (void)
{
    char *dir = mkd ("wp", 0700);
    char *f = g_build_filename (dir, "file", NULL), *other = g_build_filename (dir, "other", NULL);
    struct stat st;
    mode_t old = umask (0);
    CHECK (walpe_write_private (f, "one", -1, NULL));
    CHECK (stat (f, &st) == 0 && (st.st_mode & 07777) == 0600 && st.st_size == 3);
    /* an existing file with a wide mode */
    CHECK (chmod (f, 0666) == 0);
    CHECK (walpe_write_private (f, "three", 5, NULL));
    CHECK (stat (f, &st) == 0 && (st.st_mode & 07777) == 0600 && st.st_size == 5);
    /* a symbolic link in its place is replaced, what it pointed at is
     * left alone */
    CHECK (g_file_set_contents (other, "keep", -1, NULL));
    CHECK (unlink (f) == 0 && symlink (other, f) == 0);
    CHECK (walpe_write_private (f, "new", -1, NULL));
    CHECK (lstat (f, &st) == 0 && S_ISREG (st.st_mode) && (st.st_mode & 07777) == 0600);
    char *txt = NULL;
    CHECK (g_file_get_contents (other, &txt, NULL, NULL) && !g_strcmp0 (txt, "keep"));
    g_free (txt);
    CHECK (dir_entries (dir) == 2);   /* no temp file left */
    /* a directory that does not exist: an error, nothing created */
    char *nowhere = g_build_filename (dir, "none", "file", NULL);
    GError *e = NULL;
    CHECK (!walpe_write_private (nowhere, "x", -1, &e) && e);
    g_clear_error (&e);
    CHECK (dir_entries (dir) == 2);
    umask (old);
    g_free (nowhere);
    g_free (other);
    g_free (f);
    g_free (dir);
}

/* ------------------------------------------------------------- config */

/* child: load (and with "save" write) the config of this environment,
 * print what the parent checks */
static int config_child (const char *what)
{
    umask (022);
    WalpeConfig *c = walpe_config_load ();
    if (!c)
        return 1;
    if (strstr (what, "save"))
        walpe_config_save (c);
    g_print ("fps=%d volume=%.3f scale=%d density=%d threads=%d focus_fps=%d battery_fps=%d "
             "slack=%d lowres=%d anim=%d focus=%d", c->fps, c->volume, c->render_scale,
             c->particle_density, c->software_threads, c->focus_fps, c->battery_fps,
             c->texture_slack, c->particle_lowres, c->preview_anim, c->focus_action);
    g_print (" dir=%s cur=%s backend=%s fxq=%s\n", c->wallpapers_dir ? "set" : "NULL",
             c->current ? "set" : "NULL", c->video_backend ? c->video_backend : "NULL",
             c->fx_quality ? c->fx_quality : "NULL");
    walpe_config_free (c);
    return 0;
}

static gboolean in_range (const char *out, const char *key, double lo, double hi)
{
    char *k = g_strconcat (key, "=", NULL);
    const char *p = out ? strstr (out, k) : NULL;
    g_free (k);
    if (!p)
        return FALSE;
    double v = g_ascii_strtod (strchr (p, '=') + 1, NULL);
    return v >= lo && v <= hi;
}

static void test_config (void)
{
    char *cfg = mkd ("cfg", 0700), *rt = mkd ("cfg-rt", 0700);
    char *cenv = g_strconcat ("XDG_CONFIG_HOME=", cfg, NULL);
    char *renv = g_strconcat ("XDG_RUNTIME_DIR=", rt, NULL);
    char *wdir = g_build_filename (cfg, "walpe", NULL);
    char *file = g_build_filename (wdir, "config.json", NULL);
    struct stat st;

    /* first save: directory and file for the owner alone (umask 022 in
     * the child) */
    char *out = child ("--config", "save", cenv, renv, NULL);
    CHECK (out != NULL);
    g_free (out);
    CHECK (stat (wdir, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK (stat (file, &st) == 0 && (st.st_mode & 07777) == 0600);
    CHECK (dir_entries (wdir) == 1);

    /* a file from an earlier version (0644): narrowed by a load alone,
     * its values kept; and it stays narrow after a save */
    CHECK (g_file_set_contents (file, "{\"fps\": 48, \"steam_user\": \"somebody\", \"props\": "
                                      "{\"/w\\u001fkey\": \"typed by the user\"}}", -1, NULL));
    CHECK (chmod (file, 0644) == 0);
    out = child ("--config", "load", cenv, renv, NULL);
    CHECK (out && strstr (out, "fps=48 "));
    g_free (out);
    CHECK (stat (file, &st) == 0 && (st.st_mode & 07777) == 0600);
    CHECK (chmod (file, 0644) == 0);
    out = child ("--config", "save", cenv, renv, NULL);
    CHECK (out && strstr (out, "fps=48 "));
    g_free (out);
    CHECK (stat (file, &st) == 0 && (st.st_mode & 07777) == 0600);
    char *txt = NULL;
    CHECK (g_file_get_contents (file, &txt, NULL, NULL) && strstr (txt, "typed by the user") &&
           strstr (txt, "somebody"));
    g_free (txt);
    CHECK (dir_entries (wdir) == 1);

    /* a file that is not JSON is kept under another name: that copy is
     * not world-readable either */
    CHECK (g_file_set_contents (file, "{ \"steam_user\": \"somebody\", broken", -1, NULL));
    CHECK (chmod (file, 0644) == 0);
    out = child ("--config", "load", cenv, renv, NULL);
    CHECK (out != NULL);
    g_free (out);
    {
        GDir *d = g_dir_open (wdir, 0, NULL);
        const char *n;
        guint copies = 0;
        while (d && (n = g_dir_read_name (d))) {
            char *p = g_build_filename (wdir, n, NULL);
            if (strstr (n, ".broken-")) {
                copies++;
                CHECK (stat (p, &st) == 0 && (st.st_mode & 077) == 0);
            }
            g_unlink (p);
            g_free (p);
        }
        if (d)
            g_dir_close (d);
        CHECK (copies == 1);
    }

    /* files somebody else may have written: every member of the wrong
     * type, numbers far outside any range, odd keys. They load (under
     * the sanitizers: without a report) into values inside the ranges. */
    static const char *const hostile[] = {
        /* wrong types everywhere */
        "{\"config_version\":\"x\",\"wallpapers_dir\":7,\"current\":[1],\"video_backend\":{},"
        "\"fps\":\"60\",\"volume\":\"1\",\"paused\":\"yes\",\"monitor_current\":[],"
        "\"transparent_windows\":5,\"fx_quality\":3,\"texture_slack\":\"2\",\"render_scale\":[],"
        "\"particle_lowres\":9,\"particle_density\":{},\"software_threads\":\"8\","
        "\"steam_user\":[],\"we_assets\":{},\"focus_action\":1,\"focus_fps\":null,"
        "\"battery_fps\":true,\"preview_anim\":0,\"quality_preset\":4,\"fx_rate\":\"x\","
        "\"wallpaper_quality\":[],\"props\":[],\"layers\":7}",
        /* numbers far outside */
        "{\"config_version\":1e308,\"fps\":1e308,\"volume\":-1e308,\"render_scale\":-1e308,"
        "\"particle_density\":1e308,\"software_threads\":1e308,\"focus_fps\":-1e308,"
        "\"battery_fps\":1e308,\"texture_slack\":1e308,\"fx_rate\":1e308,\"fps_idle\":-1e308,"
        "\"render_scale\":9e99}",
        "{\"config_version\":-5,\"fps\":-2147483649,\"volume\":4294967296,"
        "\"render_scale\":4294967297,\"particle_density\":-9223372036854775808,"
        "\"software_threads\":9223372036854775807,\"focus_fps\":0,\"battery_fps\":-1,"
        "\"fps_idle\":2147483648,\"fx_rate\":-2147483649}",
        /* tables with odd members */
        "{\"monitor_current\":{\"\":\"/a\",\"m\":5,\"n\":\"\",\"o\":null,\"p\":{\"q\":1}},"
        "\"props\":{\"\":\"v\",\"\\u001f\":\"v\",\"/p\\u001f\":\"v\",\"\\u001fk\":\"v\","
        "\"nokey\":\"v\",\"/p\\u001fk\":7,\"~nobody-such-user/x\\u001fk\":\"v\"},"
        "\"wallpaper_quality\":{\"\":{\"fx_rate\":2},\"/a\":7,\"/b\":[],\"/c\":{\"fx_rate\":99,"
        "\"tex_res\":\"none\",\"quality_preset\":{}},\"/d\":{\"quality_preset\":\"low\"}},"
        "\"layers\":{\"\":[],\"/a\":7,\"/b\":[7,null,[],{}],\"/c\":[{\"key\":7},"
        "{\"key\":\"1:x\",\"dx\":1e308,\"dy\":\"1\",\"scale\":0},{\"key\":\"1:y\",\"dx\":5,"
        "\"visible\":9},{\"key\":\"1:z\",\"dx\":5},{\"key\":\"1:z\",\"dx\":6}]}}",
    };
    for (guint i = 0; i < G_N_ELEMENTS (hostile); i++) {
        CHECK (g_file_set_contents (file, hostile[i], -1, NULL));
        out = child ("--config", "save", cenv, renv, NULL);
        if (!out) {
            g_printerr ("FAIL: hostile config %u did not load and save\n", i);
            fails++;
            continue;
        }
        CHECK (in_range (out, "fps", 5, 200));
        CHECK (in_range (out, "volume", 0, 1));
        CHECK (in_range (out, "scale", 25, 100));
        CHECK (in_range (out, "density", 5, 100));
        CHECK (in_range (out, "threads", 0, 32));
        CHECK (in_range (out, "focus_fps", 1, 200));
        CHECK (in_range (out, "battery_fps", 0, 200));
        CHECK (in_range (out, "slack", 0, 2));
        CHECK (in_range (out, "lowres", 0, 2));
        CHECK (in_range (out, "anim", 0, 2));
        CHECK (in_range (out, "focus", 0, 2));
        CHECK (strstr (out, "dir=set") && strstr (out, "cur=set"));
        CHECK (strstr (out, "backend=ffmpeg") && strstr (out, "fxq=exact"));
        g_free (out);
        /* what was saved loads again */
        out = child ("--config", "load", cenv, renv, NULL);
        CHECK (out != NULL);
        g_free (out);
    }
    g_free (file);
    g_free (wdir);
    g_free (renv);
    g_free (cenv);
    g_free (rt);
    g_free (cfg);
}

/* ------------------------------------------------------------ modules */

static void test_module_trust (void)
{
    char *dir = mkd ("mod", 0755);
    char *f = g_build_filename (dir, "walpe-video.so", NULL);
    char *why = NULL;
    CHECK (g_file_set_contents (f, "x", 1, NULL) && chmod (f, 0644) == 0);
    CHECK (walpe_module_file_trusted (f, &why) && !why);
    CHECK (chmod (f, 0664) == 0 && walpe_module_file_trusted (f, NULL));   /* a group of one's own */
    CHECK (chmod (f, 0666) == 0 && !walpe_module_file_trusted (f, &why) && why);
    g_clear_pointer (&why, g_free);
    CHECK (chmod (f, 0644) == 0);
    CHECK (chmod (dir, 0777) == 0 && !walpe_module_file_trusted (f, &why) && why);
    g_clear_pointer (&why, g_free);
    CHECK (chmod (dir, 01777) == 0 && walpe_module_file_trusted (f, NULL));   /* sticky: /tmp */
    CHECK (chmod (dir, 0755) == 0);
    if (getuid () == 0) {
        CHECK (chown (f, 65534, 65534) == 0 && !walpe_module_file_trusted (f, NULL));
        CHECK (chown (f, 0, 0) == 0 && chown (dir, 65534, 65534) == 0 &&
               !walpe_module_file_trusted (f, NULL));
        CHECK (chown (dir, 0, 0) == 0);
    }
    char *none = g_build_filename (dir, "missing.so", NULL);
    CHECK (!walpe_module_file_trusted (none, &why) && why);
    g_clear_pointer (&why, g_free);
    g_free (none);
    g_free (f);
    g_free (dir);
}

/* ------------------------------------------- layer list of a scene gone */

static gpointer unpublish_soon (gpointer owner)
{
    g_usleep (80 * 1000);
    walpe_layers_unpublish (owner);
    return NULL;
}

static void test_layers_wait (void)
{
    /* what the daemon does for {"cmd":"layers"}: ask for a new list, wait
     * for the scene that shows the wallpaper to publish it. Here the
     * scene is unloaded while the request waits (a wallpaper switch, a
     * reload after an option changed): there is no list any more, and the
     * freed one must not be read (a sanitizer build stops right there;
     * a plain build returned whatever the freed memory held). */
    static int owner_a, owner_b;
    const char *path = "/wallpapers/gone";
    for (int round = 0; round < 3; round++) {
        /* a list as big as a real scene's, so that its memory is not kept
         * around unchanged by the allocator's small-object lists */
        GString *json = g_string_new ("{\"size\":[1920,1080],\"layers\":[");
        for (int i = 0; i < 400; i++)
            g_string_append_printf (json, "%s{\"key\":\"%d:layer\",\"id\":%d}", i ? "," : "", i, i);
        g_string_append (json, "]}");
        walpe_layers_publish (&owner_a, path, 0, g_string_free (json, FALSE));
        guint64 serial = walpe_layers_request_list ();   /* newer than what is published */
        CHECK (serial >= 1);
        GThread *t = g_thread_new ("unload", unpublish_soon, &owner_a);
        gboolean fresh = TRUE;
        gint64 t0 = g_get_monotonic_time ();
        char *got = walpe_layers_published (path, serial, 1500, &fresh);
        gint64 took = g_get_monotonic_time () - t0;
        g_thread_join (t);
        CHECK (got == NULL);
        CHECK (!fresh);
        CHECK (took < 1400 * 1000);   /* told at once, not after the full wait */
        g_free (got);
    }
    /* the ordinary cases still hold: the last list without waiting, a
     * newer one when it arrives in time, the old one when none does */
    walpe_layers_publish (&owner_b, path, 5, g_strdup ("{\"layers\":[]}"));
    gboolean fresh = TRUE;
    char *got = walpe_layers_published (path, 0, 0, NULL);
    CHECK (!g_strcmp0 (got, "{\"layers\":[]}"));
    g_free (got);
    got = walpe_layers_published (path, 6, 100, &fresh);
    CHECK (!g_strcmp0 (got, "{\"layers\":[]}") && !fresh);
    g_free (got);
    got = walpe_layers_published (path, 5, 100, &fresh);
    CHECK (got && fresh);
    g_free (got);
    walpe_layers_unpublish (&owner_b);
    CHECK (walpe_layers_published (path, 0, 50, NULL) == NULL);
}

/* ----------------------------------------------------------------- main */

static void rm_rf (const char *p)
{
    struct stat st;
    if (lstat (p, &st) != 0)
        return;
    if (S_ISDIR (st.st_mode)) {
        chmod (p, 0700);
        GDir *d = g_dir_open (p, 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name (d))) {
            char *c = g_build_filename (p, n, NULL);
            rm_rf (c);
            g_free (c);
        }
        if (d)
            g_dir_close (d);
        g_rmdir (p);
    } else {
        g_unlink (p);
    }
}

int main (int argc, char **argv)
{
    if (argc >= 2 && !strcmp (argv[1], "--sockpath")) {
        char *p = walpe_socket_path ();
        /* asked twice: the same answer */
        char *again = walpe_socket_path ();
        int rc = g_strcmp0 (p, again) ? 1 : 0;
        g_print ("%s\n", p);
        g_free (again);
        g_free (p);
        return rc;
    }
    if (argc >= 3 && !strcmp (argv[1], "--config"))
        return config_child (argv[2]);

    self_exe = g_file_read_link ("/proc/self/exe", NULL);
    if (!self_exe)
        self_exe = g_strdup (argv[0]);
    top = g_dir_make_tmp ("walpe-sec-local-XXXXXX", NULL);
    if (!top) {
        g_printerr ("sec-local-test: no temp directory\n");
        return 1;
    }
    test_socket_path ();
    test_server ();
    test_error_text ();
    test_write_private ();
    test_config ();
    test_module_trust ();
    test_layers_wait ();
    rm_rf (top);
    g_free (top);
    g_free (self_exe);
    if (fails)
        g_printerr ("sec-local-test: %d check(s) failed\n", fails);
    else
        g_print ("sec-local-test: ok%s\n", skipped ? " (some cases skipped: see above)" : "");
    return fails ? 1 : 0;
}
