/* webinput.h - pointer input and navigation guard for an off-screen
 * WebKit page (part of walpe-web.so; main thread only). See webinput.c. */
#pragma once
#include <gtk/gtk.h>
#include <webkit/webkit.h>

typedef struct WebInput WebInput;

/* win = the page's off-screen toplevel, view = its only child. Call before
 * the first load: adds a document-start user script and connects the
 * guards (navigation, context menu, option menus, choosers, fullscreen). */
WebInput *web_input_new (GtkWidget *win, WebKitWebView *view, const char *label);
/* URI prefix of the wallpaper's own files ("http://127.0.0.1:PORT/TOKEN/"):
 * the main frame may only navigate inside it */
void web_input_set_base (WebInput *wi, const char *base_uri);
void web_input_free (WebInput *wi);

/* x, y = page coordinates (CSS px of the view); time = X server time of
 * the real event, 0 = not known */
void web_input_move (WebInput *wi, double x, double y, guint32 time);
/* the page forgot where the pointer is (a new document): the next move is
 * delivered even to the position it already has */
void web_input_refresh (WebInput *wi);
/* the pointer left the page: hover ends, held buttons are released */
void web_input_leave (WebInput *wi, guint32 time);
/* the page is about to be hidden (suspend) with buttons held: tell it now,
 * through the script, that they are up. The real releases web_input_leave
 * queues for GDK are not read before the window is hidden, so the page
 * never saw a mouseup and kept its drag state. Call before web_input_leave. */
void web_input_cancel (WebInput *wi);
/* button: 1 left, 2 middle, 3 right */
void web_input_button (WebInput *wi, int button, gboolean down, double x, double y, guint32 time);
/* wheel clicks: dy > 0 = down, dx > 0 = right */
void web_input_scroll (WebInput *wi, int dx, int dy, double x, double y, guint32 time);

/* the page posted 'input': it received a trusted pointer event */
void web_input_ack (WebInput *wi);
/* TRUE once after a main-frame load that was refused or failed for a URI
 * outside the wallpaper: the page is still the same one (the caller skips
 * its "page loaded" work for that WEBKIT_LOAD_FINISHED) */
gboolean web_input_take_nav_blocked (WebInput *wi);
/* "native", "native (unconfirmed)", "js", "off" (static string) */
const char *web_input_mode_name (WebInput *wi);

/* log lines a page can trigger at will (dialogs, refused navigations and
 * loads): at most WEB_LOG_BURST per kind and minute reach the log, so a
 * page looping on alert() cannot fill the disk. TRUE = log this one. */
enum { WEB_LOG_DIALOG, WEB_LOG_NAV, WEB_LOG_FILE, WEB_LOG_DOWNLOAD, WEB_LOG_PERMISSION,
       WEB_LOG_LOAD, WEB_LOG_KINDS };
#define WEB_LOG_BURST 5
gboolean web_log_gate (int kind);
/* page-chosen text for a log line: at most max bytes, control characters
 * (a newline would forge a log line) replaced by spaces. g_free. */
char *web_log_text (const char *s, gsize max);
