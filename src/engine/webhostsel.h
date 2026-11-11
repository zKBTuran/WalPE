/* webhostsel.h - where the page of a web wallpaper lives while nobody sees
 * its window. Pure; web.c and tests/webhost_select_test.c include it.
 *
 * WebKitGTK runs a page only in a window that is mapped, and what is
 * shown on the desktop is never that window (the picture is captured and
 * drawn by the daemon). So the window has to exist somewhere out of sight:
 *
 *   x11      an override-redirect X window moved off screen. On an X11
 *            session this is what WalPE has always done. On a Wayland
 *            session it works through Xwayland, when there is one.
 *   private  a Wayland display of the module's own, inside the daemon
 *            (webhost_wl.c): GTK connects to it instead of the session's
 *            compositor. Needs no X at all; pointer and keyboard input
 *            reach the page as real Wayland events; the session's
 *            clipboard and selections are simply not there.
 *
 * WALPE_WEB_HOST = auto (default) | x11 | private.
 * auto: x11 wherever an X display is there (DISPLAY set) - an X11 backend
 * exactly as before, a Wayland backend through Xwayland - and private
 * only when there is no X display at all. So with an X display the
 * private page host is opt-in (WALPE_WEB_HOST=private): it has not run on
 * real compositors yet, a failure of its display ends the daemon, and the
 * X window through Xwayland is the tested path. (Should the private
 * display fail WebKit's start-up test where auto chose it, x11 is taken
 * when DISPLAY is set after all.) */
#pragma once
#include <glib.h>
#include <string.h>

typedef enum {
    WEB_HOST_NONE,      /* no way to host a page; *why says what is missing */
    WEB_HOST_X11,
    WEB_HOST_PRIVATE,
} WebHost;

typedef struct {
    const char *setting;        /* WALPE_WEB_HOST; NULL / "" / unknown word = auto */
    int backend;                /* the daemon's backend: 1 X11, 2 Wayland, 0 = not known
                                 * (the start-up test's child process) */
    const char *backend_env;    /* WALPE_BACKEND, a hint while backend is 0 */
    gboolean x_display;         /* DISPLAY is set */
    gboolean wayland_display;   /* WAYLAND_DISPLAY is set */
    gboolean built_x11;         /* GTK's X11 backend + Xlib are in this build */
    gboolean built_private;     /* webhost_wl.c is in this build */
    gboolean private_failed;    /* the private display failed its start-up test */
} WebHostEnv;

static inline const char *web_host_name (WebHost h)
{
    return h == WEB_HOST_X11 ? "x11" : h == WEB_HOST_PRIVATE ? "private" : "none";
}

/* does the daemon draw on Wayland, as far as this process can tell? */
static inline gboolean web_host_on_wayland (const WebHostEnv *e)
{
    if (e->backend == 2)
        return TRUE;
    if (e->backend == 1)
        return FALSE;
    if (e->backend_env && *e->backend_env) {
        if (!g_ascii_strcasecmp (e->backend_env, "x11") || !g_ascii_strcasecmp (e->backend_env, "xorg"))
            return FALSE;
        if (!g_ascii_strcasecmp (e->backend_env, "wayland") || !g_ascii_strcasecmp (e->backend_env, "wl"))
            return TRUE;
    }
    return e->wayland_display;
}

/* *why (static text, may be NULL): for WEB_HOST_NONE what is missing, for
 * a fallback why the first choice was not taken; NULL = nothing to say */
static inline WebHost web_host_choose (const WebHostEnv *e, const char **why)
{
    const char *w = NULL;
    WebHost h = WEB_HOST_NONE;
    const char *s = e->setting && *e->setting ? e->setting : "auto";
    if (!g_ascii_strcasecmp (s, "x11") || !g_ascii_strcasecmp (s, "xwayland")) {
        if (!e->built_x11)
            w = "WALPE_WEB_HOST=x11, but this build has no X11 page host";
        else if (!e->x_display)
            w = "WALPE_WEB_HOST=x11, but DISPLAY is not set (no X server or Xwayland to "
                "put the page's window on)";
        else
            h = WEB_HOST_X11;
    } else if (!g_ascii_strcasecmp (s, "private")) {
        if (!e->built_private)
            w = "WALPE_WEB_HOST=private, but this build has no private page host (it needs "
                "libwayland-server and GTK's Wayland backend at build time)";
        else
            h = WEB_HOST_PRIVATE;
    } else if (!web_host_on_wayland (e)) {
        /* X11: unchanged */
        if (e->built_x11 && e->x_display)
            h = WEB_HOST_X11;
        else if (e->built_private && !e->private_failed)
            h = WEB_HOST_PRIVATE;
        else
            w = "no X display for the page's window (DISPLAY is not set) and no private "
                "page host in this build";
    } else if (e->built_x11 && e->x_display) {
        /* Wayland with an X display (Xwayland): the off-screen X window,
         * as on X11; the private page host only by name */
        h = WEB_HOST_X11;
        if (e->private_failed)
            w = "the private page host failed its start-up test: using an off-screen X "
                "window through Xwayland instead";
    } else if (e->built_private && !e->private_failed) {
        /* no X display at all: the one place left for a page's window */
        h = WEB_HOST_PRIVATE;
    } else {
        w = e->private_failed
                ? "the private page host failed its start-up test and there is no Xwayland "
                  "(DISPLAY is not set) to fall back to"
                : "web wallpapers on Wayland need Xwayland (DISPLAY is not set) or the "
                  "private page host (not in this build)";
    }
    if (why)
        *why = w;
    return h;
}
