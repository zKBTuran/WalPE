/* x11desk.h - a desktop environment's own desktop window, and where the
 * wallpaper window of a monitor goes because of it.
 *
 * A bare window manager (dwm, i3, openbox, bspwm ...) leaves the root
 * window free: the wallpaper window is the lowest child of the root and
 * every other window is above it. A full desktop (XFCE, LXQt, LXDE, MATE,
 * Cinnamon, a GNOME session with desktop icons) runs a file manager that
 * puts a window of its own over the whole root: _NET_WM_WINDOW_TYPE_DESKTOP,
 * the size of the screen or of a monitor, the desktop's wallpaper and its
 * icons drawn into it, and the window manager keeps it below every other
 * window. A wallpaper window at the bottom of the root is behind it.
 *
 * Two kinds of such a window, told apart by what X says of it:
 *  - opaque (a 24-bit visual, or no compositing manager runs): xfdesktop,
 *    pcmanfm --desktop, pcmanfm-qt --desktop, caja. Nothing below it is
 *    ever seen. The wallpaper window is made a child of it, as large as
 *    the monitor (what xwinwrap -ov does with the window it finds): the
 *    window manager stacks the desktop window and never looks at its
 *    children, so the wallpaper is exactly as high in the stack as the
 *    desktop is - above the desktop's own picture, below every window and
 *    panel - and no restacking by the window manager can come between.
 *    The icons the file manager draws are behind the wallpaper; clicks go
 *    through to the file manager (the wallpaper window takes no input).
 *  - translucent (a 32-bit visual under a compositing manager): nemo-desktop
 *    on Cinnamon, the "Desktop Icons NG" window on GNOME. The compositor
 *    shows what is below it between the icons: the wallpaper window stays
 *    the lowest child of the root, and the icons are seen above it.
 *
 * Pure functions: platform/x11.c feeds them what the server says,
 * tests/x11desk_test.c made-up windows. */
#pragma once
#include <glib.h>
#include <string.h>

/* WALPE_X11_DESKTOP */
typedef enum {
    WALPE_X11_DESK_AUTO,    /* into an opaque desktop window, else the root */
    WALPE_X11_DESK_ROOT,    /* always the lowest child of the root (as up to 0.14.0) */
    WALPE_X11_DESK_WINDOW,  /* into a desktop window, also a translucent one */
} WalpeX11DeskMode;

typedef struct { int x, y, width, height; } WalpeDeskRect;

/* a mapped client window of type _NET_WM_WINDOW_TYPE_DESKTOP */
typedef struct {
    unsigned long client;   /* the window itself */
    unsigned long top;      /* the child of the root it is in (itself, or the window
                             * manager's frame around it); 0 = not known */
    WalpeDeskRect r;        /* in root coordinates */
    int depth;              /* of its visual */
} WalpeDeskWin;

static inline const char *walpe_x11_desk_mode_name (WalpeX11DeskMode m)
{
    return m == WALPE_X11_DESK_ROOT ? "root" : m == WALPE_X11_DESK_WINDOW ? "window" : "auto";
}

/* 1 = s names a mode (*out), 0 = not set or empty (*out = auto), -1 = a
 * word that is none (*out = auto) */
static inline int walpe_x11_desk_parse (const char *s, WalpeX11DeskMode *out)
{
    *out = WALPE_X11_DESK_AUTO;
    if (!s || !*s)
        return 0;
    if (!g_ascii_strcasecmp (s, "auto"))
        return 1;
    if (!g_ascii_strcasecmp (s, "root")) {
        *out = WALPE_X11_DESK_ROOT;
        return 1;
    }
    if (!g_ascii_strcasecmp (s, "window")) {
        *out = WALPE_X11_DESK_WINDOW;
        return 1;
    }
    return -1;
}

/* does the desktop window lie over the whole monitor? (A desktop-type
 * window that is smaller - a desklet, a system monitor that asks for the
 * type to be kept below everything - is no file manager's desktop.) */
static inline gboolean walpe_x11_desk_covers (const WalpeDeskWin *w, const WalpeDeskRect *mon)
{
    return mon->width > 0 && mon->height > 0 && w->r.x <= mon->x && w->r.y <= mon->y &&
           (gint64) w->r.x + w->r.width >= (gint64) mon->x + mon->width &&
           (gint64) w->r.y + w->r.height >= (gint64) mon->y + mon->height;
}

/* is what lies below the desktop window seen through it? */
static inline gboolean walpe_x11_desk_translucent (const WalpeDeskWin *w, gboolean compositor)
{
    return compositor && w->depth == 32;
}

/* The desktop window the wallpaper window of monitor `mon` is made a
 * child of: its index in wins, or -1 = none, the wallpaper window is the
 * lowest child of the root. Of several that qualify, the first (the list
 * is in the order the windows were mapped: the one that was there first
 * stays the choice when another one appears). */
static inline int walpe_x11_desk_host (WalpeX11DeskMode mode, const WalpeDeskRect *mon,
                                       const WalpeDeskWin *wins, int n, gboolean compositor)
{
    if (mode == WALPE_X11_DESK_ROOT)
        return -1;
    for (int i = 0; i < n; i++) {
        if (!wins[i].client || !walpe_x11_desk_covers (&wins[i], mon))
            continue;
        if (mode == WALPE_X11_DESK_AUTO && walpe_x11_desk_translucent (&wins[i], compositor))
            continue;
        return i;
    }
    return -1;
}

/* Is the wallpaper of `mon` hidden where it is? Only when it is a child
 * of the root and an opaque desktop window lies over the monitor
 * (WALPE_X11_DESKTOP=root on a desktop with a file manager's desktop):
 * the index of that window, or -1. For the one line in the log. */
static inline int walpe_x11_desk_hides (const WalpeDeskRect *mon, const WalpeDeskWin *wins, int n,
                                        gboolean compositor)
{
    for (int i = 0; i < n; i++)
        if (wins[i].client && walpe_x11_desk_covers (&wins[i], mon) &&
            !walpe_x11_desk_translucent (&wins[i], compositor))
            return i;
    return -1;
}

/* is the child of the root `top` a desktop window (or the frame around
 * one)? The pointer over it is over the desktop. */
static inline gboolean walpe_x11_desk_is_top (unsigned long top, const WalpeDeskWin *wins, int n)
{
    for (int i = 0; top && i < n; i++)
        if (wins[i].top == top || wins[i].client == top)
            return TRUE;
    return FALSE;
}
