/* uiipc.c - asynchronous daemon IPC (see uiipc.h) */
#include "uiipc.h"
#include "common/config.h"
#include "common/util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    char *json;
    guint flags;
    UiIpcDone cb;
    gpointer user;
    JsonNode *reply;   /* worker -> main */
    const char *err;   /* static string */
} Req;

static const char ERR_DOWN[] = "walpe-daemon is not running";
static const char ERR_BUSY[] = "walpe-daemon did not answer in time (busy?)";
/* two lanes, each one worker: user requests stay in order and a status
 * poll stuck on a busy daemon never delays them */
static GAsyncQueue *queue[2];
static guint pending;          /* main thread only */
static void (*down_hook) (void);

/* ------------------------------------------------------------ worker */

static int wait_fd (int fd, short ev, gint64 deadline)
{
    for (;;) {
        gint64 remain = deadline - g_get_monotonic_time ();
        if (remain <= 0)
            return 0;
        struct pollfd p = { .fd = fd, .events = ev };
        int r = poll (&p, 1, (int) ((remain + 999) / 1000));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r;
        return (p.revents & (ev | POLLHUP | POLLERR)) ? 1 : 0;
    }
}

/* one request/response with a hard deadline for connect+write+read */
static char *request_once (const char *sock, const char *json, int timeout_ms, const char **err)
{
    gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        *err = "socket() failed";
        return NULL;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf (addr.sun_path, sizeof (addr.sun_path), "%s", sock);
    if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
        /* a full listen backlog (daemon main loop busy) answers EAGAIN on
         * a non-blocking unix socket: retry until the deadline */
        gboolean ok = FALSE, full = FALSE;
        while (errno == EAGAIN || errno == EINPROGRESS) {
            if (errno == EINPROGRESS) {
                if (wait_fd (fd, POLLOUT, deadline) > 0) {
                    int soerr = 0;
                    socklen_t sl = sizeof (soerr);
                    getsockopt (fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                    ok = soerr == 0;
                }
                break;
            }
            full = TRUE;
            if (g_get_monotonic_time () >= deadline)
                break;
            g_usleep (20000);
            if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) == 0) {
                ok = TRUE;
                break;
            }
            full = errno == EAGAIN;
        }
        if (!ok) {
            close (fd);
            /* The backlog was full to the end: somebody listens there and
             * does not get to its socket - a daemon that hangs or is stopped,
             * whose queue the requests before this one filled (each of them
             * stays in it when its time is up). That is "does not answer",
             * as for the requests before it. "Not running" made the window
             * start a second daemon, which ended at once because the first
             * holds the lock, and then report "the engine keeps crashing". */
            *err = full ? ERR_BUSY : ERR_DOWN;
            return NULL;
        }
    }
    GString *req = g_string_new (json);
    g_string_append_c (req, '\n');
    gsize off = 0;
    while (off < req->len) {
        ssize_t n = send (fd, req->str + off, req->len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (gsize) n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR) && wait_fd (fd, POLLOUT, deadline) > 0)
            continue;
        break;
    }
    gboolean sent = off == req->len;
    g_string_free (req, TRUE);
    if (!sent) {
        close (fd);
        *err = "walpe-daemon did not accept the request";
        return NULL;
    }
    GString *acc = g_string_new (NULL);
    gboolean done = FALSE;
    while (!done) {
        int w = wait_fd (fd, POLLIN, deadline);
        if (w <= 0)
            break;
        char buf[16384];
        ssize_t n = recv (fd, buf, sizeof (buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            break;
        g_string_append_len (acc, buf, n);
        if (memchr (buf, '\n', (size_t) n) || acc->len > (4u << 20))
            done = TRUE;
    }
    close (fd);
    char *nl = memchr (acc->str, '\n', acc->len);
    char *out = NULL;
    if (nl)
        out = g_strndup (acc->str, (gsize) (nl - acc->str));
    else
        *err = acc->len ? "truncated reply from walpe-daemon" : ERR_BUSY;
    g_string_free (acc, TRUE);
    return out;
}

static gboolean shut;    /* ui_ipc_shutdown: main thread only */
static gint busy;        /* atomic: requests queued or with a worker */

void ui_ipc_flush (guint max_ms)
{
    gint64 end = g_get_monotonic_time () + (gint64) max_ms * 1000;
    while (g_atomic_int_get (&busy) > 0 && g_get_monotonic_time () < end)
        g_usleep (2000);
}

void ui_ipc_shutdown (void)
{
    shut = TRUE;
}

static gboolean deliver (gpointer data)
{
    Req *r = data;
    pending--;
    JsonObject *o = r->reply && JSON_NODE_HOLDS_OBJECT (r->reply)
                        ? json_node_get_object (r->reply) : NULL;
    if (!shut && !o && r->err == ERR_DOWN && (r->flags & UI_IPC_AUTOSTART) && down_hook)
        down_hook ();
    if (r->cb && !shut)
        r->cb (o, o ? NULL : (r->err ? r->err : "bad reply from walpe-daemon"), r->user);
    if (r->reply)
        json_node_unref (r->reply);
    g_free (r->json);
    g_free (r);
    return G_SOURCE_REMOVE;
}

static gpointer worker_main (gpointer u)
{
    GAsyncQueue *q = u;
    char *sock = walpe_socket_path ();
    for (;;) {
        Req *r = g_async_queue_pop (q);
        int tmo = (r->flags & UI_IPC_SLOW) ? 8000 : 2500;
        const char *err = NULL;
        char *raw = request_once (sock, r->json, tmo, &err);
        if (raw) {
            r->reply = wpj_parse_data (raw, -1);
            g_free (raw);
        }
        r->err = err;
        g_atomic_int_add (&busy, -1);
        g_main_context_invoke (NULL, deliver, r);
    }
    g_free (sock);
    return NULL;
}

/* -------------------------------------------------------------- API */

void ui_ipc_send (const char *json, guint flags, UiIpcDone cb, gpointer user)
{
    if (!queue[0]) {
        for (int i = 0; i < 2; i++) {
            queue[i] = g_async_queue_new ();
            g_thread_unref (g_thread_new (i ? "walpe-ipc-poll" : "walpe-ipc", worker_main,
                                          queue[i]));
        }
    }
    Req *r = g_new0 (Req, 1);
    r->json = g_strdup (json);
    r->flags = flags;
    r->cb = cb;
    r->user = user;
    pending++;
    g_atomic_int_inc (&busy);
    g_async_queue_push (queue[(flags & UI_IPC_POLL) ? 1 : 0], r);
}

void ui_ipc_send_node (JsonNode *req, guint flags, UiIpcDone cb, gpointer user)
{
    char *s = json_to_string (req, FALSE);
    json_node_unref (req);
    ui_ipc_send (s, flags, cb, user);
    g_free (s);
}

guint ui_ipc_pending (void)
{
    return pending;
}

gboolean ui_ipc_ok (JsonObject *reply)
{
    return reply && wpj_bool (reply, "ok", FALSE);
}

gboolean ui_ipc_err_is_down (const char *err)
{
    return err == ERR_DOWN;
}

void ui_ipc_set_down_hook (void (*fn) (void))
{
    down_hook = fn;
}
