/* wayland_logic.h - the rules of the Wayland backend that need no
 * compositor: how an output's properties become the rectangle the daemon
 * renders, which buffer size and scale a surface gets, where a pointer
 * position on a surface lies in that rectangle, when a surface counts as
 * "not shown", which layer a wallpaper surface goes on, and the life cycle
 * of a layer surface. Pure functions and small state records, no
 * libwayland types, no I/O: platform/wayland.c feeds them with what the
 * protocol delivers and does what they answer;
 * tests/wayland_logic_test.c feeds them with made-up values (like pace.h
 * and present.h and their tests). */
#pragma once
#include <glib.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ scale
 * wp_fractional_scale_v1 expresses a scale as a numerator over 120
 * (1.25 = 150, 1.5 = 180, 1.75 = 210). */
#define WL_SCALE_DEN 120

/* a size in layout units times a scale n/120, rounded half away from zero
 * (the rounding the fractional-scale protocol prescribes for buffer sizes) */
static inline int wl_scale120_apply (int logical, int scale120)
{
    if (logical <= 0 || scale120 <= 0)
        return 0;
    return (int) (((gint64) logical * scale120 + WL_SCALE_DEN / 2) / WL_SCALE_DEN);
}

/* the scale n/120 nearest to pixels / logical; 0 = cannot tell. A
 * compositor rounds the logical size of a fractionally scaled output to a
 * whole number (1366 px at 1.25 are 1092.8 -> 1093 units), so the plain
 * quotient is a little off; on the 1/120 grid it is exact again. */
static inline int wl_scale120_from_sizes (int pixels, int logical)
{
    if (pixels <= 0 || logical <= 0)
        return 0;
    return (int) (((gint64) pixels * WL_SCALE_DEN * 2 + logical) / (2 * (gint64) logical));
}

/* wl_output.transform: the values with bit 0 set (90, 270 and their
 * flipped forms) show the mode with width and height exchanged */
static inline gboolean wl_transform_swaps (int transform)
{
    return (transform & 1) != 0;
}

/* the eight values the protocol knows; anything else reads as "not turned" */
static inline int wl_transform_sane (int transform)
{
    return transform >= 0 && transform <= 7 ? transform : 0;
}

/* a mode's refresh rate in mHz as Hz; 0 = unknown, which is also the
 * answer to a number that is none (negative, or above 1000 Hz) */
static inline double wl_refresh_hz_from_mhz (int mhz)
{
    return mhz > 0 && mhz <= 1000000 ? mhz / 1000.0 : 0.0;
}

/* ----------------------------------------------------------------- limits
 * Sizes and positions arrive from the compositor as 32-bit numbers of any
 * value. Nothing real lies beyond these bounds; a number beyond them (a
 * compositor that is broken, or not a friend) is cut to the bound before
 * anything is computed from it, so that no product, sum or conversion
 * below can leave the range of an int. The rectangle the daemon is told
 * lies within the same bounds (they are the ones the platform layer keeps
 * an output list to, whichever backend filled it in). */
#define WL_DIM_MAX 65535            /* width / height: a mode, a logical size, the pixels */
#define WL_POS_MAX (1 << 24)        /* a position, layout units or pixels, either sign */

static inline int wl_dim (int v)
{
    return CLAMP (v, 0, WL_DIM_MAX);
}

static inline int wl_pos (int v)
{
    return CLAMP (v, -WL_POS_MAX, WL_POS_MAX);
}

/* ------------------------------------------------------------------- text
 * An output's name, description, make and model arrive from the compositor
 * as bytes of any kind and any length (libwayland checks neither). The
 * daemon uses them as keys of its configuration, puts them into the status
 * JSON and into log lines, so they are made harmless first: valid UTF-8,
 * no control characters and no line separators (a "?" stands for each
 * byte or character taken out), at most max_bytes bytes, never cut inside
 * a character. NULL = nothing usable was told (g_free the result). */
#define WL_NAME_MAX 64
#define WL_TEXT_MAX 256

