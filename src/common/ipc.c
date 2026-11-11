#include "ipc.h"
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* limits of the wire protocol (one JSON line each way) */
#define IPC_MAX_REQUEST (1u << 20)      /* server: longest request line */
#define IPC_MAX_REPLY   (16u << 20)     /* client: longest reply line */
#define IPC_REPLY_TIMEOUT_S 3           /* server: a client that does not read */
#define IPC_MAX_SESSIONS 64             /* server: connections open at once */
#define IPC_MAX_SUBSCRIBERS 8           /* server: connections that get state pushes */

/* ----------------------------------------------------------------- server */

typedef struct {
    WalpeIpcHandler handler;
    gpointer user;
} IpcCtx;

typedef struct {
    IpcCtx *ctx;
    GSocketConnection *conn;
    GString *acc;           /* bytes received, not yet a complete line */
    GCancellable *cancel;   /* ends the pending read (eviction) */
    gint64 active_us;       /* last data from the client */
    gboolean subscribed;    /* gets walpe_ipc_broadcast lines */
    gboolean reading;       /* a read is pending: session_read frees the session */
    /* a reply the socket did not take at once (the client is slow, or does
     * not read): kept here and sent when the socket takes more. Nothing is
     * read from the client meanwhile, so at most one reply waits. */
    GString *out;
    gsize out_off;          /* bytes of out already sent */
    GSource *out_src;       /* the socket can take more */
    guint out_timer;        /* a client that does not read is dropped */
    gint64 out_us;          /* the last time the socket took something */
    gboolean close_when_sent;
    char buf[4096];
} IpcSession;

static GList *sessions;      /* main loop only */
static guint n_sessions;
static guint n_subscribed;
static IpcSession *current;  /* the session whose request the handler is working on */

static void session_pump (IpcSession *s);

static gsize session_unsent (IpcSession *s)
{
    return s->out->len - s->out_off;
}

static void session_out_stop (IpcSession *s)
{
    if (s->out_src) {
        g_source_destroy (s->out_src);
        g_source_unref (s->out_src);
        s->out_src = NULL;
    }
    if (s->out_timer) {
        g_source_remove (s->out_timer);
        s->out_timer = 0;
    }
}

/* never while its read is pending (reading): that read is cancelled
 * instead and its callback, session_read, comes here */
static void session_free (IpcSession *s)
{
    if (s->subscribed)
        n_subscribed--;
    if (current == s)
        current = NULL;
    n_sessions--;
    sessions = g_list_remove (sessions, s);
    session_out_stop (s);
    g_object_unref (s->cancel);
    g_io_stream_close (G_IO_STREAM (s->conn), NULL, NULL);
    g_object_unref (s->conn);
    g_string_free (s->acc, TRUE);
    g_string_free (s->out, TRUE);
    g_free (s);
}

/* hand the socket what it takes now, without waiting. FALSE = the
 * connection is gone */
