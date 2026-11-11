/* vistrack.h - "Skip hidden areas": which pixels of each monitor's
 * wallpaper window other windows leave visible.
 *
 * Event driven, on an X connection of its own (nothing the daemon's own
 * event handling selects or deselects can disturb it, and the other way
 * round): the tracker keeps the list of the root window's children - their
 * geometry, stacking order, map state, bounding shape and whether the
 * wallpaper can be seen through them - up to date from SubstructureNotify,
 * ShapeNotify and PropertyNotify events, and works the visible region of
 * every monitor out of that list with rectangle arithmetic
 * (common/region.h). The X server is asked about a window only when it is
 * first mapped or one of its properties changes; moving, resizing and
 * restacking windows costs no request at all.
 *
 * What hides the wallpaper:
 *  - no compositing manager running (the _NET_WM_CM_Sn selection has no
 *    owner): every mapped window above the wallpaper window, exactly as
 *    the X server clips it - its bounding shape, borders included. This
 *    is not an estimate: those pixels of the wallpaper window do not
 *    exist on screen. Translucency needs a compositor, so ARGB visuals
 *    and _NET_WM_WINDOW_OPACITY mean nothing here.
 *  - a compositing manager running: the manager decides what is seen
 *    through a window, and X does not say what it decided (picom's
 *    opacity-rule, inactive-opacity, fading, rounded corners set no
 *    property). WALPE_SKIP_AUTO therefore treats everything as visible.
 *    WALPE_SKIP_ALWAYS trusts what X does say: a window hides the
 *    wallpaper when its visual has no alpha channel (depth 32 = ARGB),
 *    no _NET_WM_WINDOW_OPACITY below 100 % is set on it or its client
 *    window and its class is not in the "see-through windows" list; an
 *    ARGB window hides what it declares in _NET_WM_OPAQUE_REGION. A
 *    window that was just mapped or moved hides nothing for a moment
 *    (the manager may still be fading or animating it). */
#pragma once
#include <glib.h>
#include "common/region.h"
#include "common/config.h"   /* WalpeSkipHidden */

typedef struct {
    int x, y, width, height;    /* the monitor in root coordinates */
    unsigned long xid;          /* its wallpaper window (an X window id) */
} WalpeVisMon;

typedef struct WalpeVisTrack WalpeVisTrack;

/* main loop: the visible region of monitor `mon` (index into the array
 * given to walpe_vistrack_new) changed; `visible` is in the monitor's own
 * coordinates (0,0 = its top-left) and only valid during the call */
typedef void (*WalpeVisChanged) (int mon, const WalpeRegion *visible, gpointer user);

/* Opens the connection, reads the window tree once and reports every
 * monitor's region (the callback runs before this returns). NULL when the
 * display cannot be opened. The monitors are fixed for its lifetime. */
WalpeVisTrack *walpe_vistrack_new (const WalpeVisMon *mons, int n, WalpeSkipHidden mode,
                                   WalpeVisChanged cb, gpointer user);
void walpe_vistrack_free (WalpeVisTrack *t);
void walpe_vistrack_set_mode (WalpeVisTrack *t, WalpeSkipHidden mode);
/* comma-separated WM_CLASS names that never hide the wallpaper (config
 * transparent_windows); NULL / "" = none */
void walpe_vistrack_set_see_through (WalpeVisTrack *t, const char *classes);
/* under a compositing manager: pixels taken off every side of a hiding
 * window (rounded corners), and how long a window must have been still
 * before it hides anything */
void walpe_vistrack_set_tuning (WalpeVisTrack *t, int inset_px, int settle_ms);
/* a compositing manager owns _NET_WM_CM_Sn right now */
gboolean walpe_vistrack_compositor (WalpeVisTrack *t);
/* the regions are being worked out (FALSE: WALPE_SKIP_AUTO under a
 * compositing manager - everything counts as visible) */
gboolean walpe_vistrack_clipping (WalpeVisTrack *t);

typedef struct {
    guint64 events;         /* X events handled */
    guint64 recomputes;     /* times the regions were worked out */
    guint64 changes;        /* times a monitor's region came out different */
    guint64 roundtrips;     /* requests that waited for the X server's answer */
    gint64 cpu_us;          /* CPU time spent in the tracker (events + regions) */
    guint windows;          /* children of the root it knows */
} WalpeVisStats;
void walpe_vistrack_stats (WalpeVisTrack *t, WalpeVisStats *st);
