/* fsplace.h - which monitors a fullscreen window is on.
 *
 * "Pause for fullscreen apps" pauses the monitor the focused fullscreen
 * window is on and leaves the others running. On X11 the window system
 * tells a client every window's rectangle and no monitor: the monitor is
 * worked out here, from the window's rectangle and the monitors'
 * rectangles, all in root coordinates. Pure functions: platform/x11.c
 * feeds them what the server says, tests/fsplace_test.c made-up layouts.
 *
 * The rule:
 *  - the monitor that holds most of the window counts, however little of
 *    that monitor the window fills: a tiling window manager may keep a
 *    border or a gap around a fullscreen window, or leave it in its tile
 *    ("fake fullscreen"). Two monitors holding the same amount: the one
 *    the window's centre is on, then the first of them;
 *  - any other monitor counts when the window covers at least half of it:
 *    a window that spans monitors (a game across two screens, a window
 *    given several monitors with _NET_WM_FULLSCREEN_MONITORS, which the
 *    window manager turns into such a rectangle) is on each of them, a
 *    border or shadow that reaches a few pixels into the neighbour is not;
 *  - a window that lies on no monitor at all is on none. */
#pragma once
#include <glib.h>

typedef struct { int x, y, width, height; } WalpeFsRect;

/* the part of a monitor a window must cover to count as on it when most of
 * the window is elsewhere: 1 / WALPE_FS_SPAN_DIV of the monitor's area */
#define WALPE_FS_SPAN_DIV 2

/* pixels two rectangles share (0 = none, or one of them is empty) */
static inline gint64 walpe_fs_overlap (const WalpeFsRect *a, const WalpeFsRect *b)
{
    if (a->width <= 0 || a->height <= 0 || b->width <= 0 || b->height <= 0)
        return 0;
    gint64 x1 = MAX ((gint64) a->x, (gint64) b->x), y1 = MAX ((gint64) a->y, (gint64) b->y);
    gint64 x2 = MIN ((gint64) a->x + a->width, (gint64) b->x + b->width);
    gint64 y2 = MIN ((gint64) a->y + a->height, (gint64) b->y + b->height);
    return x2 > x1 && y2 > y1 ? (x2 - x1) * (y2 - y1) : 0;
}

/* The monitors `win` is on: on[i] for each of the n monitors (the rule is
 * at the top of this file). Returns how many; 0 = on none of them. */
static inline int walpe_fs_place (const WalpeFsRect *win, const WalpeFsRect *mons, int n,
                                  gboolean *on)
{
    int best = -1, count = 0;
    gint64 best_px = 0;
    gboolean best_centre = FALSE;
    /* (halves kept: the centre of a 1 px window is its pixel) */
    gint64 cx2 = 2 * (gint64) win->x + win->width, cy2 = 2 * (gint64) win->y + win->height;
    for (int i = 0; i < n; i++) {
        const WalpeFsRect *m = &mons[i];
        on[i] = FALSE;
        gint64 px = walpe_fs_overlap (win, m);
        if (px <= 0)
            continue;
        gboolean centre = cx2 >= 2 * (gint64) m->x && cx2 < 2 * ((gint64) m->x + m->width) &&
                          cy2 >= 2 * (gint64) m->y && cy2 < 2 * ((gint64) m->y + m->height);
        if (px > best_px || (px == best_px && centre && !best_centre)) {
            best = i;
            best_px = px;
            best_centre = centre;
        }
        /* it covers a good part of this monitor: on it, wherever the rest is */
        if (px * WALPE_FS_SPAN_DIV >= (gint64) m->width * m->height) {
            on[i] = TRUE;
            count++;
        }
    }
    if (best >= 0 && !on[best]) {
        on[best] = TRUE;
        count++;
    }
    return count;
}

/* What the fullscreen rule says for each monitor.
 *   focus_fullscreen  the window that has the focus is fullscreen (a
 *                     fullscreen window without the focus pauses nothing,
 *                     on whichever monitor it sits)
 *   win               that window's rectangle, NULL = it could not be
 *                     asked for
 * on[i] = monitor i has that window on it. *everywhere = TRUE: such a
 * window exists and where it is is not known (no rectangle, or an empty
 * one); it then counts for every monitor, as the rule did before it
 * looked at monitors, and on[] is all FALSE. */
static inline void walpe_fs_decide (gboolean focus_fullscreen, const WalpeFsRect *win,
                                    const WalpeFsRect *mons, int n, gboolean *on,
                                    gboolean *everywhere)
{
    *everywhere = FALSE;
    for (int i = 0; i < n; i++)
        on[i] = FALSE;
    if (!focus_fullscreen)
        return;
    if (!win || win->width <= 0 || win->height <= 0) {
        *everywhere = TRUE;
        return;
    }
    walpe_fs_place (win, mons, n, on);
}
