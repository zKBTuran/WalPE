/* pace.h - frame pacing rules shared by the daemon and its tests (version 0.12).
 * Pure functions, no state: which monitor leads a shared rendering, what
 * rate a monitor's content is paced at, a display mode's refresh rate. */
#pragma once
#include <glib.h>
#include <math.h>

/* refresh rate of a display mode (RandR XRRModeInfo fields): dot clock /
 * (htotal * vtotal), doubled for interlaced modes (fields per second),
 * halved for doublescan. 0 when the mode has no timings (Xvfb, some
 * virtual outputs). */
static inline double walpe_mode_refresh (unsigned long dot_clock, unsigned htotal,
                                         unsigned vtotal, gboolean interlace,
                                         gboolean doublescan)
{
    if (!dot_clock || !htotal || !vtotal)
        return 0.0;
    double v = (double) vtotal;
    if (doublescan)
        v *= 2.0;
    if (interlace)
        v /= 2.0;
    return (double) dot_clock / ((double) htotal * v);
}

/* the member of a render_shared group that renders the scene: the monitor
 * with the highest refresh rate, so the fastest monitor gets a new frame
 * every refresh and the slower ones show the latest frame at theirs. With
 * the first monitor leading (versions 0.10 and 0.11), a 60 Hz first monitor held a
 * 144 Hz second monitor to 60 new frames a second. hz[i] = refresh of
 * member i (0 = unknown, counts as lowest); ties and all-unknown keep the
 * first member (the old rule). n >= 1. */
static inline int walpe_pick_leader (const double *hz, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (hz[i] > hz[best] + 0.5)
            best = i;
    return best;
}

/* frames per second a monitor's content is paced at: the user's cap
 * (config fps), but no more than the monitor shows. hz = the monitor's
 * refresh (0 = unknown: the cap alone). Rounded up, so the frame clock is
 * never slower than the display (a swap that blocks on vsync then sets the
 * pace; where it does not block - a compositor, Xvfb - the clock does). */
static inline int walpe_paced_fps (int cap, double hz)
{
    if (cap < 1)
        cap = 1;
    if (hz < 20.0)          /* unknown or implausible: do not trust it */
        return cap;
    int r = (int) ceil (hz - 0.005);
    return MIN (cap, r);
}

/* frame_align: the frame clocks of all monitors on one grid.
 * Every scene has its own frame clock, started when it was loaded, so two
 * monitors at 60 fps present at two unrelated phases and a compositing
 * manager is handed up to 120 damage events a second, each of which makes
 * it (and the X server) compose and present once more. With the grid the
 * monitors wake at the same instants - multiples of 1 / fps on the monotonic
 * clock - and their presents arrive together; frames, frame count and
 * pictures are the same, only the phase moves (by less than one frame).
 * now_us: the time now; last_due_us: the previous deadline (0 = none);
 * interval_us: 1e6 / fps. Returns the next deadline: the grid point nearest
 * to one interval after the previous deadline; a clock that is behind
 * (deadline already passed) draws now, as the free-running clock does, and
 * falls back onto the grid with its next frame. Grid points are
 * k * 1e6 / fps with fps = round (1e6 / interval), so clocks at 60 and
 * 30 fps share every second point exactly. */
static inline gint64 walpe_frame_grid_next (gint64 now_us, gint64 last_due_us, gint64 interval_us)
{
    if (interval_us <= 0)
        return now_us;
    gint64 next = last_due_us ? last_due_us + interval_us : now_us + interval_us;
    if (next < now_us)
        return now_us;          /* behind: render now, no burst to catch up */
    gint64 fps = (1000000 + interval_us / 2) / interval_us;
    if (fps < 1)
        fps = 1;
    /* nearest grid index to `next` (rounded), as a time */
    gint64 k = (next * fps + 500000) / 1000000;
    gint64 g = k * 1000000 / fps;
    if (g < now_us)             /* the nearest point just passed: the one after it */
        g = (k + 1) * 1000000 / fps;
    return g;
}

/* ---- rules of the two-monitor pacing, shared with tests/pixfps_test.c

 * A monitor that shows another monitor's frames (render_shared) takes the
 * newest frame once per refresh of its own, because its leader may publish
 * more frames than it can show (limit 144, a 144 Hz leader, a 60 Hz
 * follower). TRUE = that cannot happen: the leader's frame clock - the fps
 * setting, at most the leader's refresh rate - is not faster than the
 * follower's display, and every frame is presented when it arrives. (With
 * the wait, a follower whose leader's frames came a little before "its
 * slot" woke twice per frame and showed each frame up to one refresh late.)
 * follower_hz unknown (< 20): FALSE, the caller has no slot to wait for. */
static inline gboolean walpe_follow_free (int fps, double leader_hz, double follower_hz)
{
    if (follower_hz < 20.0)
        return FALSE;
    return walpe_paced_fps (fps, leader_hz) <= (int) ceil (follower_hz - 0.005);
}

/* The frame rate limit a follower applies on its own (0 = none): its
 * limit `own` when that is below what its leader renders at (`lead`, the
 * highest limit of the group's visible monitors). Only the focus rule
 * gives two monitors of one group different limits: an application has
 * the focus on the follower's monitor. The follower then presents the
 * newest frame `own` times a second instead of every frame. */
static inline int walpe_follow_cap (int own, int lead)
{
    return own > 0 && lead > 0 && own < lead ? own : 0;
}

/* The pointer position a scene gets (-1..1 across the monitor, clamped):
 * root position (rx, ry) against the monitor at gx, gy of gw x gh. With
 * pointer_share the daemon passes the geometry of the monitor of the
 * scene's group that the pointer is on, so the shared picture is the one
 * that monitor's own rendering would show. */
static inline void walpe_pointer_norm (int rx, int ry, int gx, int gy, int gw, int gh,
                                       float *nx, float *ny)
{
    float x = ((float) (rx - gx) / (float) MAX (1, gw)) * 2 - 1;
    float y = ((float) (ry - gy) / (float) MAX (1, gh)) * 2 - 1;
    *nx = CLAMP (x, -1.0f, 1.0f);
    *ny = CLAMP (y, -1.0f, 1.0f);
}

/* Milliseconds between two pointer queries while the pointer moves.
 * paced = the frame rate of the fps setting on the fastest monitor; need =
 * the highest frame rate limit among the wallpapers that use the pointer
 * (0 = unknown, or a web page: the full rate). A scene held at 15 frames
 * a second by a focus / battery rule or fps_idle is asked 15 times a
 * second, not 60; without such a limit it is the frame rate, 30..144. */
static inline guint walpe_pointer_poll_ms (int paced, int need)
{
    if (need >= 5 && need < paced)
        return (guint) (1000 / need);
    return (guint) (1000 / CLAMP (paced, 30, 144));
}

/* "<name>=WxH+X+Y@HZ" suffix of a WALPE_MONITORS entry: the refresh rate
 * after '@' (0 when absent or not a number) */
static inline double walpe_monitor_spec_hz (const char *spec)
{
    const char *at = spec ? strrchr (spec, '@') : NULL;
    double hz = at ? g_ascii_strtod (at + 1, NULL) : 0.0;
    return hz > 0.0 && hz < 1000.0 ? hz : 0.0;
}
