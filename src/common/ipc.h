/* ipc.h - single-instance control: newline-delimited JSON over a unix socket */
#pragma once
#include <glib.h>

/* Synchronous handler: receives request json (no trailing newline, "" on EOF),
 * returns a reply string (g_free'd by server) or NULL. Runs on the main loop. */
typedef char *(*WalpeIpcHandler) (const char *request_json, gpointer user);

gboolean walpe_ipc_serve (const char *sock_path, WalpeIpcHandler handler, gpointer user);
/* daemon shutdown: remove the socket (sock_path may be NULL) and give up
 * the single-instance lock taken by walpe_ipc_serve, so a daemon started
 * while this one still tears down finds the place free */
void walpe_ipc_release (const char *sock_path);

/* Restart in place (exec): the listening socket stays open across it, so a
 * client that connects meanwhile waits in the backlog instead of finding
 * no daemon (and starting a second one). walpe_ipc_listen_fd(): the
 * listening socket of this process, -1 = none; the caller clears its
 * close-on-exec flag and passes the number on. The new process image calls
 * walpe_ipc_adopt_fd(fd) before walpe_ipc_serve(), which then serves that
 * socket (when it really is a listening socket bound to sock_path)
 * instead of binding a new one. */
int      walpe_ipc_listen_fd (void);
void     walpe_ipc_adopt_fd (int fd);
/* For a new image that could not serve for a while (it waited for its
 * display to come back): closes every connection that queued up on the
 * handed-over socket meanwhile, unanswered. Their senders were told that
 * no daemon replies; what they asked for then is not carried out later.
 * After walpe_ipc_adopt_fd () and before walpe_ipc_serve (). Returns the
 * number of connections closed. */
int      walpe_ipc_drop_queued (void);

/* State pushes. A handler may call walpe_ipc_subscribe_current() while it
 * works on a request: that connection then stays a subscriber and gets
 * every walpe_ipc_broadcast() line (FALSE: no room / not in a handler). */
gboolean walpe_ipc_subscribe_current (void);
/* Inside a handler: the process id of the client whose request it is, as
 * the socket tells it (SO_PEERCRED); 0 = not in a handler, or not told.
 * For the log line of a request like "quit": asked once, when it happens. */
int walpe_ipc_current_pid (void);
guint walpe_ipc_subscribers (void);
void walpe_ipc_broadcast (const char *line);

/* Client helpers (blocking, short-lived): returns reply string or NULL. */
char *walpe_ipc_request (const char *sock_path, const char *request_json, int timeout_ms);
gboolean walpe_daemon_running (const char *sock_path);
/* something listens on the socket (a connection is taken, or its queue of
 * waiting connections is full): tells a daemon that is there but does not
 * answer - busy, stopped, hung - from none at all. Sends nothing. */
gboolean walpe_ipc_listening (const char *sock_path);
