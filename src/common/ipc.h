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

/* State pushes. A handler may call walpe_ipc_subscribe_current() while it
 * works on a request: that connection then stays a subscriber and gets
 * every walpe_ipc_broadcast() line (FALSE: no room / not in a handler). */
gboolean walpe_ipc_subscribe_current (void);
guint walpe_ipc_subscribers (void);
void walpe_ipc_broadcast (const char *line);

/* Client helpers (blocking, short-lived): returns reply string or NULL. */
char *walpe_ipc_request (const char *sock_path, const char *request_json, int timeout_ms);
gboolean walpe_daemon_running (const char *sock_path);
