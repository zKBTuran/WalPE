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
    WALPE_X11_DESK_AUTO,    /* into an opaque desktop window, else the root; the root
                             * under a window manager that does not know the desktop
                             * type (walpe_x11_desk_allowed) */
    WALPE_X11_DESK_ROOT,    /* always the lowest child of the root (as up to 0.14.0) */
    WALPE_X11_DESK_WINDOW,  /* into a desktop window, also a translucent one */
} WalpeX11DeskMode;

typedef struct { int x, y, width, height; } WalpeDeskRect;

/* WALPE_X11_WINDOW: what kind of window the wallpaper window is.
 *
 * Override-redirect is the kind no window manager touches: none tiles it,
 * frames it, focuses it or lists it, and it stays where the daemon puts
 * it - at the bottom of the root's children. That is right wherever
 * windows are shown in the order the X server has them: without a
 * compositing manager, and under xfwm4's, marco's and picom's.
 * Mutter and its forks (Muffin = Cinnamon, GNOME Shell on X11, Budgie,
 * Gala) do not: an override-redirect window that is no menu, tooltip or
 * the like goes into their group of windows that are painted above
 * everything else - every window and the panel. KWin does the same with
 * every window it does not manage. Lowest in the X server's order,
 * topmost on screen: the wallpaper covers the whole desktop. There the
 * wallpaper window is one the window manager manages, of the type
 * _NET_WM_WINDOW_TYPE_DESKTOP: both keep that type in their lowest
 * layer, below every window and - lowered - below the file manager's
 * translucent icons window. It takes no focus and no input, and is on
 * every workspace.
 *
 * KWin with Plasma's desktop is one case more. plasmashell's desktop
 * window has a 32-bit visual and paints every pixel of it (Plasma's
 * wallpaper, icons and widgets are that window): a wallpaper window below
 * it is not seen, and one beside it in KWin's desktop layer is put behind
 * it at the first click on the desktop. Under KWin a desktop window is
 * taken for opaque whatever its visual, and the wallpaper window goes into
 * it as into xfdesktop's. */
typedef enum {
    WALPE_X11_WIN_AUTO,     /* managed under Mutter, its forks and KWin, else override-redirect */
    WALPE_X11_WIN_OVERRIDE, /* always override-redirect (as up to 0.14.0) */
    WALPE_X11_WIN_MANAGED,  /* always a managed window of the desktop type */
} WalpeX11WinMode;

static inline const char *walpe_x11_win_mode_name (WalpeX11WinMode m)
{
    return m == WALPE_X11_WIN_OVERRIDE ? "override" : m == WALPE_X11_WIN_MANAGED ? "managed" : "auto";
}

/* 1 = s names a mode (*out), 0 = not set or empty (*out = auto), -1 = a
 * word that is none (*out = auto) */
static inline int walpe_x11_win_parse (const char *s, WalpeX11WinMode *out)
{
    *out = WALPE_X11_WIN_AUTO;
    if (!s || !*s)
        return 0;
    if (!g_ascii_strcasecmp (s, "auto"))
        return 1;
    if (!g_ascii_strcasecmp (s, "override")) {
        *out = WALPE_X11_WIN_OVERRIDE;
        return 1;
    }
    if (!g_ascii_strcasecmp (s, "managed")) {
        *out = WALPE_X11_WIN_MANAGED;
        return 1;
    }
    return -1;
}

/* is this window manager (_NET_WM_NAME of its _NET_SUPPORTING_WM_CHECK
 * window; NULL = none runs or it says none) KWin? */
static inline gboolean walpe_x11_wm_is_kwin (const char *wm_name)
{
    return wm_name && !g_ascii_strncasecmp (wm_name, "kwin", 4);
}

/* Does the window manager of this name paint override-redirect windows
 * above everything? "Mutter (Muffin)", "GNOME Shell", "Mutter",
 * "Mutter(Budgie)", "Mutter(Gala)"; "KWin". */
static inline gboolean walpe_x11_wm_override_on_top (const char *wm_name)
{
    if (!wm_name || !*wm_name)
        return FALSE;
    char *low = g_ascii_strdown (wm_name, -1);
    gboolean hit = strstr (low, "mutter") != NULL || strstr (low, "gnome shell") != NULL ||
                   strstr (low, "muffin") != NULL;
    g_free (low);
    return hit || walpe_x11_wm_is_kwin (wm_name);
}

/* is the wallpaper window a managed one under this window manager? */
static inline gboolean walpe_x11_win_managed (WalpeX11WinMode mode, const char *wm_name)
{
    return mode == WALPE_X11_WIN_MANAGED ||
           (mode == WALPE_X11_WIN_AUTO && walpe_x11_wm_override_on_top (wm_name));
}

/* Is what lies below a desktop window with a 32-bit visual seen through
 * it under this window manager and a compositing manager? Not under KWin
 * (Plasma's desktop window, above); elsewhere it is (nemo-desktop). The
 * `compositor` the rules below are given is this answer. */
static inline gboolean walpe_x11_desk_see_through (const char *wm_name, gboolean compositor)
{
    return compositor && !walpe_x11_wm_is_kwin (wm_name);
}

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

/* Does the window manager treat a window of the desktop type as a
 * desktop - below every other window, no frame, no place in its layout?
 * It says so itself: _NET_WM_WINDOW_TYPE_DESKTOP is among the atoms of
 * _NET_SUPPORTED on the root. xfwm4, openbox, marco, Muffin, Mutter, KWin,
 * IceWM, Fluxbox, bspwm and awesome list it. dwm and i3 do not, and to
 * them such a window is a client like any other: tiled, focused, raised,
 * made fullscreen when it asks to be. Without a window manager there is
 * no such property at all.
 * `supported`: the atoms of the property (NULL / 0: none), `a_desktop`:
 * the atom of that type. */
static inline gboolean walpe_x11_wm_knows_desktop (const unsigned long *supported, unsigned long n,
                                                   unsigned long a_desktop)
{
    for (unsigned long i = 0; supported && a_desktop && i < n; i++)
        if (supported[i] == a_desktop)
            return TRUE;
    return FALSE;
}

/* May a wallpaper window go into a desktop window at all, and does such a
 * window count as the desktop (no fullscreen application whatever state
 * it carries)? `auto`: only where the window manager treats the type as a
 * desktop (above). Under dwm, i3 or no window manager a window of that
 * type that lies over the whole monitor is some program's fullscreen
 * window - a second wallpaper program, a visualizer, a file manager's
 * desktop that the window manager shows as one more client - and the
 * wallpaper window drawn into it would cover what that program shows;
 * there it stays the lowest window of the root, as up to 0.14.0.
 * `window` is the user's word for "into it, whatever the window manager
 * says"; `root` never. */
static inline gboolean walpe_x11_desk_allowed (WalpeX11DeskMode mode, gboolean wm_knows_desktop)
{
    return mode == WALPE_X11_DESK_WINDOW || (mode == WALPE_X11_DESK_AUTO && wm_knows_desktop);
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
