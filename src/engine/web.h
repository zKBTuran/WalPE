/* web.h - Tier 3: WebKitGTK web wallpaper backend. */
#pragma once
#include "content.h"

/* render-thread constructor: GTK/WebKit objects are created on the main
 * thread (the daemon's default GMainContext) via an idle */
WalpeContent *walpe_web_new (const char *path, const WalpeLoadOpts *opts,
                             GError **err);
/* main thread: pointer input on one monitor, for the page it shows.
 * viewer = the request_user the daemon gave that monitor's content. */
enum { WALPE_WEB_IN_MOVE, WALPE_WEB_IN_LEAVE, WALPE_WEB_IN_DOWN, WALPE_WEB_IN_UP,
       WALPE_WEB_IN_SCROLL };
typedef struct {
    int kind;           /* WALPE_WEB_IN_* */
    int button;         /* DOWN / UP: 1 left, 2 middle, 3 right */
    double x, y;        /* position on the monitor, pixels from its top-left
                         * (outside it while a button is held: a drag) */
    int mon_w, mon_h;   /* the monitor's size */
    int dx, dy;         /* SCROLL: wheel clicks, dy > 0 = down, dx > 0 = right */
    guint32 time;       /* X server time of the event, 0 = not known */
} WalpeWebInput;
void walpe_web_input (gpointer viewer, const WalpeWebInput *in);
/* how pointer input reaches the live pages: "native" (real input events,
 * confirmed by the page), "native (unconfirmed)", "js" (script-dispatched
 * fallback), "off"; "" = no web page; g_free */
char *walpe_web_input_status (void);
/* diagnostics: snapshots taken so far (all live web wallpapers) */
void walpe_web_stats (guint *snapshots);
/* comma-separated GStreamer elements the current page's media need but the
 * system lacks ("" = none or not checked); g_free */
char *walpe_web_media_missing (void);
/* why web wallpapers are disabled in this daemon ("" = they are not):
 * WebKit failed its start-up test in a child process; g_free */
char *walpe_web_error (void);
/* `walpe-daemon --web-probe`: start WebKit once in this process and
 * return 0 if that worked (the caller _exit()s with the result) */
int walpe_web_probe_run (void);
/* how pages are rendered, for status: the renderer WebGL uses in the page
 * process as the start-up test found it, marked when it is a software
 * rasteriser ("" = not known yet); g_free */
char *walpe_web_info (void);
