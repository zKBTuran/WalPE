/* trayipc.h - non-blocking daemon requests (see trayipc.c) */
#pragma once
#include <glib.h>

/* reply = the daemon's JSON line, or NULL (not running / timeout / error);
 * owned by the caller of the callback only for its duration */
typedef void (*TrayIpcCb) (const char *reply, gpointer user);
void tray_ipc (const char *json, int timeout_ms, TrayIpcCb cb, gpointer user);
