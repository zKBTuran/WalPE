/* ipc_test - the control socket (src/common/ipc.c), no daemon needed:
 * a server with an echo-like handler on this process's main loop, clients
 * on a worker thread. Covers: replies larger than 64 KiB
 * arrive whole, a request without end is cut off, the socket file is
 * owner-only, a second server on the same path is refused while the first
 * runs (single-instance lock), a client that never reads does not hold
 * the server up for other clients and is dropped, requests sent in one go
 * are answered in order to a client that reads late, and the place is free
 * again after walpe_ipc_release. */
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
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

    /* a client that asks for large replies and never reads them does not
     * hold the server up: another client is answered at once (the reply
     * used to be written with a blocking write of up to three seconds on
     * the server's main loop), and the one that takes nothing for three
     * seconds is dropped */
    fd = raw_connect ();
    CHECK (fd >= 0);
    if (fd >= 0) {
        for (int i = 0; i < 8; i++)
            if (send (fd, "{\"big\":200000}\n", 15, MSG_NOSIGNAL | MSG_DONTWAIT) <= 0)
                break;
        g_usleep (300 * 1000);          /* the server has the requests and a reply it cannot send */
        gint64 t0 = g_get_monotonic_time ();
        r = walpe_ipc_request (sock, "{\"cmd\":\"after noread\"}", 15000);
        CHECK (r != NULL);
        CHECK (g_get_monotonic_time () - t0 < 1500 * 1000);
        g_free (r);
        g_usleep (5 * G_USEC_PER_SEC);
        /* what the socket had taken is still there to read, then the end:
         * far less than the eight replies */
        struct timeval tv = { .tv_sec = 10 };
        setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        gsize got = 0;
        ssize_t n;
        char blk[65536];
        while ((n = recv (fd, blk, sizeof blk, 0)) > 0)
            got += (gsize) n;
        CHECK (n == 0 || errno == ECONNRESET);      /* closed by the server, not our timeout */
        CHECK (got < 8 * 200000);
        close (fd);
    }

    /* requests sent in one go by a client that starts reading late: every
     * reply arrives whole and in order (a reply waits for the socket, the
     * requests behind it wait for the reply) */
    fd = raw_connect ();
    CHECK (fd >= 0);
    if (fd >= 0) {
        GString *reqs = g_string_new (NULL);
        for (int i = 0; i < 40; i++)
            g_string_append_printf (reqs, "{\"big\":%d}\n", 100000 + i);
        g_string_append (reqs, "{\"cmd\":\"last\"}\n");
        CHECK (send (fd, reqs->str, reqs->len, MSG_NOSIGNAL) == (ssize_t) reqs->len);
        g_string_free (reqs, TRUE);
        g_usleep (700 * 1000);
        struct timeval tv = { .tv_sec = 20 };
        setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        GString *all = g_string_new (NULL);
        int lines = 0;
        char blk[65536];
        ssize_t n;
        while (lines < 41 && (n = recv (fd, blk, sizeof blk, 0)) > 0) {
            for (ssize_t k = 0; k < n; k++)
                lines += blk[k] == '\n';
            g_string_append_len (all, blk, n);
        }
        CHECK (lines == 41);
        const char *p = all->str;
        for (int i = 0; i < 40 && lines == 41; i++) {
            const char *e = strchr (p, '\n');
            CHECK (e && (gsize) (e - p) == (gsize) (100000 + i) + strlen ("{\"ok\":true,\"data\":\"\"}"));
            p = e ? e + 1 : p;
        }
        CHECK (lines != 41 || g_str_has_prefix (p, "{\"ok\":true,\"echo\":"));
        g_string_free (all, TRUE);
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
