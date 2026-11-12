/* uiipc_test - what the window is told when the daemon gives no answer
 * (src/ui/uiipc.c), no daemon and no display needed. "Not running" makes
 * the window start a daemon; "did not answer" makes it wait. Covers: no
 * socket and a socket file nobody listens on are "not running"; a listener
 * that never accepts is "did not answer" - also when its queue of waiting
 * connections is full, which is what the requests sent to a hung or
 * stopped daemon do to it after a while; a listener that answers. */
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ui/uiipc.h"
#include "common/config.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { gboolean done, ok, down; char *err; } Answer;

static void on_reply (JsonObject *reply, const char *err, gpointer user)
{
    Answer *a = user;
    a->done = TRUE;
    a->ok = ui_ipc_ok (reply);
    a->down = !reply && ui_ipc_err_is_down (err);
    a->err = g_strdup (err);
}

/* one status request as the window's poll sends it; the answer (2.5 s at most) */
static Answer ask (void)
{
    Answer a = { 0 };
    ui_ipc_send ("{\"cmd\":\"status\"}", UI_IPC_POLL, on_reply, &a);
    gint64 end = g_get_monotonic_time () + 20 * G_USEC_PER_SEC;
    while (!a.done && g_get_monotonic_time () < end)
        g_main_context_iteration (NULL, TRUE);
    CHECK (a.done);
    return a;
}

static int listener (const char *path, int backlog)
{
    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy (a.sun_path, path, sizeof (a.sun_path));
    g_unlink (path);
    CHECK (fd >= 0 && bind (fd, (struct sockaddr *) &a, sizeof (a)) == 0 && listen (fd, backlog) == 0);
    return fd;
}

/* connections nobody accepts until the next one is refused with EAGAIN */
static int fill_queue (const char *path, int *fds, int max)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy (a.sun_path, path, sizeof (a.sun_path));
    int n = 0;
    while (n < max) {
        int c = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (c < 0)
            break;
        if (connect (c, (struct sockaddr *) &a, sizeof (a)) < 0) {
            gboolean full = errno == EAGAIN;
            close (c);
            return full ? n : -1;
        }
        fds[n++] = c;
    }
    return -1;
}

static gpointer answer_thread (gpointer u)
{
    int lfd = GPOINTER_TO_INT (u);
    int c = accept (lfd, NULL, NULL);
    if (c >= 0) {
        char buf[256];
        if (recv (c, buf, sizeof (buf), 0) > 0)
            (void) !send (c, "{\"ok\":true}\n", 12, MSG_NOSIGNAL);
        close (c);
    }
    return NULL;
}

int main (void)
{
    char *dir = g_dir_make_tmp ("walpe-uiipc-XXXXXX", NULL);
    g_assert (dir);
    g_chmod (dir, 0700);
    /* (the workers read the socket's place once, when the first request is sent) */
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
    char *sock = walpe_socket_path ();
    if (!g_str_has_prefix (sock, dir)) {
        /* (a temporary directory whose path is too long for a socket) */
        g_print ("uiipc: skipped, the socket would not be in %s\n", dir);
        g_rmdir (dir);
        return 77;
    }

    /* no socket at all */
    Answer a = ask ();
    CHECK (!a.ok && a.down);
    g_free (a.err);

    /* somebody listens and never accepts: the request is taken into the queue, no answer */
    int lfd = listener (sock, 1);
    a = ask ();
    CHECK (!a.ok && !a.down && a.err);
    g_free (a.err);

    /* the same listener with its queue full: still somebody who does not
     * answer, not nobody */
    int held[64];
    int n = fill_queue (sock, held, 64);
    CHECK (n >= 0);
    gint64 t0 = g_get_monotonic_time ();
    a = ask ();
    CHECK (!a.ok && a.err);
    CHECK (!a.down);
    /* (and it was waited for as long as for any other answer) */
    CHECK (g_get_monotonic_time () - t0 > 2 * G_USEC_PER_SEC);
    g_free (a.err);

    /* it gets to its socket again: answered */
    for (int i = 0; i < n; i++)
        close (held[i]);
    for (;;) {
        /* (the connections given up above are still queued: take them out) */
        struct timeval tv = { 0 };
        fd_set r;
        FD_ZERO (&r);
        FD_SET (lfd, &r);
        if (select (lfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
        int c = accept (lfd, NULL, NULL);
        if (c < 0)
            break;
        close (c);
    }
    GThread *th = g_thread_new ("answer", answer_thread, GINT_TO_POINTER (lfd));
    a = ask ();
    CHECK (a.ok && !a.down);
    g_free (a.err);
    g_thread_join (th);

    /* the listener is gone and its socket file stayed (a daemon that was killed) */
    close (lfd);
    CHECK (g_file_test (sock, G_FILE_TEST_EXISTS));
    a = ask ();
    CHECK (!a.ok && a.down);
    g_free (a.err);

    g_unlink (sock);
    g_rmdir (dir);
    g_free (sock);
    g_free (dir);
    if (fails)
        g_printerr ("%d check(s) failed\n", fails);
    else
        g_print ("uiipc: ok\n");
    return fails ? 1 : 0;
}