static inline char *wl_text_clean (const char *s, gsize max_bytes)
{
    if (!s)
        return NULL;
    GString *out = g_string_new (NULL);
    while (*s) {
        gunichar c = g_utf8_get_char_validated (s, -1);
        gboolean valid = c != (gunichar) -1 && c != (gunichar) -2;
        /* an invalid byte is taken out alone; the next one is looked at afresh */
        gsize n = valid ? (gsize) (g_utf8_next_char (s) - s) : 1;
        gboolean keep = valid && !g_unichar_iscntrl (c) && c != 0x2028 && c != 0x2029;
        if (out->len + (keep ? n : 1) > max_bytes)
            break;
        if (keep)
            g_string_append_len (out, s, (gssize) n);
        else
            g_string_append_c (out, '?');
        s += n;
    }
    if (!out->len) {
        g_string_free (out, TRUE);
        return NULL;
    }
    return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------------- outputs
 * What the protocol says about one output (wl_output, and zxdg_output_v1
 * when the compositor has it). */
typedef struct {
    guint32 id;                 /* wl_registry name of the wl_output */
    const char *name;           /* connector name; NULL = not told */
    int geo_x, geo_y;           /* wl_output.geometry: position in the compositor's layout */
    int mode_w, mode_h;         /* current mode as the panel has it (before the
                                 * transform); 0 = no mode told */
    int refresh_mhz;            /* of that mode; 0 = unknown */
    int int_scale;              /* wl_output.scale; values below 1 are read as 1 */
    int transform;              /* wl_output.transform (0..7) */
    gboolean have_xdg_pos, have_xdg_size;
    int xdg_x, xdg_y;           /* zxdg_output_v1.logical_position */
    int xdg_w, xdg_h;           /* zxdg_output_v1.logical_size */
} WlOutProps;

/* what the daemon is told about it (platform.h: WalpeOutput) */
typedef struct {
    const WlOutProps *src;
    int x, y, width, height;    /* pixels; rectangles of one layout never overlap */
    int lx, ly, lw, lh;         /* the compositor's layout units */
    int scale120;               /* pixels per layout unit, n/120 */
    double refresh_hz;
} WlOutRect;

static inline int wl_out_int_scale (const WlOutProps *p)
{
    return CLAMP (p->int_scale, 1, 16);
}

/* the mode as it lies in the layout: after the transform */
static inline void wl_out_mode_transformed (const WlOutProps *p, int *w, int *h)
{
    gboolean sw = wl_transform_swaps (wl_transform_sane (p->transform));
    *w = wl_dim (sw ? p->mode_h : p->mode_w);
    *h = wl_dim (sw ? p->mode_w : p->mode_h);
}

/* position and size in layout units: zxdg_output_v1's when told, else the
 * geometry position and the transformed mode divided by the integer scale */
static inline void wl_out_logical (const WlOutProps *p, int *lx, int *ly, int *lw, int *lh)
{
    int mw, mh, sc = wl_out_int_scale (p);
    wl_out_mode_transformed (p, &mw, &mh);
    *lx = wl_pos (p->have_xdg_pos ? p->xdg_x : p->geo_x);
    *ly = wl_pos (p->have_xdg_pos ? p->xdg_y : p->geo_y);
    if (p->have_xdg_size && p->xdg_w > 0 && p->xdg_h > 0) {
        *lw = wl_dim (p->xdg_w);
        *lh = wl_dim (p->xdg_h);
    } else {
        /* a mode smaller than half the scale still is one unit, not none */
        *lw = mw > 0 ? MAX ((mw + sc / 2) / sc, 1) : 0;
        *lh = mh > 0 ? MAX ((mh + sc / 2) / sc, 1) : 0;
    }
}

/* The size in pixels the daemon renders for the output. With wp_viewporter
 * a buffer of any size can be shown on a surface of any size, so it is the
 * panel's own mode (after the transform): one buffer pixel per panel pixel
 * whatever the scale, fractional scales included. Without it a buffer can
 * only be an integer multiple of the surface size: logical size times
 * wl_output.scale. An output that tells no mode gets the second rule too. */
static inline void wl_out_pixels (const WlOutProps *p, gboolean viewporter, int *w, int *h)
{
    int mw, mh, lx, ly, lw, lh, sc = wl_out_int_scale (p);
    wl_out_mode_transformed (p, &mw, &mh);
    wl_out_logical (p, &lx, &ly, &lw, &lh);
    if (viewporter && mw > 0 && mh > 0) {
        *w = mw;
        *h = mh;
    } else {
        *w = MIN (lw * sc, WL_DIM_MAX);
        *h = MIN (lh * sc, WL_DIM_MAX);
    }
}

/* is enough known about the output to list it? A mode or a logical size */
static inline gboolean wl_out_usable (const WlOutProps *p)
{
    return (p->mode_w > 0 && p->mode_h > 0) || (p->have_xdg_size && p->xdg_w > 0 && p->xdg_h > 0);
}

/* list order: by logical position (x, then y), then by name, then by id -
 * never by the order in which the outputs were plugged in */
static inline int wl_out_cmp (const WlOutRect *a, const WlOutRect *b)
{
    if (a->lx != b->lx)
        return a->lx < b->lx ? -1 : 1;
    if (a->ly != b->ly)
        return a->ly < b->ly ? -1 : 1;
    int c = g_strcmp0 (a->src->name, b->src->name);
    if (c)
        return c;
    return a->src->id < b->src->id ? -1 : a->src->id > b->src->id;
}

static inline gboolean wl_rects_overlap (const WlOutRect *a, const WlOutRect *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width && a->y < b->y + b->height &&
           b->y < a->y + a->height;
}

/* The layout the daemon sees: out[0..n-1] in list order, each with its
 * pixel rectangle. A rectangle's origin is the logical position times the
 * output's own scale, rounded; where outputs of different scales would
 * then overlap (a scale-2 panel left of a scale-1 panel: 0..3840 and
 * 1920..), all of them are put in one row instead, left to right in list
 * order with y = 0. Depends on output properties only, never on a
 * surface, so two reads of an unchanged layout are equal. */
static inline void wl_out_layout (const WlOutProps *props, int n, gboolean viewporter, WlOutRect *out)
{
    for (int i = 0; i < n; i++) {
        WlOutRect r;
        memset (&r, 0, sizeof r);
        r.src = &props[i];
        wl_out_logical (&props[i], &r.lx, &r.ly, &r.lw, &r.lh);
        wl_out_pixels (&props[i], viewporter, &r.width, &r.height);
        r.scale120 = wl_scale120_from_sizes (r.width, r.lw);
        if (r.scale120 < WL_SCALE_DEN / 4 || r.scale120 > WL_SCALE_DEN * 16)
            r.scale120 = wl_out_int_scale (&props[i]) * WL_SCALE_DEN;
        r.refresh_hz = wl_refresh_hz_from_mhz (props[i].refresh_mhz);
        /* insertion sort: n is the number of monitors */
        int k = i;
        while (k > 0 && wl_out_cmp (&r, &out[k - 1]) < 0) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = r;
    }
    gboolean overlap = FALSE;
    for (int i = 0; i < n; i++) {
        out[i].x = wl_pos ((int) floor ((double) out[i].lx * out[i].scale120 / WL_SCALE_DEN + 0.5));
        out[i].y = wl_pos ((int) floor ((double) out[i].ly * out[i].scale120 / WL_SCALE_DEN + 0.5));
    }
    for (int i = 0; i < n && !overlap; i++)
        for (int k = i + 1; k < n && !overlap; k++)
            overlap = wl_rects_overlap (&out[i], &out[k]);
    if (overlap) {
        int x = 0;
        for (int i = 0; i < n; i++) {
            out[i].x = x;
            out[i].y = 0;
            x += out[i].width;
        }
    }
}

/* ---------------------------------------------------------- surface sizes
 * Three sizes belong to a surface and are kept apart:
 *   the buffer size      what the EGL window's buffers have (pixels the
 *                        daemon renders),
 *   the logical size     what the compositor gave the surface in its
 *                        layout units (the configure event),
 *   the buffer scale /   how the first is shown on the second: an integer
 *   viewport             wl_surface.set_buffer_scale, or with wp_viewporter
 *                        a destination size (any ratio, also a buffer
 *                        smaller than the output: a lower render
 *                        resolution scaled up by the compositor). */
#define WL_BUFFER_MAX 16384

typedef struct {
    int out_w, out_h;           /* the output's pixel size (wallpaper), 0 = none */
    int asked_w, asked_h;       /* window / probe: the size asked for */
    int user_w, user_h;         /* a buffer size set from outside (0 = none) */
    int conf_w, conf_h;         /* the last configure, layout units (0 = left to the client) */
    int int_scale;              /* wl_output.scale of its output */
    int scale120;               /* the surface's preferred fractional scale; 0 = not told */
    gboolean viewporter;
} WlSizeIn;

typedef struct {
    int buf_w, buf_h;           /* size of the EGL window */
    int buffer_scale;           /* wl_surface.set_buffer_scale */
    gboolean viewport;          /* wp_viewport.set_destination (dst_w, dst_h) */
    int dst_w, dst_h;           /* the logical size the buffer is shown at */
} WlSizeOut;

static inline void wl_surface_sizes (const WlSizeIn *in, WlSizeOut *out)
{
    int sc = CLAMP (in->int_scale, 1, 16);
    int s120 = in->scale120 > 0 ? in->scale120 : sc * WL_SCALE_DEN;
    int lw = in->conf_w, lh = in->conf_h;
    /* a configure of 0 leaves that dimension to the client: what the
     * output (or the size asked for) comes to in layout units */
    if (lw <= 0)
        lw = in->out_w > 0 ? (int) (((gint64) in->out_w * WL_SCALE_DEN + s120 / 2) / s120)
                           : in->asked_w;
    if (lh <= 0)
        lh = in->out_h > 0 ? (int) (((gint64) in->out_h * WL_SCALE_DEN + s120 / 2) / s120)
                           : in->asked_h;
    lw = CLAMP (lw, 1, WL_DIM_MAX);
    lh = CLAMP (lh, 1, WL_DIM_MAX);
    memset (out, 0, sizeof *out);
    out->dst_w = lw;
    out->dst_h = lh;
    if (in->viewporter) {
        out->viewport = TRUE;
        out->buffer_scale = 1;
        if (in->user_w > 0 && in->user_h > 0) {
            out->buf_w = in->user_w;
            out->buf_h = in->user_h;
        } else if (in->out_w > 0 && in->out_h > 0) {
            out->buf_w = in->out_w;
            out->buf_h = in->out_h;
        } else if (in->asked_w > 0 && in->asked_h > 0) {
            out->buf_w = in->asked_w;
            out->buf_h = in->asked_h;
        } else {
            out->buf_w = wl_scale120_apply (lw, s120);
            out->buf_h = wl_scale120_apply (lh, s120);
        }
    } else if (in->out_w > 0 && in->out_h > 0) {
        /* a wallpaper without viewporter: an integer multiple of the
         * logical size, which is what the protocol then requires */
        out->buffer_scale = sc;
        out->buf_w = lw * sc;
        out->buf_h = lh * sc;
    } else {
        /* a window or a probe without viewporter: the size asked for, one
         * buffer pixel per layout unit */
        out->buffer_scale = 1;
        out->buf_w = in->asked_w > 0 ? in->asked_w : lw;
        out->buf_h = in->asked_h > 0 ? in->asked_h : lh;
    }
    out->buf_w = CLAMP (out->buf_w, 1, WL_BUFFER_MAX);
    out->buf_h = CLAMP (out->buf_h, 1, WL_BUFFER_MAX);
}

/* ---------------------------------------------------------------- pointer
 * A position on a surface (layout units, fractions included) as a pixel of
 * the output's rectangle: the coordinates the daemon uses for the pointer
 * on both backends. logical_w / _h: the surface's logical size. */
static inline void wl_pointer_to_output (double sx, double sy, int logical_w, int logical_h,
                                         int out_x, int out_y, int out_w, int out_h,
                                         int *px, int *py)
{
    double fx = logical_w > 0 ? sx * out_w / logical_w : sx;
    double fy = logical_h > 0 ? sy * out_h / logical_h : sy;
    /* into the rectangle while still a double: a position far outside (a
     * drag, or a compositor's odd value on a tiny logical size) must not be
     * converted to an int before it is in range */
    double mx = MAX (out_w - 1, 0), my = MAX (out_h - 1, 0);
    *px = out_x + (int) floor (CLAMP (fx, 0.0, mx));
    *py = out_y + (int) floor (CLAMP (fy, 0.0, my));
}

/* wl_pointer.button codes (Linux input event codes) as the daemon's button
 * numbers: 1 left, 2 middle, 3 right; 0 = a button the daemon has no use for */
static inline int wl_button_number (guint32 code)
{
    switch (code) {
    case 0x110: return 1;       /* BTN_LEFT */
    case 0x111: return 3;       /* BTN_RIGHT */
    case 0x112: return 2;       /* BTN_MIDDLE */
    default:    return 0;
    }
}

/* Scrolling as wheel clicks. A wheel reports whole steps (value120: 120
 * per detent, or the older discrete count); a touchpad reports a distance
 * only, of which 15 units are taken as one click (what one wheel detent
 * is worth in the same unit). Remainders are carried per axis. */
#define WL_AXIS_CLICK_UNITS 15.0
/* no group of axis events is worth more clicks than this (bounds what is
 * added up from the compositor's numbers) */
#define WL_AXIS_STEPS_MAX 1000

typedef struct {
    double dist[2];             /* distance not yet turned into clicks */
    int v120[2];                /* value120 not yet turned into clicks */
} WlAxisAcc;

/* one group of axis events of one axis (0 vertical, 1 horizontal) ->
 * clicks, negative = up / left. stop: the fingers left the touchpad. */
static inline int wl_axis_clicks (WlAxisAcc *a, int axis, double value, int discrete, int value120,
                                  gboolean stop)
{
    int clicks = 0;
    if (axis < 0 || axis > 1)
        return 0;
    value = CLAMP (value, -WL_AXIS_CLICK_UNITS * WL_AXIS_STEPS_MAX,
                   WL_AXIS_CLICK_UNITS * WL_AXIS_STEPS_MAX);
    discrete = CLAMP (discrete, -WL_AXIS_STEPS_MAX, WL_AXIS_STEPS_MAX);
    value120 = CLAMP (value120, -120 * WL_AXIS_STEPS_MAX, 120 * WL_AXIS_STEPS_MAX);
    if (value120) {
        a->v120[axis] += value120;
        clicks = a->v120[axis] / 120;
        a->v120[axis] -= clicks * 120;
        a->dist[axis] = 0;
    } else if (discrete) {
        clicks = discrete;
        a->dist[axis] = 0;
    } else if (value != 0.0) {
        /* a change of direction starts from nothing */
        if ((a->dist[axis] > 0) != (value > 0))
            a->dist[axis] = 0;
        a->dist[axis] += value;
        clicks = (int) (a->dist[axis] / WL_AXIS_CLICK_UNITS);
        a->dist[axis] -= clicks * WL_AXIS_CLICK_UNITS;
    }
    if (stop)
        a->dist[axis] = 0;
    return clicks;
}

/* the daemon's wheel buttons: 4 up, 5 down, 6 left, 7 right */
static inline int wl_axis_button (int axis, int clicks)
{
    if (!clicks)
        return 0;
    return axis == 0 ? (clicks < 0 ? 4 : 5) : (clicks < 0 ? 6 : 7);
}

/* ------------------------------------------------- frames and visibility
 * A compositor sends a surface's frame callback when it has put the
 * surface on screen and it is a good time to draw the next frame; a
 * surface that is not shown (the output sleeps, a lock screen or a
 * fullscreen window is above, the workspace is elsewhere) gets none. So
 * one callback is kept outstanding per surface; a swap with "interval 1"
 * waits for it, but never for long, and a callback that stays away marks
 * the surface hidden until it arrives. Nothing here blocks or reads a
 * clock: the caller passes the time. */
typedef struct {
    gboolean cb_pending;        /* a frame callback is outstanding */
    gint64 cb_since_us;         /* since when */
    gboolean hidden;            /* it stayed away: the surface is not shown */
    guint64 commits;            /* swaps that went out with the surface ready */
    guint64 callbacks;          /* frame callbacks that arrived */
    guint64 timeouts;           /* waits that ended without one */
} WlFrameState;

/* how long a callback may stay away before the surface counts as hidden:
 * four refresh periods, at least 100 ms, at most 250 ms */
static inline gint64 wl_frame_limit_us (double refresh_hz)
{
    gint64 four = refresh_hz >= 1.0 ? (gint64) (4e6 / refresh_hz) : 0;
    return CLAMP (four, (gint64) 100000, (gint64) 250000);
}

/* before a swap: TRUE = request a frame callback with this commit (none is
 * outstanding) */
static inline gboolean wl_frame_pre_swap (WlFrameState *f, gint64 now_us)
{
    f->commits++;
    if (f->cb_pending)
        return FALSE;
    f->cb_pending = TRUE;
    f->cb_since_us = now_us;
    return TRUE;
}

/* A swap that was announced (wl_frame_pre_swap) did not take place - it
 * failed, and only swaps that went through are reported. The callback
 * asked for with it has not been committed and waits for the next commit,
 * so how long it has been outstanding says nothing about the compositor:
 * its age starts again. */
static inline void wl_frame_swap_missed (WlFrameState *f, gint64 now_us)
{
    if (f->cb_pending)
        f->cb_since_us = now_us;
}

/* after a swap: how long to wait for the outstanding callback; 0 = not at
 * all (interval 0, nothing outstanding, or the surface is hidden: a hidden
 * surface must not hold its thread on every frame) */
static inline gint64 wl_frame_wait_us (const WlFrameState *f, int interval, double refresh_hz)
{
    if (interval < 1 || !f->cb_pending || f->hidden)
        return 0;
    return wl_frame_limit_us (refresh_hz);
}

/* the callback arrived. TRUE = the surface was hidden and is shown again */
static inline gboolean wl_frame_done (WlFrameState *f)
{
    gboolean was = f->hidden;
    f->cb_pending = FALSE;
    f->hidden = FALSE;
    f->callbacks++;
    return was;
}

/* after the wait (or, with interval 0, after looking for events without
 * waiting): TRUE = the surface has just become hidden. waited: this call
 * follows a wait of wl_frame_wait_us that ended without the callback. */
static inline gboolean wl_frame_after_swap (WlFrameState *f, gboolean waited, gint64 now_us,
                                            double refresh_hz)
{
    if (!f->cb_pending || f->hidden)
        return FALSE;
    if (waited)
        f->timeouts++;
    else if (now_us - f->cb_since_us <= wl_frame_limit_us (refresh_hz))
        return FALSE;
    f->hidden = TRUE;
    return TRUE;
}

/* the surface lost its role: nothing is outstanding any more */
static inline void wl_frame_reset (WlFrameState *f)
{
    f->cb_pending = FALSE;
    f->cb_since_us = 0;
}

/* wp_presentation_feedback.presented: the time stamp in microseconds, and
 * the refresh rate from the period in nanoseconds (0 = not told) */
static inline gint64 wl_presented_us (guint32 sec_hi, guint32 sec_lo, guint32 nsec)
{
    guint64 sec = ((guint64) sec_hi << 32) | sec_lo;
    /* (2^40 s are 34 000 years: beyond that the number is not a time, and
     * the microseconds and a clock offset added later must fit an int64) */
    sec = MIN (sec, G_GUINT64_CONSTANT (1) << 40);
    return (gint64) (sec * G_GUINT64_CONSTANT (1000000) + nsec / 1000);
}

static inline double wl_refresh_hz_from_ns (guint32 refresh_ns)
{
    return refresh_ns > 0 ? 1e9 / (double) refresh_ns : 0.0;
}

/* ------------------------------------------------------------ which layer
 * A wallpaper belongs on the layer shell's "background" layer, and that is
 * where it goes, with one exception: KWin, the compositor of KDE Plasma.
 * KWin keeps every background-layer surface in one stacking layer, and
 * Plasma's own desktop - a window that fills the monitor with Plasma's
 * wallpaper, the desktop icons and the widgets, opaque - is in that layer
 * too. Inside the layer KWin orders the windows as it orders ordinary
 * ones: a new one goes in front (KWin 6: right behind the active window
 * when that is Plasma's desktop), and a click on a window raises it. Our
 * surface takes no input, so a click on the "desktop" lands on Plasma's
 * window underneath, KWin raises that, and the wallpaper is behind an
 * opaque window from then on; nothing tells us, and no request of the
 * protocol brings a surface back to the front. Read in KWin 6.8.3 and
 * 5.27.11 (layershellv1window.cpp: belongsToLayer; layers.cpp:
 * raiseWindow, restackWindowUnderActive; window.cpp:
 * getMousePressCommand / getMouseCommand). Other wallpaper programs met
 * the same thing and answered it the same way (swww, mpvpaper: their
 * "bottom" layer option).
 *
 * On "bottom" KWin puts the surface one stacking layer higher: above
 * Plasma's desktop whatever is clicked, below every ordinary window. What
 * that costs: Plasma's desktop icons and widgets are behind the wallpaper
 * and cannot be seen. No layer puts a surface between Plasma's wallpaper
 * and its icons - they are one window. The add-on for Plasma (below)
 * shows them again, with the surface where it is.
 *
 * KWin is told by a global only it announces; these two are offered to
 * every client by 5.27 and by 6.8 (wayland_server.cpp). */
typedef enum { WL_LAYER_BACKGROUND = 0, WL_LAYER_BOTTOM = 1 } WlLayer;

typedef enum {
    WL_LAYER_WHY_DEFAULT = 0,   /* where a wallpaper belongs */
    WL_LAYER_WHY_KWIN,          /* the compositor is KWin */
    WL_LAYER_WHY_ASKED,         /* WALPE_WL_LAYER names it */
} WlLayerWhy;

static inline gboolean wl_global_is_kwin (const char *interface)
{
    return interface && (!strcmp (interface, "org_kde_plasma_shell") ||
                         !strcmp (interface, "org_kde_plasma_virtual_desktop_management"));
}

/* WALPE_WL_LAYER: 1 = it names a layer (*out), 0 = not set, empty or
 * "auto", -1 = no value this program knows (taken as "auto") */
static inline int wl_layer_parse (const char *text, WlLayer *out)
{
    if (!text || !*text || !g_ascii_strcasecmp (text, "auto"))
        return 0;
    if (!g_ascii_strcasecmp (text, "background")) {
        *out = WL_LAYER_BACKGROUND;
        return 1;
    }
    if (!g_ascii_strcasecmp (text, "bottom")) {
        *out = WL_LAYER_BOTTOM;
        return 1;
    }
    return -1;
}

static inline WlLayer wl_layer_choose (const char *asked, gboolean kwin, WlLayerWhy *why)
{
    WlLayer l = WL_LAYER_BACKGROUND;
    WlLayerWhy w = WL_LAYER_WHY_DEFAULT;
    if (wl_layer_parse (asked, &l) > 0) {
        w = WL_LAYER_WHY_ASKED;
    } else if (kwin) {
        l = WL_LAYER_BOTTOM;
        w = WL_LAYER_WHY_KWIN;
    }
    if (why)
        *why = w;
    return l;
}

static inline const char *wl_layer_name (WlLayer l)
{
    return l == WL_LAYER_BOTTOM ? "bottom" : "background";
}

/* ---------------------------------------------- the add-on for KDE Plasma
 * On the bottom layer the wallpaper covers Plasma's desktop icons. The
 * add-on in contrib/plasma (installed by the user, never by the build)
 * brings them back, and needs nothing from the daemon: a wallpaper type
 * "WalPE" for Plasma that makes Plasma's desktop window see-through, and
 * with it a KWin script that tells KWin to keep that window right above
 * the daemon's surface (workspace.constrain, KWin 6.5: KWin applies such
 * a rule after it has sorted the windows by layer, so it holds across
 * the two layers). The surface stays on the bottom layer in both cases:
 * an opaque desktop window can then never be in front of it, whatever
 * part of the add-on is missing, stopped or started late.
 *
 * What the daemon does with the add-on is tell the user about it: the
 * start-up log and status say whether the icons are hidden and what
 * brings them back. It learns that from two files and not from a D-Bus
 * conversation, which would cost every Plasma user a bus connection for
 * a sentence: the installed package, and the settings of Plasma's
 * desktops (does one have the wallpaper type). The files say what is
 * set up, not what is on screen this moment (Plasma writes its settings
 * a few seconds after a change). */
#define WL_PLASMA_TYPE_ID "io.github.walpe.plasma"

typedef enum {
    WL_PLASMA_ADDON_NONE = 0,     /* not installed */
    WL_PLASMA_ADDON_UNSELECTED,   /* installed, no desktop has the wallpaper type */
    WL_PLASMA_ADDON_SELECTED,     /* a desktop has the wallpaper type "WalPE" */
} WlPlasmaAddon;

/* Does a KDE settings file hold the line KEY=VALUE, in the group GROUP
 * (the text between the outer brackets of its header, e.g. "Plugins")
 * or, with GROUP NULL, in any group? */
static inline gboolean wl_kconfig_has (const char *text, const char *group, const char *key,
                                       const char *value)
{
    gboolean in_group = group == NULL;
    gsize klen = strlen (key), vlen = strlen (value);
    for (const char *p = text; p && *p;) {
        const char *end = strchr (p, '\n');
        gsize len = end ? (gsize) (end - p) : strlen (p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;
        if (len && p[0] == '[') {
            if (group)
                in_group = len == strlen (group) + 2 && p[len - 1] == ']' &&
                           !strncmp (p + 1, group, len - 2);
        } else if (in_group && len == klen + 1 + vlen && !strncmp (p, key, klen) && p[klen] == '=' &&
                   !strncmp (p + klen + 1, value, vlen)) {
            return TRUE;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return FALSE;
}

/* appletsrc: the settings of Plasma's desktops
 * (plasma-org.kde.plasma.desktop-appletsrc); NULL = no such file */
static inline WlPlasmaAddon wl_plasma_addon_state (gboolean installed, const char *appletsrc)
{
    if (!installed)
        return WL_PLASMA_ADDON_NONE;
    if (!wl_kconfig_has (appletsrc, NULL, "wallpaperplugin", WL_PLASMA_TYPE_ID))
        return WL_PLASMA_ADDON_UNSELECTED;
    return WL_PLASMA_ADDON_SELECTED;
}

static inline const char *wl_plasma_addon_name (WlPlasmaAddon a)
{
    switch (a) {
    case WL_PLASMA_ADDON_UNSELECTED:
        return "installed, no desktop has the wallpaper type WalPE";
    case WL_PLASMA_ADDON_SELECTED:
        return "in use: a desktop has the wallpaper type WalPE";
    default:
        return "not installed";
    }
}

/* The one line for the user, or NULL when there is nothing to do: on
 * KWin the icons are hidden unless the add-on is in use. With the
 * layer "background" asked for, the add-on works the same way, and
 * without it the log has said what happens to the wallpaper. */
static inline const char *wl_plasma_hint (gboolean kwin, WlPlasmaAddon a)
{
    if (!kwin || a == WL_PLASMA_ADDON_SELECTED)
        return NULL;
    if (a == WL_PLASMA_ADDON_UNSELECTED)
        return "Plasma's desktop icons and widgets are hidden behind the wallpaper. The WalPE add-on "
               "for Plasma is installed: to see them over the wallpaper, right-click the desktop "
               "and choose the wallpaper type \"WalPE\" in Plasma's \"Desktop and Wallpaper\" "
               "settings (in WalPE's source tree `contrib/plasma/install.sh select` does the same; "
               "docs/WAYLAND.md, \"KDE Plasma\")";
    return "Plasma's desktop icons and widgets are hidden behind the wallpaper. To see them over "
           "the wallpaper (Plasma 6.5 or newer), install the WalPE add-on for Plasma - a package "
           "of WalPE can bring it; from WalPE's source tree it is `contrib/plasma/install.sh "
           "install` - then right-click the desktop and choose the wallpaper type \"WalPE\" in "
           "Plasma's \"Desktop and Wallpaper\" settings (docs/WAYLAND.md, \"KDE Plasma\")";
}

/* ------------------------------------------------- layer surface life cycle
 * The role object (zwlr_layer_surface_v1) of a wallpaper surface:
 *
 *   NONE --create--> WAIT --configure--> READY --closed--> LOST or DEAD
 *                      |                   ^                 |
 *                      +--timeout--> ...   +---configure-- WAIT <--create--+
 *
 *   NONE   no role object yet.
 *   WAIT   created, set up and committed without a buffer; a buffer must
 *          not be attached before the first configure is acknowledged.
 *   READY  configured: frames may be presented. Every further configure
 *          is acknowledged with the next frame.
 *   LOST   the compositor closed it while its output is still there: the
 *          role may be created again (a bounded number of times, and not
 *          in the first moment), which needs an instant at which no frame
 *          is on its way - the render thread does it right before a swap.
 *   DEAD   final: the output is gone, the layer shell is gone, or the
 *          re-creations were used up. Reported to the daemon once.
 *
 * wl_ls_event () takes what happened and answers what to do (WL_LS_ACT_*). */
typedef enum { WL_LS_NONE = 0, WL_LS_WAIT, WL_LS_READY, WL_LS_LOST, WL_LS_DEAD } WlLsState;

typedef enum {
    WL_LS_EV_CREATE,            /* the role object was created and committed */
    WL_LS_EV_CONFIGURE,         /* configure (serial, w, h) */
    WL_LS_EV_CLOSED,            /* the compositor closed the surface */
    WL_LS_EV_OUTPUT_GONE,       /* its wl_output was removed */
    WL_LS_EV_SHELL_GONE,        /* the layer shell global was removed */
    WL_LS_EV_TIMEOUT,           /* no configure came in time after a create */
    WL_LS_EV_GIVE_UP,           /* LOST for too long: nobody draws on it */
} WlLsEvent;

enum {
    WL_LS_ACT_DESTROY_ROLE  = 1 << 0,   /* destroy the role object */
    WL_LS_ACT_REPORT_CLOSED = 1 << 1,   /* tell the daemon: the surface is gone for good */
    WL_LS_ACT_ASK_FRAME     = 1 << 2,   /* ask the daemon for a frame (the next swap re-creates) */
    WL_LS_ACT_SIZE_CHANGED  = 1 << 3,   /* the logical size differs from the one before */
    WL_LS_ACT_RECREATED     = 1 << 4,   /* READY again after LOST */
};

#define WL_LS_RECREATE_WINDOW_US (10 * (gint64) G_USEC_PER_SEC)
/* A closed surface is not made again at once: when an output is switched
 * off the compositor closes its surfaces first and removes the output a
 * moment later, and a role asked for on an output that is on its way out
 * may be put on another one. */
#define WL_LS_RECREATE_GRACE_US 150000

typedef struct {
    WlLsState state;
    gboolean ack_due;           /* `serial` is to be acknowledged before the next commit */
    guint32 serial;
    int conf_w, conf_h;         /* the last configure */
    gboolean output_alive;
    gboolean was_ready;         /* READY was reached at least once */
    gboolean recreating;        /* the WAIT in progress follows a LOST */
    int recreate_max;           /* re-creations allowed within one window; 0 = none */
    int attempts;               /* started in the current window */
    gint64 window_start_us;
    gint64 lost_since_us;       /* when it became LOST */
    guint recreated;            /* re-creations that ended READY */
    gboolean closed_reported;
} WlLayerState;

static inline void wl_ls_init (WlLayerState *s, int recreate_max)
{
    memset (s, 0, sizeof *s);
    s->output_alive = TRUE;
    s->recreate_max = recreate_max;
}

/* frames may be presented (a buffer may be attached) */
static inline gboolean wl_ls_can_present (const WlLayerState *s)
{
    return s->state == WL_LS_READY;
}

static inline gboolean wl_ls_budget (const WlLayerState *s, gint64 now_us)
{
    if (!s->output_alive || s->recreate_max < 1)
        return FALSE;
    if (s->attempts > 0 && now_us - s->window_start_us > WL_LS_RECREATE_WINDOW_US)
        return TRUE;            /* a new window starts with the next attempt */
    return s->attempts < s->recreate_max;
}

/* the render thread, before a swap: create the role again now? */
static inline gboolean wl_ls_wants_create (const WlLayerState *s, gint64 now_us)
{
    return s->state == WL_LS_LOST && wl_ls_budget (s, now_us) &&
           now_us - s->lost_since_us >= WL_LS_RECREATE_GRACE_US;
}

static inline guint wl_ls_fail (WlLayerState *s, gboolean had_role)
{
    guint act = had_role ? WL_LS_ACT_DESTROY_ROLE : 0;
    s->state = WL_LS_DEAD;
    s->ack_due = FALSE;
    s->recreating = FALSE;
    if (!s->closed_reported && s->was_ready) {
        s->closed_reported = TRUE;
        act |= WL_LS_ACT_REPORT_CLOSED;
    }
    return act;
}

static inline guint wl_ls_event (WlLayerState *s, WlLsEvent ev, guint32 serial, int w, int h,
                                 gint64 now_us)
{
    gboolean had_role = s->state == WL_LS_WAIT || s->state == WL_LS_READY;
    guint act = 0;
    switch (ev) {
    case WL_LS_EV_CREATE:
        if (s->state != WL_LS_NONE && s->state != WL_LS_LOST)
            return 0;
        if (s->state == WL_LS_LOST) {
            if (s->attempts == 0 || now_us - s->window_start_us > WL_LS_RECREATE_WINDOW_US) {
                s->attempts = 0;
                s->window_start_us = now_us;
            }
            s->attempts++;
            s->recreating = TRUE;
        }
        s->state = WL_LS_WAIT;
        s->ack_due = FALSE;
        return 0;
    case WL_LS_EV_CONFIGURE:
        if (!had_role)
            return 0;           /* an event that was on its way to a role already destroyed */
        /* Serials only grow. A configure older than the one already taken
         * can still turn up: one that sat on the surface's own queue when
         * the role object was handed to the main thread's queue, and is
         * delivered after a newer one came in there. It is out of date. */
        if (s->state == WL_LS_READY && (gint32) (serial - s->serial) < 0)
            return 0;
        if (s->was_ready && (w != s->conf_w || h != s->conf_h))
            act |= WL_LS_ACT_SIZE_CHANGED;
        s->serial = serial;
        s->ack_due = TRUE;
        s->conf_w = w;
        s->conf_h = h;
        if (s->state == WL_LS_WAIT) {
            s->state = WL_LS_READY;
            if (s->recreating) {
                s->recreating = FALSE;
                s->recreated++;
                act |= WL_LS_ACT_RECREATED;
            }
            s->was_ready = TRUE;
        }
        return act;
    case WL_LS_EV_CLOSED:
        if (!had_role)
            return 0;
        if (s->was_ready && wl_ls_budget (s, now_us)) {
            s->state = WL_LS_LOST;
            s->lost_since_us = now_us;
            s->ack_due = FALSE;
            s->recreating = FALSE;
            return WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_ASK_FRAME;
        }
        return wl_ls_fail (s, TRUE);
    case WL_LS_EV_OUTPUT_GONE:
        s->output_alive = FALSE;
        if (s->state == WL_LS_DEAD)
            return 0;
        return wl_ls_fail (s, had_role);
    case WL_LS_EV_SHELL_GONE:
        if (s->state == WL_LS_DEAD)
            return 0;
        return wl_ls_fail (s, had_role);
    case WL_LS_EV_TIMEOUT:
        if (s->state != WL_LS_WAIT)
            return 0;
        if (s->recreating && wl_ls_budget (s, now_us)) {
            s->state = WL_LS_LOST;
            s->lost_since_us = now_us;
            s->recreating = FALSE;
            return WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_ASK_FRAME;
        }
        return wl_ls_fail (s, TRUE);
    case WL_LS_EV_GIVE_UP:
        if (s->state != WL_LS_LOST)
            return 0;
        return wl_ls_fail (s, FALSE);
    }
    return 0;
}

static inline const char *wl_ls_state_name (WlLsState st)
{
    static const char *const names[] = { "none", "wait", "ready", "lost", "dead" };
    return (guint) st < G_N_ELEMENTS (names) ? names[st] : "?";
}
