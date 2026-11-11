/* ipc_test - the control socket (src/common/ipc.c), no daemon needed:
 * a server with an echo-like handler on this process's main loop, clients
 * on a worker thread. Covers: replies larger than 64 KiB
 * arrive whole, a request without end is cut off, the socket file is
 * owner-only, a second server on the same path is refused while the first
 * runs (single-instance lock), a client that never reads does not stall
 * the server, and the place is free again after walpe_ipc_release. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/ipc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static GMainLoop *loop;
static char *sock;

/* {"big":N} -> N bytes of 'x' in a JSON string; anything else is echoed */
static char *handler (const char *req, gpointer user)
{
    (void) user;
    const char *b = strstr (req, "\"big\":");
    if (b) {
        gsize n = (gsize) g_ascii_strtoull (b + 6, NULL, 10);
        GString *s = g_string_new ("{\"ok\":true,\"data\":\"");
        for (gsize i = 0; i < n; i++)
            g_string_append_c (s, 'x');
        g_string_append (s, "\"}");
        return g_string_free (s, FALSE);
    }
    return g_strdup_printf ("{\"ok\":true,\"echo\":%u}", (guint) strlen (req));
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

static gpointer client_thread (gpointer u)
{
    (void) u;
    /* plain request */
    char *r = walpe_ipc_request (sock, "{\"cmd\":\"hello\"}", 3000);
    CHECK (r && strstr (r, "\"ok\":true"));
    g_free (r);

    /* a reply well above the old 64 KiB limit arrives whole */
    r = walpe_ipc_request (sock, "{\"big\":300000}", 5000);
    CHECK (r && strlen (r) == 300000 + strlen ("{\"ok\":true,\"data\":\"\"}"));
    CHECK (r && g_str_has_suffix (r, "\"}"));
    g_free (r);

    /* the probe sees the server */
    CHECK (walpe_daemon_running (sock));

    /* socket file: owner only */
    struct stat st;
    CHECK (stat (sock, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* a second server on the same path is refused while this one runs */
    CHECK (!walpe_ipc_serve (sock, handler, NULL));
    r = walpe_ipc_request (sock, "{\"cmd\":\"still there\"}", 3000);
    CHECK (r != NULL);   /* and the refused one did not remove our socket */
    g_free (r);

    /* a request line without end: answered with an error, connection closed */
    int fd = raw_connect ();
    CHECK (fd >= 0);
    if (fd >= 0) {
        char blk[4096];
        memset (blk, 'a', sizeof (blk));
        gsize sent = 0;
        while (sent < (4u << 20)) {
            ssize_t n = send (fd, blk, sizeof (blk), MSG_NOSIGNAL);
            if (n <= 0)
                break;
            sent += (gsize) n;
        }
        CHECK (sent < (4u << 20));      /* the server hung up well before 4 MiB */
        char buf[256] = { 0 };
        ssize_t n = recv (fd, buf, sizeof (buf) - 1, 0);
        CHECK (n > 0 && strstr (buf, "request too long"));
        close (fd);
    }

    /* a client that asks for large replies and never reads them must not
     * block the server for good */
    fd = raw_connect ();
    CHECK (fd >= 0);
    if (fd >= 0) {
        for (int i = 0; i < 8; i++)
            if (send (fd, "{\"big\":200000}\n", 15, MSG_NOSIGNAL | MSG_DONTWAIT) <= 0)
                break;
        gint64 t0 = g_get_monotonic_time ();
        r = walpe_ipc_request (sock, "{\"cmd\":\"after noread\"}", 15000);
        CHECK (r != NULL);
        CHECK (g_get_monotonic_time () - t0 < 12 * G_USEC_PER_SEC);
        g_free (r);
        close (fd);
    }

    /* invalid UTF-8 gets an error reply instead of a silent hang-up */
    fd = raw_connect ();
    if (fd >= 0) {
        CHECK (send (fd, "{\"p\":\"\xff\xfe\"}\n", 11, MSG_NOSIGNAL) == 11);
        char buf[256] = { 0 };
        ssize_t n = recv (fd, buf, sizeof (buf) - 1, 0);
        CHECK (n > 0 && strstr (buf, "not UTF-8"));
        close (fd);
    }

    g_main_loop_quit (loop);
    return NULL;
}

int main (void)
{
    char *dir = g_dir_make_tmp ("walpe-ipc-test-XXXXXX", NULL);
    sock = g_build_filename (dir, "walpe.sock", NULL);
    loop = g_main_loop_new (NULL, FALSE);

    CHECK (!walpe_daemon_running (sock));           /* nothing there yet */
    CHECK (walpe_ipc_request (sock, "{}", 200) == NULL);
    mode_t old = umask (0);                         /* the mode must not depend on it */
    CHECK (walpe_ipc_serve (sock, handler, NULL));
    umask (old);
    GThread *t = g_thread_new ("client", client_thread, NULL);
    g_main_loop_run (loop);
    g_thread_join (t);

    /* after the release the place is free: a new server starts */
    walpe_ipc_release (sock);
    CHECK (!g_file_test (sock, G_FILE_TEST_EXISTS));
    CHECK (walpe_ipc_serve (sock, handler, NULL));
    walpe_ipc_release (sock);

    char *lock = g_strconcat (sock, ".lock", NULL);
    g_unlink (lock);
    g_rmdir (dir);
    g_free (lock);
    g_free (sock);
    g_free (dir);
    if (fails)
        g_printerr ("ipc-test: %d check(s) failed\n", fails);
    else
        g_print ("ipc-test: ok\n");
    return fails ? 1 : 0;
}
