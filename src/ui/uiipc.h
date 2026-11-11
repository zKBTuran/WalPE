/* uiipc.h - asynchronous daemon IPC for the GUI.
 *
 * The GTK main thread never touches the socket: requests are queued to
 * one worker thread (in order) that connects non-blocking, writes and
 * reads with hard deadlines, and hands the parsed reply back to the main
 * loop. A busy daemon (loading a scene) or a dead one therefore costs the
 * UI nothing but a late callback. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

enum {
    UI_IPC_AUTOSTART = 1 << 0,  /* user action: a dead daemon runs the down hook */
    UI_IPC_SLOW      = 1 << 1,  /* the daemon may be busy (open, setprops): 8 s */
    UI_IPC_POLL      = 1 << 2,  /* status poll: own lane, never delays actions */
};

/* main thread: reply = the reply object (borrowed for the call) or NULL
 * when the daemon was unreachable or timed out; err = short reason */
typedef void (*UiIpcDone) (JsonObject *reply, const char *err, gpointer user);

/* queue a request (copied). cb may be NULL. Must be called on the main
 * thread; cb runs there. */
void ui_ipc_send (const char *json, guint flags, UiIpcDone cb, gpointer user);
/* same, building the request from a JsonBuilder root (consumed) */
void ui_ipc_send_node (JsonNode *req, guint flags, UiIpcDone cb, gpointer user);
/* requests queued or in flight */
guint ui_ipc_pending (void);
/* The window is gone: from now on no callback runs - a reply that is in
 * flight is dropped when it arrives, a request sent later (a panel saving
 * its last change while it is destroyed) goes out without one. The
 * callbacks would use the widgets of the destroyed window. */
void ui_ipc_shutdown (void);
/* Before the process ends: wait (at most max_ms, without running the main
 * loop) until the worker threads have sent what is queued and read the
 * answers - the last change made before quitting is not cut off. */
void ui_ipc_flush (guint max_ms);
/* reply helper: ok == TRUE */
gboolean ui_ipc_ok (JsonObject *reply);
/* err from a callback means "no daemon listening" (socket missing or
 * refused), as opposed to a timeout of a live but busy daemon */
gboolean ui_ipc_err_is_down (const char *err);
/* called (main thread) when an UI_IPC_AUTOSTART request found no daemon */
void ui_ipc_set_down_hook (void (*fn) (void));