static gboolean session_flush (IpcSession *s)
{
    GSocket *sk = g_socket_connection_get_socket (s->conn);
    while (session_unsent (s)) {
        GError *err = NULL;
        gssize n = g_socket_send_with_blocking (sk, s->out->str + s->out_off, session_unsent (s),
                                                FALSE, NULL, &err);
        if (n > 0) {
            s->out_off += (gsize) n;
            s->out_us = g_get_monotonic_time ();
            continue;
        }
        gboolean later = g_error_matches (err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
        g_clear_error (&err);
        return later;
    }
    g_string_truncate (s->out, 0);
    s->out_off = 0;
    return TRUE;
}

static gboolean session_out_ready (GSocket *sk, GIOCondition cond, gpointer user)
{
    (void) sk; (void) cond;
    IpcSession *s = user;
    if (!session_flush (s)) {
        session_free (s);
        return G_SOURCE_REMOVE;
    }
    if (session_unsent (s))
        return G_SOURCE_CONTINUE;
    session_out_stop (s);
    if (s->close_when_sent)
        session_free (s);
    else
        session_pump (s);       /* the requests that waited behind this reply */
    return G_SOURCE_REMOVE;
}

static gboolean session_out_check (gpointer user)
{
    IpcSession *s = user;
    if (g_get_monotonic_time () - s->out_us < IPC_REPLY_TIMEOUT_S * G_USEC_PER_SEC)
        return G_SOURCE_CONTINUE;
    s->out_timer = 0;
    session_free (s);           /* it asked and does not read */
    return G_SOURCE_REMOVE;
}

/* A reply for the client. It is never waited for: what the socket does not
 * take at once is sent when it does (session_out_ready), and a client that
 * takes nothing for IPC_REPLY_TIMEOUT_S is dropped. (The write used to
 * block for up to that long on the daemon's main loop: one client that
 * asked and did not read held up every other client, the X events and a
 * web wallpaper's page for three seconds, and the next such connection
 * for three more.) FALSE = the connection is gone: the caller frees the
 * session. */
static gboolean session_send (IpcSession *s, const char *reply)
{
    if (!reply)
        reply = "{\"ok\":false,\"error\":\"internal\"}";
    /* a line that is not UTF-8 is no JSON to any client (json-glib refuses
     * the whole document, GIO's line reader drops the line): a file name,
     * an output name or a message from a library with other bytes in it
     * must not make `status` unreadable. Such bytes become U+FFFD; they
     * are never part of the JSON syntax, so the line stays one document. */
    g_autofree char *valid = NULL;
    if (!g_utf8_validate (reply, -1, NULL))
        reply = valid = g_utf8_make_valid (reply, -1);
    g_string_append (s->out, reply);
    g_string_append_c (s->out, '\n');
    if (!session_flush (s))
        return FALSE;
    if (session_unsent (s) && !s->out_src) {
        s->out_us = g_get_monotonic_time ();
        s->out_src = g_socket_create_source (g_socket_connection_get_socket (s->conn), G_IO_OUT, NULL);
        g_source_set_callback (s->out_src, G_SOURCE_FUNC (session_out_ready), s, NULL);
        g_source_attach (s->out_src, NULL);
        s->out_timer = g_timeout_add_seconds (1, session_out_check, s);
    }
    return TRUE;
}

static void session_read (GObject *src, GAsyncResult *res, gpointer user)
{
    IpcSession *s = user;
    gssize n = g_input_stream_read_finish (G_INPUT_STREAM (src), res, NULL);
    s->reading = FALSE;
    if (n <= 0) {
        session_free (s);   /* EOF or error: the client is gone */
        return;
    }
    g_string_append_len (s->acc, s->buf, n);
    s->active_us = g_get_monotonic_time ();
    session_pump (s);
}

/* the complete request lines received so far, one at a time and only
 * while the reply before is out; then the next read */
static void session_pump (IpcSession *s)
{
    char *nl;
    while (!session_unsent (s) && (nl = memchr (s->acc->str, '\n', s->acc->len))) {
        gsize ll = (gsize) (nl - s->acc->str);
        /* a NUL byte ends the C string the handler gets: what followed it
         * on the line was dropped without a word and the part before it
         * carried out. No JSON text holds one; the line is refused whole. */
        gboolean nul = memchr (s->acc->str, 0, ll) != NULL;
        char *line = g_strndup (s->acc->str, ll);
        g_string_erase (s->acc, 0, (gssize) ll + 1);
        current = s;
        char *reply = nul ? g_strdup ("{\"ok\":false,\"error\":\"bad json (NUL byte)\"}")
                    : g_utf8_validate (line, -1, NULL)
                          ? s->ctx->handler (line, s->ctx->user)
                          : g_strdup ("{\"ok\":false,\"error\":\"bad json (not UTF-8)\"}");
        current = NULL;
        gboolean ok = session_send (s, reply);
        g_free (reply);
        g_free (line);
        if (!ok) {
            session_free (s);
            return;
        }
    }
    if (session_unsent (s))
        return;     /* session_out_ready goes on from here */
    if (s->acc->len > IPC_MAX_REQUEST) {
        /* no request is anywhere near this long: a client that never sends
         * the newline must not grow the daemon without bound */
        if (session_send (s, "{\"ok\":false,\"error\":\"request too long\"}") && session_unsent (s))
            s->close_when_sent = TRUE;
        else
            session_free (s);
        return;
    }
    s->reading = TRUE;
    g_input_stream_read_async (g_io_stream_get_input_stream (G_IO_STREAM (s->conn)), s->buf,
                               sizeof (s->buf), G_PRIORITY_DEFAULT, s->cancel, session_read, s);
}

static gboolean on_incoming (GSocketService *svc, GSocketConnection *conn,
                             GSocket *src_sock, gpointer user)
{
    (void) svc; (void) src_sock;
    IpcCtx *ctx = user;
    /* the socket is for the user's own processes (GUI, tray, walpe-ctl):
     * another account must not drive the daemon even where the socket
     * file is reachable (no XDG_RUNTIME_DIR: it lives in ~/.cache) */
    GCredentials *cr = g_socket_get_credentials (g_socket_connection_get_socket (conn), NULL);
    uid_t peer = cr ? g_credentials_get_unix_user (cr, NULL) : (uid_t) -1;
    g_clear_object (&cr);
    if (peer != getuid () && peer != 0) {
        g_warning ("ipc: connection from another user (uid %ld) refused", (long) peer);
        return TRUE;    /* dropped: nobody holds a reference */
    }
    if (n_sessions >= IPC_MAX_SESSIONS) {
        /* every client opens one connection per request; a process that
         * piles them up must not use up the daemon's file descriptors, nor
         * lock the real clients out: the connection that has been silent
         * longest makes room once it idled for a second */
        IpcSession *old = NULL;
        for (GList *l = sessions; l; l = l->next) {
            IpcSession *o = l->data;
            /* a subscriber (tray, GUI) is silent by design: not the one to
             * drop; nor the one whose request is being worked on */
            if (!o->subscribed && o != current && !g_cancellable_is_cancelled (o->cancel) &&
                (!old || o->active_us < old->active_us))
                old = o;
        }
        if (!old || g_get_monotonic_time () - old->active_us < G_USEC_PER_SEC) {
            static gint64 warned_us;
            gint64 now = g_get_monotonic_time ();
            if (now - warned_us > 10 * G_USEC_PER_SEC) {
                warned_us = now;
                g_warning ("ipc: %u connections open, new ones are refused", n_sessions);
            }
            return TRUE;
        }
        if (old->reading)
            g_cancellable_cancel (old->cancel);     /* its read ends, session_read frees it */
        else
            session_free (old);                     /* it has a reply to take and does not */
    }
    n_sessions++;
    IpcSession *s = g_new0 (IpcSession, 1);
    s->ctx = ctx;
    s->conn = g_object_ref (conn);
    s->acc = g_string_new (NULL);
    s->out = g_string_new (NULL);
    s->cancel = g_cancellable_new ();
    s->active_us = g_get_monotonic_time ();
    sessions = g_list_prepend (sessions, s);
    session_pump (s);       /* nothing received yet: the first read */
    return TRUE;
}

/* ---- state pushes: a client that sent "subscribe" keeps its connection
 * and is told when the daemon's state changes, instead of asking every
 * 2 s whether it did (tray and GUI did, each with a new connection). */

gboolean walpe_ipc_subscribe_current (void)
{
    if (!current)
        return FALSE;
    if (current->subscribed)
        return TRUE;
    if (n_subscribed >= IPC_MAX_SUBSCRIBERS)
        return FALSE;
    current->subscribed = TRUE;
    n_subscribed++;
    return TRUE;
}

guint walpe_ipc_subscribers (void)
{
    return n_subscribed;
}

/* one line to every subscriber. Never blocks: a subscriber whose socket
 * buffer is full (it stopped reading) is dropped instead. */
void walpe_ipc_broadcast (const char *line)
{
    if (!n_subscribed || !line)
        return;
    g_autofree char *valid = NULL;   /* as send_reply: one line of UTF-8 */
    if (!g_utf8_validate (line, -1, NULL))
        line = valid = g_utf8_make_valid (line, -1);
    char *msg = g_strconcat (line, "\n", NULL);
    gsize len = strlen (msg);
    GSList *gone = NULL;
    for (GList *l = sessions; l; l = l->next) {
        IpcSession *s = l->data;
        if (!s->subscribed || g_cancellable_is_cancelled (s->cancel))
            continue;
        GSocket *sk = g_socket_connection_get_socket (s->conn);
        /* (behind a reply it has not taken yet the line would land in the
         * middle of that reply: such a subscriber is not reading either) */
        gssize n = session_unsent (s) ? -1
                                      : g_socket_send_with_blocking (sk, msg, len, FALSE, NULL, NULL);
        if (n != (gssize) len) {
            s->subscribed = FALSE;
            n_subscribed--;
            if (s->reading || s == current)
                g_cancellable_cancel (s->cancel);   /* its (next) read ends, session_read frees it */
            else
                gone = g_slist_prepend (gone, s);
        }
    }
    g_slist_free_full (gone, (GDestroyNotify) session_free);
    g_free (msg);
}

/* single instance: an exclusive flock on "<socket>.lock", held until
 * walpe_ipc_release() or the process ends (the kernel drops it however
 * the daemon dies, so there is no stale lock). The status probe alone
 * (walpe_daemon_running) took a daemon that was busy for 300 ms for a
 * dead one: a second daemon then removed the live socket and both drew on
 * the same root window. */
static int instance_lock_fd = -1;

static gboolean instance_lock (const char *sock_path)
{
    char *lp = g_strconcat (sock_path, ".lock", NULL);
    int fd = open (lp, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    g_free (lp);
    if (fd < 0)
        return TRUE;    /* no lock file possible here: the probe decides, as before */
    for (int tries = 0;; tries++) {
        if (flock (fd, LOCK_EX | LOCK_NB) == 0) {
            instance_lock_fd = fd;
            return TRUE;
        }
        if (errno == EINTR)
            continue;
        if (errno != EWOULDBLOCK) {
            close (fd);
            return TRUE;    /* flock unsupported (odd filesystem): the probe decides */
        }
        /* held. A daemon that answers is running; one that does not may be
         * on its way out (it releases the lock right after its socket):
         * give it two seconds, then it counts as running too. One probe
         * only: each leaves a connection in a stuck daemon's backlog. */
        if ((tries == 0 && walpe_daemon_running (sock_path)) || tries >= 20) {
            close (fd);
            return FALSE;
        }
        g_usleep (100000);
    }
}

void walpe_ipc_release (const char *sock_path)
{
    if (sock_path)
        unlink (sock_path);
    if (instance_lock_fd >= 0) {
        close (instance_lock_fd);
        instance_lock_fd = -1;
    }
}

/* the listening socket (kept for walpe_ipc_listen_fd) and one handed over
 * by the previous process image (walpe_ipc_adopt_fd) */
static GSocket *listen_sock;
static int adopt_fd = -1;

int walpe_ipc_listen_fd (void)
{
    return listen_sock ? g_socket_get_fd (listen_sock) : -1;
}

void walpe_ipc_adopt_fd (int fd)
{
    adopt_fd = fd;
}

/* the inherited fd as a GSocket when it is a listening unix socket bound
 * to sock_path; anything else is left alone (the number came from the
 * environment: it may be any descriptor) and NULL returned */
static GSocket *adopt_listener (const char *sock_path)
{
    int fd = adopt_fd;
    adopt_fd = -1;
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    socklen_t al = sizeof addr;
    int acc = 0;
    socklen_t ol = sizeof acc;
    memset (&addr, 0, sizeof addr);
    gboolean ok = getsockname (fd, (struct sockaddr *) &addr, &al) == 0 &&
                  addr.sun_family == AF_UNIX &&
                  al > offsetof (struct sockaddr_un, sun_path) &&
                  strncmp (addr.sun_path, sock_path, sizeof addr.sun_path) == 0 &&
                  getsockopt (fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &ol) == 0 && acc;
    GSocket *sock = ok ? g_socket_new_from_fd (fd, NULL) : NULL;
    if (!sock) {
        g_warning ("ipc: the inherited socket (fd %d) is not the listener of %s; binding anew",
                   fd, sock_path);
        return NULL;
    }
    fcntl (fd, F_SETFD, FD_CLOEXEC);   /* not for our children */
    return sock;
}

gboolean walpe_ipc_serve (const char *sock_path, WalpeIpcHandler handler, gpointer user)
{
    if (g_str_has_prefix (sock_path, "/dev/null/")) {
        /* config.h WALPE_SOCKET_NONE: walpe_socket_path() found no
         * directory of the user's own and said why */
        g_warning ("ipc: no private directory for the control socket; not serving");
        return FALSE;
    }
    if (strlen (sock_path) >= sizeof (((struct sockaddr_un *) 0)->sun_path)) {
        /* bind() would use a cut-off name that is never found again */
        g_warning ("ipc: socket path %s is longer than the %d bytes a unix socket address "
                   "holds; set XDG_RUNTIME_DIR to a short directory", sock_path,
                   (int) sizeof (((struct sockaddr_un *) 0)->sun_path) - 1);
        return FALSE;
    }
    IpcCtx *ctx = g_new (IpcCtx, 1);
    ctx->handler = handler;
    ctx->user = user;

    /* the single-instance lock decides, also for an image that was
     * restarted in place: its lock fd is close-on-exec, so the previous
     * image's lock ended at the exec and this one takes it again. The
     * listening socket it may have handed over is adopted after that. */
    if (!instance_lock (sock_path)) {
        g_warning ("another walpe-daemon is already running (it holds %s.lock)", sock_path);
        g_free (ctx);
        return FALSE;
    }

    GError *err = NULL;
    GSocket *sock = adopt_listener (sock_path);
    if (!sock) {
        /* nothing handed over by a previous image of this process: the
         * ordinary start */
        if (walpe_daemon_running (sock_path)) {
            /* a daemon from before the lock file existed */
            g_warning ("another walpe-daemon is already running (socket %s is live)", sock_path);
            walpe_ipc_release (NULL);
            g_free (ctx);
            return FALSE;
        }
        unlink (sock_path);

        sock = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                             G_SOCKET_PROTOCOL_DEFAULT, &err);
        if (!sock) {
            g_warning ("ipc socket: %s", err->message);
            g_error_free (err);
            walpe_ipc_release (NULL);
            g_free (ctx);
            return FALSE;
        }
        /* owner only, whatever the umask: Linux creates the socket file with
         * the mode of the socket inode, so set it before bind (no window in
         * which the file is group/world accessible), and again by path after */
        fchmod (g_socket_get_fd (sock), 0600);
        GSocketAddress *saddr = g_unix_socket_address_new (sock_path);
        gboolean bound = g_socket_bind (sock, saddr, TRUE, &err) &&
                         g_socket_listen (sock, &err);
        g_object_unref (saddr);
        if (!bound) {
            g_warning ("ipc bind/listen: %s", err->message);
            g_error_free (err);
            g_object_unref (sock);
            walpe_ipc_release (NULL);
            g_free (ctx);
            return FALSE;
        }
    }
    if (chmod (sock_path, 0600) != 0)
        g_warning ("ipc: chmod 0600 %s: %s", sock_path, g_strerror (errno));
    GSocketService *svc = g_socket_service_new ();
    g_socket_listener_add_socket (G_SOCKET_LISTENER (svc), sock, NULL, &err);
    if (err) {
        g_warning ("ipc listener: %s", err->message);
        g_error_free (err);
        g_object_unref (svc);
        g_object_unref (sock);
        walpe_ipc_release (sock_path);
        g_free (ctx);
        return FALSE;
    }
    g_signal_connect (svc, "incoming", G_CALLBACK (on_incoming), ctx);
    g_socket_service_start (svc);
    g_object_ref (svc); /* live for the process lifetime */
    listen_sock = sock; /* our reference: walpe_ipc_listen_fd() */
    return TRUE;
}

/* ----------------------------------------------------------------- client */

/* connected non-blocking socket, or -1. A daemon whose main loop is stuck
 * stops accepting: once its backlog is full a blocking connect() would
 * hang for good, so the connect is bounded by the deadline too. */
static int connect_unix (const char *path, gint64 deadline)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (!path || strlen (path) >= sizeof (addr.sun_path))
        return -1;      /* a truncated path would be some other socket */
    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memcpy (addr.sun_path, path, strlen (path) + 1);
    for (;;) {
        if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) == 0)
            return fd;
        if (errno == EINTR)
            continue;
        /* EAGAIN: the listen backlog is full (unix sockets) */
        if (errno != EAGAIN || g_get_monotonic_time () >= deadline)
            break;
        g_usleep (20000);
    }
    close (fd);
    return -1;
}

