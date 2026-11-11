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
       WALPE_WEB_IN_SCROLL,
       /* keyboard (module ABI 11), as wl_keyboard reports it on the
        * wallpaper's surface. Delivered to a page only by the private page
        * host (as real key events); ignored with the X11 host, where no
        * keyboard input has ever reached a wallpaper. */
       WALPE_WEB_IN_KEYMAP,     /* keymap_fd, keymap_size; viewer may be NULL */
       WALPE_WEB_IN_KBD_FOCUS,  /* pressed = this monitor's page gained it */
       WALPE_WEB_IN_KEY,        /* key, pressed, time */
       WALPE_WEB_IN_MODIFIERS,  /* mods; viewer may be NULL */
};
typedef struct {
    int kind;           /* WALPE_WEB_IN_* */
    int button;         /* DOWN / UP: 1 left, 2 middle, 3 right */
    double x, y;        /* position on the monitor, pixels from its top-left
                         * (outside it while a button is held: a drag) */
    int mon_w, mon_h;   /* the monitor's size */
    int dx, dy;         /* SCROLL: wheel clicks, dy > 0 = down, dx > 0 = right */
    guint32 time;       /* time of the event in ms (X server / compositor
                         * clock), 0 = not known */
    /* ---- ABI 11 */
    guint32 key;        /* KEY: evdev key code (wl_keyboard.key) */
    gboolean pressed;   /* KEY: down; KBD_FOCUS: gained */
    guint32 mods[4];    /* MODIFIERS: depressed, latched, locked, group */
    int keymap_fd;      /* KEYMAP: an xkb keymap, text format v1; the caller
                         * keeps its descriptor (the module duplicates it) */
    guint32 keymap_size;
} WalpeWebInput;
void walpe_web_input (gpointer viewer, const WalpeWebInput *in);
/* where the pages' windows live: "x11" (an off-screen X window; on Wayland
 * through Xwayland), "private" (a Wayland display of the module's own),
 * "" = no page was created yet; g_free. WALPE_WEB_HOST=auto|x11|private. */
char *walpe_web_host (void);
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
