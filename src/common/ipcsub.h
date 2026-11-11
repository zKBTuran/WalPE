/* ipcsub.h - a standing connection to walpe-daemon that is told about
 * state changes ("subscribe"), for the tray and the GUI.
 *
 * Both used to ask for the status every 2 s, each time over a new
 * connection, for as long as they ran: two wake-ups of their own and one
 * of the daemon per poll, around the clock. A subscriber sleeps until the
 * daemon has something to say. When the daemon is not running the
 * subscription waits for its socket to appear (a directory watch, no
 * timer) and connects again by itself. */
#pragma once
#include <glib.h>

typedef struct WalpeIpcSub WalpeIpcSub;

/* line = the daemon's status reply as JSON text (once after connecting,
 * then once per state change; borrowed for the call), or NULL when the
 * daemon went away / cannot be reached. Runs on the main context the
 * subscription was created on. */
typedef void (*WalpeIpcSubCb) (const char *line, gpointer user);

WalpeIpcSub *walpe_ipc_sub_new (const char *sock_path, WalpeIpcSubCb cb, gpointer user);
void walpe_ipc_sub_free (WalpeIpcSub *s);
/* connected and receiving pushes */
gboolean walpe_ipc_sub_live (WalpeIpcSub *s);
/* the daemon that answered does not know "subscribe" (an older build
 * still running): the caller polls as it used to */
gboolean walpe_ipc_sub_unsupported (WalpeIpcSub *s);
/* try to connect now (the caller just started a daemon) */
void walpe_ipc_sub_kick (WalpeIpcSub *s);