/* wait until fd is ready for ev or the deadline (monotonic us) passed;
 * a signal (SIGCHLD in the GUI / tray) is not a failure. poll, not
 * select: a process with more than 1023 open files has fds FD_SET
 * cannot hold. */
static gboolean wait_io (int fd, short ev, gint64 deadline)
{
    for (;;) {
        gint64 remain = deadline - g_get_monotonic_time ();
        if (remain <= 0)
            return FALSE;
        struct pollfd p = { .fd = fd, .events = ev };
        int r = poll (&p, 1, (int) MIN ((remain + 999) / 1000, 60000));
        if (r > 0)
            return TRUE;
        if (r < 0 && errno != EINTR)
            return FALSE;
    }
}

/* the whole buffer; MSG_NOSIGNAL: a daemon that went away must not kill
 * the client with SIGPIPE */
static gboolean send_all (int fd, const char *data, gsize len, gint64 deadline)
{
    gsize off = 0;
    while (off < len) {
        ssize_t n = send (fd, data + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            off += (gsize) n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_io (fd, POLLOUT, deadline))
                return FALSE;
        } else if (!(n < 0 && errno == EINTR)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* one reply line (without the newline), NULL when none arrived whole */
static char *recv_line (int fd, gint64 deadline, gsize max)
{
    GString *acc = g_string_new (NULL);
    char *nl = NULL;
    while (!nl && acc->len <= max && wait_io (fd, POLLIN, deadline)) {
        char buf[16384];
        ssize_t n = recv (fd, buf, sizeof (buf), MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            break;
        gsize from = acc->len;
        g_string_append_len (acc, buf, n);
        nl = memchr (acc->str + from, '\n', (gsize) n);
    }
    /* a reply cut short (timeout, limit, daemon gone) is no reply: half a
     * JSON document is of no use to any caller */
    char *out = nl ? g_strndup (acc->str, (gsize) (nl - acc->str)) : NULL;
    g_string_free (acc, TRUE);
    return out;
}

char *walpe_ipc_request (const char *sock_path, const char *request_json, int timeout_ms)
{
    gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
    int fd = connect_unix (sock_path, deadline);
    if (fd < 0)
        return NULL;
    g_autoptr (GString) req = g_string_new (request_json);
    g_string_append_c (req, '\n');
    char *out = send_all (fd, req->str, req->len, deadline) ? recv_line (fd, deadline, IPC_MAX_REPLY)
                                                              : NULL;
    close (fd);
    return out;
}

gboolean walpe_daemon_running (const char *sock_path)
{
    gint64 deadline = g_get_monotonic_time () + 300 * 1000;
    int fd = connect_unix (sock_path, deadline);
    if (fd < 0)
        return FALSE;
    const char *probe = "{\"cmd\":\"status\"}\n";
    gboolean alive = FALSE;
    if (send_all (fd, probe, strlen (probe), deadline) && wait_io (fd, POLLIN, deadline)) {
        char buf[256];
        alive = recv (fd, buf, sizeof (buf), MSG_DONTWAIT) > 0;
    }
    close (fd);
    return alive;
}
